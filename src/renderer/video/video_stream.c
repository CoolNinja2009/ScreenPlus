/* Stream wallpapers: online videos played the way mpv plays them, without saving them. FFmpeg (built
 * without network code) reads the media through this AVIOContext, from byte ranges fetched with WinHTTP
 * into RAM in chunks of NS_CHUNK bytes. Like mpv with yt-dlp, a page's best picture and best sound are
 * two files (YouTube offers more than 360p only that way): each is one NetStream (track 0 and 1), and the
 * video source reads both side by side. RAM holds only two parts of a video: its first HEAD_SECONDS,
 * kept for good so every loop restarts at once, and the next AHEAD_SECONDS from where it plays.
 * Footage already played is dropped and fetched again on the next loop, so a long video costs a few
 * tens of MB, not its size. When the wallpaper pauses, FFmpeg stops reading and the network goes quiet. Links to pages (WPFLAG_YTDLP) are resolved by yt-dlp to media addresses (common/ytdlp.c,
 * cached in cache\streams until shortly before they expire) and resolved again, to the same formats,
 * before they lapse or when the server refuses them. Network trouble pauses the picture and is retried
 * with backoff. */
#include "video_internal.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include "../../common/ytdlp.h"
#include <winhttp.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define NS_CHUNK (4 << 20)
#define NS_SLOTS 8                       /* until the size is known */
#define NS_AHEAD 6                       /* chunks ahead when the length is unknown */
#define HEAD_SECONDS  20.0               /* kept for good: every loop starts at once */
#define AHEAD_SECONDS 30.0               /* kept ready ahead of the picture */
#define NS_MAXCHUNKS  32                 /* each part: 128 MB at most */
#define NS_IOBUF (64 << 10)
#define ST_NORANGES 1000                 /* range_get: the server sends whole files only */

typedef struct Slot {
    int64_t  index;                      /* chunk number, -1: empty */
    int      len;
    int      state;                      /* 0 empty, 1 loading, 2 ready */
    uint32_t used;
    uint8_t *data;
} Slot;

struct NetStream {
    wchar_t            page[SP_MAX_PATH];  /* the wallpaper's link */
    int                ytdlp, maxRes;
    int                track;              /* 0: picture (or both), 1: sound */
    volatile LONG     *quit;               /* the video source is closing */
    YtStream          *yt;                 /* resolved address (ytdlp) */
    const wchar_t     *url;                /* media address in use */
    int64_t            size;
    int64_t            pos;                /* read position (decode thread only) */
    double             resolvedAt;         /* time_ms() of the last yt-dlp run, 0: none */

    SRWLOCK            lock;
    CONDITION_VARIABLE work, ready;
    Slot              *slots;
    int                slotCount;          /* chunks the RAM cache holds */
    int                ahead;              /* chunks kept ready ahead of the reader */
    int                head;               /* first chunks, never dropped */
    int64_t            want;               /* chunk the reader is in */
    uint32_t           tick;
    int                stop, failed;
    wchar_t            error[160];         /* why it failed for good */
    HANDLE             thread, stopEvent;

    HINTERNET          session, conn;      /* kept alive between ranges (prefetch thread, or open) */
    HINTERNET volatile req;                /* the range in flight: closing it cancels it */
    wchar_t            host[256];
    INTERNET_PORT      port;
    AVIOContext       *avio;
};

/* ---------------------------------------------------------------- network */

/* GET bytes [start, start + cap) into buf. 0: got them (or the whole resource, when it is that small
 * and the server ignores ranges); else the HTTP status, -1 for a network error, ST_NORANGES. */
