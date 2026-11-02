/* Depth estimation child process:
 *   ScreenPlus.exe --depth <image|video> <dir> [--model <key>] [--video] [--notify <hwnd>] [--cpu | --bench | --crashed] [--png]
 * Runs a depth model (depth.c's k_models: Depth Anything 2 or 3, unquantized FP32 ONNX) through ONNX Runtime.
 * A picture gives <dir>\depth[-<key>].png, an 8-bit map at the model's own resolution where 255 is nearest (the
 * wallpaper page samples it with linear filtering); --png also saves the picture as <dir>\image.png, for formats
 * a web page can't show. --video runs every frame the page will need (DEPTH_VIDEO_FPS at most) and writes
 * <dir>\depth[-<key>].bin: small grey JPEGs back to back, then an index (see write_video), posting
 * WM_SP_DEPTH (2, done | total << 16) to --notify as it goes. Exit code 0: done; 2: picture or video
 * unreadable; 3: ONNX Runtime missing; 4: inference failed; 5: a video codec the web page can't play.
 *
 * Which processor: the GPU (DirectML, the highest-performance adapter) is not always the quicker way to get
 * one picture done: DirectML compiles the whole model at every start (about 2 s) where a fast CPU needs about
 * 1 s in all. So the first run times both on the real picture and keeps the faster one in the model's
 * backend.json; later runs use it, and fall back to the CPU if a recorded GPU stops working. The test is
 * redone when ONNX Runtime or the graphics adapters change, or with --bench. --cpu runs on the CPU and records
 * nothing; --crashed is the parent's retry after this process died (almost surely the GPU path): it runs on the
 * CPU and records the CPU as the choice. A video compiles once and runs hundreds of frames, so it takes the GPU
 * unless the record says its runs are slower there.
 *
 * Depth Anything 3 takes a batch of views ([1,N,3,H,W]) and returns metric depth (far = larger): it gets one
 * view and its depth is turned into inverse depth, the scale Depth Anything 2 returns. Video frames are
 * normalised between percentiles that follow the video smoothly, so the depth does not pulse from frame to frame.
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
#include "../common/json.h"
#include "../common/webfetch.h"
#include <onnxruntime_c_api.h>
#include <wincodec.h>
#include <dxgi.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_LONG  1036    /* longest side allowed (74 patches of 14): ultrawide pictures stay quick */
#define VIDEO_SHORT 288     /* video depth frames: shorter side (the page filters them; they stay small) */
#define DILATE_PX   3       /* the reference's 5 px dilation and 4 px blur are for a full-size picture; */
#define BLUR_PX     2       /* these are the same amounts at the model's resolution */

static const OrtApi *g_o;
static const char   *g_ortVersion = "";

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

/* The model's input size: shorter side `shortSide` (its training size), longer side at most 1036, both
 * multiples of 14. */
static void model_size(UINT w, UINT h, int shortSide, int *mw, int *mh)
{
    double s = (double)shortSide / SP_MIN(w, h), lw = w * s, lh = h * s;
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

/* A loaded model: created once, run once for a picture or for every frame of a video. */
typedef struct Sess {
    OrtEnv            *env;
    OrtSessionOptions *so;
    OrtSession        *s;
    OrtMemoryInfo     *mi;
    int                w, h, v3, gpu;
} Sess;

static void sess_close(Sess *x)
{
    if (x->mi) g_o->ReleaseMemoryInfo(x->mi);
    if (x->s) g_o->ReleaseSession(x->s);
    if (x->so) g_o->ReleaseSessionOptions(x->so);
    if (x->env) g_o->ReleaseEnv(x->env);
    memset(x, 0, sizeof *x);
}

/* gpu: DirectML, else the CPU threads. v3: Depth Anything 3's batch-of-views input. */
static BOOL sess_open(Sess *x, HMODULE dll, int gpu, const wchar_t *model, int v3, int w, int h)
{
    BOOL ok = FALSE;
    memset(x, 0, sizeof *x);
    x->w = w;
    x->h = h;
    x->v3 = v3;
    x->gpu = gpu;
    ORT(g_o->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "screenplus", &x->env));
    ORT(g_o->CreateSessionOptions(&x->so));
    ORT(g_o->SetSessionGraphOptimizationLevel(x->so, ORT_ENABLE_ALL));
    /* fixed shapes: DirectML compiles the whole graph, the CPU provider folds the shape arithmetic */
    ORT(g_o->AddFreeDimensionOverrideByName(x->so, "batch_size", 1));
    ORT(g_o->AddFreeDimensionOverrideByName(x->so, "num_images", 1));
    ORT(g_o->AddFreeDimensionOverrideByName(x->so, "height", h));
    ORT(g_o->AddFreeDimensionOverrideByName(x->so, "width", w));
    if (gpu) {
        ORT(g_o->DisableMemPattern(x->so));
        ORT(g_o->SetSessionExecutionMode(x->so, ORT_SEQUENTIAL));
        if (!append_dml(x->so, dll)) goto done;
    }
    ORT(g_o->CreateSession(x->env, model, x->so, &x->s));
    ORT(g_o->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &x->mi));
    ok = TRUE;
