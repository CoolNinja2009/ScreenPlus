/* Store: browse, search and download MotionBGS (motionbgs.com) live wallpapers. The site has no JSON API, so
 * this reads its server-rendered pages as docs/store/motionbgs.md maps them:
 *   - a listing is pages of 36 cards: the latest (/hx2/latest/<n>/, the home page's infinite scroll, cards
 *     only), a search (/search?q=<q>&page=<n>), or the tag listing a search lands on when the query names a
 *     tag (/tag:<slug>/<n>/). These are the site's own searches, so the results match its search page;
 *   - a wallpaper's page lists its downloads (/dl/<kind>/<id>/) with resolution and size.
 * Speed: WORKERS threads share one keep-alive HTTP/2 session (http_open_shared), so after the first request
 * nothing waits on a handshake. Page reads go first, then previews newest first (what is on screen now), then
 * the details of the cards on screen; the next page is asked for a screen and a half before the end. Previews
 * are the site's own 364x205 / 546x308 crops in WebP, the ones browsers fetch and so the ones Cloudflare keeps
 * (~0.1 s, where the JPEG crops mostly come from the origin in 0.4-1.2 s); JPEG where Windows has no WebP codec.
 * They are kept CACHE_DAYS in <cache>\store (a revisit is instant) and decoded by the thumbnail worker.
 * Nothing full size is downloaded until Download, which hands the best video
 * to url_import.c: it runs one job at a time, so presses queue. The UI thread owns all state; a worker gets a
 * copy of what it needs and posts its result back (WM_SP_STORE), where results of an older listing are dropped. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITE       L"https://motionbgs.com"
#define WORKERS    8
#define PAGE_MAX   (2 << 20)        /* bytes kept of a page */
#define IMAGE_MAX  (2 << 20)
#define WAIT_MAX   48               /* previews and details waiting; beyond, the oldest previews are dropped */
#define ITEMS_MAX  1800             /* 50 pages listed at most */
#define QUEUED_MAX 32               /* downloads waiting for url_import */
#define CACHE_DAYS 7
#define DAY_TICKS  864000000000ull  /* FILETIME units */

enum { T_LIST = 0, T_DETAIL, T_PREVIEW };
enum { L_LATEST = 0, L_SEARCH, L_TAG };

typedef struct Task {
    struct Task *next;
    int     kind, gen, index, id, page, list, urgent;
    char    tag[96];                /* L_TAG: the tag's slug */
    wchar_t url[512];
    wchar_t dest[SP_MAX_PATH];      /* T_PREVIEW: the cache file */
} Task;

typedef struct Result {
    int          kind, gen, index, id, page, ok;
    int          more;
    char         tag[96];           /* page 1 of a search that landed on this tag's listing */
    StoreItem   *items;
    int          count;
    StoreVariant var[STORE_VARIANTS];
    int          varCount;
    wchar_t      error[200];
} Result;

typedef struct Queued {
    int     id;
    wchar_t url[96];
    wchar_t title[SP_NAME_LEN];
} Queued;

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
static int     g_gen, g_started, g_list, g_page, g_cap;   /* listing number and kind, last page read, items room */
static char    g_tag[96];
static wchar_t g_cache[SP_MAX_PATH];
static Queued  g_queue[QUEUED_MAX];
static int     g_queued;
static int     g_active = -1;       /* the id url_import is downloading for the Store */
static int     g_seen;              /* UrlImportState.finished when it started */
static int     g_done[256], g_doneCount;   /* ids added to the library this session (a ring) */
static int     g_webp;              /* previews in WebP: Windows decodes it */

/* ---------------------------------------------------------------- page reading (workers) */

static const char *find(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    while (p && end - p >= (ptrdiff_t)n) {
        p = (const char *)memchr(p, needle[0], (size_t)(end - p) - n + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, n) == 0) return p;
        p++;
    }
    return NULL;
}