static int range_get(NetStream *n, int64_t start, uint8_t *buf, int cap, int *got, int64_t *total)
{
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    wchar_t host[256];
    *got = 0;
    if (!WinHttpCrackUrl(n->url, 0, 0, &uc) || !uc.dwHostNameLength || uc.dwHostNameLength >= SP_COUNTOF(host)) return 400;
    str_copyn(host, SP_COUNTOF(host), uc.lpszHostName, uc.dwHostNameLength);
    if (!n->session) {
        n->session = WinHttpOpen(L"Mozilla/5.0 (compatible; ScreenPlus/" SP_VERSION_W L")", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!n->session) return -1;
        WinHttpSetTimeouts(n->session, 10000, 10000, 20000, 30000);
    }
    if (!n->conn || wcscmp(host, n->host) || uc.nPort != n->port) {
        if (n->conn) WinHttpCloseHandle(n->conn);
        n->conn = WinHttpConnect(n->session, host, uc.nPort, 0);
        str_copy(n->host, SP_COUNTOF(n->host), host);
        n->port = uc.nPort;
        if (!n->conn) return -1;
    }
    const wchar_t *path = uc.lpszUrlPath ? uc.lpszUrlPath : uc.lpszExtraInfo;
    size_t plen = path ? (size_t)uc.dwUrlPathLength + uc.dwExtraInfoLength : 0;
    wchar_t *object = (wchar_t *)calloc(plen + 2, sizeof(wchar_t));
    if (!object) return -1;
    str_copyn(object, plen + 2, path, plen);
    wchar_t *frag = wcschr(object, L'#');
    if (frag) *frag = 0;
    if (!object[0]) str_copy(object, plen + 2, L"/");
    HINTERNET req = WinHttpOpenRequest(n->conn, L"GET", object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    free(object);
    if (!req) return -1;
    InterlockedExchangePointer((PVOID volatile *)&n->req, req);
    wchar_t range[80];
    str_printf(range, SP_COUNTOF(range), L"Range: bytes=%lld-%lld\r\n", start, start + cap - 1);
    WinHttpAddRequestHeaders(req, range, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (n->yt && n->yt->headers[0])                /* the headers yt-dlp used to get the address */
        WinHttpAddRequestHeaders(req, n->yt->headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    int status = -1;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD st = 0, sz = sizeof st;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        status = (int)st;
        wchar_t v[96];
        sz = sizeof v;
        if (status == 206 && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, v, &sz,
                                                 WINHTTP_NO_HEADER_INDEX)) {
            const wchar_t *slash = wcschr(v, L'/');      /* "bytes 0-4194303/123456789" */
            if (slash && slash[1] != L'*') *total = _wcstoi64(slash + 1, NULL, 10);
        } else if (status == 200) {
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, v, &sz, WINHTTP_NO_HEADER_INDEX))
                *total = _wcstoi64(v, NULL, 10);
            if (start != 0 || *total <= 0 || *total > cap) status = ST_NORANGES;
        }
        if (status == 206 || status == 200) {
            DWORD r = 0;
            while (*got < cap && WinHttpReadData(req, buf + *got, (DWORD)(cap - *got), &r) && r) *got += (int)r;
            int64_t expect = *total > 0 ? SP_MIN((int64_t)cap, *total - start) : cap;
            status = *got >= expect ? 0 : -1;    /* cut short: retried */
        }
    }
    if (InterlockedExchangePointer((PVOID volatile *)&n->req, NULL)) WinHttpCloseHandle(req);
    return status;
}

/* A fresh address for the same format (YouTube's last about six hours). At most once a minute. */
static int refresh(NetStream *n)
{
    double now = time_ms();
    if (!n->ytdlp || (n->resolvedAt && now - n->resolvedAt < 60000.0)) return 0;
    n->resolvedAt = now;
    YtStream *fresh = (YtStream *)calloc(1, sizeof *fresh);
    if (!fresh) return 0;
    int64_t was = n->track ? fresh->asize : fresh->size;
    int ok = ytdlp_resolve(n->page, n->maxRes, n->yt->format, n->stopEvent, NULL, fresh) && fresh->streamable &&
             (!n->track || fresh->aurl[0]);
    was = n->track ? fresh->asize : fresh->size;
    ok = ok && (was <= 0 || n->size <= 0 || was == n->size);                 /* the same file */
    if (ok) {
        memcpy(n->yt, fresh, sizeof *fresh);
        n->url = n->track ? n->yt->aurl : n->yt->url;
        ytdlp_cache_save(n->page, n->yt);
        LOGI(L"video: new stream address for %ls", n->page);
    } else if (!n->stop) {
        LOGW(L"video: could not renew the stream address for %ls: %ls", n->page, fresh->error);
    }
    free(fresh);
    return ok;
}

