/* Store: live wallpapers and still pictures from several sites (store_sources.c, docs/store/sources.md) in one
 * feed, searched and filtered as if it were one site, with copies of one wallpaper folded into the best of them.
 *   - A listing reads page n of every site the filters leave in, all at once, and merges the round one card from
 *     each site in turn, once all are in or ROUND_MS after the first (FIRST_MS for a listing's first round, which
 *     also pays every site's TLS handshake, so the first screen mixes them): a slow site never holds the feed back,
 *     its cards join later. The next round is asked for a screen and a half before the end is reached.
 *   - Copies: every preview gets a 64-bit difference hash of its centre 16:9. Two cards of one kind whose hashes
 *     are near (or a little further apart when their titles agree) are one wallpaper: the one with more pixels,
 *     then the larger file (more detail at one resolution), stays. A copy whose resolution only its page tells
 *     has that page read first.
 *   - Speed: WORKERS threads share one keep-alive HTTP/2 session (http_open_shared). Page reads go first, then
 *     previews newest first (what is on screen), then the pages of cards on screen. Previews are the sites' own
 *     small crops (WebP where Windows decodes it: the ones browsers fetch, so the ones CDNs keep), cached
 *     CACHE_DAYS in <cache>\store.
 *   - Download hands the best file to url_import.c, one job at a time, so presses queue. A site that hands files
 *     out only on its own page (a captcha, a download limit, an encrypted link) gets that page opened instead.
 * The UI thread owns all state; a worker gets a copy of what it needs and posts its result back (WM_SP_STORE),
 * where results of an older listing are dropped. */
#include "store_src.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include <wincodec.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKERS     8
#define MAX_SITES   8
#define PAGE_MAX    (2 << 20)       /* bytes kept of a page */
#define IMAGE_MAX   (2 << 20)
#define WAIT_MAX    64              /* previews and details waiting; beyond, the oldest previews are dropped */
#define ITEMS_MAX   3000
#define QUEUED_MAX  32              /* downloads waiting for url_import */
#define CACHE_DAYS  7
#define DAY_TICKS   864000000000ull /* FILETIME units */
#define ROUND_MS    450
#define FIRST_MS    1000

enum { T_LIST = 0, T_DETAIL, T_PREVIEW };

typedef struct Task {
    struct Task *next;
    int          kind, gen, index, source, page, urgent, cached;
    uint32_t     key;
    StoreQuery   query;
    char         tag[96];
    StoreItem    item;              /* T_DETAIL, T_PREVIEW: a copy of the card */
    wchar_t      url[600];
    wchar_t      dest[SP_MAX_PATH]; /* T_PREVIEW: the cache file */
} Task;

typedef struct Result {
    int        kind, gen, index, source, page, ok, hashed;
    uint32_t   key;
    uint64_t   hash;
    StoreList  list;                /* T_LIST */
    StoreItem  item;                /* T_DETAIL: the card with what its page told */
    wchar_t    error[200];
} Result;

typedef struct Queued {
    uint32_t key;
    wchar_t  url[200];
    wchar_t  title[SP_NAME_LEN];
} Queued;

typedef struct Site {               /* a site's part in the current listing */
    int       page, more, busy, off, got;
    char      tag[96];
    StoreList round;                /* its last page, until the round is merged */
    wchar_t   error[200];
} Site;

static struct {
    SRWLOCK            lock;
    CONDITION_VARIABLE wake;
    Task              *lists, *previews, *details;   /* lists and urgent details: oldest first; the rest newest */
    int                waiting;                       /* previews + details */
    int                quit;
    HANDLE             threads[WORKERS];
    int                threadCount;
} Q = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT };

static StoreState S;
static StoreQuery g_query;
static Site       g_site[MAX_SITES];
static int        g_gen, g_started, g_cap, g_viewCap, g_out;   /* g_out: sites still to answer this round */
static UINT_PTR   g_roundTimer;
static wchar_t    g_cache[SP_MAX_PATH];
static int        g_webp;           /* previews in WebP: Windows decodes it */
static Queued     g_queue[QUEUED_MAX];
static int        g_queued, g_hasActive, g_seen;
static uint32_t   g_active;         /* the card url_import is downloading for the Store; g_seen: finished then */
static uint32_t   g_done[256];      /* cards added to the library this session (a ring) */
static int        g_doneCount;

/* ---------------------------------------------------------------- workers */