done:
    if (!ok) sess_close(x);
    return ok;
}

/* One inference: w*h floats of depth into out, larger = nearer (Depth Anything 3's metric depth inverted). */
static BOOL sess_run(Sess *x, float *chw, float *out)
{
    OrtValue *in = NULL, *res = NULL;
    OrtTensorTypeAndShapeInfo *info = NULL;
    BOOL ok = FALSE;
    int w = x->w, h = x->h;
    int64_t shape[5] = { 1, 1, 3, h, w };
    ORT(g_o->CreateTensorWithDataAsOrtValue(x->mi, chw, (size_t)w * h * 3 * sizeof(float), x->v3 ? shape : shape + 1, x->v3 ? 5 : 4,
                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
    const char *inName[] = { "pixel_values" }, *outName[] = { "predicted_depth" };
    ORT(g_o->Run(x->s, NULL, inName, (const OrtValue *const *)&in, 1, outName, 1, &res));
    float *data = NULL;
    int64_t dims[4] = { 0 };
    size_t nd = 0;
    ORT(g_o->GetTensorMutableData(res, (void **)&data));
    ORT(g_o->GetTensorTypeAndShape(res, &info));
    ORT(g_o->GetDimensionsCount(info, &nd));
    if (nd == 3 || nd == 4) ORT(g_o->GetDimensions(info, dims, nd));
    if ((nd != 3 && nd != 4) || dims[nd - 2] != h || dims[nd - 1] != w) {
        LOGE(L"depth: unexpected output shape (%d dimensions, %lld x %lld)", (int)nd, nd >= 3 ? (long long)dims[nd - 2] : 0LL,
             nd >= 3 ? (long long)dims[nd - 1] : 0LL);
        goto done;
    }
    size_t n = (size_t)w * h;
    if (x->v3) for (size_t i = 0; i < n; i++) out[i] = 1.0f / SP_MAX(data[i], 1e-4f);
    else memcpy(out, data, n * sizeof(float));
    ok = TRUE;
done:
    if (info) g_o->ReleaseTensorTypeAndShapeInfo(info);
    if (res) g_o->ReleaseValue(res);
    if (in) g_o->ReleaseValue(in);
    return ok;
}

/* A picture: load, run, unload. ms (optional) receives what one job costs on this backend (loading plus the
 * run), runMs (optional) the run alone. */
static BOOL run_model(HMODULE dll, int gpu, const wchar_t *model, int v3, float *chw, int w, int h, float *out, double *ms, double *runMs)
{
    Sess x;
    double t0 = time_ms();
    if (!sess_open(&x, dll, gpu, model, v3, w, h)) return FALSE;
    double t1 = time_ms();
    BOOL ok = sess_run(&x, chw, out);
    double t2 = time_ms();
    sess_close(&x);
    if (!ok) return FALSE;
    if (ms) *ms = t2 - t0;
    if (runMs) *runMs = t2 - t1;
    LOGI(L"depth: %ls inference %dx%d: session %.0f ms, run %.0f ms", gpu ? L"DirectML" : L"CPU", w, h, t1 - t0, t2 - t1);
    return TRUE;
}

static HMODULE load_runtime(void)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), paths_root(), L"depth\\ort\\onnxruntime.dll");
    HMODULE m = LoadLibraryExW(p, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        wchar_t vc[40];
        BOOL vcOk = vcruntime_ok(vc, SP_COUNTOF(vc));
        LOGE(L"depth: could not load %ls (error %lu); Microsoft Visual C++ runtime: %ls%ls", p, GetLastError(), vc[0] ? vc : L"not installed",
             vcOk ? L"" : L" (14.44 or newer is needed: screenplus vcredist install)");
        return NULL;
    }
    typedef const OrtApiBase *(ORT_API_CALL *GetBase)(void);
    GetBase base = (GetBase)GetProcAddress(m, "OrtGetApiBase");
    g_o = base ? base()->GetApi(ORT_API_VERSION) : NULL;
    if (g_o) g_ortVersion = base()->GetVersionString();
    if (!g_o) {
        LOGE(L"depth: %ls does not provide ONNX Runtime API %d", p, ORT_API_VERSION);
        FreeLibrary(m);
        return NULL;
    }
    return m;
}