static BOOL fetch(NetStream *n, int64_t idx, uint8_t *buf, int *len)
{
    int64_t start = idx * (int64_t)NS_CHUNK;
    int want = (int)SP_MIN((int64_t)NS_CHUNK, n->size - start);
    DWORD delay = 1000;
    for (int tries = 0; !n->stop; tries++) {
        if (n->ytdlp && n->yt->expires && time_unix_now() > n->yt->expires - 600) refresh(n);   /* before it lapses */
        int64_t total = -1;
        int st = range_get(n, start, buf, want, len, &total);
        if (st == 0) return TRUE;
        if (n->stop) break;
        if ((st == 403 || st == 404 || st == 410) && refresh(n)) continue;          /* an expired address */
        if (st == ST_NORANGES || (st >= 400 && st < 500 && st != 408 && st != 429)) {
            LOGW(L"video: stream %ls refused (HTTP %d)", n->page, st);
            str_printf(n->error, SP_COUNTOF(n->error), L"The video's server stopped sending it (HTTP %d)", st);
            return FALSE;
        }
        if (tries == 2) LOGW(L"video: stream %ls: network trouble (%d), retrying", n->page, st);
        if (WaitForSingleObject(n->stopEvent, delay) == WAIT_OBJECT_0) break;
        delay = SP_MIN(delay * 2, 30000u);
    }
    return FALSE;
}

/* ---------------------------------------------------------------- cache */

static Slot *find(NetStream *n, int64_t idx)
{
    for (int i = 0; i < n->slotCount; i++)
        if (n->slots[i].state && n->slots[i].index == idx) return &n->slots[i];
    return NULL;
}

/* Where the next chunk goes: an empty slot, else the least recently used one outside the read
 * window (the first chunk stays). NULL: every slot is needed. */
static Slot *victim(NetStream *n)
{
    Slot *best = NULL;
    for (int i = 0; i < n->slotCount; i++) {
        Slot *s = &n->slots[i];
        if (s->state == 0) return s;
        if (s->state == 1 || s->index < n->head || (s->index >= n->want && s->index < n->want + n->ahead)) continue;
        if (!best || (int32_t)(s->used - best->used) < 0) best = s;
    }
    return best;
}

static unsigned __stdcall prefetch_main(void *arg)
{
    NetStream *n = (NetStream *)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus video stream");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int64_t chunks = (n->size + NS_CHUNK - 1) / NS_CHUNK;
    AcquireSRWLockExclusive(&n->lock);
    while (!n->stop) {
        int64_t next = -1;
        for (int64_t k = 0; k < n->ahead && k < chunks && next < 0; k++) {
            int64_t i = (n->want + k) % chunks;    /* near the end: the start again, for the loop */
            if (!find(n, i)) next = i;
        }
        Slot *s = next >= 0 ? victim(n) : NULL;
        if (!s) {                                  /* window full: wait for the reader to move on */
            SleepConditionVariableSRW(&n->work, &n->lock, INFINITE, 0);
            continue;
        }
        s->index = next;
        s->state = 1;
        if (!s->data) s->data = (uint8_t *)malloc(NS_CHUNK);
        ReleaseSRWLockExclusive(&n->lock);
        int len = 0;
        BOOL ok = s->data && fetch(n, next, s->data, &len);
        AcquireSRWLockExclusive(&n->lock);
        if (ok) {
            s->len = len;
            s->state = 2;
            s->used = ++n->tick;
        } else {
            s->state = 0;
            s->index = -1;
            if (!n->stop) n->failed = 1;
        }
        WakeAllConditionVariable(&n->ready);
    }
    ReleaseSRWLockExclusive(&n->lock);
    return 0;
}

/* ---------------------------------------------------------------- AVIOContext */

static int ns_read(void *opaque, uint8_t *buf, int size)
{
    NetStream *n = (NetStream *)opaque;
    if (n->pos >= n->size) return AVERROR_EOF;
    int64_t idx = n->pos / NS_CHUNK;
    AcquireSRWLockExclusive(&n->lock);
    if (n->want != idx) {
        n->want = idx;
        WakeConditionVariable(&n->work);
    }
    Slot *s;
    while (!(s = find(n, idx)) || s->state != 2) {
        if (*n->quit || n->stop || n->failed) {
            int r = n->failed ? AVERROR(EIO) : AVERROR_EXIT;
            ReleaseSRWLockExclusive(&n->lock);
            return r;
        }
        WakeConditionVariable(&n->work);
        SleepConditionVariableSRW(&n->ready, &n->lock, 250, 0);   /* re-checks quit while the network is slow */
    }
    int off = (int)(n->pos - idx * NS_CHUNK), take = SP_MIN(size, s->len - off);
    if (take > 0) {
        memcpy(buf, s->data + off, (size_t)take);
        s->used = ++n->tick;
    }
    ReleaseSRWLockExclusive(&n->lock);
    if (take <= 0) return AVERROR_EOF;
    n->pos += take;
    return take;
}

