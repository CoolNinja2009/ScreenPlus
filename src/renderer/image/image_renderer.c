/* Static image wallpapers: WIC decode + EXIF orientation + cover scaling on a thread-pool
 * worker, then a single GDI BitBlt per WM_PAINT. No continuous rendering at all. */
#include "../renderer.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void renderer_post(HWND notify, uint32_t rid, int code, HRESULT hr, const wchar_t *fmt, ...)
{
    RenderEvent *ev = (RenderEvent *)calloc(1, sizeof *ev);
    if (!ev) return;
    ev->rid = rid;
    ev->code = code;
    ev->hr = hr;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(ev->msg, SP_COUNTOF(ev->msg), _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    if (!notify || !PostMessageW(notify, WM_SP_RENDER_EVENT, 0, (LPARAM)ev)) free(ev);
}

/* ------------------------------------------------------------------ WIC decode */

static UINT exif_orientation(IWICBitmapFrameDecode *frame)
{
    IWICMetadataQueryReader *q = NULL;
    UINT o = 1;
    if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q)) || !q) return 1;
    static const wchar_t *keys[] = { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" };
    for (size_t i = 0; i < SP_COUNTOF(keys); i++) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, keys[i], &pv))) {
            if (pv.vt == VT_UI2) o = pv.uiVal;
            PropVariantClear(&pv);
            break;
        }
    }
    IWICMetadataQueryReader_Release(q);
    return (o >= 1 && o <= 8) ? o : 1;
}

static WICBitmapTransformOptions orientation_transform(UINT o)
{
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

HBITMAP image_decode_cover(const wchar_t *path, IStream *stream, int w, int h, HRESULT *hrOut)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapFlipRotator *rot = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapSource *src = NULL;
    HBITMAP bmp = NULL;
    HRESULT hr;
    if (w <= 0 || h <= 0) { hr = E_INVALIDARG; goto done; }

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (FAILED(hr)) goto done;
    if (stream)
        hr = IWICImagingFactory_CreateDecoderFromStream(f, stream, NULL, WICDecodeMetadataCacheOnDemand, &dec);
    else
        hr = IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (FAILED(hr)) goto done;

    UINT sw = 0, sh = 0;
    hr = IWICBitmapFrameDecode_GetSize(frame, &sw, &sh);
    if (FAILED(hr) || !sw || !sh) { if (SUCCEEDED(hr)) hr = E_FAIL; goto done; }

    src = (IWICBitmapSource *)frame;
    IWICBitmapSource_AddRef(src);
    UINT orient = exif_orientation(frame);
    if (orient != 1) {
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFlipRotator(f, &rot)) &&
            SUCCEEDED(IWICBitmapFlipRotator_Initialize(rot, src, orientation_transform(orient)))) {
            IWICBitmapSource_Release(src);
            src = (IWICBitmapSource *)rot;
            IWICBitmapSource_AddRef(src);
            if (orient >= 5) { UINT t = sw; sw = sh; sh = t; }
        }
    }

    /* scale to cover the target, then center-crop */
    double scale = SP_MAX((double)w / sw, (double)h / sh);
    UINT dw = (UINT)SP_MAX((double)w, ceil(sw * scale));
    UINT dh = (UINT)SP_MAX((double)h, ceil(sh * scale));
    hr = IWICImagingFactory_CreateBitmapScaler(f, &scaler);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapScaler_Initialize(scaler, src, dw, dh, WICBitmapInterpolationModeHighQualityCubic);
    if (FAILED(hr)) hr = IWICBitmapScaler_Initialize(scaler, src, dw, dh, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateFormatConverter(f, &conv);
    if (FAILED(hr)) goto done;
    hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)scaler, &GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { hr = E_OUTOFMEMORY; goto done; }
    WICRect rc = { (INT)((dw - (UINT)w) / 2), (INT)((dh - (UINT)h) / 2), w, h };
    hr = IWICFormatConverter_CopyPixels(conv, &rc, (UINT)w * 4, (UINT)w * 4 * (UINT)h, (BYTE *)bits);
    if (FAILED(hr)) { DeleteObject(bmp); bmp = NULL; }