/* ---------------------------------------------------------------- GPU or CPU */

typedef struct Backend {
    int    gpu;             /* the faster one: 1 DirectML, 0 CPU */
    double gpuMs, cpuMs;    /* what the test measured for one job (0: that backend failed or was not tried) */
    double gpuRun, cpuRun;  /* ... of which the run itself (the rest is loading the model): depth_estimate's figures */
    char   ort[16];         /* the record is only valid for this ONNX Runtime ... */
    char   fp[17];          /* ... and these graphics adapters */
} Backend;

/* FNV-1a of the ONNX Runtime version and every hardware adapter (their hashes added, so the order of the
 * adapters doesn't matter): a new GPU or a new runtime means the test is worth doing again. */
static void adapter_fingerprint(char out[17])
{
    uint64_t sum = 0;
    IDXGIFactory1 *fac = NULL;
#define MIX(h, p, n) do { for (size_t i_ = 0; i_ < (n); i_++) (h) = ((h) ^ ((const uint8_t *)(p))[i_]) * 1099511628211ULL; } while (0)
    uint64_t base = 1469598103934665603ULL;
    MIX(base, g_ortVersion, strlen(g_ortVersion));
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) {
        IDXGIAdapter1 *ad = NULL;
        for (UINT i = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(fac, i, &ad)); i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                uint64_t h = 1469598103934665603ULL;
                MIX(h, &d.VendorId, sizeof d.VendorId);
                MIX(h, &d.DeviceId, sizeof d.DeviceId);
                MIX(h, &d.SubSysId, sizeof d.SubSysId);
                MIX(h, &d.Revision, sizeof d.Revision);
                MIX(h, d.Description, wcslen(d.Description) * sizeof(wchar_t));
                sum += h;
            }
            SP_RELEASE(ad);
        }
        SP_RELEASE(fac);
    }
#undef MIX
    snprintf(out, 17, "%016llx", (unsigned long long)(base ^ sum));
}

static int g_model;          /* k_models: each model keeps its own record in its folder */

static void backend_path(wchar_t *out, size_t cap)
{
    depth_model_dir(g_model, out, cap);
    path_join(out, cap, out, L"backend.json");
}

/* The record, if there is one made by this ONNX Runtime on these adapters. */
static BOOL read_backend(Backend *b, const char *fp)
{
    wchar_t p[SP_MAX_PATH];
    backend_path(p, SP_COUNTOF(p));
    JVal *j = json_parse_file(p);
    BOOL ok = FALSE;
    if (j) {
        ZeroMemory(b, sizeof *b);
        b->gpu = strcmp(json_str(json_get(j, "backend"), "cpu"), "gpu") == 0;
        b->gpuMs = json_num(json_get(j, "gpu_ms"), 0.0);
        b->cpuMs = json_num(json_get(j, "cpu_ms"), 0.0);
        b->gpuRun = json_num(json_get(j, "gpu_run_ms"), 0.0);
        b->cpuRun = json_num(json_get(j, "cpu_run_ms"), 0.0);
        strncpy_s(b->ort, sizeof b->ort, json_str(json_get(j, "ort"), ""), _TRUNCATE);
        strncpy_s(b->fp, sizeof b->fp, json_str(json_get(j, "fingerprint"), ""), _TRUNCATE);
        ok = strcmp(b->ort, g_ortVersion) == 0 && strcmp(b->fp, fp) == 0;
        json_free(j);
    }
    return ok;
}

