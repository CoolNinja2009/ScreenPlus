/* "Show a still while Screen+ starts" (see stills.h). One still per display, made once per wallpaper,
 * look and display size and named after them (cache\stills\<display>-<content>.jpg), so a file that
 * exists is already the Windows wallpaper and a normal start does no work at all. A picture, or the
 * frame a video starts on, is cover-cropped to the display and graded the way video.hlsl grades it; a
 * web page is the live page as WebView2 drew it (html_grab, engine.c). One background thread makes and
 * sets them in order, at background priority. The user's own Windows wallpaper is kept in original.txt
 * before the first still and put back when the setting goes off. */
#include "stills.h"
#include "../renderer/renderer.h"
#include "../renderer/html/wallpaper_api.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/ipc.h"
#include <objbase.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct StillJob {
    struct StillJob *next;
    MonitorInfo      mon;
    WallpaperEntry   wp;
    wchar_t          path[SP_MAX_PATH];
    uint8_t         *jpeg;       /* a grabbed page; NULL: made from the wallpaper's file */
    size_t           jpegSize;
    int              restore;    /* the setting went off */
} StillJob;

static SRWLOCK   g_lock = SRWLOCK_INIT;
static StillJob *g_head, *g_tail;
static int       g_running;

static void stills_dir(wchar_t *out, size_t cap) { path_join(out, cap, paths_cache(), L"stills"); }

static void original_file(wchar_t *out, size_t cap)
{
    wchar_t dir[SP_MAX_PATH];
    stills_dir(dir, SP_COUNTOF(dir));
    path_join(out, cap, dir, L"original.txt");
}

static int is_span(const MonitorInfo *mon) { return wcscmp(mon->id, SP_SPAN_ID) == 0; }

BOOL stills_path(const MonitorInfo *mon, const WallpaperEntry *wp, wchar_t *out, size_t cap)
{
    wchar_t propsDir[SP_MAX_PATH], props[SP_MAX_PATH], dir[SP_MAX_PATH], file[96], key[SP_MAX_PATH + 160];
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME look = { 0 };        /* Customise changes (props\<id>.json) make a new still */
    if (!wp->id[0]) return FALSE;
    path_join(propsDir, SP_COUNTOF(propsDir), paths_root(), L"props");
    str_printf(file, SP_COUNTOF(file), L"%ls.json", wp->id);
    path_join(props, SP_COUNTOF(props), propsDir, file);
    if (GetFileAttributesExW(props, GetFileExInfoStandard, &a)) look = a.ftLastWriteTime;
    str_printf(key, SP_COUNTOF(key), L"%ls|%ls|%ld|%ld|%lx%08lx", wp->id, wp->path, mon->rect.right - mon->rect.left,
               mon->rect.bottom - mon->rect.top, look.dwHighDateTime, look.dwLowDateTime);
    str_printf(file, SP_COUNTOF(file), L"%08x-%08x.jpg", hash_fnv1a_w(mon->id), hash_fnv1a_w(key));
    stills_dir(dir, SP_COUNTOF(dir));
    path_join(out, cap, dir, file);
    return TRUE;
}

/* ---------------------------------------------------------------- files */

static HRESULT write_all(const wchar_t *path, const void *data, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, (DWORD)n, &written, NULL) && written == n;
    CloseHandle(f);
    return ok ? S_OK : E_FAIL;
}

static char *read_all(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    char *buf = GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20) ? (char *)malloc((size_t)size.QuadPart + 1) : NULL;
    DWORD got = 0;
    if (buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL)) buf[got] = 0;
    else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    return buf;
}

static BOOL copy_stream(IStream *st, uint8_t **out, size_t *size)
{
    HGLOBAL g = NULL;
    STATSTG ss;
    if (FAILED(GetHGlobalFromStream(st, &g)) || !g || FAILED(IStream_Stat(st, &ss, STATFLAG_NONAME))) return FALSE;
    size_t n = (size_t)ss.cbSize.QuadPart;
    const void *p = GlobalLock(g);
    uint8_t *buf = p && n ? (uint8_t *)malloc(n) : NULL;
    if (buf) memcpy(buf, p, n);
    if (p) GlobalUnlock(g);
    *out = buf;
    *size = n;
    return buf != NULL;
}

/* ---------------------------------------------------------------- making a still */