done:
    SP_RELEASE(conv);
    SP_RELEASE(scaler);
    SP_RELEASE(src);
    SP_RELEASE(rot);
    SP_RELEASE(frame);
    SP_RELEASE(dec);
    SP_RELEASE(f);
    if (hrOut) *hrOut = hr;
    return bmp;
}

/* ------------------------------------------------------------------ renderer
 * Decode on a thread-pool worker straight into an immutable GPU texture, present it once with a
 * flip-model swap chain (the only path composed on the 24H2+ layered desktop host), then drop
 * both the CPU and GPU copies: DWM keeps showing the presented frame. GDI is only a fallback
 * when Direct3D is unavailable. */

typedef struct ImageRenderer {
    Renderer         base;
    volatile LONG    refs;
    volatile LONG    generation;
    SRWLOCK          lock;
    HWND             hwnd;
    HWND             notify;
    wchar_t          path[SP_MAX_PATH];
    int              w, h;
    /* guarded by lock */
    ID3D11Texture2D *tex;          /* pending upload, released after presenting */
    int              texW, texH;
    HBITMAP          bmp;          /* GDI fallback only */
    int              bmpW, bmpH;
    /* engine thread */
    IDXGISwapChain1 *swap;
    int              readyPosted;
    int              presented;
} ImageRenderer;

typedef struct DecodeJob {
    ImageRenderer *r;
    LONG           generation;
    int            w, h;
} DecodeJob;

static void img_release(ImageRenderer *r)
{
    if (InterlockedDecrement(&r->refs) == 0) {
        SP_RELEASE(r->tex);
        if (r->bmp) DeleteObject(r->bmp);
        free(r);
    }
}