static char *fetch_text(const wchar_t *url, int *len, int *status, wchar_t *err, size_t cap)
{
    HttpGet *g = http_open_shared(url, err, cap);
    if (!g) return NULL;
    char *buf = NULL;
    *status = http_status(g);
    if (*status == 200) {
        int size = 256 << 10, n = 0;
        buf = (char *)malloc((size_t)size + 1);
        while (buf) {
            n += http_read(g, buf + n, size - n);
            if (n < size || size >= PAGE_MAX) break;    /* http_read stops short only at the end */
            char *more = (char *)realloc(buf, (size_t)size * 2 + 1);
            if (!more) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = more;
            size *= 2;
        }
        if (buf) {
            buf[n] = 0;
            *len = n;
        } else {
            str_copy(err, cap, L"Out of memory");
        }
    } else {
        str_printf(err, cap, L"the site answered with HTTP %d", *status);
    }
    http_close(g);
    return buf;
}

static int fetch_file(const wchar_t *url, const wchar_t *dest, wchar_t *err, size_t cap)
{
    HttpGet *g = http_open_shared(url, err, cap);
    if (!g) return 0;
    int ok = 0;
    if (http_status(g) == 200 && str_istarts(http_content_type(g), L"image/")) {
        wchar_t part[SP_MAX_PATH];
        str_printf(part, SP_COUNTOF(part), L"%ls.part", dest);    /* the thumbnail loader never sees half a file */
        ok = http_save(g, part, IMAGE_MAX, NULL, NULL, NULL, err, cap) && MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileW(part);
    } else {
        str_printf(err, cap, L"no preview (HTTP %d)", http_status(g));
    }
    http_close(g);
    return ok;
}

/* Difference hash of the picture's centre 16:9 (so one picture cropped differently by two sites lines up):
 * shrunk to 9 x 8 grey, one bit per pair of neighbours, set where the left one is brighter. */
static uint64_t dhash(IWICImagingFactory *f, const wchar_t *path, int *ok)
{
    IWICBitmapDecoder *d = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapClipper *cl = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    UINT w = 0, h = 0;
    uint64_t hash = 0;
    BYTE px[72];
    *ok = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &w, &h)) && w > 16 && h > 16) {
        WICRect rc = { 0, 0, (INT)w, (INT)h };
        if ((uint64_t)w * 9 > (uint64_t)h * 16) {
            rc.Width = (INT)((uint64_t)h * 16 / 9);
            rc.X = ((INT)w - rc.Width) / 2;
        } else {
            rc.Height = (INT)((uint64_t)w * 9 / 16);
            rc.Y = ((INT)h - rc.Height) / 2;
        }
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(f, &cl)) &&
            SUCCEEDED(IWICBitmapClipper_Initialize(cl, (IWICBitmapSource *)fr, &rc)) &&
            SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) &&
            SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)cl, 9, 8, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat8bppGray,
                                                     WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, 9, sizeof px, px))) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) hash = (hash << 1) | (uint64_t)(px[y * 9 + x] > px[y * 9 + x + 1]);
            *ok = 1;
        }
    }
    SP_RELEASE(cv);
    SP_RELEASE(sc);
    SP_RELEASE(cl);
    SP_RELEASE(fr);
    SP_RELEASE(d);
    return hash;
}

static void run(const Task *t, IWICImagingFactory *wic)
{
    Result *r = (Result *)calloc(1, sizeof *r);
    if (!r) return;
    const StoreSource *src = &k_sources[t->source];
    r->kind = t->kind;
    r->gen = t->gen;
    r->index = t->index;
    r->source = t->source;
    r->page = t->page;
    r->key = t->key;
    if (t->kind == T_PREVIEW) {
        r->ok = t->cached || fetch_file(t->url, t->dest, r->error, SP_COUNTOF(r->error));
        if (r->ok && wic) r->hash = dhash(wic, t->dest, &r->hashed);
    } else {
        int len = 0, status = 0;
        char *html = fetch_text(t->url, &len, &status, r->error, SP_COUNTOF(r->error));
        if (html && t->kind == T_LIST) {
            src->parse_list(&t->query, t->page, t->tag, html, len, &r->list);
            r->ok = 1;
        } else if (html) {
            r->item = t->item;
            r->ok = src->parse_detail(html, len, &r->item);
            if (!r->ok) str_copy(r->error, SP_COUNTOF(r->error), L"its page lists no files");
        } else {
            r->ok = t->kind == T_LIST && t->page > 1 && status == 404;    /* past a listing's last page: the end */
        }
        free(html);
    }
    if (!PostMessageW(g_app.shellWnd, WM_SP_STORE, 0, (LPARAM)r)) {
        free(r->list.items);
        free(r);
    }
}

static Task *pop_locked(void)
{
    Task **head = Q.lists ? &Q.lists : Q.previews ? &Q.previews : Q.details ? &Q.details : NULL;
    if (!head) return NULL;
    Task *t = *head;
    *head = t->next;
    if (head != &Q.lists) Q.waiting--;
    return t;
}

