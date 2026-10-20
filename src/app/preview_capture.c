#include "preview.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../settings/config.h"
#include "../renderer/renderer.h"
#include <shlobj.h>
#include <wincodec.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define CAP_W       960      /* capture window, physical px: CSS 1920x1080 at rasterization scale 0.5 */
#define CAP_H       540
#define GIF_W       480
#define GIF_H       270
#define GIF_FRAMES  40       /* ~4 s at the grab interval below */
#define GRAB_MS     100
#define WARMUP_MS   2500     /* after the page loaded: let intros and fade-ins settle */
#define DEADLINE_MS 45000

enum { T_WARMUP = 0x6001, T_NEXT, T_DEADLINE };

void preview_paths(const wchar_t *id, wchar_t *gif, wchar_t *jpg, size_t cap)
{
    wchar_t dir[SP_MAX_PATH], name[SP_ID_LEN + 8];
    path_join(dir, SP_COUNTOF(dir), paths_cache(), L"previews");
    if (gif) {
        str_printf(name, SP_COUNTOF(name), L"%ls.gif", id);
        path_join(gif, cap, dir, name);
    }
    if (jpg) {
        str_printf(name, SP_COUNTOF(name), L"%ls.jpg", id);
        path_join(jpg, cap, dir, name);
    }
}

/* ================================================================ shell side: one recording at a time */

static struct {
    INIT_ONCE        once;
    CRITICAL_SECTION lock;
    HANDLE           thread, job;
    HWND             notify;
    wchar_t          queue[64][SP_ID_LEN];
    int              count;
    wchar_t          tried[512][SP_ID_LEN];
    int              triedCount;
} Q = { INIT_ONCE_STATIC_INIT };