/* The renderers' grading on BGRA pixels: video.hlsl apply_grade with grade_fill's constants. */
static void grade(uint8_t *px, size_t n, const WallpaperAdjust *a)
{
    float bright = a->brightness / 100.0f * 0.5f, contrast = 1.0f + a->contrast / 100.0f;
    float gamma = a->gamma >= 0 ? 1.0f / (1.0f + a->gamma / 100.0f) : 1.0f + (float)(-a->gamma) / 100.0f;
    float sat = 1.0f + a->saturation / 100.0f, hue = a->hue * 3.14159265f / 180.0f, v = a->tintBrightness / 100.0f;
    float tint[3] = { ((a->tint >> 16) & 255) / 255.0f * v, ((a->tint >> 8) & 255) / 255.0f * v, (a->tint & 255) / 255.0f * v };
    float lift = a->tintLift / 100.0f, hc = cosf(hue), hs = sinf(hue);
    for (size_t i = 0; i < n; i++, px += 4) {
        float c[3] = { px[2] / 255.0f, px[1] / 255.0f, px[0] / 255.0f };
        for (int k = 0; k < 3; k++) {
            float t = (c[k] + bright - 0.5f) * contrast + 0.5f;
            c[k] = powf(SP_CLAMP(t, 0.0f, 1.0f), gamma);
        }
        float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int k = 0; k < 3; k++) c[k] = luma + (c[k] - luma) * sat;
        if (fabsf(hue) > 0.001f) {                 /* rotate the chroma in YIQ */
            float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            float ci = 0.595916f * c[0] - 0.274556f * c[1] - 0.284113f * c[2];
            float cq = 0.211456f * c[0] - 0.522591f * c[1] + 0.311135f * c[2];
            float ri = ci * hc - cq * hs, rq = ci * hs + cq * hc;
            c[0] = y + 0.956295f * ri + 0.621024f * rq;
            c[1] = y - 0.272121f * ri - 0.637934f * rq;
            c[2] = y - 1.106843f * ri + 1.704585f * rq;
        }
        for (int k = 0; k < 3; k++) {
            float t = c[k] + (SP_MAX(c[k], tint[k]) - c[k]) * lift;   /* the tint lifts the blacks */
            px[2 - k] = (uint8_t)(SP_CLAMP(t, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

static HRESULT save_jpeg(const wchar_t *path, const uint8_t *bgra, int w, int h)
{
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *bag = NULL;
    IWICBitmap *bmp = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = { 0 };
        VARIANT v;
        opt.pstrName = L"ImageQuality";
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.92f;
        IPropertyBag2_Write(bag, 1, &opt, &v);
        hr = IWICBitmapFrameEncode_Initialize(fe, bag);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, (UINT)w, (UINT)h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromMemory(f, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppBGRA, (UINT)w * 4,
                                                                        (UINT)w * 4 * (UINT)h, (BYTE *)bgra, &bmp);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)bmp, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    SP_RELEASE(bmp);
    SP_RELEASE(bag);
    SP_RELEASE(fe);
    SP_RELEASE(enc);
    SP_RELEASE(st);
    SP_RELEASE(f);
    return hr;
}

/* A video's first frame, else a picture: the image itself, or what stands in for a page or stream
 * (a depth wallpaper's original picture, a stream's thumbnail). */
static HRESULT make_from_file(const WallpaperEntry *e, int w, int h, const wchar_t *out)
{
    uint8_t *bgra = NULL;
    HRESULT hr = E_FAIL;
    if (e->type == WP_VIDEO && !wallpaper_is_url(e->path)) {
        if (!ffmpeg_first_frame(e->path, w, h, &bgra)) return E_FAIL;
    } else {
        const wchar_t *pic = e->type == WP_IMAGE ? e->path : e->thumb;
        if (!pic[0] || wallpaper_is_url(pic)) return E_FAIL;
        HBITMAP bmp = image_decode_cover(pic, NULL, w, h, &hr);
        DIBSECTION ds;
        if (!bmp) return FAILED(hr) ? hr : E_FAIL;
        if (GetObjectW(bmp, (int)sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmBits &&
            (bgra = (uint8_t *)malloc((size_t)w * h * 4)) != NULL)
            memcpy(bgra, ds.dsBm.bmBits, (size_t)w * h * 4);
        DeleteObject(bmp);
        if (!bgra) return E_OUTOFMEMORY;
    }
    WallpaperAdjust a;
    if (wpapi_load_adjust(e->id, &a) && adjust_active(&a)) grade(bgra, (size_t)w * h, &a);
    hr = save_jpeg(out, bgra, w, h);
    free(bgra);
    return hr;
}

/* ---------------------------------------------------------------- the Windows wallpaper */

static LPWSTR find_monitor(IDesktopWallpaper *dw, const MonitorInfo *mon)
{
    UINT n = 0;
    IDesktopWallpaper_GetMonitorDevicePathCount(dw, &n);
    for (UINT i = 0; i < n; i++) {
        LPWSTR id = NULL;
        RECT rc;
        if (FAILED(IDesktopWallpaper_GetMonitorDevicePathAt(dw, i, &id)) || !id) continue;
        if (str_ieq(id, mon->id) || (SUCCEEDED(IDesktopWallpaper_GetMonitorRECT(dw, id, &rc)) && EqualRect(&rc, &mon->rect)))
            return id;
        CoTaskMemFree(id);
    }
    return NULL;
}

/* Once, before the first still: the user's own Windows wallpaper ("position=n", then display TAB picture
 * lines). Not when one of ours is already there: then nothing of the user's is left to keep. */
static void keep_original(IDesktopWallpaper *dw)
{
    wchar_t file[SP_MAX_PATH], dir[SP_MAX_PATH], line[SP_MAX_PATH * 2 + 8];
    original_file(file, SP_COUNTOF(file));
    if (file_exists(file)) return;
    stills_dir(dir, SP_COUNTOF(dir));
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    IDesktopWallpaper_GetPosition(dw, &pos);
    StrBuf b = { 0 };
    sb_printf(&b, "position=%d\n", (int)pos);
    UINT n = 0;
    BOOL ours = FALSE;
    IDesktopWallpaper_GetMonitorDevicePathCount(dw, &n);
    for (UINT i = 0; i < n; i++) {
        LPWSTR id = NULL, pic = NULL;
        if (SUCCEEDED(IDesktopWallpaper_GetMonitorDevicePathAt(dw, i, &id)) && id &&
            SUCCEEDED(IDesktopWallpaper_GetWallpaper(dw, id, &pic)) && pic) {
            ours |= str_istarts(pic, dir);
            str_printf(line, SP_COUNTOF(line), L"%ls\t%ls\n", id, pic);
            sb_put_wide(&b, line);
        }
        CoTaskMemFree(pic);
        CoTaskMemFree(id);
    }
    if (!ours && !b.failed && b.p) write_all(file, b.p, b.len);
    sb_free(&b);
}

/* the Windows wallpaper of this display, or of all of them as one picture (span) */
static HRESULT set_wallpaper(const MonitorInfo *mon, const wchar_t *path)
{
    IDesktopWallpaper *dw = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DesktopWallpaper, NULL, CLSCTX_ALL, &IID_IDesktopWallpaper, (void **)&dw);
    if (FAILED(hr)) return hr;
    keep_original(dw);
    LPWSTR id = is_span(mon) ? NULL : find_monitor(dw, mon);
    if (!is_span(mon) && !id) hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (SUCCEEDED(hr)) hr = IDesktopWallpaper_SetPosition(dw, is_span(mon) ? DWPOS_SPAN : DWPOS_FILL);
    if (SUCCEEDED(hr)) hr = IDesktopWallpaper_SetWallpaper(dw, id, path);
    CoTaskMemFree(id);
    IDesktopWallpaper_Release(dw);
    return hr;
}

/* What is the Windows wallpaper now stays: a span still replaces every other one; a display's still
 * replaces that display's older ones and a span still. */
static void prune(const MonitorInfo *mon, const wchar_t *keep)
{
    wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH], f[SP_MAX_PATH], mine[12], span[12];
    WIN32_FIND_DATAW fd;
    str_printf(mine, SP_COUNTOF(mine), L"%08x-", hash_fnv1a_w(mon->id));
    str_printf(span, SP_COUNTOF(span), L"%08x-", hash_fnv1a_w(SP_SPAN_ID));
    stills_dir(dir, SP_COUNTOF(dir));
    path_join(pat, SP_COUNTOF(pat), dir, L"*.jpg");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
        if (!str_ieq(f, keep) && (is_span(mon) || str_istarts(fd.cFileName, mine) || str_istarts(fd.cFileName, span)))
            DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void make_and_set(StillJob *j)
{
    wchar_t dir[SP_MAX_PATH], tmp[SP_MAX_PATH + 8];
    int w = j->mon.rect.right - j->mon.rect.left, h = j->mon.rect.bottom - j->mon.rect.top;
    if (file_exists(j->path) || w <= 0 || h <= 0) return;      /* made by an earlier job */
    stills_dir(dir, SP_COUNTOF(dir));
    dir_create_recursive(dir);
    str_printf(tmp, SP_COUNTOF(tmp), L"%ls.part", j->path);
    HRESULT hr = j->jpeg ? write_all(tmp, j->jpeg, j->jpegSize) : make_from_file(&j->wp, w, h, tmp);
    if (SUCCEEDED(hr) && !MoveFileExW(tmp, j->path, MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr)) hr = set_wallpaper(&j->mon, j->path);
    if (FAILED(hr)) {
        DeleteFileW(tmp);
        DeleteFileW(j->path);                                  /* not the Windows wallpaper: made again next time */
        LOGW(L"stills: no still of '%ls' on %ls (0x%08lX)", j->wp.name, j->mon.name, (unsigned long)hr);
        return;
    }
    prune(&j->mon, j->path);
    LOGI(L"stills: the Windows wallpaper on %ls is a still of '%ls'", j->mon.name, j->wp.name);
}

static void restore_original(void)
{
    wchar_t file[SP_MAX_PATH], dir[SP_MAX_PATH], pat[SP_MAX_PATH], f[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    IDesktopWallpaper *dw = NULL;
    original_file(file, SP_COUNTOF(file));
    char *text = read_all(file);
    wchar_t *w = text ? wide_from_utf8(text, -1) : NULL;
    if (w && SUCCEEDED(CoCreateInstance(&CLSID_DesktopWallpaper, NULL, CLSCTX_ALL, &IID_IDesktopWallpaper, (void **)&dw))) {
        for (wchar_t *line = w, *next; *line; line = next) {
            next = wcschr(line, L'\n');
            if (next) *next++ = 0;
            else next = line + wcslen(line);
            wchar_t *tab = wcschr(line, L'\t');
            if (str_istarts(line, L"position=")) IDesktopWallpaper_SetPosition(dw, (DESKTOP_WALLPAPER_POSITION)_wtoi(line + 9));
            else if (tab) {
                *tab = 0;
                IDesktopWallpaper_SetWallpaper(dw, line, tab + 1);
            }
        }
        IDesktopWallpaper_Release(dw);
        LOGI(L"stills: the user's own Windows wallpaper is back");
    }
    free(w);
    free(text);
    stills_dir(dir, SP_COUNTOF(dir));                          /* every still goes, original.txt with them */
    path_join(pat, SP_COUNTOF(pat), dir, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* ---------------------------------------------------------------- queue */

static DWORD WINAPI worker(LPVOID unused)
{
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   /* never in the wallpaper's way */
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        StillJob *j = g_head;
        if (j && !(g_head = j->next)) g_tail = NULL;
        if (!j) g_running = 0;
        ReleaseSRWLockExclusive(&g_lock);
        if (!j) break;
        if (j->restore) restore_original();
        else make_and_set(j);
        free(j->jpeg);
        free(j);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

static void queue(StillJob *j)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_tail) g_tail->next = j;
    else g_head = j;
    g_tail = j;
    int start = !g_running;
    g_running = 1;
    ReleaseSRWLockExclusive(&g_lock);
    if (!start) return;
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else {                                                     /* the next job tries again */
        AcquireSRWLockExclusive(&g_lock);
        g_running = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void stills_make(const MonitorInfo *mon, const WallpaperEntry *wp, IStream *jpeg)
{
    StillJob *j = (StillJob *)calloc(1, sizeof *j);
    if (!j || !stills_path(mon, wp, j->path, SP_COUNTOF(j->path)) || (jpeg && !copy_stream(jpeg, &j->jpeg, &j->jpegSize))) {
        free(j);
        return;
    }
    j->mon = *mon;
    j->wp = *wp;
    queue(j);
}

void stills_restore(void)
{
    StillJob *j = (StillJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->restore = 1;
    queue(j);
}