static void write_backend(const Backend *b)
{
    char text[400];
    wchar_t p[SP_MAX_PATH];
    int n = snprintf(text, sizeof text, "{\"backend\":\"%s\",\"gpu_ms\":%.0f,\"cpu_ms\":%.0f,\"gpu_run_ms\":%.0f,\"cpu_run_ms\":%.0f,"
                     "\"ort\":\"%s\",\"fingerprint\":\"%s\"}\n", b->gpu ? "gpu" : "cpu", b->gpuMs, b->cpuMs, b->gpuRun, b->cpuRun, b->ort, b->fp);
    backend_path(p, SP_COUNTOF(p));
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0;
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, text, (DWORD)n, &w, NULL);
    CloseHandle(f);
}

/* Reads the model once so that neither backend pays for a cold disk in the test. */
static void warm_file(const wchar_t *path)
{
    static char buf[1 << 20];
    DWORD n = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    while (ReadFile(f, buf, sizeof buf, &n, NULL) && n) { }
    CloseHandle(f);
}

/* Times one job on the GPU and on the CPU (this very picture), records the faster and leaves its depth in out. */
static BOOL benchmark(HMODULE dll, const wchar_t *model, int v3, float *chw, int w, int h, float *out, Backend *rec)
{
    size_t n = (size_t)w * h;
    float *cpuOut = (float *)malloc(n * sizeof(float));
    if (!cpuOut) return FALSE;
    warm_file(model);
    rec->gpuMs = rec->cpuMs = rec->gpuRun = rec->cpuRun = 0.0;
    BOOL okGpu = run_model(dll, 1, model, v3, chw, w, h, out, &rec->gpuMs, &rec->gpuRun);
    BOOL okCpu = run_model(dll, 0, model, v3, chw, w, h, cpuOut, &rec->cpuMs, &rec->cpuRun);
    if (!okGpu) rec->gpuMs = rec->gpuRun = 0.0;
    if (!okCpu) rec->cpuMs = rec->cpuRun = 0.0;
    rec->gpu = okGpu && (!okCpu || rec->gpuMs <= rec->cpuMs);    /* only a quicker CPU wins: a tie keeps the work off the CPU */
    if (!rec->gpu && okCpu) memcpy(out, cpuOut, n * sizeof(float));
    free(cpuOut);
    if (okGpu || okCpu) write_backend(rec);
    LOGI(L"depth: test of both: GPU %ls, CPU %ls -> the %ls is used from now on", okGpu ? L"worked" : L"failed", okCpu ? L"worked" : L"failed",
         rec->gpu ? L"GPU" : L"CPU");
    LOGI(L"depth: one job costs %.0f ms on the GPU and %.0f ms on the CPU", rec->gpuMs, rec->cpuMs);
    return okGpu || okCpu;
}

/* ---------------------------------------------------------------- video */

/* lo and hi between which the depth spreads: the 0.5th and 99.5th percentile of a sample (a few stray pixels
 * then can't squeeze the rest of the frame into a narrow band). */
static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void percentiles(const float *d, size_t n, float *sample, size_t cap, float *lo, float *hi)
{
    size_t step = SP_MAX((size_t)1, n / cap), m = 0;
    for (size_t i = 0; i < n && m < cap; i += step) sample[m++] = d[i];
    qsort(sample, m, sizeof(float), cmp_float);
    *lo = sample[(size_t)(m * 0.005)];
    *hi = sample[SP_MIN(m - 1, (size_t)(m * 0.995))];
}

/* Area-average downscale of a w x h grey map to ow x oh. */
static void shrink(const uint8_t *src, int w, int h, uint8_t *dst, int ow, int oh)
{
    for (int y = 0; y < oh; y++) {
        int y0 = y * h / oh, y1 = SP_MAX(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; x++) {
            int x0 = x * w / ow, x1 = SP_MAX(x0 + 1, (x + 1) * w / ow), sum = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) sum += src[(size_t)yy * w + xx];
            dst[(size_t)y * ow + x] = (uint8_t)(sum / ((y1 - y0) * (x1 - x0)));
        }
    }
}