static int64_t ns_seek(void *opaque, int64_t offset, int whence)
{
    NetStream *n = (NetStream *)opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return n->size;
    int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? n->pos + offset : whence == SEEK_END ? n->size + offset : -1;
    if (p < 0 || p > n->size) return AVERROR(EINVAL);
    n->pos = p;
    return p;
}

/* ---------------------------------------------------------------- lifetime */

/* Chunks that hold `seconds` of a file of `size` bytes lasting `duration` seconds. */
static int chunks_for(double seconds, int64_t size, double duration, int fallback)
{
    if (duration <= 0 || size <= 0) return fallback;
    double bytes = seconds * (double)size / duration;
    return (int)SP_CLAMP((int64_t)(bytes / NS_CHUNK) + 1, 1, NS_MAXCHUNKS);
}

NetStream *netstream_open(const wchar_t *url, int ytdlp, int maxRes, int track, volatile LONG *quit, wchar_t *err,
                          size_t errCap)
{
    NetStream *n = (NetStream *)calloc(1, sizeof *n);
    if (!n) {
        str_copy(err, errCap, L"Out of memory");
        return NULL;
    }
    double t0 = time_ms();
    str_copy(n->page, SP_COUNTOF(n->page), url);
    n->ytdlp = ytdlp;
    n->track = track;
    n->maxRes = maxRes > 0 ? maxRes : 1080;
    n->slotCount = NS_SLOTS;
    n->ahead = NS_AHEAD;
    n->head = 1;
    n->quit = quit;
    n->url = n->page;
    InitializeSRWLock(&n->lock);
    InitializeConditionVariable(&n->work);
    InitializeConditionVariable(&n->ready);
    n->slots = (Slot *)calloc(NS_SLOTS, sizeof *n->slots);
    for (int i = 0; n->slots && i < NS_SLOTS; i++) n->slots[i].index = -1;
    n->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Slot *s0 = n->slots;
    if (s0) s0->data = (uint8_t *)malloc(NS_CHUNK);
    if (!n->stopEvent || !s0 || !s0->data || (ytdlp && !(n->yt = (YtStream *)calloc(1, sizeof *n->yt)))) {
        str_copy(err, errCap, L"Out of memory");
        netstream_close(n);
        return NULL;
    }
    int cached = 0, got = 0, st = -1;
    int64_t total = -1;
    if (ytdlp) {
        cached = ytdlp_cache_load(url, n->yt);
        if (!cached) {
            n->resolvedAt = time_ms();
            if (!ytdlp_resolve(url, n->maxRes, NULL, n->stopEvent, quit, n->yt) || !n->yt->streamable) {
                str_copy(err, errCap, n->yt->error[0] ? n->yt->error : L"This video can't be streamed; import it with Download instead");
                netstream_close(n);
                return NULL;
            }
            ytdlp_cache_save(url, n->yt);
        }
        if (track && !n->yt->aurl[0]) {               /* one file holds both: nothing more to read */
            err[0] = 0;
            netstream_close(n);
            return NULL;
        }
        n->url = track ? n->yt->aurl : n->yt->url;
    } else if (track) {
        err[0] = 0;
        netstream_close(n);
        return NULL;
    }
    /* the first chunk tells the size and stays: every loop starts there */
    st = range_get(n, 0, s0->data, NS_CHUNK, &got, &total);
    if (st != 0 && cached && !*quit && refresh(n)) st = range_get(n, 0, s0->data, NS_CHUNK, &got, &total);
    if (st != 0) {
        if (st == ST_NORANGES) str_copy(err, errCap, L"The server doesn't allow streaming this video");
        else if (st > 0) str_printf(err, errCap, L"The video could not be loaded (HTTP %d)", st);
        else str_copy(err, errCap, L"The video's server could not be reached");
        netstream_close(n);
        return NULL;
    }
    n->size = total > 0 ? total : got;
    int64_t chunks = (n->size + NS_CHUNK - 1) / NS_CHUNK;
    double duration = n->yt ? n->yt->duration : 0;
    n->head = chunks_for(HEAD_SECONDS, n->size, duration, 2);
    n->ahead = SP_MAX(chunks_for(AHEAD_SECONDS, n->size, duration, NS_AHEAD), 2);
    n->slotCount = (int)SP_MIN((int64_t)n->head + n->ahead + 2, SP_MAX(chunks, 1));   /* +2: the one in use, one spare */
    n->slotCount = SP_MAX(n->slotCount, NS_SLOTS);
    if (n->slotCount > NS_SLOTS) {
        Slot *more = (Slot *)realloc(n->slots, (size_t)n->slotCount * sizeof *more);
        if (!more) {
            n->slotCount = NS_SLOTS;
            n->head = 1;
            n->ahead = NS_AHEAD;
        } else {
            n->slots = more;
            s0 = &n->slots[0];
            ZeroMemory(&n->slots[NS_SLOTS], (size_t)(n->slotCount - NS_SLOTS) * sizeof *more);
            for (int i = NS_SLOTS; i < n->slotCount; i++) n->slots[i].index = -1;
        }
    }
    s0->index = 0;
    s0->len = got;
    s0->state = 2;
    s0->used = ++n->tick;
    uint8_t *iob = (uint8_t *)av_malloc(NS_IOBUF);
    n->avio = iob ? avio_alloc_context(iob, NS_IOBUF, 0, n, ns_read, NULL, ns_seek) : NULL;
    if (!n->avio) av_free(iob);
    if (n->avio) n->thread = (HANDLE)_beginthreadex(NULL, 0, prefetch_main, n, 0, NULL);
    if (!n->thread) {
        str_copy(err, errCap, L"Out of memory");
        netstream_close(n);
        return NULL;
    }
    LOGI(L"video: streaming %ls%ls (%.0f MB, RAM %d MB: start %d + ahead %d MB%ls), started in %.0f ms", track ? L"sound of " : L"",
         url, n->size / 1048576.0, (int)SP_MIN((int64_t)n->slotCount, chunks) * (NS_CHUNK >> 20), n->head * (NS_CHUNK >> 20), n->ahead * (NS_CHUNK >> 20),
         !ytdlp ? L"" : cached ? L", saved address" : L", via yt-dlp", time_ms() - t0);
    return n;
}