static BOOL CALLBACK q_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&Q.lock);
    Q.job = CreateJobObjectW(NULL, NULL);       /* recordings end together with Screen+ */
    if (Q.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(Q.job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return TRUE;
}

static unsigned __stdcall q_worker(void *arg)
{
    (void)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus preview recorder");
    for (;;) {
        wchar_t id[SP_ID_LEN];
        HWND notify;
        EnterCriticalSection(&Q.lock);
        if (!Q.count) {
            CloseHandle(Q.thread);
            Q.thread = NULL;
            LeaveCriticalSection(&Q.lock);
            return 0;
        }
        str_copy(id, SP_ID_LEN, Q.queue[0]);
        memmove(&Q.queue[0], &Q.queue[1], sizeof Q.queue[0] * (size_t)--Q.count);
        notify = Q.notify;
        LeaveCriticalSection(&Q.lock);

        wchar_t cmd[SP_MAX_PATH + 64];
        str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --capture-preview %ls", paths_exe(), id);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = { 0 };
        DWORD code = 1;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                           NULL, NULL, &si, &pi)) {
            if (Q.job) AssignProcessToJobObject(Q.job, pi.hProcess);
            ResumeThread(pi.hThread);
            if (WaitForSingleObject(pi.hProcess, DEADLINE_MS + 15000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        LOGI(L"preview: recording %ls %ls (exit %lu)", id, code == 0 ? L"finished" : L"failed", code);
        if (code == 0 && notify) {
            wchar_t *msg = _wcsdup(id);
            if (msg && !PostMessageW(notify, WM_SP_PREVIEW, 0, (LPARAM)msg)) free(msg);
        }
    }
}

void preview_request(const wchar_t *id, HWND notify)
{
    if (!id || !id[0]) return;
    InitOnceExecuteOnce(&Q.once, q_init, NULL, NULL);
    EnterCriticalSection(&Q.lock);
    for (int i = 0; i < Q.triedCount; i++) {
        if (wcscmp(Q.tried[i], id) == 0) {
            LeaveCriticalSection(&Q.lock);
            return;
        }
    }
    if (Q.triedCount < (int)SP_COUNTOF(Q.tried)) str_copy(Q.tried[Q.triedCount++], SP_ID_LEN, id);
    if (Q.count < (int)SP_COUNTOF(Q.queue)) str_copy(Q.queue[Q.count++], SP_ID_LEN, id);
    if (notify) Q.notify = notify;
    if (!Q.thread) Q.thread = (HANDLE)_beginthreadex(NULL, 0, q_worker, NULL, 0, NULL);
    LeaveCriticalSection(&Q.lock);
}

/* ================================================================ helper process */

static struct {
    Renderer *r;
    uint8_t  *frames;              /* GIF_FRAMES * GIF_W * GIF_H * 4, BGRA */
    double    at[GIF_FRAMES];      /* grab times, ms */
    double    start;
    int       n, done, result;
    wchar_t   gif[SP_MAX_PATH], jpg[SP_MAX_PATH];
} C;

static uint8_t *frame(int i) { return C.frames + (size_t)i * GIF_W * GIF_H * 4; }

static HRESULT open_encoder(IWICImagingFactory *f, const GUID *container, const wchar_t *path,
                            IWICStream **st, IWICBitmapEncoder **enc)
{
    HRESULT hr = IWICImagingFactory_CreateStream(f, st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(*st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, container, NULL, enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(*enc, (IStream *)*st, WICBitmapEncoderNoCache);
    return hr;
}

static HRESULT save_jpeg(IWICImagingFactory *f, const wchar_t *path, uint8_t *bgra)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *bag = NULL;
    IWICBitmap *bmp = NULL;
    HRESULT hr = open_encoder(f, &GUID_ContainerFormatJpeg, path, &st, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = { 0 };
        opt.pstrName = L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        IPropertyBag2_Write(bag, 1, &opt, &v);
        hr = IWICBitmapFrameEncode_Initialize(fe, bag);
    }
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, GIF_W, GIF_H);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromMemory(f, GIF_W, GIF_H, &GUID_WICPixelFormat32bppBGRA, GIF_W * 4,
                                                                        GIF_W * GIF_H * 4, bgra, &bmp);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)bmp, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    SP_RELEASE(bmp);
    SP_RELEASE(bag);
    SP_RELEASE(fe);
    SP_RELEASE(enc);
    SP_RELEASE(st);
    return hr;
}

/* Animated GIF: every frame gets its own 256-colour palette (error diffusion), looping forever. */
static HRESULT save_gif(IWICImagingFactory *f, const wchar_t *path)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICMetadataQueryWriter *meta = NULL;
    HRESULT hr = open_encoder(f, &GUID_ContainerFormatGif, path, &st, &enc);
    if (SUCCEEDED(hr) && SUCCEEDED(IWICBitmapEncoder_GetMetadataQueryWriter(enc, &meta))) {
        static UCHAR app[] = { 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0' };
        static UCHAR loop[] = { 3, 1, 0, 0, 0 };      /* sub-block: loop count 0 = forever */
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_UI1 | VT_VECTOR;
        v.caub.cElems = sizeof app;
        v.caub.pElems = app;
        IWICMetadataQueryWriter_SetMetadataByName(meta, L"/appext/Application", &v);
        v.caub.cElems = sizeof loop;
        v.caub.pElems = loop;
        IWICMetadataQueryWriter_SetMetadataByName(meta, L"/appext/Data", &v);
        SP_RELEASE(meta);
    }
    for (int i = 0; SUCCEEDED(hr) && i < C.n; i++) {
        IWICBitmapFrameEncode *fe = NULL;
        IWICBitmap *bmp = NULL;
        IWICPalette *pal = NULL;
        IWICFormatConverter *cv = NULL;
        WICPixelFormatGUID pf = GUID_WICPixelFormat8bppIndexed;
        hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, NULL);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fe, NULL);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, GIF_W, GIF_H);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromMemory(f, GIF_W, GIF_H, &GUID_WICPixelFormat32bppBGRA, GIF_W * 4,
                                                                            GIF_W * GIF_H * 4, frame(i), &bmp);
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreatePalette(f, &pal);
        if (SUCCEEDED(hr)) hr = IWICPalette_InitializeFromBitmap(pal, (IWICBitmapSource *)bmp, 256, FALSE);
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &cv);
        if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)bmp, &GUID_WICPixelFormat8bppIndexed,
                                                               WICBitmapDitherTypeErrorDiffusion, pal, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPalette(fe, pal);
        if (SUCCEEDED(hr) && SUCCEEDED(IWICBitmapFrameEncode_GetMetadataQueryWriter(fe, &meta))) {
            double d = i + 1 < C.n ? C.at[i + 1] - C.at[i] : GRAB_MS;
            PROPVARIANT v;
            PropVariantInit(&v);
            v.vt = VT_UI2;
            v.uiVal = (USHORT)SP_CLAMP((int)(d / 10.0 + 0.5), 2, 100);   /* 1/100 s */
            IWICMetadataQueryWriter_SetMetadataByName(meta, L"/grctlext/Delay", &v);
            SP_RELEASE(meta);
        }
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)cv, NULL);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
        SP_RELEASE(cv);
        SP_RELEASE(pal);
        SP_RELEASE(bmp);
        SP_RELEASE(fe);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    SP_RELEASE(enc);
    SP_RELEASE(st);
    return hr;
}

