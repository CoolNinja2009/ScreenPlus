/* Depth estimation child process: ScreenPlus.exe --depth <image> <dir> [--cpu] [--png]
 * Runs Depth Anything V2 Small (unquantized FP32 ONNX) through ONNX Runtime on one picture and writes
 * <dir>\depth.png, an 8-bit map at the model's own resolution where 255 is nearest (the wallpaper page
 * samples it with linear filtering). The GPU is used through DirectML (the best adapter), the CPU when
 * there is none or DirectML fails. --png also saves the picture as <dir>\image.png, for formats a web page
 * can't show. Exit code 0: done; 2: picture unreadable; 3: ONNX Runtime missing; 4: inference failed.
 *
 * It is its own process so that a GPU driver fault cannot take the tray app down, and so that the
 * model's memory goes back to Windows when it exits. Pre/post-processing follows the Python reference
 * this was modelled on: ImageNet normalisation, min-max normalisation, a foreground dilation (outlines
 * move with the thing they surround) and a blur (no tearing at depth edges). */
#include "app.h"
#include "../renderer/renderer.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include <onnxruntime_c_api.h>
#include <wincodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_SHORT 518     /* the size the model was trained at: the picture's shorter side */
#define MODEL_LONG  1036    /* longest side allowed (74 patches of 14): ultrawide pictures stay quick */
#define DILATE_PX   3       /* the reference's 5 px dilation and 4 px blur are for a full-size picture; */
#define BLUR_PX     2       /* these are the same amounts at the model's resolution */

static const OrtApi *g_o;

#define ORT(call) do { OrtStatus *st_ = (call); if (st_) { status_failed(st_, __LINE__); goto done; } } while (0)

static void status_failed(OrtStatus *st, int line)
{
    LOGE(L"depth: ONNX Runtime error at line %d: %hs", line, g_o->GetErrorMessage(st));
    g_o->ReleaseStatus(st);
}

/* ---------------------------------------------------------------- picture in / out (WIC) */

/* The picture upright (EXIF orientation applied); *w and *h are its upright size. NULL: unreadable. */
static IWICBitmapSource *open_upright(IWICImagingFactory *f, const wchar_t *path, UINT *w, UINT *h)
{
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmap *mem = NULL;
    IWICBitmapFlipRotator *rot = NULL;
    IWICBitmapSource *src = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, w, h)) && *w && *h) {
        src = (IWICBitmapSource *)frame;
        IWICBitmapSource_AddRef(src);
        UINT o = image_exif_orientation(frame);
        /* a JPEG can only be read top to bottom: rotating straight from the decoder decodes it again and again
         * (seconds), so a rotated picture is decoded into memory once first */
        if (o != 1 && SUCCEEDED(IWICImagingFactory_CreateBitmapFromSource(f, src, WICBitmapCacheOnLoad, &mem)) &&
            SUCCEEDED(IWICImagingFactory_CreateBitmapFlipRotator(f, &rot)) &&
            SUCCEEDED(IWICBitmapFlipRotator_Initialize(rot, (IWICBitmapSource *)mem, (WICBitmapTransformOptions)image_orientation_transform(o)))) {
            IWICBitmapSource_Release(src);
            src = (IWICBitmapSource *)rot;
            IWICBitmapSource_AddRef(src);
            if (o >= 5) { UINT t = *w; *w = *h; *h = t; }
        }
    }
    SP_RELEASE(rot);
    SP_RELEASE(mem);
    SP_RELEASE(frame);
    SP_RELEASE(dec);
    return src;
}

/* The model's input size: shorter side 518, longer side at most 1036, both multiples of 14. */
static void model_size(UINT w, UINT h, int *mw, int *mh)
{
    double s = (double)MODEL_SHORT / SP_MIN(w, h), lw = w * s, lh = h * s;
    double cap = (double)MODEL_LONG / SP_MAX(lw, lh);
    if (cap < 1.0) { lw *= cap; lh *= cap; }
    *mw = SP_MAX(14, (int)(lw / 14.0 + 0.5) * 14);
    *mh = SP_MAX(14, (int)(lh / 14.0 + 0.5) * 14);
}

/* w x h RGB bytes of the picture, stretched to the model size: the depth map then lines up with the
 * whole picture in UV space whatever the rounding to 14 px did to the aspect ratio. */