static const char *after(const char *p, const char *end, const char *needle)
{
    p = find(p, end, needle);
    return p ? p + strlen(needle) : NULL;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_slug(char c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '_'; }

/* [a, b) of the page as text: entities decoded, trimmed */
static void text_of(const char *a, const char *b, wchar_t *out, size_t cap)
{
    out[0] = 0;
    wchar_t *w = a && b && b > a ? wide_from_utf8(a, (int)(b - a)) : NULL;
    if (!w) return;
    html_unescape(w);
    str_copy(out, cap, w);
    free(w);
    str_trim(out);
}

/* One card: <a title=… href=/<slug>> … <img … src=/i/c/364x205/media/<id>/<poster> …>
 * <span class=ttl>Title</span> <span class=frm> 4K </span> </a> */
static int parse_card(const char *p, const char *end, StoreItem *it)
{
    ZeroMemory(it, sizeof *it);
    const char *h = after(p, end, " href=/"), *img = after(p, end, " src=/i/c/");
    const char *m = img ? after(img, end, "/media/") : NULL, *tt = after(p, end, "class=ttl>");
    if (!h || !m || !tt) return 0;
    const char *he = h;
    while (he < end && is_slug(*he)) he++;
    if (he == h || (*he != '>' && *he != ' ')) return 0;     /* /mobile/…, /tag:…: not a wallpaper's page */
    text_of(h, he, it->slug, SP_COUNTOF(it->slug));
    const char *f = m;
    while (f < end && is_digit(*f) && it->id < 100000000) it->id = it->id * 10 + (*f++ - '0');
    if (it->id <= 0 || f >= end || *f != '/') return 0;
    const char *fe = ++f;
    while (fe < end && *fe != ' ' && *fe != '>' && *fe != '"') fe++;
    text_of(f, fe, it->poster, SP_COUNTOF(it->poster));
    text_of(tt, find(tt, end, "</span>"), it->title, SP_COUNTOF(it->title));
    const char *b = after(p, end, "class=frm>");
    if (b) text_of(b, find(b, end, "</span>"), it->badge, SP_COUNTOF(it->badge));
    /* newer posters are named "<stem>.<W>x<H>.jpg" after the source video */
    const wchar_t *ext = wcsrchr(it->poster, L'.'), *d = ext;
    while (d && d > it->poster && d[-1] != L'.') d--;
    if (d && d > it->poster) {
        wchar_t *x = NULL, *e = NULL;
        long w = wcstol(d, &x, 10), hh = x && *x == L'x' ? wcstol(x + 1, &e, 10) : 0;
        if (w > 0 && hh > 0 && e == ext) {
            it->w = (int)w;
            it->h = (int)hh;
        }
    }
    return it->slug[0] && it->title[0] && it->poster[0];
}

static int parse_list(const Task *t, const char *html, int len, Result *r)
{
    const char *end = html + len, *p = html;
    int cap = 0;
    while ((p = find(p, end, "<a title=")) != NULL) {
        const char *ce = find(p, end, "</a>");
        if (!ce) break;
        if (r->count == cap) {
            int nc = cap ? cap * 2 : 40;
            StoreItem *n = (StoreItem *)realloc(r->items, sizeof(StoreItem) * (size_t)nc);
            if (!n) break;
            r->items = n;
            cap = nc;
        }
        if (parse_card(p, ce, &r->items[r->count])) r->count++;
        p = ce + 4;
    }
    if (t->list == L_SEARCH && t->page == 1) {     /* the query named a tag: the site redirected to its listing */
        const char *c = after(html, end, "<link href=https://motionbgs.com/tag:"), *ce = c;
        while (ce && ce < end && is_slug(*ce)) ce++;
        if (c && ce > c && ce - c < (ptrdiff_t)sizeof r->tag && find(ce, SP_MIN(end, ce + 24), "rel=canonical"))
            memcpy(r->tag, c, (size_t)(ce - c));
    }
    char pat[160];
    const char *tag = r->tag[0] ? r->tag : t->tag;
    if (t->list == L_LATEST) snprintf(pat, sizeof pat, "hx-get=/hx2/latest/%d/", t->page + 1);
    else if (tag[0]) snprintf(pat, sizeof pat, "/tag:%s/%d/", tag, t->page + 1);
    else snprintf(pat, sizeof pat, "page=%d", t->page + 1);
    size_t n = strlen(pat);
    for (const char *q = find(html, end, pat); q && !r->more; q = find(q + 1, end, pat))
        r->more = q + n >= end || !is_digit(q[n]);
    return 1;
}

/* A wallpaper's page, per download: <a href=/dl/<kind>/<id> …> … <span class=font-bold>4K</span> Wallpaper
 * (23.9Mb) … <div class=text-xs>3840x2160 mp4 file</div> … </a> */
static int parse_detail(const char *html, int len, Result *r)
{
    const char *end = html + len;
    for (const char *p = after(html, end, "href=/dl/"); p && r->varCount < STORE_VARIANTS; p = after(p, end, "href=/dl/")) {
        const char *k = p, *ae = find(p, end, "</a>");
        while (k < end && is_slug(*k)) k++;
        if (!ae || k == p || k - p >= 8 || *k != '/') continue;
        StoreVariant v;
        ZeroMemory(&v, sizeof v);
        for (int i = 0; i < (int)(k - p); i++) v.kind[i] = (wchar_t)p[i];
        int dup = 0;
        for (int i = 0; i < r->varCount; i++) dup |= wcscmp(r->var[i].kind, v.kind) == 0;
        const char *res = after(k, ae, "class=text-xs>");
        while (res && res < ae && is_digit(*res) && v.w < 100000) v.w = v.w * 10 + (*res++ - '0');
        if (res && res < ae && *res == 'x')
            for (res++; res < ae && is_digit(*res) && v.h < 100000; res++) v.h = v.h * 10 + (*res - '0');
        for (const char *q = k; q + 1 < ae; q++) {
            if (*q == '(' && is_digit(q[1])) {
                v.mb = (float)atof(q + 1);
                break;
            }
        }
        if (!dup && v.w > 0 && v.h > 0) r->var[r->varCount++] = v;
    }
    if (!r->varCount) str_copy(r->error, SP_COUNTOF(r->error), L"its page lists no downloads");
    return r->varCount > 0;
}

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
        str_printf(err, cap, L"MotionBGS answered with HTTP %d", *status);
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

static void run(const Task *t)
{
    Result *r = (Result *)calloc(1, sizeof *r);
    if (!r) return;
    r->kind = t->kind;
    r->gen = t->gen;
    r->index = t->index;
    r->id = t->id;
    r->page = t->page;
    if (t->kind == T_PREVIEW) {
        r->ok = fetch_file(t->url, t->dest, r->error, SP_COUNTOF(r->error));
    } else {
        int len = 0, status = 0;
        char *html = fetch_text(t->url, &len, &status, r->error, SP_COUNTOF(r->error));
        if (html) r->ok = t->kind == T_LIST ? parse_list(t, html, len, r) : parse_detail(html, len, r);
        else r->ok = t->kind == T_LIST && t->page > 1 && status == 404;    /* past a tag's last page: the end */
        free(html);
    }
    if (!PostMessageW(g_app.shellWnd, WM_SP_STORE, 0, (LPARAM)r)) {
        free(r->items);
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
    (void)arg;
    for (;;) {
        Task *t = NULL;
        AcquireSRWLockExclusive(&Q.lock);
        while (!Q.quit) {
            t = pop_locked();
            if (t) break;
            SleepConditionVariableSRW(&Q.wake, &Q.lock, INFINITE, 0);
        }
        ReleaseSRWLockExclusive(&Q.lock);
        if (!t) return 0;
        run(t);
        free(t);
    }
}

/* ---------------------------------------------------------------- state (UI thread) */

static void cache_path(int id, int big, wchar_t *out, size_t cap)
{
    wchar_t name[40];
    str_printf(name, SP_COUNTOF(name), L"%d-%d.%ls", id, big ? 546 : 364, g_webp ? L"webp" : L"jpg");
    path_join(out, cap, g_cache, name);
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
        if (d) IWICBitmapDecoder_Release(d);
        IWICImagingFactory_Release(f);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

static uint64_t age(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime, now = filetime_now();
    return now > t ? now - t : 0;
}

static int cached(int id, int big)
{
    wchar_t p[SP_MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    cache_path(id, big, p, SP_COUNTOF(p));
    return GetFileAttributesExW(p, GetFileExInfoStandard, &a) && age(&a.ftLastWriteTime) < CACHE_DAYS * DAY_TICKS;
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

static BOOL start(void)
{
    if (Q.threadCount) return TRUE;
    path_join(g_cache, SP_COUNTOF(g_cache), paths_cache(), L"store");
    dir_create_recursive(g_cache);
    sweep_cache();
    g_webp = webp_decodes();
    LOGI(L"store: previews as %ls", g_webp ? L"WebP" : L"JPEG (no WebP codec)");
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
    if (t->gen != g_gen || t->index < 0 || t->index >= S.count || S.items[t->index].id != t->id) return;
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
    t->id = index >= 0 ? S.items[index].id : 0;
    return t;
}

static void query_encode(const wchar_t *q, wchar_t *out, size_t cap)
{
    static const wchar_t hex[] = L"0123456789ABCDEF";
    char *u8 = utf8_from_wide(q, NULL);
    size_t j = 0;
    for (const unsigned char *c = (const unsigned char *)(u8 ? u8 : ""); *c && j + 4 < cap; c++) {
        if (is_slug((char)*c) || *c == '.' || *c == '~') {
            out[j++] = (wchar_t)*c;
        } else if (*c == ' ') {
            out[j++] = L'+';
        } else {
            out[j++] = L'%';
            out[j++] = hex[*c >> 4];
            out[j++] = hex[*c & 15];
        }
    }
    out[j] = 0;
    free(u8);
}

static void load(int page)
{
    Task *t = task(T_LIST, -1);
    if (!t) {
        S.failed = 1;
        str_copy(S.error, SP_COUNTOF(S.error), L"Out of memory.");
        return;
    }
    t->page = page;
    t->list = g_list;
    strcpy_s(t->tag, sizeof t->tag, g_tag);
    if (g_list == L_LATEST) {
        str_printf(t->url, SP_COUNTOF(t->url), SITE L"/hx2/latest/%d/", page);
    } else if (g_list == L_TAG) {               /* page 1 was the search itself */
        str_printf(t->url, SP_COUNTOF(t->url), SITE L"/tag:%hs/%d/", g_tag, page);
    } else {
        wchar_t q[300];
        query_encode(S.query, q, SP_COUNTOF(q));
        if (page > 1) str_printf(t->url, SP_COUNTOF(t->url), SITE L"/search?q=%ls&page=%d", q, page);
        else str_printf(t->url, SP_COUNTOF(t->url), SITE L"/search?q=%ls", q);
    }
    S.loading = 1;
    S.failed = 0;
    push(t);
}

static void want_detail(int i, int urgent)
{
    Task *t = task(T_DETAIL, i);
    if (!t) return;
    t->urgent = urgent;
    str_printf(t->url, SP_COUNTOF(t->url), SITE L"/%ls", S.items[i].slug);
    if (S.items[i].detail != STORE_READY) S.items[i].detail = STORE_LOADING;
    push(t);
}

const StoreState *store_get(void) { return &S; }

BOOL store_search(const wchar_t *query)
{
    wchar_t q[SP_COUNTOF(S.query)];
    str_copy(q, SP_COUNTOF(q), query ? query : L"");
    str_sanitize_line(q);
    str_trim(q);
    if (g_started && !S.failed && wcscmp(q, S.query) == 0) return FALSE;
    g_started = 1;
    g_gen++;
    purge();
    S.count = 0;
    S.more = 0;
    S.loading = 0;
    S.error[0] = 0;
    str_copy(S.query, SP_COUNTOF(S.query), q);
    g_list = q[0] ? L_SEARCH : L_LATEST;
    g_tag[0] = 0;
    g_page = 0;
    if (start()) load(1);
    LOGD(L"store: listing '%ls'", q);
    ui_on_status();
    return TRUE;
}

void store_open(void)
{
    if (!g_started) store_search(L"");
}

void store_more(void)
{
    if (!g_started || S.loading || S.failed || !S.more || S.count >= ITEMS_MAX) return;
    load(g_page + 1);
    ui_on_status();
}

void store_retry(void)
{
    if (S.loading) return;
    if (!S.count) {
        g_started = 0;                          /* the same query again, from its first page */
        store_search(S.query);
        return;
    }
    S.failed = 0;
    store_more();
}

void store_want(int i, int pixelW, int detail)
{
    if (i < 0 || i >= S.count || !Q.threadCount) return;
    StoreItem *it = &S.items[i];
    if (it->preview == STORE_NONE) {
        int big = pixelW > 380;                 /* the 546x308 crop where the card is wider than the small one */
        if (cached(it->id, big) || cached(it->id, !big)) {
            it->previewBig = cached(it->id, big) ? big : !big;
            it->preview = STORE_READY;
        } else {
            Task *t = task(T_PREVIEW, i);
            if (t) {
                str_printf(t->url, SP_COUNTOF(t->url), SITE L"/i/c/%ls/media/%d/%ls%ls", big ? L"546x308" : L"364x205",
                           it->id, it->poster, g_webp ? L".webp" : L"");
                cache_path(it->id, big, t->dest, SP_COUNTOF(t->dest));
                it->previewBig = big;
                it->preview = STORE_LOADING;
                push(t);
            }
        }
    }
    if (detail && it->detail == STORE_NONE) want_detail(i, 0);
}

BOOL store_preview_entry(int i, WallpaperEntry *e)
{
    if (i < 0 || i >= S.count || S.items[i].preview != STORE_READY) return FALSE;
    ZeroMemory(e, sizeof *e);
    str_printf(e->id, SP_COUNTOF(e->id), L"mbgs%d", S.items[i].id);
    e->type = WP_IMAGE;
    cache_path(S.items[i].id, S.items[i].previewBig, e->path, SP_COUNTOF(e->path));
    str_copy(e->thumb, SP_COUNTOF(e->thumb), e->path);
    return TRUE;
}

const StoreVariant *store_best(const StoreItem *it)
{
    const StoreVariant *b = NULL;
    for (int i = 0; i < it->varCount; i++) {
        const StoreVariant *v = &it->var[i];
        int gif = wcscmp(v->kind, L"gif") == 0, bgif = b && wcscmp(b->kind, L"gif") == 0;
        if (!b || (bgif && !gif) || (gif == bgif && (int64_t)v->w * v->h > (int64_t)b->w * b->h)) b = v;
    }
    return b;
}

void store_page_url(int i, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (i >= 0 && i < S.count) str_printf(out, cap, SITE L"/%ls", S.items[i].slug);
}

/* ---------------------------------------------------------------- downloads (UI thread) */

static StoreItem *find_id(int id)
{
    for (int i = 0; i < S.count; i++)
        if (S.items[i].id == id) return &S.items[i];
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
    it->dl = STOREDL_IDLE;                      /* from here on the queue tells how it is doing */
    if (!v || g_queued == QUEUED_MAX) {
        it->dl = STOREDL_FAILED;
        app_notice(NOTICE_WARN, v ? L"Too many downloads are waiting; try '%ls' again in a moment."
                                  : L"'%ls' has nothing to download.", it->title);
        return;
    }
    Queued *q = &g_queue[g_queued++];
    q->id = it->id;
    str_printf(q->url, SP_COUNTOF(q->url), SITE L"/dl/%ls/%d/", v->kind, it->id);
    str_copy(q->title, SP_COUNTOF(q->title), it->title);
}

static void pump(void)
{
    while (g_active < 0 && g_queued) {
        UrlImportState st;
        url_import_get(&st);
        if (st.busy) return;                    /* a link import from the Library goes first */
        Queued q = g_queue[0];
        unqueue(0);
        g_seen = st.finished;
        if (url_import_start_titled(q.url, URLMODE_DOWNLOAD, q.title)) {
            g_active = q.id;
            LOGI(L"store: downloading '%ls' (%ls)", q.title, q.url);
        } else {
            StoreItem *it = find_id(q.id);
            if (it) it->dl = STOREDL_FAILED;
        }
    }
}

int store_dl_state(int i, int *percent)
{
    if (percent) *percent = -1;
    if (i < 0 || i >= S.count) return STOREDL_IDLE;
    int id = S.items[i].id;
    if (id == g_active) {
        if (percent) {
            UrlImportState st;
            url_import_get(&st);
            *percent = st.percent;
        }
        return STOREDL_RUNNING;
    }
    for (int k = 0; k < g_queued; k++)
        if (g_queue[k].id == id) return STOREDL_WAIT;
    for (int k = 0; k < SP_MIN(g_doneCount, (int)SP_COUNTOF(g_done)); k++)
        if (g_done[k] == id) return STOREDL_DONE;
    return S.items[i].dl;
}

void store_download(int i)
{
    if (i < 0 || i >= S.count) return;
    StoreItem *it = &S.items[i];
    int st = store_dl_state(i, NULL);
    if (st == STOREDL_RUNNING) {                /* pressed again: stop it */
        url_import_cancel();
        return;
    }
    if (st == STOREDL_WAIT) {                   /* pressed again: no longer wanted */
        for (int k = 0; k < g_queued; k++) {
            if (g_queue[k].id == it->id) {
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
        want_detail(i, 1);                      /* ahead of the previews */
    }
    pump();
    ui_on_status();
}

static void on_list(const Result *r)
{
    S.loading = 0;
    if (!r->ok) {
        S.failed = 1;
        str_copy(S.error, SP_COUNTOF(S.error), r->error[0] ? r->error : L"MotionBGS could not be read");
        LOGW(L"store: page %d of '%ls' failed: %ls", r->page, S.query, S.error);
        return;
    }
    if (r->tag[0]) {
        g_list = L_TAG;
        strcpy_s(g_tag, sizeof g_tag, r->tag);
    }
    g_page = r->page;
    S.more = r->more;
    if (S.count + r->count > g_cap) {
        int nc = SP_MAX(g_cap * 2, S.count + r->count);
        StoreItem *n = (StoreItem *)realloc(S.items, sizeof(StoreItem) * (size_t)nc);
        if (!n) {
            S.more = 0;
            return;
        }
        S.items = n;
        g_cap = nc;
    }
    for (int i = 0; i < r->count && S.count < ITEMS_MAX; i++) {
        int dup = 0;                            /* new wallpapers shift the latest: a page may repeat a card */
        for (int k = 0; k < S.count && !dup; k++) dup = S.items[k].id == r->items[i].id;
        if (!dup) S.items[S.count++] = r->items[i];
    }
    if (S.count >= ITEMS_MAX) S.more = 0;
}

static void on_detail(StoreItem *it, const Result *r)
{
    if (r->ok) {
        memcpy(it->var, r->var, sizeof it->var);
        it->varCount = r->varCount;
        it->detail = STORE_READY;
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
        } else if (r->index >= 0 && r->index < S.count && S.items[r->index].id == r->id) {
            StoreItem *it = &S.items[r->index];
            if (r->kind == T_PREVIEW) it->preview = r->ok ? STORE_READY : STORE_FAILED;
            else on_detail(it, r);
        }
    }
    free(r->items);
    free(r);
    pump();
    ui_on_status();
}

void store_on_import(void)
{
    UrlImportState st;
    url_import_get(&st);
    if (g_active >= 0 && !st.busy && st.finished != g_seen) {    /* the Store's download ended */
        StoreItem *it = find_id(g_active);
        if (st.lastOk) g_done[g_doneCount++ % SP_COUNTOF(g_done)] = g_active;
        else if (it) it->dl = st.failed ? STOREDL_FAILED : STOREDL_IDLE;   /* failed, or cancelled */
        if (st.failed) app_notice(NOTICE_ERROR, L"%ls", st.text);
        g_active = -1;
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