/* A grey JPEG of px into buf; the byte count, 0 on failure. */
static DWORD encode_jpeg(IWICImagingFactory *f, const uint8_t *px, int w, int h, uint8_t *buf, DWORD cap)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *pb = NULL;
    GUID pf = GUID_WICPixelFormat8bppGray;
    ULARGE_INTEGER pos = { 0 };
    LARGE_INTEGER zero = { 0 };
    HRESULT hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(st, buf, cap);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, &pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fe, pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, (UINT)w, (UINT)h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fe, (UINT)h, (UINT)w, (UINT)(w * h), (BYTE *)px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (SUCCEEDED(hr)) hr = IWICStream_Seek(st, zero, STREAM_SEEK_CUR, &pos);
    SP_RELEASE(pb);
    SP_RELEASE(fe);
    SP_RELEASE(enc);
    SP_RELEASE(st);
    return SUCCEEDED(hr) ? (DWORD)pos.QuadPart : 0;
}

static BOOL put(HANDLE f, const void *p, DWORD n)
{
    DWORD w = 0;
    return WriteFile(f, p, n, &w, NULL) && w == n;
}

typedef struct Notify { HWND wnd; double last; } Notify;

static void notify(Notify *nt, int done, int total)
{
    double now = time_ms();
    if (!nt->wnd || (now - nt->last < 250.0 && done < total)) return;
    nt->last = now;
    PostMessageW(nt->wnd, WM_SP_DEPTH, 2, (LPARAM)(SP_MIN(done, 0xffff) | (SP_MIN(total, 0xffff) << 16)));
}

/* Every frame the page needs: frame k is the depth at k / fps seconds. depth.bin is
 *   "SPDV" u32 1 | the JPEGs | index: u32 offset, u32 size per frame | u32 count, u32 w, u32 h, f32 fps, u32 index offset, "SPDV"
 * (little endian; a frame the video has no picture of repeats the one before). */
