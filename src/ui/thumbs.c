#include "thumbs.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../app/preview.h"
#include <shobjidl.h>
#include <wincodec.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PENDING  96
#define ANIM_FRAMES  40     /* per animation; with the size caps below, at most ~3.7 MB each */
#define ANIM_MAX_W   240    /* card animations are stored at most this wide (scaled up when drawn) */
#define ANIM_MAX_WL  480    /* ... and the large "now showing" preview */

typedef struct Req {
    wchar_t         id[SP_ID_LEN];
    wchar_t         path[SP_MAX_PATH];
    wchar_t         thumb[SP_MAX_PATH];   /* package preview named by LivelyInfo.json / project.json */
    wchar_t         anim[SP_MAX_PATH];    /* animation source (animation requests only) */
    WallpaperType   type;
    WallpaperFormat format;
    int             w, h, large;
} Req;

typedef struct Queue {
    Req *q;
    int  count;
} Queue;

static struct {
    HANDLE           thread, event;
    CRITICAL_SECTION lock;
    Queue            still, anim;      /* stills always go first; animations fill in afterwards */
    volatile LONG    quit;
    HWND             notify;
    int              init;
} T;

static uint8_t *cover_from_source(IWICImagingFactory *f, IWICBitmapSource *src, int w, int h)
{
    UINT sw = 0, sh = 0;
    if (FAILED(IWICBitmapSource_GetSize(src, &sw, &sh)) || !sw || !sh) return NULL;
    double s = SP_MAX((double)w / sw, (double)h / sh);
    UINT dw = (UINT)SP_MAX(w, (int)(sw * s + 0.5)), dh = (UINT)SP_MAX(h, (int)(sh * s + 0.5));
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    uint8_t *buf = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, src, dw, dh, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        buf = (uint8_t *)malloc((size_t)w * h * 4);
        WICRect rc = { (INT)(dw - (UINT)w) / 2, (INT)(dh - (UINT)h) / 2, w, h };
        if (buf && FAILED(IWICFormatConverter_CopyPixels(cv, &rc, (UINT)w * 4, (UINT)(w * h * 4), buf))) {
            free(buf);
            buf = NULL;
        }
    }
    SP_RELEASE(cv);
    SP_RELEASE(sc);
    return buf;
}

static uint8_t *from_shell(IWICImagingFactory *f, const wchar_t *path, int w, int h)
{
    IShellItemImageFactory *sf = NULL;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void **)&sf))) return NULL;
    int box = SP_MAX(w, h) * 16 / 9;
    SIZE sz = { box, box };
    HBITMAP hb = NULL;
    uint8_t *buf = NULL;
    if (SUCCEEDED(IShellItemImageFactory_GetImage(sf, sz, SIIGBF_THUMBNAILONLY, &hb)) && hb) {
        IWICBitmap *wb = NULL;
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromHBITMAP(f, hb, NULL, WICBitmapIgnoreAlpha, &wb))) {
            buf = cover_from_source(f, (IWICBitmapSource *)wb, w, h);
            IWICBitmap_Release(wb);
        }
        DeleteObject(hb);
    }
    IShellItemImageFactory_Release(sf);
    return buf;
}

static uint8_t *from_file(IWICImagingFactory *f, const wchar_t *path, int w, int h)
{
    IWICBitmapDecoder *d = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    uint8_t *buf = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)))
        buf = cover_from_source(f, (IWICBitmapSource *)fr, w, h);
    SP_RELEASE(fr);
    SP_RELEASE(d);
    return buf;
}

static uint8_t *html_preview(IWICImagingFactory *f, const wchar_t *entry, int w, int h)
{
    static const wchar_t *names[] = { L"preview.jpg", L"preview.png", L"preview.webp", L"preview.gif", L"thumbnail.jpg",
                                      L"thumbnail.png", L"screenshot.jpg", L"screenshot.png", L"cover.jpg", L"cover.png" };
    wchar_t dir[SP_MAX_PATH], p[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), entry);
    for (size_t i = 0; i < SP_COUNTOF(names); i++) {
        path_join(p, SP_COUNTOF(p), dir, names[i]);
        if (file_exists(p)) {
            uint8_t *b = from_file(f, p, w, h);
            if (b) return b;
        }
    }
    return NULL;
}

static int is_gif(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    return dot && _wcsicmp(dot, L".gif") == 0;
}