static uint8_t *scaled_rgb(IWICImagingFactory *f, IWICBitmapSource *src, int w, int h)
{
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 3);
    HRESULT hr = px ? IWICImagingFactory_CreateBitmapScaler(f, &sc) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        hr = IWICBitmapScaler_Initialize(sc, src, (UINT)w, (UINT)h, WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr)) hr = IWICBitmapScaler_Initialize(sc, src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant);
    }
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &cv);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, NULL,
                                            0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(cv, NULL, (UINT)w * 3, (UINT)w * 3 * (UINT)h, px);
    SP_RELEASE(cv);
    SP_RELEASE(sc);
    if (FAILED(hr)) { free(px); px = NULL; }
    return px;
}

/* PNG from `src` (a WIC source) or, when that is NULL, from `px`. */
static BOOL save_png(IWICImagingFactory *f, const wchar_t *path, IWICBitmapSource *src, UINT w, UINT h, const GUID *fmt, const uint8_t *px)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *pb = NULL;
    GUID pf = *fmt;
    HRESULT hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, &pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fe, pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, w, h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (SUCCEEDED(hr)) hr = src ? IWICBitmapFrameEncode_WriteSource(fe, src, NULL) : IWICBitmapFrameEncode_WritePixels(fe, h, w, w * h, (BYTE *)px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    SP_RELEASE(pb);
    SP_RELEASE(fe);
    SP_RELEASE(enc);
    SP_RELEASE(st);                                   /* closes the file */
    if (FAILED(hr)) LOGE(L"depth: could not write %ls (0x%08lx)", path, (unsigned long)hr);
    return SUCCEEDED(hr);
}

/* The picture as a PNG a web page can show (formats such as HEIC or TIFF). */
static BOOL save_picture_copy(IWICImagingFactory *f, IWICBitmapSource *src, UINT w, UINT h, const wchar_t *path)
{
    IWICFormatConverter *cv = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, src, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeCustom)))
        ok = save_png(f, path, (IWICBitmapSource *)cv, w, h, &GUID_WICPixelFormat32bppBGRA, NULL);
    SP_RELEASE(cv);
    return ok;
}

/* ---------------------------------------------------------------- pre/post-processing */

static float *to_tensor(const uint8_t *rgb, int w, int h)
{
    static const float mean[3] = { 0.485f, 0.456f, 0.406f }, inv[3] = { 1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f };
    float lut[3][256];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) lut[c][v] = ((float)v / 255.0f - mean[c]) * inv[c];
    size_t n = (size_t)w * h;
    float *t = (float *)malloc(n * 3 * sizeof(float));
    if (!t) return NULL;
    for (size_t i = 0; i < n; i++) {
        t[i] = lut[0][rgb[i * 3]];
        t[n + i] = lut[1][rgb[i * 3 + 1]];
        t[2 * n + i] = lut[2][rgb[i * 3 + 2]];
    }
    return t;
}

/* Grey-scale dilation with a disc: the nearest thing wins over a radius of r pixels. */
static void dilate(uint8_t *px, int w, int h, int r)
{
    uint8_t *src = (uint8_t *)malloc((size_t)w * h);
    int ext[16];
    if (!src || r <= 0 || r >= 16) { free(src); return; }
    memcpy(src, px, (size_t)w * h);
    for (int d = 0; d <= r; d++) ext[d] = (int)(sqrt((double)(r * r - d * d)) + 0.5);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int dy = -r; dy <= r; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= h) continue;
                int e = ext[dy < 0 ? -dy : dy], x1 = SP_MIN(x + e, w - 1);
                const uint8_t *row = src + (size_t)yy * w;
                for (int xx = SP_MAX(x - e, 0); xx <= x1; xx++) if (row[xx] > m) m = row[xx];
            }
            px[(size_t)y * w + x] = m;
        }
    }
    free(src);
}

/* Separable Gaussian blur with OpenCV's sigma for a (2r+1) kernel, so the result matches the reference. */
static void blur(uint8_t *px, int w, int h, int r)
{
    float *tmp = (float *)malloc((size_t)w * h * sizeof(float)), k[16], sum = 0.0f, sigma = 0.3f * (float)(r - 1) + 0.8f;
    if (!tmp || r <= 0 || r >= 8) { free(tmp); return; }
    for (int i = -r; i <= r; i++) sum += k[i + r] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
    for (int i = 0; i <= 2 * r; i++) k[i] /= sum;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = 0.0f;
            for (int i = -r; i <= r; i++) a += k[i + r] * px[(size_t)y * w + SP_CLAMP(x + i, 0, w - 1)];
            tmp[(size_t)y * w + x] = a;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = 0.0f;
            for (int i = -r; i <= r; i++) a += k[i + r] * tmp[(size_t)SP_CLAMP(y + i, 0, h - 1) * w + x];
            px[(size_t)y * w + x] = (uint8_t)SP_CLAMP((int)(a + 0.5f), 0, 255);
        }
    }
    free(tmp);
}