/* Average per-channel change between two frames (sampled). */
static double frame_change(const uint8_t *a, const uint8_t *b)
{
    long long sum = 0, count = 0;
    for (size_t i = 0; i < (size_t)GIF_W * GIF_H * 4; i += 4 * 7) {
        sum += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]);
        count += 3;
    }
    return count ? (double)sum / count : 0.0;
}

/* Writes <id>.jpg always and <id>.gif only for pages that move; files appear atomically. */
static int write_previews(void)
{
    IWICImagingFactory *f = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)))
        return 7;
    /* Fast pages change between neighbouring frames; slow drifts only across the whole recording. */
    double motion = C.n > 1 ? frame_change(frame(0), frame(C.n - 1)) : 0.0;
    for (int i = 1; i < C.n; i++) motion = SP_MAX(motion, frame_change(frame(i - 1), frame(i)));
    int moving = C.n >= 4 && motion > 0.6;
    wchar_t tmp[SP_MAX_PATH];
    str_printf(tmp, SP_COUNTOF(tmp), L"%ls.tmp", C.jpg);
    HRESULT hr = save_jpeg(f, tmp, frame(C.n / 2));
    if (SUCCEEDED(hr) && !MoveFileExW(tmp, C.jpg, MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && moving) {
        str_printf(tmp, SP_COUNTOF(tmp), L"%ls.tmp", C.gif);
        hr = save_gif(f, tmp);
        if (SUCCEEDED(hr) && !MoveFileExW(tmp, C.gif, MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    } else if (SUCCEEDED(hr)) {
        DeleteFileW(C.gif);
    }
    if (FAILED(hr)) DeleteFileW(tmp);
    SP_RELEASE(f);
    LOGI(L"preview: %d frames, motion %.2f -> %ls (0x%08lX)", C.n, motion, moving ? L"animated GIF" : L"still image",
         (unsigned long)hr);
    return SUCCEEDED(hr) ? 0 : 7;
}

static void finish(int result)
{
    if (C.done) return;
    C.done = 1;
    C.result = C.n ? write_previews() : result;
    PostQuitMessage(0);
}

static void on_grab(HWND h, IStream *st)
{
    double now = time_ms();
    if (st && C.n < GIF_FRAMES) {
        LARGE_INTEGER zero = { 0 };
        IStream_Seek(st, zero, STREAM_SEEK_SET, NULL);
        HRESULT hr = S_OK;
        HBITMAP bmp = image_decode_cover(NULL, st, GIF_W, GIF_H, &hr);
        DIBSECTION ds;
        if (bmp && GetObjectW(bmp, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBits) {
            uint8_t *dst = frame(C.n);
            memcpy(dst, ds.dsBm.bmBits, (size_t)GIF_W * GIF_H * 4);
            for (size_t i = 3; i < (size_t)GIF_W * GIF_H * 4; i += 4) dst[i] = 255;
            if (!C.n) C.start = now;
            C.at[C.n++] = now;
        }
        if (bmp) DeleteObject(bmp);
    }
    if (st) IStream_Release(st);
    if (C.n >= GIF_FRAMES) {
        finish(0);
        return;
    }
    if (!st && !C.n) {
        finish(5);
        return;
    }
    double due = C.start + (double)C.n * GRAB_MS;     /* keep a steady pace even when a grab is slow */
    SetTimer(h, T_NEXT, (UINT)SP_MAX(1.0, due - time_ms()), NULL);
}

static LRESULT CALLBACK cap_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SP_RENDER_EVENT: {
        RenderEvent *ev = (RenderEvent *)lp;
        if (ev && ev->code == REV_READY) {
            SetTimer(h, T_WARMUP, WARMUP_MS, NULL);
        } else if (ev && ev->code == REV_FAILED) {
            LOGW(L"preview: the page failed: %ls", ev->msg);
            finish(4);
        }
        free(ev);
        return 0;
    }
    case WM_SP_HTML_GRAB:
        on_grab(h, (IStream *)lp);
        return 0;
    case WM_TIMER:
        if (wp == T_WARMUP || wp == T_NEXT) {
            KillTimer(h, wp);
            if (!html_grab(C.r)) finish(5);
        } else if (wp == T_DEADLINE) {
            finish(6);
        } else if (C.r && C.r->vt->on_timer) {
            C.r->vt->on_timer(C.r, wp);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int preview_capture_main(const wchar_t *id)
{
    log_init(L"capture");
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Config cfg;
    memset(&cfg, 0, sizeof cfg);
    config_load(&cfg, paths_config());
    WallpaperEntry wp;
    const WallpaperEntry *e = config_find(&cfg, id);
    int ok = e && e->type == WP_HTML;
    if (ok) wp = *e;
    config_free(&cfg);
    if (!ok) {
        LOGW(L"preview: %ls is not an HTML wallpaper in the library", id);
        return 2;
    }
    preview_paths(id, C.gif, C.jpg, SP_MAX_PATH);
    wchar_t dir[SP_MAX_PATH], udf[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), C.gif);
    SHCreateDirectoryExW(NULL, dir, NULL);
    path_join(udf, SP_COUNTOF(udf), paths_cache(), L"WebView2Capture");
    html_capture_mode(udf);
    C.frames = (uint8_t *)malloc((size_t)GIF_FRAMES * GIF_W * GIF_H * 4);
    C.result = 3;
    LOGI(L"preview: recording '%ls'", wp.name);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = cap_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ScreenPlus.PreviewCapture";
    RegisterClassExW(&wc);
    /* Entirely outside the desktop: never seen, never focused. */
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - CAP_W - 64, y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Screen+ preview", WS_POPUP,
                                x, y, CAP_W, CAP_H, NULL, NULL, wc.hInstance, NULL);
    if (hwnd && C.frames) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        RendererContext ctx = { 0 };
        ctx.hwnd = hwnd;
        ctx.notify = hwnd;
        ctx.rid = 1;
        ctx.wp = &wp;
        ctx.width = CAP_W;
        ctx.height = CAP_H;
        ctx.dpi = 96;
        ctx.refreshHz = 60;
        ctx.audio = FALSE;          /* muted */
        ctx.rasterScale = 0.5;
        wchar_t err[SP_ERR_LEN] = L"";
        C.r = html_renderer_create(&ctx, err, SP_COUNTOF(err));
        if (C.r) {
            SetTimer(hwnd, T_DEADLINE, DEADLINE_MS, NULL);
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            C.r->vt->destroy(C.r);
        } else {
            LOGW(L"preview: %ls", err);
        }
    }
    if (hwnd) DestroyWindow(hwnd);
    html_shutdown();
    free(C.frames);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return C.result;
}