static void CALLBACK decode_work(PTP_CALLBACK_INSTANCE inst, void *ctx)
{
    (void)inst;
    DecodeJob *job = (DecodeJob *)ctx;
    ImageRenderer *r = job->r;
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    double t0 = time_ms();
    HRESULT hr = S_OK;
    HBITMAP bmp = image_decode_cover(r->path, NULL, job->w, job->h, &hr);
    if (SUCCEEDED(hrInit)) CoUninitialize();

    ID3D11Texture2D *tex = NULL;
    if (bmp && job->generation == r->generation) {
        DIBSECTION ds;
        wchar_t err[SP_ERR_LEN];
        if (GetObjectW(bmp, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBits &&
            gfx_texture_from_bgra(ds.dsBm.bmBits, job->w, job->h, &tex, err, SP_COUNTOF(err))) {
            DeleteObject(bmp);                    /* GPU copy made: no CPU copy retained */
            bmp = NULL;
        } else {
            LOGW(L"image: Direct3D unavailable, using GDI (not visible on the Windows 11 24H2+ desktop host)");
        }
    }
    if (job->generation != r->generation) {           /* superseded or destroyed */
        if (bmp) DeleteObject(bmp);
        SP_RELEASE(tex);
    } else if (!bmp && !tex) {
        renderer_post(r->notify, r->base.rid, REV_FAILED, hr,
                      hr == WINCODEC_ERR_COMPONENTNOTFOUND ? L"No image decoder is installed for this format (0x%08lX)"
                      : hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
                          ? L"Image file not found (0x%08lX)" : L"Could not decode image (0x%08lX)",
                      (unsigned long)hr);
    } else {
        AcquireSRWLockExclusive(&r->lock);
        SP_RELEASE(r->tex);
        if (r->bmp) DeleteObject(r->bmp);
        r->tex = tex;
        r->texW = r->bmpW = job->w;
        r->texH = r->bmpH = job->h;
        r->bmp = bmp;
        ReleaseSRWLockExclusive(&r->lock);
        LOGI(L"image: decoded %ls at %dx%d in %.1f ms (%ls)", path_filename(r->path), job->w, job->h, time_ms() - t0,
             tex ? L"Direct3D" : L"GDI");
        InvalidateRect(r->hwnd, NULL, FALSE);   /* presented from WM_PAINT once the window is visible */
        if (!r->readyPosted) {
            r->readyPosted = 1;
            renderer_post(r->notify, r->base.rid, REV_READY, S_OK, NULL);
        }
    }
    free(job);
    img_release(r);
}

static void schedule_decode(ImageRenderer *r)
{
    DecodeJob *job = (DecodeJob *)calloc(1, sizeof *job);
    if (!job) return;
    job->r = r;
    job->generation = InterlockedIncrement(&r->generation);
    job->w = r->w;
    job->h = r->h;
    InterlockedIncrement(&r->refs);
    if (!TrySubmitThreadpoolCallback(decode_work, job, NULL)) {
        free(job);
        img_release(r);
        renderer_post(r->notify, r->base.rid, REV_FAILED, E_FAIL, L"Could not schedule image decoding");
    }
}

static void img_set_state(Renderer *base, RenderState st, BOOL keepFrame) { (void)base; (void)st; (void)keepFrame; }
static void img_set_audio(Renderer *base, BOOL on) { (void)base; (void)on; }

static void img_resize(Renderer *base, int w, int h)
{
    ImageRenderer *r = (ImageRenderer *)base;
    if (w == r->w && h == r->h) return;
    r->w = w;
    r->h = h;
    schedule_decode(r);
}

/* Called from WM_PAINT on the engine thread (only delivered while the window is visible). */
static BOOL img_paint(Renderer *base, HDC dc, const RECT *rc)
{
    ImageRenderer *r = (ImageRenderer *)base;
    AcquireSRWLockExclusive(&r->lock);
    ID3D11Texture2D *tex = r->tex;
    int tw = r->texW, th = r->texH;
    r->tex = NULL;
    ReleaseSRWLockExclusive(&r->lock);
    if (tex) {
        HRESULT hr = gfx_present(r->hwnd, &r->swap, tex, tw, th);
        ID3D11Texture2D_Release(tex);
        if (FAILED(hr)) {
            renderer_post(r->notify, r->base.rid, REV_FAILED, hr, L"Could not present the image (0x%08lX)", (unsigned long)hr);
            return TRUE;
        }
        r->presented = 1;
        return TRUE;
    }
    if (r->swap) return TRUE;   /* already on screen via the swap chain */

    BOOL drawn = FALSE;          /* GDI fallback */
    AcquireSRWLockShared(&r->lock);
    if (r->bmp) {
        HDC mem = CreateCompatibleDC(dc);
        if (mem) {
            HGDIOBJ old = SelectObject(mem, r->bmp);
            int cw = rc->right - rc->left, ch = rc->bottom - rc->top;
            if (cw == r->bmpW && ch == r->bmpH) {
                BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            } else {
                SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, 0, 0, cw, ch, mem, 0, 0, r->bmpW, r->bmpH, SRCCOPY);
            }
            SelectObject(mem, old);
            DeleteDC(mem);
            drawn = TRUE;
        }
    }
    ReleaseSRWLockShared(&r->lock);
    return drawn;
}

static void img_describe(Renderer *base, wchar_t *buf, size_t cap)
{
    ImageRenderer *r = (ImageRenderer *)base;
    str_printf(buf, cap, L"WIC image · %dx%d · %ls", r->w, r->h, r->swap ? L"Direct3D 11 flip swap chain" : L"GDI");
}

static void img_destroy(Renderer *base)
{
    ImageRenderer *r = (ImageRenderer *)base;
    InterlockedIncrement(&r->generation);   /* cancel pending results */
    gfx_release(&r->swap);
    img_release(r);
}

static const RendererVtbl k_imageVtbl = { img_set_state, img_resize, img_set_audio, img_paint, img_describe, img_destroy, NULL, NULL, NULL, NULL };

Renderer *image_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap)
{
    if (!file_exists(ctx->wp->path)) {
        str_copy(err, errCap, L"Image file not found");
        return NULL;
    }
    ImageRenderer *r = (ImageRenderer *)calloc(1, sizeof *r);
    if (!r) {
        str_copy(err, errCap, L"Out of memory");
        return NULL;
    }
    r->base.vt = &k_imageVtbl;
    r->base.rid = ctx->rid;
    r->base.type = WP_IMAGE;
    r->refs = 1;
    InitializeSRWLock(&r->lock);
    r->hwnd = ctx->hwnd;
    r->notify = ctx->notify;
    r->w = ctx->width;
    r->h = ctx->height;
    str_copy(r->path, SP_COUNTOF(r->path), ctx->wp->path);
    schedule_decode(r);
    return &r->base;
}