/* ---------------------------------------------------------------- ONNX Runtime */

/* The first six slots of OrtDmlApi (dml_provider_factory.h, which is C++-only in practice). */
typedef struct DmlApi {
    void *slots[5];
    OrtStatus *(ORT_API_CALL *AppendWithOptions)(OrtSessionOptions *, int *preferenceAndFilter);
} DmlApi;
typedef OrtStatus *(ORT_API_CALL *DmlAppendLegacy)(OrtSessionOptions *, int deviceId);

/* DirectML on the highest-performance GPU (not the first adapter, which may be the integrated one). */
static BOOL append_dml(OrtSessionOptions *so, HMODULE dll)
{
    const void *api = NULL;
    OrtStatus *st = g_o->GetExecutionProviderApi("DML", ORT_API_VERSION, &api);
    if (!st && api) {
        int opts[2] = { 1 /* high performance */, 1 /* GPU */ };
        st = ((const DmlApi *)api)->AppendWithOptions(so, opts);
        if (!st) return TRUE;
    }
    if (st) {
        LOGW(L"depth: DirectML adapter selection failed: %hs", g_o->GetErrorMessage(st));
        g_o->ReleaseStatus(st);
    }
    DmlAppendLegacy legacy = (DmlAppendLegacy)GetProcAddress(dll, "OrtSessionOptionsAppendExecutionProvider_DML");
    if (!legacy) return FALSE;
    st = legacy(so, 0);
    if (!st) return TRUE;
    LOGW(L"depth: DirectML is unavailable: %hs", g_o->GetErrorMessage(st));
    g_o->ReleaseStatus(st);
    return FALSE;
}

/* One inference: w*h floats of depth (larger = nearer) into out. gpu: DirectML, else the CPU threads. */
static BOOL run_model(HMODULE dll, int gpu, const wchar_t *model, float *chw, int w, int h, float *out)
{
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtSession *sess = NULL;
    OrtMemoryInfo *mi = NULL;
    OrtValue *in = NULL, *res = NULL;
    OrtTensorTypeAndShapeInfo *info = NULL;
    BOOL ok = FALSE;
    double t0 = time_ms();
    ORT(g_o->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "screenplus", &env));
    ORT(g_o->CreateSessionOptions(&so));
    ORT(g_o->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
    /* fixed shapes: DirectML compiles the whole graph, the CPU provider folds the shape arithmetic */
    ORT(g_o->AddFreeDimensionOverrideByName(so, "batch_size", 1));
    ORT(g_o->AddFreeDimensionOverrideByName(so, "height", h));
    ORT(g_o->AddFreeDimensionOverrideByName(so, "width", w));
    if (gpu) {
        ORT(g_o->DisableMemPattern(so));
        ORT(g_o->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        if (!append_dml(so, dll)) goto done;
    }
    ORT(g_o->CreateSession(env, model, so, &sess));
    double t1 = time_ms();
    int64_t shape[4] = { 1, 3, h, w };
    ORT(g_o->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi));
    ORT(g_o->CreateTensorWithDataAsOrtValue(mi, chw, (size_t)w * h * 3 * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
    const char *inName[] = { "pixel_values" }, *outName[] = { "predicted_depth" };
    ORT(g_o->Run(sess, NULL, inName, (const OrtValue *const *)&in, 1, outName, 1, &res));
    float *data = NULL;
    int64_t dims[4] = { 0 };
    size_t nd = 0;
    ORT(g_o->GetTensorMutableData(res, (void **)&data));
    ORT(g_o->GetTensorTypeAndShape(res, &info));
    ORT(g_o->GetDimensionsCount(info, &nd));
    if (nd == 3) ORT(g_o->GetDimensions(info, dims, nd));
    if (nd != 3 || dims[1] != h || dims[2] != w) {
        LOGE(L"depth: unexpected output shape (%d dimensions, %lld x %lld)", (int)nd, nd == 3 ? (long long)dims[1] : 0LL, nd == 3 ? (long long)dims[2] : 0LL);
        goto done;
    }
    memcpy(out, data, (size_t)w * h * sizeof(float));
    ok = TRUE;
    LOGI(L"depth: %ls inference %dx%d: session %.0f ms, run %.0f ms", gpu ? L"DirectML" : L"CPU", w, h, t1 - t0, time_ms() - t1);
done:
    if (info) g_o->ReleaseTensorTypeAndShapeInfo(info);
    if (res) g_o->ReleaseValue(res);
    if (in) g_o->ReleaseValue(in);
    if (mi) g_o->ReleaseMemoryInfo(mi);
    if (sess) g_o->ReleaseSession(sess);
    if (so) g_o->ReleaseSessionOptions(so);
    if (env) g_o->ReleaseEnv(env);
    return ok;
}

static HMODULE load_runtime(void)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), paths_root(), L"depth\\ort\\onnxruntime.dll");
    HMODULE m = LoadLibraryExW(p, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        LOGE(L"depth: could not load %ls (error %lu); the Microsoft Visual C++ Redistributable (x64) may be missing", p, GetLastError());
        return NULL;
    }
    typedef const OrtApiBase *(ORT_API_CALL *GetBase)(void);
    GetBase base = (GetBase)GetProcAddress(m, "OrtGetApiBase");
    g_o = base ? base()->GetApi(ORT_API_VERSION) : NULL;
    if (!g_o) {
        LOGE(L"depth: %ls does not provide ONNX Runtime API %d", p, ORT_API_VERSION);
        FreeLibrary(m);
        return NULL;
    }
    return m;
}