/* The GIF a package names in its manifest (Lively "Preview", Wallpaper Engine "preview"). */
static int manifest_gif(const wchar_t *dir, const wchar_t *manifest, const char *key, wchar_t *out, size_t cap)
{
    wchar_t p[SP_MAX_PATH], name[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), dir, manifest);
    if (!file_exists(p)) return 0;
    JVal *j = json_parse_file(p);
    json_str_w(json_geti(j, key), name, SP_COUNTOF(name));
    json_free(j);
    if (!name[0]) return 0;
    path_join(out, cap, dir, name);
    return is_gif(out) && file_exists(out);
}

/* Where a moving preview comes from, if the wallpaper moves: the package's preview GIF, a GIF
 * wallpaper itself, or (for videos without one) the video. Still images get no animation. */
static int anim_source(const Req *r, wchar_t *out, size_t cap)
{
    if (r->type == WP_IMAGE) {
        if (!is_gif(r->path) || !file_exists(r->path)) return 0;
        str_copy(out, cap, r->path);
        return 1;
    }
    wchar_t dir[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), r->path);
    if (wallpaper_is_url(r->path)) {
        /* web address: no folder of its own */
    } else if (r->type == WP_HTML || r->format != WPF_PLAIN) {   /* the folder belongs to this wallpaper */
        if (manifest_gif(dir, L"LivelyInfo.json", "Preview", out, cap)) return 1;
        if (manifest_gif(dir, L"project.json", "preview", out, cap)) return 1;
        if (is_gif(r->thumb) && file_exists(r->thumb)) {
            str_copy(out, cap, r->thumb);
            return 1;
        }
        static const wchar_t *names[] = { L"preview.gif", L"lively_p.gif" };
        for (size_t i = 0; i < SP_COUNTOF(names); i++) {
            path_join(out, cap, dir, names[i]);
            if (file_exists(out)) return 1;
        }
    }
    if (r->type == WP_VIDEO && file_exists(r->path)) {
        str_copy(out, cap, r->path);
        return 1;
    }
    if (r->type != WP_HTML) return 0;
    /* No preview animation of its own: record one, like Lively. A recording that produced only
     * the still means the page does not move. */
    wchar_t jpg[SP_MAX_PATH];
    preview_paths(r->id, NULL, jpg, SP_COUNTOF(jpg));
    preview_paths(r->id, out, NULL, cap);
    if (file_exists(out)) return 1;
    if (!file_exists(jpg)) preview_request(r->id, T.notify);
    return 0;
}

/* Adds (or moves to the newest position) a request; the oldest is dropped when full. Caller locks. */
static void queue_push(Queue *qu, const Req *r)
{
    for (int i = 0; i < qu->count; i++) {
        if (qu->q[i].large == r->large && wcscmp(qu->q[i].id, r->id) == 0) {
            memmove(&qu->q[i], &qu->q[i + 1], sizeof(Req) * (size_t)(qu->count - i - 1));
            qu->count--;
            break;
        }
    }
    if (qu->count == MAX_PENDING) {
        memmove(&qu->q[0], &qu->q[1], sizeof(Req) * (MAX_PENDING - 1));
        qu->count--;
    }
    qu->q[qu->count++] = *r;
}

static void post(ThumbResult *res)
{
    if (!T.notify || !PostMessageW(T.notify, WM_SP_THUMB, 0, (LPARAM)res)) thumbs_free(res);
}

static void make_still(IWICImagingFactory *f, Req *r)
{
    uint8_t *px = NULL;
    if (f) {
        /* Packages name their own preview (e.g. Lively's "lively_t.jpg"); prefer it over guessing. */
        if (r->thumb[0] && file_exists(r->thumb)) px = from_file(f, r->thumb, r->w, r->h);
        if (!px && r->type == WP_HTML) {
            px = html_preview(f, r->path, r->w, r->h);
            if (!px) {                    /* nothing shipped: the recorded preview, or record one */
                wchar_t jpg[SP_MAX_PATH];
                preview_paths(r->id, NULL, jpg, SP_COUNTOF(jpg));
                if (file_exists(jpg)) px = from_file(f, jpg, r->w, r->h);
                else preview_request(r->id, T.notify);
            }
        } else if (!px && file_exists(r->path)) {
            px = from_shell(f, r->path, r->w, r->h);
            if (!px && r->type == WP_IMAGE) px = from_file(f, r->path, r->w, r->h);
            if (!px && r->type == WP_VIDEO) ffmpeg_thumbnail(r->path, r->w, r->h, &px);
        }
    }
    ThumbResult *res = (ThumbResult *)calloc(1, sizeof *res);
    if (!res) {
        free(px);
        return;
    }
    str_copy(res->id, SP_ID_LEN, r->id);
    res->large = r->large;
    res->w = r->w;
    res->h = r->h;
    res->bgra = px;
    res->frames = 1;
    post(res);
    if (px && anim_source(r, r->anim, SP_COUNTOF(r->anim))) {
        EnterCriticalSection(&T.lock);
        queue_push(&T.anim, r);
        LeaveCriticalSection(&T.lock);
    }
}