static int write_video(IWICImagingFactory *wic, HMODULE dll, const DepthModel *m, const wchar_t *model, const wchar_t *video,
                       const wchar_t *out, int mode, Notify *nt)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    struct SwsContext *sws = NULL;
    Sess x = { 0 };
    HANDLE f = INVALID_HANDLE_VALUE;
    uint8_t *rgb = NULL, *u8 = NULL, *small_ = NULL, *jpg = NULL;
    float *chw = NULL, *depth = NULL, *sample = NULL;
    uint32_t *index = NULL;
    wchar_t tmp[SP_MAX_PATH];
    int rc = 2, mw = 0, mh = 0, ow = 0, oh = 0, count = 0, total = 0;
    double t0 = time_ms();
    char *u = utf8_from_wide(video, NULL);
    str_printf(tmp, SP_COUNTOF(tmp), L"%ls.tmp", out);

    if (!u || !pkt || !fr || !video_ffmpeg_available(NULL, 0) || avformat_open_input(&fmt, u, NULL, NULL) < 0) goto done;
    avformat_find_stream_info(fmt, NULL);
    const AVCodec *codec = NULL;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vi < 0 || !codec) goto done;
    AVStream *st = fmt->streams[vi];
    enum AVCodecID id = st->codecpar->codec_id;
    if (id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC && id != AV_CODEC_ID_VP8 && id != AV_CODEC_ID_VP9 && id != AV_CODEC_ID_AV1) {
        LOGE(L"depth: the video's codec (%hs) does not play in WebView2", avcodec_get_name(id));
        rc = 5;
        goto done;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++) fmt->streams[i]->discard = (int)i == vi ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    if (!(dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(dec, st->codecpar) < 0) goto done;
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0;                                    /* as many as FFmpeg likes: decoding is not the slow part */
    if (avcodec_open2(dec, codec, NULL) < 0 || dec->width <= 0 || dec->height <= 0) goto done;

    double secs = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : st->duration > 0 ? st->duration * av_q2d(st->time_base) : 0.0;
    double srcFps = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? av_q2d(st->avg_frame_rate) : DEPTH_VIDEO_FPS;
    double fps = SP_MIN((double)DEPTH_VIDEO_FPS, srcFps);
    if (secs > 0 && secs * fps > DEPTH_VIDEO_FRAMES) fps = DEPTH_VIDEO_FRAMES / secs;     /* long videos: fewer per second */
    total = secs > 0 ? SP_MIN((int)ceil(secs * fps), DEPTH_VIDEO_FRAMES) : 0;
    model_size((UINT)dec->width, (UINT)dec->height, m->shortSide, &mw, &mh);
    double k = (double)SP_MIN(VIDEO_SHORT, SP_MIN(mw, mh)) / SP_MIN(mw, mh);
    ow = SP_MAX(16, (int)(mw * k + 0.5));
    oh = SP_MAX(16, (int)(mh * k + 0.5));
    DWORD jcap = (DWORD)(ow * oh) + 65536;
    size_t n = (size_t)mw * mh;
    rgb = (uint8_t *)malloc(n * 3);
    u8 = (uint8_t *)malloc(n);
    small_ = (uint8_t *)malloc((size_t)ow * oh);
    jpg = (uint8_t *)malloc(jcap);
    depth = (float *)malloc(n * sizeof(float));
    sample = (float *)malloc(20000 * sizeof(float));
    index = (uint32_t *)malloc((size_t)DEPTH_VIDEO_FRAMES * 2 * sizeof(uint32_t));
    if (!rgb || !u8 || !small_ || !jpg || !depth || !sample || !index) goto done;
    LOGI(L"depth: video %dx%d, %.1f s at %.2f fps (%hs) -> %d depth frames at %.2f fps, model input %dx%d, saved %dx%d",
         dec->width, dec->height, secs, srcFps, avcodec_get_name(id), total, fps, mw, mh, ow, oh);

    rc = 4;
    {
        Backend rec = { 0 };
        char fp[17] = "";
        int gpu = mode != DEPTH_CPU && mode != DEPTH_CRASHED;
        if (gpu) {
            adapter_fingerprint(fp);
            if (read_backend(&rec, fp) && rec.cpuRun > 0 && (rec.gpuRun <= 0 || rec.cpuRun < rec.gpuRun)) gpu = 0;   /* the GPU really is slower */
        }
        if (!(gpu && sess_open(&x, dll, 1, model, m->v3, mw, mh)) && !sess_open(&x, dll, 0, model, m->v3, mw, mh)) goto done;
        LOGI(L"depth: model loaded on the %ls in %.0f ms", x.gpu ? L"GPU" : L"CPU", time_ms() - t0);
    }
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    uint32_t pos = 8, head[2] = { 0x56445053u /* "SPDV" */, 1 };
    if (f == INVALID_HANDLE_VALUE || !put(f, head, sizeof head)) goto done;

    double next = 0.0, start = st->start_time != AV_NOPTS_VALUE ? st->start_time * av_q2d(st->time_base) : 0.0;
    float slo = 0.0f, shi = 0.0f;
    int eof = 0, runs = 0;
    double runMs = 0.0;
    while (count < DEPTH_VIDEO_FRAMES) {
        int got = avcodec_receive_frame(dec, fr);
        if (got == AVERROR(EAGAIN)) {
            if (eof) break;
            if (av_read_frame(fmt, pkt) < 0) {
                eof = 1;
                avcodec_send_packet(dec, NULL);
            } else {
                if (pkt->stream_index == vi) avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
            }
            continue;
        }
        if (got < 0) break;
        int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
        double t = ts != AV_NOPTS_VALUE ? ts * av_q2d(st->time_base) - start : next;
        if (t + 0.5 / srcFps < next || (total && count >= total)) {   /* not a frame the page needs */
            av_frame_unref(fr);
            continue;
        }
        /* this picture, stretched to the model's size like a still one */
        uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
        int ls[4] = { mw * 3, 0, 0, 0 };
        sws = sws_getCachedContext(sws, fr->width, fr->height, (enum AVPixelFormat)fr->format, mw, mh, AV_PIX_FMT_RGB24, SWS_AREA, NULL, NULL, NULL);
        if (!sws) goto done;
        sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, ls);
        av_frame_unref(fr);
        free(chw);
        double r0 = time_ms();
        if (!(chw = to_tensor(rgb, mw, mh)) || !sess_run(&x, chw, depth)) goto done;
        runMs += time_ms() - r0;
        runs++;
        float lo, hi;
        percentiles(depth, n, sample, 20000, &lo, &hi);
        if (count == 0) { slo = lo; shi = hi; }
        else { slo += (lo - slo) * 0.15f; shi += (hi - shi) * 0.15f; }   /* the range follows the video, smoothly */
        float scale = 255.0f / (shi - slo + 1e-8f);
        for (size_t i = 0; i < n; i++) u8[i] = (uint8_t)SP_CLAMP((int)((depth[i] - slo) * scale), 0, 255);
        shrink(u8, mw, mh, small_, ow, oh);
        dilate(small_, ow, oh, 2);
        blur(small_, ow, oh, 1);
        DWORD len = encode_jpeg(wic, small_, ow, oh, jpg, jcap);
        if (!len || !put(f, jpg, len)) goto done;
        /* a gap in the video (variable frame rate): this picture also stands in for the frame times it skipped */
        int limit = total ? total : DEPTH_VIDEO_FRAMES;
        do {
            index[count * 2] = pos;
            index[count * 2 + 1] = len;
            count++;
        } while (count < limit && (count + 0.5) / fps <= t);
        next = count / fps;
        pos += len;
        notify(nt, count, total ? total : count);
    }
    if (count == 0) goto done;
    float hdr[1] = { (float)fps };
    uint32_t tail[6] = { (uint32_t)count, (uint32_t)ow, (uint32_t)oh, 0, pos, 0x56445053u };
    memcpy(&tail[3], hdr, sizeof(float));
    if (!put(f, index, (DWORD)count * 2 * sizeof(uint32_t)) || !put(f, tail, sizeof tail)) goto done;
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(tmp, out, MOVEFILE_REPLACE_EXISTING)) goto done;
    {   /* the run time per frame, for depth_estimate */
        Backend rec;
        char fp[17] = "";
        adapter_fingerprint(fp);
        if (!read_backend(&rec, fp)) {
            ZeroMemory(&rec, sizeof rec);
            rec.gpu = x.gpu;
            strncpy_s(rec.ort, sizeof rec.ort, g_ortVersion, _TRUNCATE);
            strncpy_s(rec.fp, sizeof rec.fp, fp, _TRUNCATE);
        }
        if (x.gpu) rec.gpuRun = runMs / runs;
        else rec.cpuRun = runMs / runs;
        write_backend(&rec);
    }
    rc = 0;
    LOGI(L"depth: %d depth frames (%d runs, %.0f ms each on the %ls) in %.1f s, %.1f MB", count, runs, runMs / SP_MAX(runs, 1),
         x.gpu ? L"GPU" : L"CPU", (time_ms() - t0) / 1000.0, pos / 1048576.0);
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (rc) DeleteFileW(tmp);
    if (x.s || x.env) sess_close(&x);
    sws_freeContext(sws);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    free(index);
    free(sample);
    free(depth);
    free(chw);
    free(jpg);
    free(small_);
    free(u8);
    free(rgb);
    free(u);
    return rc;
}