static DWORD WINAPI worker(LPVOID arg)
{
    IWICImagingFactory *wic = NULL;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)arg;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
    for (;;) {
        Task *t = NULL;
        AcquireSRWLockExclusive(&Q.lock);
        while (!Q.quit) {
            t = pop_locked();
            if (t) break;
            SleepConditionVariableSRW(&Q.wake, &Q.lock, INFINITE, 0);
        }
        ReleaseSRWLockExclusive(&Q.lock);
        if (!t) break;
        run(t, wic);
        free(t);
    }
    SP_RELEASE(wic);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------- cache and queue (UI thread) */

static uint64_t age(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime, now = filetime_now();
    return now > t ? now - t : 0;
}

static int fresh(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) && age(&a.ftLastWriteTime) < CACHE_DAYS * DAY_TICKS;
}

/* the preview's address and its cache file (named after the address) */
static void preview_path(const StoreItem *it, wchar_t *url, size_t ucap, wchar_t *path, size_t pcap)
{
    wchar_t name[24];
    k_sources[it->source].preview_url(it, it->previewBig, g_webp, url, ucap);
    str_printf(name, SP_COUNTOF(name), L"%08x.img", hash_fnv1a_w(url));
    path_join(path, pcap, g_cache, name);
}

/* Previews older than CACHE_DAYS go (and parts a crash left); the rest make a revisit instant. */
static void sweep_cache(void)
{
    wchar_t pat[SP_MAX_PATH], p[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    path_join(pat, SP_COUNTOF(pat), g_cache, L"*");
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (age(&fd.ftLastWriteTime) >= CACHE_DAYS * DAY_TICKS || wcsstr(fd.cFileName, L".part")) {
            path_join(p, SP_COUNTOF(p), g_cache, fd.cFileName);
            DeleteFileW(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Windows decodes WebP with its WebP Image Extension (there on most PCs); the thumbnail worker goes through WIC. */
static int webp_decodes(void)
{
    int ok = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) && f) {
        IWICBitmapDecoder *d = NULL;
        ok = SUCCEEDED(IWICImagingFactory_CreateDecoder(f, &GUID_ContainerFormatWebp, NULL, &d)) && d;
        SP_RELEASE(d);
        SP_RELEASE(f);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

static BOOL start(void)
{
    if (Q.threadCount) return TRUE;
    path_join(g_cache, SP_COUNTOF(g_cache), paths_cache(), L"store");
    dir_create_recursive(g_cache);
    sweep_cache();
    g_webp = webp_decodes();
    LOGI(L"store: %d sites, previews as %ls", k_sourceCount, g_webp ? L"WebP" : L"JPEG (no WebP codec)");
    for (int i = 0; i < WORKERS; i++) {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) Q.threads[Q.threadCount++] = h;
    }
    if (Q.threadCount) return TRUE;
    S.failed = 1;
    str_copy(S.error, SP_COUNTOF(S.error), L"The Store could not start.");
    return FALSE;
}

/* a task dropped unserved: its card asks again when it is back near the screen */
static void undo(const Task *t)
{
    if (t->gen != g_gen || t->index < 0 || t->index >= S.count || S.items[t->index].key != t->key) return;
    if (t->kind == T_PREVIEW) S.items[t->index].preview = STORE_NONE;
    else S.items[t->index].detail = STORE_NONE;
}

static void push(Task *t)
{
    Task *drop = NULL;
    AcquireSRWLockExclusive(&Q.lock);
    if (t->kind == T_LIST || t->urgent) {
        Task **p = &Q.lists;
        while (*p) p = &(*p)->next;
        *p = t;
    } else {
        Task **head = t->kind == T_DETAIL ? &Q.details : &Q.previews;
        t->next = *head;
        *head = t;
        if (++Q.waiting > WAIT_MAX) {          /* scrolled past: the oldest request is the least wanted */
            Task **p = Q.previews ? &Q.previews : &Q.details;
            while ((*p)->next) p = &(*p)->next;
            drop = *p;
            *p = NULL;
            Q.waiting--;
        }
    }
    ReleaseSRWLockExclusive(&Q.lock);
    WakeConditionVariable(&Q.wake);
    if (drop) {
        undo(drop);
        free(drop);
    }
}

static void purge(void)
{
    AcquireSRWLockExclusive(&Q.lock);
    Task *all[3] = { Q.lists, Q.previews, Q.details };
    Q.lists = Q.previews = Q.details = NULL;
    Q.waiting = 0;
    ReleaseSRWLockExclusive(&Q.lock);
    for (int i = 0; i < 3; i++) {
        while (all[i]) {
            Task *n = all[i]->next;
            free(all[i]);
            all[i] = n;
        }
    }
}

static Task *task(int kind, int index)
{
    Task *t = (Task *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->kind = kind;
    t->gen = g_gen;
    t->index = index;
    if (index >= 0) {
        t->item = S.items[index];
        t->source = t->item.source;
        t->key = t->item.key;
    }
    return t;
}

static void want_detail(int i, int urgent)
{
    StoreItem *it = &S.items[i];
    Task *t = k_sources[it->source].detail_url ? task(T_DETAIL, i) : NULL;
    if (!t) return;
    t->urgent = urgent;
    if (!k_sources[it->source].detail_url(it, t->url, SP_COUNTOF(t->url))) {
        free(t);
        return;
    }
    if (it->detail != STORE_READY) it->detail = STORE_LOADING;
    push(t);
}

/* ---------------------------------------------------------------- copies of one wallpaper */

static int64_t pixels(const StoreItem *it)
{
    int64_t px = (int64_t)it->w * it->h;
    for (int i = 0; i < it->varCount; i++)
        if (strcmp(it->var[i].key, "gif") != 0) px = SP_MAX(px, (int64_t)it->var[i].w * it->var[i].h);
    if (!px && it->badge[0]) {
        int w, h;
        store_badge_res(it->badge, &w, &h);
        px = (int64_t)w * h;
    }
    return px;
}

static float megabytes(const StoreItem *it)
{
    const StoreVariant *v = store_best(it);
    return v && v->mb > 0 ? v->mb : it->mb;
}

/* a is the better copy: more pixels, then the larger file (more detail at one resolution), then one that downloads here */
static int better(const StoreItem *a, const StoreItem *b)
{
    int64_t pa = pixels(a), pb = pixels(b);
    if (pa != pb) return pa > pb;
    float ma = megabytes(a), mb = megabytes(b);
    if (ma != mb) return ma > mb;
    return k_sources[a->source].file_url && !k_sources[b->source].file_url;
}

static int bits64(uint64_t v)
{
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (int)((v * 0x0101010101010101ull) >> 56);
}

/* words of a title, as hashes: lower case, two letters and up, without the filler every site adds */
static int title_words(const wchar_t *t, uint32_t *out, int cap)
{
    static const wchar_t *const skip[] = { L"live", L"wallpaper", L"wallpapers", L"animated", L"background", L"desktop",
                                           L"4k", L"5k", L"8k", L"hd", L"uhd", L"the", L"and", L"of", L"in", L"on", L"with", L"for" };
    wchar_t w[32];
    int n = 0, k = 0;
    for (const wchar_t *c = t;; c++) {
        if (*c && iswalnum(*c)) {
            if (k < 31) w[k++] = (wchar_t)towlower(*c);
            continue;
        }
        w[k] = 0;
        int keep = k >= 2;
        for (size_t i = 0; keep && i < SP_COUNTOF(skip); i++) keep = wcscmp(w, skip[i]) != 0;
        if (keep && n < cap) out[n++] = hash_fnv1a_w(w);
        k = 0;
        if (!*c) break;
    }
    return n;
}

static float title_overlap(const StoreItem *a, const StoreItem *b)
{
    uint32_t x[16], y[16];
    int nx = title_words(a->title, x, 16), ny = title_words(b->title, y, 16), both = 0;
    if (!nx || !ny) return 0.0f;
    for (int i = 0; i < nx; i++) {
        for (int j = 0; j < ny; j++) {
            if (x[i] == y[j]) {
                both++;
                break;
            }
        }
    }
    return (float)both / (float)(nx + ny - both);
}

static int same_wallpaper(const StoreItem *a, const StoreItem *b)
{
    if (a->kind != b->kind || !a->hashed || !b->hashed) return 0;
    int d = bits64(a->hash ^ b->hash);
    return d <= 5 || (d <= 12 && title_overlap(a, b) >= 0.5f);
}

/* Item i has its hash (or better facts): fold it and every copy of it into the best one. */
static void fold(int i)
{
    for (int j = 0; j < S.count; j++) {
        StoreItem *a = &S.items[i], *b = &S.items[j];
        if (j == i || !same_wallpaper(a, b)) continue;
        if (!(int64_t)a->w * a->h && a->detail == STORE_NONE) want_detail(i, 1);     /* its page knows its resolution */
        if (!(int64_t)b->w * b->h && b->detail == STORE_NONE) want_detail(j, 1);
        int keep = better(a, b) ? i : better(b, a) ? j : SP_MIN(i, j), drop = keep == i ? j : i;
        S.items[drop].dupOf = keep + 1;
        if (S.items[keep].dupOf == drop + 1) S.items[keep].dupOf = 0;
    }
}

static int shown(const StoreItem *it)
{
    if (it->dupOf) return 0;
    if (S.filter[SF_KIND] && it->kind != S.filter[SF_KIND]) return 0;
    if (S.filter[SF_SOURCE] && it->source != S.filter[SF_SOURCE] - 1) return 0;
    int64_t px = pixels(it), need = (int64_t)k_resMin[g_query.res][0] * k_resMin[g_query.res][1];
    return !px || px * 100 >= need * 95;       /* unknown: shown until its page says otherwise */
}

static void rebuild_view(void)
{
    if (g_viewCap < S.count) {
        int nc = S.count + 256;
        int *v = (int *)realloc(S.view, sizeof(int) * (size_t)nc);
        if (!v) return;
        S.view = v;
        g_viewCap = nc;
    }
    S.viewCount = 0;
    for (int i = 0; i < S.count; i++)
        if (shown(&S.items[i])) S.view[S.viewCount++] = i;
}

/* ---------------------------------------------------------------- listing (UI thread) */

static uint32_t item_key(int source, const char *ref)
{
    uint32_t h = 2166136261u ^ (uint32_t)source;
    for (const unsigned char *c = (const unsigned char *)ref; *c; c++) h = (h ^ *c) * 16777619u;
    return h;
}

static void refresh_more(void)
{
    S.more = 0;
    for (int s = 0; s < k_sourceCount; s++) S.more |= !g_site[s].off && g_site[s].more;
}

static int append(const StoreItem *in, int source)
{
    if (S.count >= ITEMS_MAX) return 0;
    uint32_t key = item_key(source, in->ref);
    for (int i = 0; i < S.count; i++)
        if (S.items[i].key == key && S.items[i].source == source) return 0;     /* a page repeats a card */
    if (S.count == g_cap) {
        int nc = g_cap ? g_cap * 2 : 128;
        StoreItem *n = (StoreItem *)realloc(S.items, sizeof(StoreItem) * (size_t)nc);
        if (!n) return 0;
        S.items = n;
        g_cap = nc;
    }
    StoreItem *it = &S.items[S.count++];
    *it = *in;
    it->source = source;
    it->key = key;
    it->preview = STORE_NONE;
    it->detail = k_sources[source].detail_url ? STORE_NONE : STORE_READY;
    it->dl = STOREDL_IDLE;
    it->dupOf = 0;
    it->hashed = 0;
    if (!it->w && it->varCount) {
        const StoreVariant *b = store_best(it);
        it->w = b->w;
        it->h = b->h;
    }
    return 1;
}

static void round_merge(void)
{
    if (g_roundTimer) {
        KillTimer(NULL, g_roundTimer);
        g_roundTimer = 0;
    }
    int added = 0, any = 1;
    for (int k = 0; any; k++) {                /* one card from each site in turn */
        any = 0;
        for (int s = 0; s < k_sourceCount; s++) {
            Site *x = &g_site[s];
            if (!x->got || k >= x->round.count) continue;
            any = 1;
            added += append(&x->round.items[k], s);
        }
    }
    for (int s = 0; s < k_sourceCount; s++) {
        Site *x = &g_site[s];
        if (!x->got) continue;
        free(x->round.items);
        ZeroMemory(&x->round, sizeof x->round);
        x->got = 0;
    }
    S.loading = g_out > 0;
    refresh_more();
    if (!S.count && !S.loading && !S.more) {   /* nothing at all: was it the network? */
        int asked = 0, failed = 0;
        for (int s = 0; s < k_sourceCount; s++) {
            asked += !g_site[s].off;
            failed += !g_site[s].off && g_site[s].error[0];
            if (!S.error[0] && g_site[s].error[0]) str_printf(S.error, SP_COUNTOF(S.error), L"%ls: %ls", k_sources[s].name, g_site[s].error);
        }
        S.failed = asked && failed == asked;
    }
    if (added) rebuild_view();
}

static VOID CALLBACK round_due(HWND h, UINT msg, UINT_PTR id, DWORD time)
{
    (void)h;
    (void)msg;
    (void)time;
    KillTimer(NULL, id);
    if (id == g_roundTimer) g_roundTimer = 0;
    round_merge();                             /* what has come; the rest joins when it arrives */
    ui_on_status();
}

static void round_start(void)
{
    for (int s = 0; s < k_sourceCount; s++) {
        Site *x = &g_site[s];
        Task *t = !x->off && x->more && !x->busy ? task(T_LIST, -1) : NULL;
        if (!t) continue;
        t->source = s;
        t->page = x->page + 1;
        t->query = g_query;
        strcpy_s(t->tag, sizeof t->tag, x->tag);
        if (!k_sources[s].list_url(&g_query, t->page, x->tag, t->url, SP_COUNTOF(t->url))) {
            x->off = 1;
            free(t);
            continue;
        }
        x->busy = 1;
        g_out++;
        push(t);
    }
    S.loading = g_out > 0;
    refresh_more();
}

static void relist(void)
{
    g_gen++;
    purge();
    if (g_roundTimer) {
        KillTimer(NULL, g_roundTimer);
        g_roundTimer = 0;
    }
    for (int s = 0; s < MAX_SITES; s++) free(g_site[s].round.items);
    ZeroMemory(g_site, sizeof g_site);
    S.count = S.viewCount = 0;
    S.loading = S.more = S.failed = 0;
    S.error[0] = 0;
    str_copy(g_query.text, SP_COUNTOF(g_query.text), S.query);
    g_query.category = S.filter[SF_CATEGORY];
    g_query.res = S.filter[SF_RES];
    g_query.sort = S.filter[SF_SORT];
    for (int s = 0; s < k_sourceCount; s++) {
        const StoreSource *src = &k_sources[s];
        g_site[s].more = 1;
        g_site[s].off = (S.filter[SF_SOURCE] && S.filter[SF_SOURCE] - 1 != s) ||
                        (S.filter[SF_KIND] && S.filter[SF_KIND] != src->kind) || g_query.res > src->maxRes;
    }
    g_out = 0;
    g_started = 1;
    if (start()) round_start();
    LOGD(L"store: listing '%ls' (category %d, resolution %d, sort %d, kind %d, site %d)", S.query, S.filter[SF_CATEGORY],
         S.filter[SF_RES], S.filter[SF_SORT], S.filter[SF_KIND], S.filter[SF_SOURCE]);
    ui_on_status();
}

static void on_list(Result *r)
{
    Site *x = &g_site[r->source];
    x->busy = 0;
    if (g_out > 0) g_out--;
    if (r->ok) {
        x->page = r->page;
        x->more = r->list.more && (r->list.count || k_sources[r->source].scans);   /* an empty page: nothing more */
        if (r->list.tag[0]) strcpy_s(x->tag, sizeof x->tag, r->list.tag);
        free(x->round.items);
        x->round = r->list;
        ZeroMemory(&r->list, sizeof r->list);
        x->got = 1;
        Task *t = !x->round.count && x->more && k_sources[r->source].scans ? task(T_LIST, -1) : NULL;
        if (t) {                               /* nothing matched on this page: read the next one in this round */
            t->source = r->source;
            t->page = x->page + 1;
            t->query = g_query;
            if (k_sources[r->source].list_url(&g_query, t->page, x->tag, t->url, SP_COUNTOF(t->url))) {
                x->busy = 1;
                g_out++;
                push(t);
                return;
            }
            free(t);
        }
    } else {
        x->more = 0;
        str_copy(x->error, SP_COUNTOF(x->error), r->error[0] ? r->error : L"it could not be read");
        LOGW(L"store: %ls page %d: %ls", k_sources[r->source].name, r->page, x->error);
    }
    if (!g_out) round_merge();
    else if (!g_roundTimer) g_roundTimer = SetTimer(NULL, 0, S.count ? ROUND_MS : FIRST_MS, round_due);
}

/* ---------------------------------------------------------------- the page's calls */

const StoreState *store_get(void) { return &S; }

const wchar_t *store_source_name(int source)
{
    return source >= 0 && source < k_sourceCount ? k_sources[source].name : L"";
}

int store_filter_names(int which, const wchar_t **names, int cap)
{
    static const wchar_t *const kinds[] = { L"Live and still", L"Live wallpapers", L"Still pictures" };
    int n = 0;
    switch (which) {
    case SF_CATEGORY: for (; n < CAT_COUNT && n < cap; n++) names[n] = k_categories[n].name; break;
    case SF_RES:      for (; n < SR_COUNT && n < cap; n++) names[n] = k_resNames[n]; break;
    case SF_SORT:     for (; n < SQ_COUNT && n < cap; n++) names[n] = k_sortNames[n]; break;
    case SF_KIND:     for (; n < (int)SP_COUNTOF(kinds) && n < cap; n++) names[n] = kinds[n]; break;
    case SF_SOURCE:
        if (cap > 0) names[n++] = L"All sites";
        for (int s = 0; s < k_sourceCount && n < cap; s++) names[n++] = k_sources[s].name;
        break;
    default: break;
    }
    return n;
}

void store_set_filter(int which, int value)
{
    const wchar_t *names[32];
    if (which < 0) {
        ZeroMemory(S.filter, sizeof S.filter);
    } else {
        if (which >= SF_COUNT || value < 0 || value >= store_filter_names(which, names, 32)) return;
        if (g_started && S.filter[which] == value) return;
        S.filter[which] = value;
    }
    relist();
}

BOOL store_search(const wchar_t *query)
{
    wchar_t q[SP_COUNTOF(S.query)];
    str_copy(q, SP_COUNTOF(q), query ? query : L"");
    str_sanitize_line(q);
    str_trim(q);
    if (g_started && !S.failed && wcscmp(q, S.query) == 0) return FALSE;
    str_copy(S.query, SP_COUNTOF(S.query), q);
    relist();
    return TRUE;
}

void store_open(void)
{
    if (!g_started) relist();
}

void store_more(void)
{
    if (!g_started || g_out || !S.more || S.count >= ITEMS_MAX) return;
    round_start();
    ui_on_status();
}

void store_retry(void)
{
    if (S.loading) return;
    if (!S.count) {
        relist();
        return;
    }
    for (int s = 0; s < k_sourceCount; s++) {  /* the sites that failed get another go */
        if (!g_site[s].off && g_site[s].error[0]) {
            g_site[s].error[0] = 0;
            g_site[s].more = 1;
        }
    }
    S.failed = 0;
    store_more();
}

void store_want(int i, int pixelW, int detail)
{
    if (i < 0 || i >= S.count || !Q.threadCount) return;
    StoreItem *it = &S.items[i];
    if (it->preview == STORE_NONE) {
        it->previewBig = pixelW > 380;         /* the bigger crop where the card is wider than the small one */
        Task *t = task(T_PREVIEW, i);
        if (t) {
            preview_path(it, t->url, SP_COUNTOF(t->url), t->dest, SP_COUNTOF(t->dest));
            t->cached = fresh(t->dest);
            it->preview = STORE_LOADING;
            push(t);
        }
    }
    if (detail && it->detail == STORE_NONE) want_detail(i, 0);
}

BOOL store_preview_entry(int i, WallpaperEntry *e)
{
    if (i < 0 || i >= S.count || S.items[i].preview != STORE_READY) return FALSE;
    const StoreItem *it = &S.items[i];
    wchar_t url[600];
    ZeroMemory(e, sizeof *e);
    str_printf(e->id, SP_COUNTOF(e->id), L"s%d-%08x", it->source, it->key);
    e->type = WP_IMAGE;
    preview_path(it, url, SP_COUNTOF(url), e->path, SP_COUNTOF(e->path));
    str_copy(e->thumb, SP_COUNTOF(e->thumb), e->path);
    return TRUE;
}

const StoreVariant *store_best(const StoreItem *it)
{
    const StoreVariant *b = NULL;
    for (int i = 0; i < it->varCount; i++) {
        const StoreVariant *v = &it->var[i];
        int gif = strcmp(v->key, "gif") == 0, bgif = b && strcmp(b->key, "gif") == 0;
        if (!b || (bgif && !gif) || (gif == bgif && (int64_t)v->w * v->h > (int64_t)b->w * b->h)) b = v;
    }
    return b;
}

void store_page_url(int i, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (i >= 0 && i < S.count) k_sources[S.items[i].source].page_url(&S.items[i], out, cap);
}

/* ---------------------------------------------------------------- downloads (UI thread) */

static StoreItem *find_key(uint32_t key)
{
    for (int i = 0; i < S.count; i++)
        if (S.items[i].key == key) return &S.items[i];
    return NULL;
}

static void unqueue(int k)
{
    g_queued--;
    memmove(&g_queue[k], &g_queue[k + 1], sizeof(Queued) * (size_t)(g_queued - k));
}

static void enqueue(StoreItem *it)
{
    const StoreVariant *v = store_best(it);
    const StoreSource *src = &k_sources[it->source];
    Queued *q = &g_queue[g_queued];
    it->dl = STOREDL_IDLE;                     /* from here on the queue tells how it is doing */
    if (!v || g_queued == QUEUED_MAX || !src->file_url || !src->file_url(it, v, q->url, SP_COUNTOF(q->url))) {
        it->dl = STOREDL_FAILED;
        app_notice(NOTICE_WARN, g_queued == QUEUED_MAX ? L"Too many downloads are waiting; try '%ls' again in a moment."
                                                       : L"'%ls' has nothing to download.", it->title);
        return;
    }
    q->key = it->key;
    str_copy(q->title, SP_COUNTOF(q->title), it->title);
    g_queued++;
}

static void pump(void)
{
    while (!g_hasActive && g_queued) {
        UrlImportState st;
        url_import_get(&st);
        if (st.busy) return;                   /* a link import from the Library goes first */
        Queued q = g_queue[0];
        unqueue(0);
        g_seen = st.finished;
        if (url_import_start_titled(q.url, URLMODE_DOWNLOAD, q.title)) {
            g_active = q.key;
            g_hasActive = 1;
            LOGI(L"store: downloading '%ls' (%ls)", q.title, q.url);
        } else {
            StoreItem *it = find_key(q.key);
            if (it) it->dl = STOREDL_FAILED;
        }
    }
}

int store_dl_state(int i, int *percent)
{
    if (percent) *percent = -1;
    if (i < 0 || i >= S.count) return STOREDL_IDLE;
    const StoreItem *it = &S.items[i];
    if (!k_sources[it->source].file_url) return STOREDL_SITE;
    if (g_hasActive && it->key == g_active) {
        if (percent) {
            UrlImportState st;
            url_import_get(&st);
            *percent = st.percent;
        }
        return STOREDL_RUNNING;
    }
    for (int k = 0; k < g_queued; k++)
        if (g_queue[k].key == it->key) return STOREDL_WAIT;
    for (int k = 0; k < SP_MIN(g_doneCount, (int)SP_COUNTOF(g_done)); k++)
        if (g_done[k] == it->key) return STOREDL_DONE;
    return it->dl;
}

void store_download(int i)
{
    if (i < 0 || i >= S.count) return;
    StoreItem *it = &S.items[i];
    int st = store_dl_state(i, NULL);
    if (st == STOREDL_SITE) {                  /* the site hands its files out on its own page */
        wchar_t url[600];
        store_page_url(i, url, SP_COUNTOF(url));
        if (url[0]) shell_open(url);
        return;
    }
    if (st == STOREDL_RUNNING) {               /* pressed again: stop it */
        url_import_cancel();
        return;
    }
    if (st == STOREDL_WAIT) {                  /* pressed again: no longer wanted */
        for (int k = 0; k < g_queued; k++) {
            if (g_queue[k].key == it->key) {
                unqueue(k);
                break;
            }
        }
        it->dl = STOREDL_IDLE;
        ui_on_status();
        return;
    }
    if (st == STOREDL_DONE) return;
    if (it->detail == STORE_READY) {
        enqueue(it);
    } else {
        it->dl = STOREDL_WAIT;
        want_detail(i, 1);                     /* ahead of the previews */
    }
    pump();
    ui_on_status();
}

static void on_detail(int i, const Result *r)
{
    StoreItem *it = &S.items[i];
    if (r->ok) {
        memcpy(it->var, r->item.var, sizeof it->var);
        it->varCount = r->item.varCount;
        const StoreVariant *b = store_best(it);
        if (b && (int64_t)b->w * b->h > (int64_t)it->w * it->h) {
            it->w = b->w;
            it->h = b->h;
        }
        it->detail = STORE_READY;
        if (it->hashed) fold(i);
        rebuild_view();
    } else if (it->detail != STORE_READY) {
        it->detail = STORE_FAILED;
    }
    if (it->dl != STOREDL_WAIT) return;
    if (it->detail == STORE_READY) {
        enqueue(it);
    } else {
        it->dl = STOREDL_FAILED;
        app_notice(NOTICE_ERROR, L"Couldn't find the download for '%ls': %ls.", it->title, r->error);
    }
}

void store_on_event(WPARAM wp, LPARAM lp)
{
    Result *r = (Result *)lp;
    (void)wp;
    if (!r) return;
    if (r->gen == g_gen) {
        if (r->kind == T_LIST) {
            on_list(r);
        } else if (r->index >= 0 && r->index < S.count && S.items[r->index].key == r->key) {
            StoreItem *it = &S.items[r->index];
            if (r->kind == T_DETAIL) {
                on_detail(r->index, r);
            } else {
                it->preview = r->ok ? STORE_READY : STORE_FAILED;
                int b = bits64(r->hash);
                if (r->hashed && b >= 6 && b <= 58) {   /* a plain picture hashes alike to many: no evidence */
                    it->hash = r->hash;
                    it->hashed = 1;
                    fold(r->index);
                    rebuild_view();
                }
            }
        }
    }
    free(r->list.items);
    free(r);
    pump();
    ui_on_status();
}

void store_on_import(void)
{
    UrlImportState st;
    url_import_get(&st);
    if (g_hasActive && !st.busy && st.finished != g_seen) {     /* the Store's download ended */
        StoreItem *it = find_key(g_active);
        if (st.lastOk) g_done[g_doneCount++ % SP_COUNTOF(g_done)] = g_active;
        else if (it) it->dl = st.failed ? STOREDL_FAILED : STOREDL_IDLE;   /* failed, or cancelled */
        if (st.failed) app_notice(NOTICE_ERROR, L"%ls", st.text);
        g_hasActive = 0;
    }
    pump();
}

void store_shutdown(void)
{
    if (!Q.threadCount) return;
    AcquireSRWLockExclusive(&Q.lock);
    Q.quit = 1;
    ReleaseSRWLockExclusive(&Q.lock);
    WakeAllConditionVariable(&Q.wake);
    /* a read in flight ends within its timeouts; exiting doesn't wait for that */
    WaitForMultipleObjects((DWORD)Q.threadCount, Q.threads, TRUE, 2000);
    for (int i = 0; i < Q.threadCount; i++) CloseHandle(Q.threads[i]);
    Q.threadCount = 0;
}