static void make_animation(const Req *r)
{
    int w = r->w, h = r->h, cap = r->large ? ANIM_MAX_WL : ANIM_MAX_W;
    if (w > cap) {
        h = SP_MAX(1, h * cap / w);
        w = cap;
    }
    uint8_t *px = NULL;
    uint16_t *ms = NULL;
    int n = 0;
    if (!ffmpeg_animation(r->anim, w, h, ANIM_FRAMES, &px, &ms, &n)) return;
    ThumbResult *res = (ThumbResult *)calloc(1, sizeof *res);
    if (!res) {
        free(px);
        free(ms);
        return;
    }
    str_copy(res->id, SP_ID_LEN, r->id);
    res->large = r->large;
    res->w = w;
    res->h = h;
    res->bgra = px;
    res->frames = n;
    res->delays = ms;
    post(res);
}

static unsigned __stdcall worker(void *arg)
{
    (void)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus thumbnails");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    Req *r = (Req *)malloc(sizeof *r);
    while (r && !T.quit) {
        int kind = 0;   /* 1 still, 2 animation */
        EnterCriticalSection(&T.lock);
        if (T.still.count) {
            *r = T.still.q[--T.still.count];      /* newest first: what the user is looking at now */
            kind = 1;
        } else if (T.anim.count) {
            *r = T.anim.q[--T.anim.count];
            kind = 2;
        }
        LeaveCriticalSection(&T.lock);
        if (kind == 1) make_still(f, r);
        else if (kind == 2) make_animation(r);
        else WaitForSingleObject(T.event, INFINITE);
    }
    free(r);
    SP_RELEASE(f);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

void thumbs_start(HWND notify)
{
    if (!T.init) {
        InitializeCriticalSection(&T.lock);
        T.init = 1;
    }
    T.notify = notify;
    if (T.thread) return;
    T.quit = 0;
    T.still.q = (Req *)calloc(MAX_PENDING, sizeof(Req));
    T.anim.q = (Req *)calloc(MAX_PENDING, sizeof(Req));
    T.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!T.still.q || !T.anim.q || !T.event) return;
    T.thread = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
}

void thumbs_request(const WallpaperEntry *e, int w, int h, int large)
{
    if (!T.thread || w <= 0 || h <= 0) return;
    Req *r = (Req *)calloc(1, sizeof *r);
    if (!r) return;
    str_copy(r->id, SP_ID_LEN, e->id);
    str_copy(r->path, SP_COUNTOF(r->path), e->path);
    str_copy(r->thumb, SP_COUNTOF(r->thumb), e->thumb);
    r->type = e->type;
    r->format = e->format;
    r->w = w;
    r->h = h;
    r->large = large;
    EnterCriticalSection(&T.lock);
    for (int i = 0; i < T.anim.count; i++) {      /* a pending animation is re-queued after the new still */
        if (T.anim.q[i].large == large && wcscmp(T.anim.q[i].id, e->id) == 0) {
            memmove(&T.anim.q[i], &T.anim.q[i + 1], sizeof(Req) * (size_t)(T.anim.count - i - 1));
            T.anim.count--;
            break;
        }
    }
    queue_push(&T.still, r);
    LeaveCriticalSection(&T.lock);
    free(r);
    SetEvent(T.event);
}

void thumbs_stop(void)
{
    if (!T.thread) return;
    T.notify = NULL;
    T.quit = 1;
    SetEvent(T.event);
    if (WaitForSingleObject(T.thread, 3000) != WAIT_OBJECT_0) {
        LOGW(L"thumbnail thread still busy; detaching it");   /* never free state it may still use */
        CloseHandle(T.thread);
        T.thread = NULL;
        return;
    }
    CloseHandle(T.thread);
    CloseHandle(T.event);
    T.thread = T.event = NULL;
    free(T.still.q);
    free(T.anim.q);
    T.still.q = T.anim.q = NULL;
    T.still.count = T.anim.count = 0;
}

void thumbs_free(ThumbResult *r)
{
    if (!r) return;
    free(r->bgra);
    free(r->delays);
    free(r);
}