/* ---------------------------------------------------------------- entry point */

int depth_infer_main(const wchar_t *image, const wchar_t *dir, int mode, int wantPng, const wchar_t *modelKey, int video, HWND notifyWnd)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapSource *src = NULL;
    uint8_t *rgb = NULL, *u8 = NULL;
    float *chw = NULL, *depth = NULL;
    HMODULE dll = NULL;
    wchar_t model[SP_MAX_PATH], tmp[SP_MAX_PATH], png[SP_MAX_PATH], name[64];
    UINT sw = 0, sh = 0;
    int w = 0, h = 0, rc = 2;
    double t0 = time_ms();
    log_init(L"depth");
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_model = SP_MAX(0, depth_model_find(modelKey));
    const DepthModel *m = depth_model(g_model);
    LOGI(L"depth: %ls -> %ls with %ls (%ls)", image, dir, m->name,
         mode == DEPTH_CPU ? L"CPU only" : mode == DEPTH_CRASHED ? L"CPU, after a crash" : mode == DEPTH_BENCH ? L"test both" : L"fastest of GPU and CPU");

    depth_model_file(g_model, model, SP_COUNTOF(model));
    if (g_model) str_printf(name, SP_COUNTOF(name), L"depth-%ls%ls", m->key, video ? L".bin" : L".png");
    else str_copy(name, SP_COUNTOF(name), video ? L"depth.bin" : L"depth.png");
    path_join(png, SP_COUNTOF(png), dir, name);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) goto done;
    if (video) {
        rc = 3;
        if (!(dll = load_runtime())) goto done;
        Notify nt = { notifyWnd, 0.0 };
        rc = write_video(f, dll, m, model, image, png, mode, &nt);
        goto done;
    }
    path_join(tmp, SP_COUNTOF(tmp), dir, L"depth.tmp");
    if (!(src = open_upright(f, image, &sw, &sh))) {
        LOGE(L"depth: cannot read the picture");
        goto done;
    }
    model_size(sw, sh, m->shortSide, &w, &h);
    if (!(rgb = scaled_rgb(f, src, w, h)) || !(chw = to_tensor(rgb, w, h)) || !(depth = (float *)malloc((size_t)w * h * sizeof(float)))) {
        LOGE(L"depth: out of memory");
        goto done;
    }
    LOGI(L"depth: picture %ux%u, model input %dx%d, prepared in %.0f ms", sw, sh, w, h, time_ms() - t0);

    rc = 3;
    if (!(dll = load_runtime())) goto done;
    rc = 4;
    {
        Backend rec;
        char fp[17] = "";
        BOOL ok = FALSE, known = FALSE;
        ZeroMemory(&rec, sizeof rec);
        if (mode != DEPTH_CPU) {                             /* --cpu is the safe path: it asks the graphics stack nothing */
            adapter_fingerprint(fp);
            known = mode == DEPTH_AUTO && read_backend(&rec, fp);
        }
        if (!known) {
            ZeroMemory(&rec, sizeof rec);
            strncpy_s(rec.ort, sizeof rec.ort, g_ortVersion, _TRUNCATE);
            strncpy_s(rec.fp, sizeof rec.fp, fp, _TRUNCATE);
        }
        if (mode == DEPTH_CPU) {
            ok = run_model(dll, 0, model, m->v3, chw, w, h, depth, NULL, NULL);
        } else if (mode == DEPTH_CRASHED) {                 /* the GPU path killed the process: the CPU is the choice now */
            rec.gpu = 0;
            ok = run_model(dll, 0, model, m->v3, chw, w, h, depth, &rec.cpuMs, &rec.cpuRun);
            if (ok) write_backend(&rec);
        } else if (known && !rec.gpu) {                      /* the test found the CPU quicker */
            ok = run_model(dll, 0, model, m->v3, chw, w, h, depth, NULL, NULL);
        } else if (known) {                                  /* ... the GPU quicker; if it has stopped working, remember that */
            ok = run_model(dll, 1, model, m->v3, chw, w, h, depth, NULL, NULL);
            if (!ok) {
                LOGW(L"depth: the GPU stopped working; using the CPU from now on");
                rec.gpu = 0;
                rec.gpuMs = rec.gpuRun = 0.0;
                ok = run_model(dll, 0, model, m->v3, chw, w, h, depth, &rec.cpuMs, &rec.cpuRun);
                if (ok) write_backend(&rec);
            }
        } else {
            ok = benchmark(dll, model, m->v3, chw, w, h, depth, &rec);
        }
        if (!ok) goto done;
    }

    float lo = depth[0], hi = depth[0];
    size_t n = (size_t)w * h;
    if (m->v3) {                                         /* inverse metric depth: the sky's far tail would flatten the rest */
        float *sample = (float *)malloc(20000 * sizeof(float));
        if (!sample) goto done;
        percentiles(depth, n, sample, 20000, &lo, &hi);
        free(sample);
    } else {
        for (size_t i = 1; i < n; i++) {
            if (depth[i] < lo) lo = depth[i];
            if (depth[i] > hi) hi = depth[i];
        }
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