AVIOContext *netstream_avio(NetStream *n)
{
    avio_seek(n->avio, 0, SEEK_SET);
    return n->avio;
}

const wchar_t *netstream_failure(NetStream *n)
{
    AcquireSRWLockShared(&n->lock);
    const wchar_t *e = n->failed ? (n->error[0] ? n->error : L"The video stream stopped") : NULL;
    ReleaseSRWLockShared(&n->lock);
    return e;
}

void netstream_close(NetStream *n)
{
    if (!n) return;
    AcquireSRWLockExclusive(&n->lock);
    n->stop = 1;
    WakeAllConditionVariable(&n->work);
    WakeAllConditionVariable(&n->ready);
    ReleaseSRWLockExclusive(&n->lock);
    if (n->stopEvent) SetEvent(n->stopEvent);
    if (n->thread) {
        if (WaitForSingleObject(n->thread, 1500) == WAIT_TIMEOUT) {
            HINTERNET req = InterlockedExchangePointer((PVOID volatile *)&n->req, NULL);
            if (req) WinHttpCloseHandle(req);      /* cancels the range in flight */
            if (WaitForSingleObject(n->thread, 8000) == WAIT_TIMEOUT) {
                LOGE(L"video: stream thread for %ls did not stop; leaking it", n->page);
                return;
            }
        }
        CloseHandle(n->thread);
    }
    if (n->avio) {
        av_freep(&n->avio->buffer);
        avio_context_free(&n->avio);
    }
    for (int i = 0; n->slots && i < n->slotCount; i++) free(n->slots[i].data);
    free(n->slots);
    if (n->conn) WinHttpCloseHandle(n->conn);
    if (n->session) WinHttpCloseHandle(n->session);
    if (n->stopEvent) CloseHandle(n->stopEvent);
    free(n->yt);
    free(n);
}