/* ---------------------------------------------------------------- entry point */

int depth_infer_main(const wchar_t *image, const wchar_t *dir, int cpuOnly, int wantPng)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapSource *src = NULL;
    uint8_t *rgb = NULL, *u8 = NULL;
    float *chw = NULL, *depth = NULL;
    HMODULE dll = NULL;
    wchar_t model[SP_MAX_PATH], tmp[SP_MAX_PATH], png[SP_MAX_PATH];
    UINT sw = 0, sh = 0;
    int w = 0, h = 0, rc = 2;
    double t0 = time_ms();
    log_init(L"depth");
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    LOGI(L"depth: %ls -> %ls (%ls)", image, dir, cpuOnly ? L"CPU only" : L"GPU first, CPU as fallback");

    path_join(model, SP_COUNTOF(model), paths_root(), L"depth\\model.onnx");
    path_join(tmp, SP_COUNTOF(tmp), dir, L"depth.tmp");
    path_join(png, SP_COUNTOF(png), dir, L"depth.png");
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) ||
        !(src = open_upright(f, image, &sw, &sh))) {
        LOGE(L"depth: cannot read the picture");
        goto done;
    }
    model_size(sw, sh, &w, &h);
    if (!(rgb = scaled_rgb(f, src, w, h)) || !(chw = to_tensor(rgb, w, h)) || !(depth = (float *)malloc((size_t)w * h * sizeof(float)))) {
        LOGE(L"depth: out of memory");
        goto done;
    }
    LOGI(L"depth: picture %ux%u, model input %dx%d, prepared in %.0f ms", sw, sh, w, h, time_ms() - t0);

    rc = 3;
    if (!(dll = load_runtime())) goto done;
    rc = 4;
    if (!((!cpuOnly && run_model(dll, 1, model, chw, w, h, depth)) || run_model(dll, 0, model, chw, w, h, depth))) goto done;

    float lo = depth[0], hi = depth[0];
    size_t n = (size_t)w * h;
    for (size_t i = 1; i < n; i++) {
        if (depth[i] < lo) lo = depth[i];
        if (depth[i] > hi) hi = depth[i];
    }
    if (!(u8 = (uint8_t *)malloc(n))) goto done;
    float scale = 255.0f / (hi - lo + 1e-8f);
    for (size_t i = 0; i < n; i++) u8[i] = (uint8_t)SP_CLAMP((int)((depth[i] - lo) * scale), 0, 255);
    dilate(u8, w, h, DILATE_PX);
    blur(u8, w, h, BLUR_PX);
    if (!save_png(f, tmp, NULL, (UINT)w, (UINT)h, &GUID_WICPixelFormat8bppGray, u8) || !MoveFileExW(tmp, png, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        goto done;
    }
    if (wantPng) {
        path_join(tmp, SP_COUNTOF(tmp), dir, L"image.png");
        if (!save_picture_copy(f, src, sw, sh, tmp)) goto done;
    }
    rc = 0;
    LOGI(L"depth: finished in %.0f ms", time_ms() - t0);
done:
    free(u8);
    free(depth);
    free(chw);
    free(rgb);
    SP_RELEASE(src);
    SP_RELEASE(f);
    if (rc) LOGE(L"depth: failed (exit code %d)", rc);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    log_shutdown();
    return rc;       /* the DLL stays loaded: the process ends now */
}
