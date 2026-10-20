/* Video wallpapers: FFmpeg (libavformat/libavcodec/libswscale) decode + Direct3D 11 presentation.
 * See video_internal.h for the thread model. */
#include "video_internal.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include <d3d10.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libavutil/version.h>
#include <libavutil/ffversion.h>
#include <libavcodec/version_major.h>
#include <libavformat/version_major.h>
#include <libswscale/version_major.h>
#include <libswresample/version_major.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <process.h>

#include "video_vs.h"
#include "video_ps_nv12.h"
#include "video_ps_i420.h"
#include "video_ps_rgb.h"

#define WIDEN2(s) L##s
#define WIDEN(s)  WIDEN2(s)
#define STR2(x)   #x
#define STR(x)    STR2(x)

typedef struct CBParams {
    float uvRect[4];
    float uvClamp[4];
    float m0[4], m1[4], m2[4];
    float off[4];
} CBParams;

typedef struct PlaneSet {
    ID3D11PixelShader        *ps;
    ID3D11ShaderResourceView *srv[5];
    float                     texW, texH, slice;
    int                       frameW, frameH;
    int                       yuv;
} PlaneSet;

enum { UP_NONE = 0, UP_NV12, UP_I420, UP_BGRA };

static struct {
    SRWLOCK              lock;
    int                  ff;            /* 0 unknown, 1 available, -1 unavailable */
    wchar_t              ffErr[SP_ERR_LEN];
    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    IDXGIFactory2       *factory;
    volatile LONG        deviceLost;
    int                  videoSupport, nv12Srv, p010Srv;
    ID3D11VertexShader  *vs;
    ID3D11PixelShader   *psNV12, *psI420, *psRGB;
    ID3D11SamplerState  *smp;
    ID3D11Buffer        *cb;
    AVBufferRef         *hwdev;
    CRITICAL_SECTION     renderLock;    /* serializes immediate-context use: presenters + FFmpeg D3D11VA */
    int                  renderLockInit;
    VideoSource         *sources[SP_MAX_MONITORS];
    int                  sourceCount;
    wchar_t              adapter[128];
} g = { SRWLOCK_INIT };

/* ================================================================ FFmpeg availability */

static const wchar_t *k_ffDlls[] = {
    L"avutil-" WIDEN(STR(LIBAVUTIL_VERSION_MAJOR)) L".dll",
    L"swresample-" WIDEN(STR(LIBSWRESAMPLE_VERSION_MAJOR)) L".dll",
    L"swscale-" WIDEN(STR(LIBSWSCALE_VERSION_MAJOR)) L".dll",
    L"avcodec-" WIDEN(STR(LIBAVCODEC_VERSION_MAJOR)) L".dll",
    L"avformat-" WIDEN(STR(LIBAVFORMAT_VERSION_MAJOR)) L".dll",
};

static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    static volatile LONG budget = 60;
    static ULONGLONG windowStart;
    if (level > AV_LOG_WARNING) return;
    ULONGLONG now = GetTickCount64();
    if (now - windowStart > 60000) { windowStart = now; budget = 60; }
    if (InterlockedDecrement(&budget) < 0) return;      /* rate limit: corrupt files can be noisy */
    char line[512];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &prefix);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    log_write(level <= AV_LOG_ERROR ? LOG_ERROR : LOG_WARN, L"ffmpeg: %hs", line);
}

static int ff_probe_versions(void)
{
    __try {
        if ((avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR) return 0;
        if ((avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR) return 0;
        if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR) return 0;
        if ((swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR) return 0;
        if ((swresample_version() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

BOOL video_ffmpeg_available(wchar_t *err, size_t cap)
{
    AcquireSRWLockExclusive(&g.lock);
    if (g.ff == 0) {
        g.ff = 1;
        for (size_t i = 0; i < SP_COUNTOF(k_ffDlls); i++) {
            /* Load from the application directory only (never the current directory). */
            if (!LoadLibraryExW(k_ffDlls[i], NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                str_printf(g.ffErr, SP_COUNTOF(g.ffErr), L"FFmpeg library %ls is missing (err %lu). Reinstall Screen+.",
                           k_ffDlls[i], GetLastError());
                g.ff = -1;
                break;
            }
        }
        if (g.ff == 1) {
            int v = ff_probe_versions();
            if (v != 1) {
                str_copy(g.ffErr, SP_COUNTOF(g.ffErr), L"Installed FFmpeg libraries are incompatible with this build");
                g.ff = -1;
            } else {
                av_log_set_level(AV_LOG_WARNING);
                av_log_set_callback(ff_log_cb);
                LOGI(L"video: FFmpeg %hs loaded (avcodec %u.%u, %hs)", FFMPEG_VERSION, avcodec_version() >> 16,
                     (avcodec_version() >> 8) & 0xff, avcodec_license());
            }
        }
        if (g.ff < 0) LOGE(L"video: %ls", g.ffErr);
    }
    BOOL ok = g.ff == 1;
    if (!ok && err) str_copy(err, cap, g.ffErr);
    ReleaseSRWLockExclusive(&g.lock);
    return ok;
}

void video_ffmpeg_version(wchar_t *buf, size_t cap)
{
    str_printf(buf, cap, L"FFmpeg %hs (LGPL)", FFMPEG_VERSION);
}

/* ================================================================ D3D11 device */

static void ff_lock(void *ctx)   { EnterCriticalSection((CRITICAL_SECTION *)ctx); }
static void ff_unlock(void *ctx) { LeaveCriticalSection((CRITICAL_SECTION *)ctx); }

static void device_release(void)
{
    if (g.hwdev) av_buffer_unref(&g.hwdev);   /* never touch FFmpeg unless it was loaded */
    SP_RELEASE(g.cb);
    SP_RELEASE(g.smp);
    SP_RELEASE(g.psRGB);
    SP_RELEASE(g.psI420);
    SP_RELEASE(g.psNV12);
    SP_RELEASE(g.vs);
    SP_RELEASE(g.factory);
    SP_RELEASE(g.ctx);
    SP_RELEASE(g.dev);
    g.deviceLost = 0;
}

static BOOL device_init(wchar_t *err, size_t cap)
{
    if (!g.renderLockInit) {
        InitializeCriticalSection(&g.renderLock);
        g.renderLockInit = 1;
    }
    if (g.dev && !g.deviceLost && ID3D11Device_GetDeviceRemovedReason(g.dev) == S_OK) return TRUE;
    if (g.dev) {
        if (g.sourceCount > 0) {
            str_copy(err, cap, L"Graphics device was reset; waiting for video renderers to restart");
            return FALSE;
        }
        LOGW(L"video: recreating Direct3D device");
        device_release();
    }

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl = 0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, 4, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels + 1, 3, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    g.videoSupport = SUCCEEDED(hr);
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels + 1, 3, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, levels + 1, 3, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    }
    if (FAILED(hr)) {
        str_printf(err, cap, L"Direct3D 11 is not available (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g.dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }

    IDXGIDevice *dxgiDev = NULL;
    IDXGIAdapter *adapter = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g.dev, &IID_IDXGIDevice, (void **)&dxgiDev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgiDev, &adapter))) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad))) str_copy(g.adapter, SP_COUNTOF(g.adapter), ad.Description);
        IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&g.factory);
    }
    SP_RELEASE(adapter);
    SP_RELEASE(dxgiDev);
    if (!g.factory) {
        str_copy(err, cap, L"DXGI 1.2 is not available");
        device_release();
        return FALSE;
    }

    UINT fs = 0;
    g.nv12Srv = SUCCEEDED(ID3D11Device_CheckFormatSupport(g.dev, DXGI_FORMAT_NV12, &fs)) &&
                (fs & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && (fs & D3D11_FORMAT_SUPPORT_TEXTURE2D);
    fs = 0;
    g.p010Srv = SUCCEEDED(ID3D11Device_CheckFormatSupport(g.dev, DXGI_FORMAT_P010, &fs)) &&
                (fs & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && (fs & D3D11_FORMAT_SUPPORT_TEXTURE2D);

    hr = ID3D11Device_CreateVertexShader(g.dev, g_vs_main, sizeof g_vs_main, NULL, &g.vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g.dev, g_ps_nv12, sizeof g_ps_nv12, NULL, &g.psNV12);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g.dev, g_ps_i420, sizeof g_ps_i420, NULL, &g.psI420);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g.dev, g_ps_rgb, sizeof g_ps_rgb, NULL, &g.psRGB);
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd;
        ZeroMemory(&sd, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(g.dev, &sd, &g.smp);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd;
        ZeroMemory(&bd, sizeof bd);
        bd.ByteWidth = sizeof(CBParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb);
    }
    if (FAILED(hr)) {
        str_printf(err, cap, L"Could not create video shaders (0x%08lX)", (unsigned long)hr);
        device_release();
        return FALSE;
    }

    LOGI(L"gfx: D3D11 device on '%ls' FL %x.%x, video=%d, NV12 sampling=%d, P010 sampling=%d", g.adapter,
         (fl >> 12) & 0xf, (fl >> 8) & 0xf, g.videoSupport, g.nv12Srv, g.p010Srv);
    return TRUE;
}

/* FFmpeg D3D11VA device wrapping our device and sharing our immediate-context lock.
 * Separate from device_init so image/HTML presentation never loads FFmpeg. */
static void ensure_hwdev(void)
{
    static int logged;
    if (!g.hwdev && g.videoSupport && g.nv12Srv) {
        AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (ref) {
            AVHWDeviceContext *hd = (AVHWDeviceContext *)ref->data;
            AVD3D11VADeviceContext *d = (AVD3D11VADeviceContext *)hd->hwctx;
            d->device = g.dev;
            ID3D11Device_AddRef(g.dev);
            d->lock = ff_lock;
            d->unlock = ff_unlock;
            d->lock_ctx = &g.renderLock;
            if (av_hwdevice_ctx_init(ref) >= 0) g.hwdev = ref;
            else av_buffer_unref(&ref);
        }
    }
    if (!logged) {
        logged = 1;
        LOGI(L"video: decoding mode %ls", g.hwdev ? L"D3D11VA hardware (software fallback)" : L"software only");
    }
}

/* ================================================================ frame queue */

static void fq_init(FrameQueue *q)
{
    ZeroMemory(q, sizeof *q);
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->notFull);
    q->pushed = CreateEventW(NULL, FALSE, FALSE, NULL);
}

static int fq_push(FrameQueue *q, AVFrame *f, double pts, double dur)
{
    AcquireSRWLockExclusive(&q->lock);
    while (q->count == VQ_CAP && !q->abort) SleepConditionVariableSRW(&q->notFull, &q->lock, INFINITE, 0);
    if (q->abort) {
        ReleaseSRWLockExclusive(&q->lock);
        av_frame_free(&f);
        return 0;
    }
    VFrame *it = &q->items[(q->head + q->count) % VQ_CAP];
    it->f = f;
    it->pts = pts;
    it->dur = dur;
    q->count++;
    ReleaseSRWLockExclusive(&q->lock);
    SetEvent(q->pushed);
    return 1;
}

static int fq_peek(FrameQueue *q, VFrame *out)
{
    int ok = 0;
    AcquireSRWLockShared(&q->lock);
    if (q->count) {
        *out = q->items[q->head];
        ok = 1;
    }
    ReleaseSRWLockShared(&q->lock);
    return ok;
}

static int fq_pop(FrameQueue *q, VFrame *out)
{
    AcquireSRWLockExclusive(&q->lock);
    if (!q->count) {
        ReleaseSRWLockExclusive(&q->lock);
        return 0;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % VQ_CAP;
    q->count--;
    ReleaseSRWLockExclusive(&q->lock);
    WakeConditionVariable(&q->notFull);
    return 1;
}

static void fq_abort(FrameQueue *q)
{
    AcquireSRWLockExclusive(&q->lock);
    q->abort = 1;
    ReleaseSRWLockExclusive(&q->lock);
    WakeAllConditionVariable(&q->notFull);
    SetEvent(q->pushed);
}

static void fq_free(FrameQueue *q)
{
    VFrame f;
    while (fq_pop(q, &f)) av_frame_free(&f.f);
    if (q->pushed) CloseHandle(q->pushed);
    q->pushed = NULL;
}

/* ================================================================ helpers */

double video_media_now(VideoSource *s)
{
    return (time_ms() - s->clockBase) / 1000.0;
}

static void post_all(VideoSource *s, int code, HRESULT hr, const wchar_t *msg)
{
    EnterCriticalSection(&s->outLock);
    for (int i = 0; i < s->outputCount; i++)
        renderer_post(s->outputs[i]->notify, s->outputs[i]->base.rid, code, hr, L"%ls", msg);
    LeaveCriticalSection(&s->outLock);
}

void video_source_fail(VideoSource *s, HRESULT hr, const wchar_t *fmt, ...)
{
    if (InterlockedExchange(&s->failed, 1)) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(s->err, SP_COUNTOF(s->err), _TRUNCATE, fmt, ap);
    va_end(ap);
    LOGE(L"video: %ls [%ls]", s->err, path_filename(s->path));
    post_all(s, REV_FAILED, hr, s->err);
    fq_abort(&s->fq);
    SetEvent(s->wake);
}

static void ff_err_text(int err, wchar_t *buf, size_t cap)
{
    char tmp[128];
    av_strerror(err, tmp, sizeof tmp);
    str_printf(buf, cap, L"%hs", tmp);
}

static int interrupt_cb(void *opaque)
{
    return ((VideoSource *)opaque)->quit != 0;
}

/* ================================================================ decode thread */

static int codec_supports_d3d11(const AVCodec *codec)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return 0;
        if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && cfg->pix_fmt == AV_PIX_FMT_D3D11 &&
            (cfg->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)))
            return 1;
    }
}

static int make_hw_frames(AVCodecContext *c, int shaderResource)
{
    AVBufferRef *fr = NULL;
    if (avcodec_get_hw_frames_parameters(c, g.hwdev, AV_PIX_FMT_D3D11, &fr) < 0) return -1;
    AVHWFramesContext *fc = (AVHWFramesContext *)fr->data;
    AVD3D11VAFramesContext *dfc = (AVD3D11VAFramesContext *)fc->hwctx;
    if (fc->sw_format == AV_PIX_FMT_P010 && !g.p010Srv) { av_buffer_unref(&fr); return -1; }
    if (shaderResource) dfc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (av_hwframe_ctx_init(fr) < 0) {
        av_buffer_unref(&fr);
        return -1;
    }
    av_buffer_unref(&c->hw_frames_ctx);
    c->hw_frames_ctx = fr;
    return 0;
}

/* Hardware decode mode. Default: decoder-only surfaces + one GPU copy per presented frame.
 * Measured on an RTX 5070 (4K HEVC Main10, 30 fps): GPU copy ~10% of one core and 213 MB;
 * sampling the decoder array directly ("zero-copy", SRV-bindable decoder surfaces) ~48% and
 * 263 MB because of driver overhead; software decode ~90%. SCREENPLUS_HWDEC=zerocopy|off. */
enum { HWDEC_COPY = 0, HWDEC_OFF = 1, HWDEC_ZEROCOPY = 2 };
static int hwdec_override(void)
{
    wchar_t v[16];
    DWORD n = GetEnvironmentVariableW(L"SCREENPLUS_HWDEC", v, SP_COUNTOF(v));
    if (!n || n >= SP_COUNTOF(v)) return HWDEC_COPY;
    return str_ieq(v, L"off") ? HWDEC_OFF : str_ieq(v, L"zerocopy") ? HWDEC_ZEROCOPY : HWDEC_COPY;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    VideoSource *s = (VideoSource *)c->opaque;
    if (!s->hwFailed && g.hwdev) {
        for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p != AV_PIX_FMT_D3D11) continue;
            if (hwdec_override() == HWDEC_ZEROCOPY && make_hw_frames(c, 1) == 0) { s->hwMode = HW_ZEROCOPY; return AV_PIX_FMT_D3D11; }
            if (make_hw_frames(c, 0) == 0) { s->hwMode = HW_COPY; return AV_PIX_FMT_D3D11; }
            break;
        }
    }
    s->hwMode = HW_NONE;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

static int open_video_decoder(VideoSource *s, int tryHw)
{
    AVStream *st = s->fmt->streams[s->vidx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        video_source_fail(s, E_FAIL, L"No %ls decoder in the bundled FFmpeg build", s->codec);
        return 0;
    }
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, st->codecpar) < 0) {
        avcodec_free_context(&c);
        video_source_fail(s, E_OUTOFMEMORY, L"Could not allocate the video decoder");
        return 0;
    }
    c->pkt_timebase = st->time_base;
    c->opaque = s;
    int hw = tryHw && !s->hwFailed && g.hwdev && hwdec_override() != HWDEC_OFF && codec_supports_d3d11(codec);
    if (hw) {
        c->get_format = get_hw_format;
        c->thread_count = 1;
        c->extra_hw_frames = VQ_CAP + 2;   /* frames we hold: queue + current + one in flight */
    } else {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        c->thread_count = (int)SP_CLAMP(si.dwNumberOfProcessors, 1, 4);   /* bounded: memory per thread */
        c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    s->hwMode = HW_NONE;
    int r = avcodec_open2(c, codec, NULL);
    if (r < 0) {
        avcodec_free_context(&c);
        if (hw) {
            s->hwFailed = 1;
            return open_video_decoder(s, 0);
        }
        wchar_t e[96];
        ff_err_text(r, e, SP_COUNTOF(e));
        video_source_fail(s, E_FAIL, L"Could not open the %ls decoder: %ls", s->codec, e);
        return 0;
    }
    s->vdec = c;
    s->hwErrors = 0;
    return 1;
}

static void set_discards(VideoSource *s)
{
    for (unsigned i = 0; i < s->fmt->nb_streams; i++)
        s->fmt->streams[i]->discard = ((int)i == s->vidx || (int)i == s->aidx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

static int open_input(VideoSource *s)
{
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return AVERROR(ENOMEM);
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = s;
    int r = avformat_open_input(&fmt, s->pathUtf8, NULL, NULL);
    if (r < 0) return r;                          /* fmt freed by avformat_open_input */
    fmt->max_analyze_duration = 3 * (int64_t)AV_TIME_BASE;
    r = avformat_find_stream_info(fmt, NULL);
    if (r < 0) LOGW(L"video: stream info incomplete for %ls", path_filename(s->path));
    s->fmt = fmt;
    return 0;
}

static int media_open(VideoSource *s)
{
    double t0 = time_ms();
    int r = open_input(s);
    if (r < 0) {
        wchar_t e[96];
        ff_err_text(r, e, SP_COUNTOF(e));
        video_source_fail(s, E_FAIL, r == AVERROR_INVALIDDATA ? L"Unsupported or corrupt media file (%ls)"
                                                              : L"Could not open the video (%ls)", e);
        return 0;
    }
    int best = -1;
    for (unsigned i = 0; i < s->fmt->nb_streams; i++) {
        AVStream *st = s->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        if (best < 0 || (int64_t)st->codecpar->width * st->codecpar->height >
                        (int64_t)s->fmt->streams[best]->codecpar->width * s->fmt->streams[best]->codecpar->height)
            best = (int)i;
    }
    if (best < 0) {
        video_source_fail(s, E_FAIL, L"The file has no video stream");
        return 0;
    }
    s->vidx = best;
    s->aidx = -1;
    set_discards(s);
    AVStream *st = s->fmt->streams[best];
    s->width = st->codecpar->width;
    s->height = st->codecpar->height;
    AVRational fr = av_guess_frame_rate(s->fmt, st, NULL);
    s->fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
    str_printf(s->codec, SP_COUNTOF(s->codec), L"%hs", avcodec_get_name(st->codecpar->codec_id));
    if (!open_video_decoder(s, 1)) return 0;
    s->tmp = av_frame_alloc();
    if (!s->tmp) {
        video_source_fail(s, E_OUTOFMEMORY, L"Out of memory");
        return 0;
    }
    LOGI(L"video: opened %ls: %ls %dx%d %.3f fps, %hs, decoder ready in %.1f ms", path_filename(s->path), s->codec,
         s->width, s->height, s->fps, s->fmt->iformat ? s->fmt->iformat->name : "?", time_ms() - t0);
    return 1;
}

static int push_video_frame(VideoSource *s)
{
    AVFrame *f = s->tmp;
    AVStream *st = s->fmt->streams[s->vidx];
    double tb = av_q2d(st->time_base);
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    double dur = f->duration > 0 ? f->duration * tb : (s->fps > 1.0 ? 1.0 / s->fps : 1.0 / 30.0);
    dur = SP_CLAMP(dur, 0.001, 10.0);
    double raw = ts != AV_NOPTS_VALUE ? ts * tb : s->lastRaw + s->lastDur;
    if (!s->haveStart) {
        s->haveStart = 1;
        s->loopStartRaw = raw;
        s->loopBase = 0.0;
        s->lastPts = -1.0;
    }
    if (s->rebase) {
        s->loopBase = (s->lastPts + s->lastDur) - (raw - s->loopStartRaw);
        s->rebase = 0;
    }
    double t = s->loopBase + (raw - s->loopStartRaw);
    if (s->lastPts >= 0.0) {
        if (t <= s->lastPts) {
            t = s->lastPts + SP_CLAMP(s->lastDur, 0.001, 0.25);
        } else if (t - s->lastPts > 10.0) {     /* timestamp discontinuity */
            t = s->lastPts + SP_CLAMP(s->lastDur, 0.001, 0.25);
            s->loopBase = t - (raw - s->loopStartRaw);
        }
    }
    s->lastPts = t;
    s->lastDur = dur;
    s->lastRaw = raw;
    s->framesThisLoop++;
    s->framesDecoded++;
    if (f->format == AV_PIX_FMT_D3D11 && s->framesDecoded == 1)
        LOGI(L"video: hardware decoding active (D3D11VA, %ls)", s->hwMode == HW_ZEROCOPY ? L"zero-copy" : L"GPU copy");
    AVFrame *q = av_frame_alloc();
    if (!q) return 1;
    av_frame_move_ref(q, f);
    return fq_push(&s->fq, q, t, dur);
}

static void switch_to_software(VideoSource *s, const wchar_t *why)
{
    LOGW(L"video: hardware decoding failed (%ls); switching to software decoding", why);
    avcodec_free_context(&s->vdec);
    s->hwFailed = 1;
    if (!open_video_decoder(s, 0)) return;
    AVStream *st = s->fmt->streams[s->vidx];
    av_seek_frame(s->fmt, s->vidx, st->start_time != AV_NOPTS_VALUE ? st->start_time : 0, AVSEEK_FLAG_BACKWARD);
    s->rebase = s->haveStart;
}

static int receive_frames(VideoSource *s)
{
    for (;;) {
        int r = avcodec_receive_frame(s->vdec, s->tmp);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 1;
        if (r < 0) {
            if (s->hwMode != HW_NONE && s->framesDecoded < 8 && ++s->hwErrors >= 3) {
                switch_to_software(s, L"decode errors");
                return s->vdec != NULL;
            }
            return 1;
        }
        if (!push_video_frame(s)) return 0;
    }
}

static int decode_video(VideoSource *s, AVPacket *pkt)
{
    for (int guard = 0; guard < 64; guard++) {
        double t0 = time_ms();
        int r = avcodec_send_packet(s->vdec, pkt);
        s->decodeMsTotal += time_ms() - t0;
        if (r == AVERROR(EAGAIN)) {
            if (!receive_frames(s)) return 0;
            continue;
        }
        if (r < 0 && r != AVERROR_EOF) {
            if (s->hwMode != HW_NONE && s->framesDecoded < 8 && ++s->hwErrors >= 3) {
                switch_to_software(s, L"packet rejected");
                return s->vdec != NULL;
            }
            return 1;   /* skip a corrupt packet */
        }
        return receive_frames(s);
    }
    return 1;
}

static int seek_to_start(VideoSource *s)
{
    AVStream *st = s->fmt->streams[s->vidx];
    int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (av_seek_frame(s->fmt, s->vidx, start, AVSEEK_FLAG_BACKWARD) >= 0) return 1;
    if (avformat_seek_file(s->fmt, -1, INT64_MIN, 0, INT64_MAX, 0) >= 0) return 1;
    /* Not seekable: reopen the container (decoders are kept). */
    avformat_close_input(&s->fmt);
    if (open_input(s) < 0 || (unsigned)s->vidx >= s->fmt->nb_streams) return 0;
    set_discards(s);
    return 1;
}

static void handle_eof(VideoSource *s)
{
    avcodec_send_packet(s->vdec, NULL);       /* drain delayed frames */
    if (!receive_frames(s)) return;
    avcodec_flush_buffers(s->vdec);
    if (s->framesThisLoop == 0) {
        if (++s->emptyLoops >= 3) {
            video_source_fail(s, E_FAIL, L"The file contains no decodable video frames");
            return;
        }
    } else {
        s->emptyLoops = 0;
    }
    if (s->framesThisLoop == 1 && s->loops == 0) {
        s->still = 1;    /* single picture: keep it on screen, stop decoding */
        LOGI(L"video: %ls is a single frame; decoding stopped", path_filename(s->path));
        return;
    }
    if (s->adec) pq_put(&s->aq, NULL, s->lastPts + s->lastDur);
    if (!seek_to_start(s)) {
        video_source_fail(s, E_FAIL, L"Could not loop the video (seek failed)");
        return;
    }
    s->rebase = 1;
    s->framesThisLoop = 0;
    s->loops++;
}

static unsigned __stdcall decode_main(void *arg)
{
    VideoSource *s = (VideoSource *)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus video decode");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AVPacket *pkt = NULL;
    if (!media_open(s)) goto out;
    pkt = av_packet_alloc();
    if (!pkt) goto out;
    int readErrors = 0;
    while (!s->quit && !s->failed) {
        if (s->audioWanted && s->aidx == -1) audio_start(s);
        else if (!s->audioWanted && s->adec) audio_stop(s);
        if (s->still) {
            WaitForSingleObject(s->decodeWake, INFINITE);
            continue;
        }
        int r = av_read_frame(s->fmt, pkt);
        if (r == AVERROR_EXIT || s->quit) break;
        if (r == AVERROR_EOF || (r < 0 && s->fmt->pb && avio_feof(s->fmt->pb))) {
            handle_eof(s);
            continue;
        }
        if (r < 0) {
            if (++readErrors > 100) {
                wchar_t e[96];
                ff_err_text(r, e, SP_COUNTOF(e));
                video_source_fail(s, E_FAIL, L"Read error: %ls", e);
                break;
            }
            continue;
        }
        readErrors = 0;
        if (pkt->stream_index == s->vidx) {
            if (!decode_video(s, pkt)) {
                av_packet_unref(pkt);
                break;
            }
        } else if (pkt->stream_index == s->aidx && s->adec) {
            AVPacket *a = av_packet_clone(pkt);
            if (a) pq_put(&s->aq, a, -1.0);
        }
        av_packet_unref(pkt);
    }
out:
    av_packet_free(&pkt);
    audio_stop(s);
    return 0;
}

/* ================================================================ presentation */

static void yuv_matrix(const AVFrame *f, CBParams *cb)
{
    double kr, kb;
    switch (f->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC:        kr = 0.299;  kb = 0.114;  break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:  kr = 0.2627; kb = 0.0593; break;
    case AVCOL_SPC_SMPTE240M:  kr = 0.212;  kb = 0.087;  break;
    case AVCOL_SPC_BT709:      kr = 0.2126; kb = 0.0722; break;
    default:
        if (f->height >= 720) { kr = 0.2126; kb = 0.0722; }
        else { kr = 0.299; kb = 0.114; }
    }
    int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    double ys = full ? 1.0 : 255.0 / 219.0, cs = full ? 1.0 : 255.0 / 224.0, kg = 1.0 - kr - kb;
    float m[3][4] = {
        { (float)ys, 0.0f, (float)(2.0 * (1.0 - kr) * cs), 0.0f },
        { (float)ys, (float)(-2.0 * kb * (1.0 - kb) / kg * cs), (float)(-2.0 * kr * (1.0 - kr) / kg * cs), 0.0f },
        { (float)ys, (float)(2.0 * (1.0 - kb) * cs), 0.0f, 0.0f },
    };
    memcpy(cb->m0, m[0], sizeof cb->m0);
    memcpy(cb->m1, m[1], sizeof cb->m1);
    memcpy(cb->m2, m[2], sizeof cb->m2);
    cb->off[0] = full ? 0.0f : 16.0f / 255.0f;
    cb->off[1] = cb->off[2] = 128.0f / 255.0f;
}

static void release_upload(VideoSource *s)
{
    for (int i = 0; i < 3; i++) {
        SP_RELEASE(s->upSrv[i]);
        SP_RELEASE(s->upTex[i]);
    }
    s->upKind = UP_NONE;
}

static BOOL make_tex(int w, int h, DXGI_FORMAT fmt, int dynamic, UINT arraySize, ID3D11Texture2D **tex,
                     ID3D11ShaderResourceView **srv, DXGI_FORMAT viewFmt, ID3D11ShaderResourceView **srv2,
                     DXGI_FORMAT viewFmt2)
{
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = arraySize;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &td, NULL, tex))) return FALSE;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ZeroMemory(&vd, sizeof vd);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipLevels = 1;
    vd.Texture2DArray.ArraySize = arraySize;
    vd.Format = viewFmt;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)*tex, &vd, srv))) return FALSE;
    if (srv2) {
        vd.Format = viewFmt2;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)*tex, &vd, srv2))) return FALSE;
    }
    return TRUE;
}

static BOOL srv_for_existing(ID3D11Texture2D *tex, UINT arraySize, DXGI_FORMAT f0, DXGI_FORMAT f1,
                             ID3D11ShaderResourceView **s0, ID3D11ShaderResourceView **s1)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ZeroMemory(&vd, sizeof vd);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipLevels = 1;
    vd.Texture2DArray.ArraySize = arraySize;
    vd.Format = f0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)tex, &vd, s0))) return FALSE;
    vd.Format = f1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)tex, &vd, s1))) {
        SP_RELEASE(*s0);
        return FALSE;
    }
    return TRUE;
}

static void upload_plane(ID3D11Texture2D *tex, const uint8_t *src, int linesize, int rowBytes, int rows)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    uint8_t *dst = (uint8_t *)m.pData;
    for (int y = 0; y < rows; y++) memcpy(dst + (size_t)y * m.RowPitch, src + (ptrdiff_t)y * linesize, (size_t)rowBytes);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)tex, 0);
}

static BOOL ensure_upload(VideoSource *s, int kind, int w, int h)
{
    if (s->upKind == kind && s->upW == w && s->upH == h) return TRUE;
    release_upload(s);
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    BOOL ok = FALSE;
    switch (kind) {
    case UP_NV12:
        ok = make_tex(w, h, DXGI_FORMAT_R8_UNORM, 1, 1, &s->upTex[0], &s->upSrv[0], DXGI_FORMAT_R8_UNORM, NULL, 0) &&
             make_tex(cw, ch, DXGI_FORMAT_R8G8_UNORM, 1, 1, &s->upTex[1], &s->upSrv[1], DXGI_FORMAT_R8G8_UNORM, NULL, 0);
        break;
    case UP_I420:
        ok = make_tex(w, h, DXGI_FORMAT_R8_UNORM, 1, 1, &s->upTex[0], &s->upSrv[0], DXGI_FORMAT_R8_UNORM, NULL, 0) &&
             make_tex(cw, ch, DXGI_FORMAT_R8_UNORM, 1, 1, &s->upTex[1], &s->upSrv[1], DXGI_FORMAT_R8_UNORM, NULL, 0) &&
             make_tex(cw, ch, DXGI_FORMAT_R8_UNORM, 1, 1, &s->upTex[2], &s->upSrv[2], DXGI_FORMAT_R8_UNORM, NULL, 0);
        break;
    case UP_BGRA:
        ok = make_tex(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, &s->upTex[0], &s->upSrv[0], DXGI_FORMAT_B8G8R8A8_UNORM, NULL, 0);
        break;
    }
    if (!ok) {
        release_upload(s);
        return FALSE;
    }
    s->upKind = kind;
    s->upW = w;
    s->upH = h;
    return TRUE;
}

/* Software frames: upload planes directly when the layout is shader-friendly, otherwise
 * convert once with libswscale (to 8-bit 4:2:0 for YUV sources, BGRA for RGB/palette sources). */
static BOOL prepare_software(VideoSource *s, AVFrame *f, PlaneSet *ps)
{
    AVFrame *src = f;
    enum AVPixelFormat fmt = (enum AVPixelFormat)f->format;
    if (fmt != AV_PIX_FMT_NV12 && fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P && fmt != AV_PIX_FMT_BGRA) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
        int rgb = d && ((d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) || d->nb_components < 3);
        enum AVPixelFormat dst = rgb ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P;
        s->sws = sws_getCachedContext(s->sws, f->width, f->height, fmt, f->width, f->height, dst, SWS_BILINEAR, NULL, NULL, NULL);
        if (!s->sws) return FALSE;
        if (!s->swsFrame) s->swsFrame = av_frame_alloc();
        if (!s->swsFrame) return FALSE;
        if (s->swsFrame->width != f->width || s->swsFrame->height != f->height || s->swsFrame->format != dst) {
            av_frame_unref(s->swsFrame);
            s->swsFrame->width = f->width;
            s->swsFrame->height = f->height;
            s->swsFrame->format = dst;
            if (av_frame_get_buffer(s->swsFrame, 0) < 0) return FALSE;
        }
        sws_scale(s->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, s->swsFrame->data, s->swsFrame->linesize);
        av_frame_copy_props(s->swsFrame, f);
        src = s->swsFrame;
        fmt = dst;
    }
    int w = src->width, h = src->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (fmt) {
    case AV_PIX_FMT_NV12:
        if (!ensure_upload(s, UP_NV12, w, h)) return FALSE;
        upload_plane(s->upTex[0], src->data[0], src->linesize[0], w, h);
        upload_plane(s->upTex[1], src->data[1], src->linesize[1], cw * 2, ch);
        ps->ps = g.psNV12;
        ps->srv[0] = s->upSrv[0];
        ps->srv[1] = s->upSrv[1];
        ps->yuv = 1;
        break;
    case AV_PIX_FMT_BGRA:
        if (!ensure_upload(s, UP_BGRA, w, h)) return FALSE;
        upload_plane(s->upTex[0], src->data[0], src->linesize[0], w * 4, h);
        ps->ps = g.psRGB;
        ps->srv[4] = s->upSrv[0];
        ps->yuv = 0;
        break;
    default: /* YUV420P / YUVJ420P */
        if (!ensure_upload(s, UP_I420, w, h)) return FALSE;
        upload_plane(s->upTex[0], src->data[0], src->linesize[0], w, h);
        upload_plane(s->upTex[1], src->data[1], src->linesize[1], cw, ch);
        upload_plane(s->upTex[2], src->data[2], src->linesize[2], cw, ch);
        ps->ps = g.psI420;
        ps->srv[0] = s->upSrv[0];
        ps->srv[2] = s->upSrv[1];
        ps->srv[3] = s->upSrv[2];
        ps->yuv = 1;
        break;
    }
    ps->texW = (float)w;
    ps->texH = (float)h;
    ps->slice = 0.0f;
    return TRUE;
}

/* Hardware frames stay on the GPU: sample the decoder's texture array slice directly
 * (zero-copy) or, if the driver refuses shader access to decoder surfaces, copy the slice
 * into an NV12/P010 texture on the GPU. */
static BOOL prepare_hardware(VideoSource *s, AVFrame *f, PlaneSet *ps)
{
    ID3D11Texture2D *tex = (ID3D11Texture2D *)f->data[0];
    UINT slice = (UINT)(intptr_t)f->data[1];
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(tex, &d);
    DXGI_FORMAT f0, f1;
    if (d.Format == DXGI_FORMAT_NV12) { f0 = DXGI_FORMAT_R8_UNORM; f1 = DXGI_FORMAT_R8G8_UNORM; }
    else if (d.Format == DXGI_FORMAT_P010 || d.Format == DXGI_FORMAT_P016) { f0 = DXGI_FORMAT_R16_UNORM; f1 = DXGI_FORMAT_R16G16_UNORM; }
    else return FALSE;

    if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        if (s->hwTex != tex) {
            SP_RELEASE(s->hwSrv[0]);
            SP_RELEASE(s->hwSrv[1]);
            SP_RELEASE(s->hwTex);
            if (!srv_for_existing(tex, d.ArraySize, f0, f1, &s->hwSrv[0], &s->hwSrv[1])) return FALSE;
            s->hwTex = tex;
            ID3D11Texture2D_AddRef(tex);
        }
        ps->srv[0] = s->hwSrv[0];
        ps->srv[1] = s->hwSrv[1];
        ps->slice = (float)slice;
    } else {
        if (!s->copyTex || s->copyW != (int)d.Width || s->copyH != (int)d.Height || s->copyFmt != (int)d.Format) {
            SP_RELEASE(s->copySrv[0]);
            SP_RELEASE(s->copySrv[1]);
            SP_RELEASE(s->copyTex);
            if (!make_tex((int)d.Width, (int)d.Height, d.Format, 0, 1, &s->copyTex, &s->copySrv[0], f0, &s->copySrv[1], f1))
                return FALSE;
            s->copyW = (int)d.Width;
            s->copyH = (int)d.Height;
            s->copyFmt = (int)d.Format;
        }
        ID3D11DeviceContext_CopySubresourceRegion(g.ctx, (ID3D11Resource *)s->copyTex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)tex, slice, NULL);
        ps->srv[0] = s->copySrv[0];
        ps->srv[1] = s->copySrv[1];
        ps->slice = 0.0f;
    }
    ps->ps = g.psNV12;
    ps->texW = (float)d.Width;
    ps->texH = (float)d.Height;
    ps->yuv = 1;
    return TRUE;
}

static BOOL output_make_rtv(VideoOutput *o)
{
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(o->swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return FALSE;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)bb, NULL, &o->rtv);
    ID3D11Texture2D_Release(bb);
    return SUCCEEDED(hr);
}

static void output_apply_resize(VideoOutput *o)
{
    if (!InterlockedExchange(&o->resizePending, 0)) return;
    int w = o->newW, h = o->newH;
    if (w <= 0 || h <= 0) return;
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    SP_RELEASE(o->rtv);
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(o->swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        o->w = w;
        o->h = h;
    } else {
        LOGW(L"video: ResizeBuffers failed 0x%08lX", (unsigned long)hr);
    }
}

static void draw_output(VideoOutput *o, const PlaneSet *ps, const AVFrame *f)
{
    CBParams cb;
    ZeroMemory(&cb, sizeof cb);
    double sar = (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0) ? av_q2d(f->sample_aspect_ratio) : 1.0;
    double da = (double)ps->frameW * sar / (double)SP_MAX(ps->frameH, 1);
    double ta = (double)o->w / (double)SP_MAX(o->h, 1);
    double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (da > ta) { double vis = ta / da; u0 = (1.0 - vis) * 0.5; u1 = u0 + vis; }
    else         { double vis = da / ta; v0 = (1.0 - vis) * 0.5; v1 = v0 + vis; }
    double fx = ps->frameW / ps->texW, fy = ps->frameH / ps->texH;
    cb.uvRect[0] = (float)(u0 * fx);
    cb.uvRect[1] = (float)(v0 * fy);
    cb.uvRect[2] = (float)((u1 - u0) * fx);
    cb.uvRect[3] = (float)((v1 - v0) * fy);
    cb.uvClamp[0] = (float)(fx - 1.0 / ps->texW);
    cb.uvClamp[1] = (float)(fy - 1.0 / ps->texH);
    if (ps->yuv) yuv_matrix(f, &cb);
    cb.off[3] = ps->slice;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof cb);
        ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb, 0);
    }
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)o->w, (float)o->h, 0.0f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &o->rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g.ctx, ps->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 5, (ID3D11ShaderResourceView *const *)ps->srv);
    ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &g.smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_Draw(g.ctx, 3, 0);
    ID3D11ShaderResourceView *nulls[5] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 5, nulls);
}

static void render_frame(VideoSource *s, AVFrame *f, int redraw)
{
    PlaneSet ps;
    ZeroMemory(&ps, sizeof ps);
    ps.frameW = f->width;
    ps.frameH = f->height;
    EnterCriticalSection(&g.renderLock);
    BOOL ok;
    if (f->format == AV_PIX_FMT_D3D11) {
        ok = prepare_hardware(s, f, &ps);
        if (!ok) {   /* unusual surface format: download once and use the software path */
            AVFrame *sw = av_frame_alloc();
            ok = sw && av_hwframe_transfer_data(sw, f, 0) >= 0 && (av_frame_copy_props(sw, f), prepare_software(s, sw, &ps));
            av_frame_free(&sw);
        }
    } else {
        ok = prepare_software(s, f, &ps);
    }
    if (ok) {
        EnterCriticalSection(&s->outLock);
        for (int i = 0; i < s->outputCount; i++) {
            VideoOutput *o = s->outputs[i];
            if (!redraw && o->state != RS_ACTIVE) continue;    /* occluded outputs produce no frames */
            output_apply_resize(o);
            if (!o->rtv && !output_make_rtv(o)) continue;
            draw_output(o, &ps, f);
            HRESULT hr = IDXGISwapChain1_Present(o->swap, 0, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
                InterlockedExchange(&g.deviceLost, 1);
                LeaveCriticalSection(&s->outLock);
                LeaveCriticalSection(&g.renderLock);
                video_source_fail(s, hr, L"The graphics device was reset (0x%08lX)", (unsigned long)hr);
                return;
            }
            if (!o->readyPosted && SUCCEEDED(hr)) {
                o->readyPosted = 1;
                renderer_post(o->notify, o->base.rid, REV_READY, S_OK, NULL);
            }
        }
        LeaveCriticalSection(&s->outLock);
    } else if (s->presented == 0) {
        LeaveCriticalSection(&g.renderLock);
        video_source_fail(s, E_FAIL, L"Unsupported pixel format %hs", av_get_pix_fmt_name((enum AVPixelFormat)f->format));
        return;
    }
    LeaveCriticalSection(&g.renderLock);
}

static unsigned __stdcall present_main(void *arg)
{
    VideoSource *s = (VideoSource *)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus video present");
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    int playing = 0, anchored = 0;
    double pausedAt = 0.0;
    while (!s->quit) {
        if (InterlockedExchange(&s->redraw, 0) && s->current && !s->failed) render_frame(s, s->current, 1);
        int want = s->playing && !s->failed;
        if (!want) {
            if (playing) {
                pausedAt = video_media_now(s);
                playing = 0;
                s->clockValid = 0;
                SetEvent(s->audioWake);
            }
            WaitForSingleObject(s->wake, INFINITE);
            continue;
        }
        if (!playing) {
            playing = 1;
            if (anchored) {
                s->clockBase = time_ms() - pausedAt * 1000.0;
                s->clockValid = 1;
            }
            SetEvent(s->audioWake);
        }
        VFrame f;
        if (!fq_peek(&s->fq, &f)) {
            HANDLE hs[2] = { s->wake, s->fq.pushed };
            WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            continue;
        }
        if (!anchored) {
            s->clockBase = time_ms() - f.pts * 1000.0;
            s->clockValid = 1;
            anchored = 1;
            SetEvent(s->audioWake);
        }
        double now = video_media_now(s);
        double ahead = f.pts - now;
        if (ahead > 0.0015) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(ahead * 1.0e7);
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                HANDLE hs[2] = { s->wake, timer };
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) CancelWaitableTimer(timer);
            } else {
                WaitForSingleObject(s->wake, (DWORD)(ahead * 1000.0) + 1);
            }
            continue;
        }
        if (!fq_pop(&s->fq, &f)) continue;
        VFrame next;
        while (fq_peek(&s->fq, &next) && next.pts <= now) {   /* late: skip to the newest due frame */
            av_frame_free(&f.f);
            fq_pop(&s->fq, &f);
            s->dropped++;
        }
        if (ahead < -0.25) s->clockBase = time_ms() - f.pts * 1000.0;   /* fell far behind: re-anchor */
        render_frame(s, f.f, 0);
        av_frame_free(&s->current);
        s->current = f.f;
        s->currentPts = f.pts;
        s->presented++;
    }
    if (timer) CloseHandle(timer);
    return 0;
}

/* ================================================================ sources & outputs */

static void source_update_flags(VideoSource *s)
{
    LONG play = 0, audio = 0;
    EnterCriticalSection(&s->outLock);
    for (int i = 0; i < s->outputCount; i++) {
        if (s->outputs[i]->state == RS_ACTIVE) play = 1;
        if (s->outputs[i]->audio) audio = 1;
    }
    LeaveCriticalSection(&s->outLock);
    InterlockedExchange(&s->playing, play);
    if (InterlockedExchange(&s->audioWanted, audio) != audio) SetEvent(s->decodeWake);
    SetEvent(s->wake);
    SetEvent(s->audioWake);
}

static VideoSource *source_create(const wchar_t *path)
{
    VideoSource *s = (VideoSource *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->refs = 1;
    str_copy(s->path, SP_COUNTOF(s->path), path);
    s->pathUtf8 = utf8_from_wide(path, NULL);
    s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->audioWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->decodeWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->vidx = s->aidx = -1;
    fq_init(&s->fq);
    pq_init(&s->aq);
    InitializeCriticalSection(&s->outLock);
    if (!s->pathUtf8 || !s->wake || !s->audioWake || !s->decodeWake || !s->fq.pushed) goto fail;
    s->decodeThread = (HANDLE)_beginthreadex(NULL, 0, decode_main, s, 0, NULL);
    s->presentThread = (HANDLE)_beginthreadex(NULL, 0, present_main, s, 0, NULL);
    if (!s->decodeThread || !s->presentThread) goto fail;
    return s;
fail:
    LOGE(L"video: could not start playback threads");
    s->quit = 1;
    fq_abort(&s->fq);
    if (s->wake) SetEvent(s->wake);
    if (s->decodeThread) { WaitForSingleObject(s->decodeThread, 5000); CloseHandle(s->decodeThread); }
    if (s->presentThread) { WaitForSingleObject(s->presentThread, 5000); CloseHandle(s->presentThread); }
    fq_free(&s->fq);
    DeleteCriticalSection(&s->outLock);
    if (s->wake) CloseHandle(s->wake);
    if (s->audioWake) CloseHandle(s->audioWake);
    if (s->decodeWake) CloseHandle(s->decodeWake);
    free(s->pathUtf8);
    free(s);
    return NULL;
}

static void source_destroy(VideoSource *s)
{
    double t0 = time_ms();
    InterlockedExchange(&s->quit, 1);
    fq_abort(&s->fq);
    SetEvent(s->wake);
    SetEvent(s->audioWake);
    SetEvent(s->decodeWake);
    BOOL joined = TRUE;
    HANDLE th[2] = { s->decodeThread, s->presentThread };
    for (int i = 0; i < 2; i++) {
        if (WaitForSingleObject(th[i], 5000) != WAIT_OBJECT_0) joined = FALSE;
        CloseHandle(th[i]);
    }
    if (!joined) {   /* never free state a stuck thread may still touch */
        LOGE(L"video: playback threads did not stop in time; leaking source %ls", path_filename(s->path));
        return;
    }
    EnterCriticalSection(&g.renderLock);
    av_frame_free(&s->current);
    fq_free(&s->fq);
    release_upload(s);
    SP_RELEASE(s->copySrv[0]);
    SP_RELEASE(s->copySrv[1]);
    SP_RELEASE(s->copyTex);
    SP_RELEASE(s->hwSrv[0]);
    SP_RELEASE(s->hwSrv[1]);
    SP_RELEASE(s->hwTex);
    LeaveCriticalSection(&g.renderLock);
    pq_clear(&s->aq);
    sws_freeContext(s->sws);
    av_frame_free(&s->swsFrame);
    av_frame_free(&s->tmp);
    avcodec_free_context(&s->vdec);
    avcodec_free_context(&s->adec);
    avformat_close_input(&s->fmt);
    LOGI(L"video: released %ls (decoded %lld, presented %lld, dropped %lld, avg decode submit %.2f ms) in %.1f ms",
         path_filename(s->path), s->framesDecoded, s->presented, s->dropped,
         s->framesDecoded ? s->decodeMsTotal / (double)s->framesDecoded : 0.0, time_ms() - t0);
    DeleteCriticalSection(&s->outLock);
    CloseHandle(s->wake);
    CloseHandle(s->audioWake);
    CloseHandle(s->decodeWake);
    free(s->pathUtf8);
    free(s);
}

static void vo_set_state(Renderer *base, RenderState st, BOOL keepFrame)
{
    (void)keepFrame;   /* the swap chain keeps its last frame on screen while paused */
    VideoOutput *o = (VideoOutput *)base;
    InterlockedExchange(&o->state, (LONG)st);
    InterlockedExchange(&o->src->redraw, 1);
    source_update_flags(o->src);
}

static void vo_resize(Renderer *base, int w, int h)
{
    VideoOutput *o = (VideoOutput *)base;
    if (w <= 0 || h <= 0 || (w == o->w && h == o->h)) return;
    InterlockedExchange(&o->newW, w);
    InterlockedExchange(&o->newH, h);
    InterlockedExchange(&o->resizePending, 1);
    InterlockedExchange(&o->src->redraw, 1);
    SetEvent(o->src->wake);
}

static void vo_set_audio(Renderer *base, BOOL on)
{
    VideoOutput *o = (VideoOutput *)base;
    o->audio = on ? 1 : 0;
    source_update_flags(o->src);
}

static BOOL vo_paint(Renderer *base, HDC dc, const RECT *rc)
{
    (void)base; (void)dc; (void)rc;
    return TRUE;   /* content comes from the swap chain */
}

static void vo_describe(Renderer *base, wchar_t *buf, size_t cap)
{
    VideoOutput *o = (VideoOutput *)base;
    VideoSource *s = o->src;
    const wchar_t *mode = s->hwMode == HW_ZEROCOPY ? L"D3D11VA zero-copy"
                        : s->hwMode == HW_COPY ? L"D3D11VA + GPU copy" : L"software decode";
    str_printf(buf, cap, L"FFmpeg %hs · %ls %dx%d · %ls · %ls%ls", FFMPEG_VERSION, s->codec, s->width, s->height, mode,
               o->flipModel ? L"flip swap chain" : L"blt swap chain", s->outputCount > 1 ? L" · shared decoder" : L"");
}

static void vo_destroy(Renderer *base)
{
    VideoOutput *o = (VideoOutput *)base;
    VideoSource *s = o->src;
    AcquireSRWLockExclusive(&g.lock);
    EnterCriticalSection(&g.renderLock);
    EnterCriticalSection(&s->outLock);
    for (int i = 0; i < s->outputCount; i++) {
        if (s->outputs[i] == o) {
            s->outputs[i] = s->outputs[--s->outputCount];
            break;
        }
    }
    int remaining = s->outputCount;
    LeaveCriticalSection(&s->outLock);
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    SP_RELEASE(o->rtv);
    SP_RELEASE(o->swap);
    LeaveCriticalSection(&g.renderLock);
    if (remaining == 0) {
        for (int i = 0; i < g.sourceCount; i++) {
            if (g.sources[i] == s) {
                g.sources[i] = g.sources[--g.sourceCount];
                break;
            }
        }
        ReleaseSRWLockExclusive(&g.lock);
        source_destroy(s);
    } else {
        ReleaseSRWLockExclusive(&g.lock);
        source_update_flags(s);
    }
    free(o);
}

static const RendererVtbl k_videoVtbl = { vo_set_state, vo_resize, vo_set_audio, vo_paint, vo_describe, vo_destroy, NULL, NULL, NULL, NULL };

static BOOL create_swapchain(VideoOutput *o, wchar_t *err, size_t cap)
{
    DXGI_SWAP_CHAIN_DESC1 d;
    ZeroMemory(&d, sizeof d);
    d.Width = (UINT)o->w;
    d.Height = (UINT)o->h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    /* Flip model is required: on the Windows 11 24H2+ desktop (layered child of Progman) only
     * flip-model presents are composed (verified); blt presents and GDI stay invisible there. */
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(g.factory, (IUnknown *)g.dev, o->hwnd, &d, NULL, NULL, &o->swap);
    o->flipModel = SUCCEEDED(hr);
    if (FAILED(hr)) {
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        d.BufferCount = 1;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(g.factory, (IUnknown *)g.dev, o->hwnd, &d, NULL, NULL, &o->swap);
    }
    if (FAILED(hr)) {
        str_printf(err, cap, L"Could not create a swap chain (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    IDXGIFactory2_MakeWindowAssociation(g.factory, o->hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    return TRUE;
}

Renderer *video_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap)
{
    if (!video_ffmpeg_available(err, errCap)) return NULL;
    if (!file_exists(ctx->wp->path)) {
        str_copy(err, errCap, L"Video file not found");
        return NULL;
    }
    VideoOutput *o = (VideoOutput *)calloc(1, sizeof *o);
    if (!o) {
        str_copy(err, errCap, L"Out of memory");
        return NULL;
    }
    o->base.vt = &k_videoVtbl;
    o->base.rid = ctx->rid;
    o->base.type = WP_VIDEO;
    o->hwnd = ctx->hwnd;
    o->notify = ctx->notify;
    o->w = SP_MAX(ctx->width, 1);
    o->h = SP_MAX(ctx->height, 1);
    o->state = RS_ACTIVE;
    o->audio = ctx->audio ? 1 : 0;
    o->layered = ctx->layered ? 1 : 0;

    AcquireSRWLockExclusive(&g.lock);
    if (!device_init(err, errCap) || !create_swapchain(o, err, errCap)) {
        ReleaseSRWLockExclusive(&g.lock);
        free(o);
        return NULL;
    }
    ensure_hwdev();
    VideoSource *s = NULL;
    for (int i = 0; i < g.sourceCount; i++)
        if (!g.sources[i]->failed && str_ieq(g.sources[i]->path, ctx->wp->path)) s = g.sources[i];
    if (!s) {
        if (g.sourceCount >= SP_MAX_MONITORS || !(s = source_create(ctx->wp->path))) {
            ReleaseSRWLockExclusive(&g.lock);
            SP_RELEASE(o->swap);
            free(o);
            str_copy(err, errCap, L"Could not start video playback");
            return NULL;
        }
        g.sources[g.sourceCount++] = s;
    } else {
        LOGI(L"video: sharing decoder for %ls with another display", path_filename(s->path));
    }
    o->src = s;
    EnterCriticalSection(&s->outLock);
    s->outputs[s->outputCount++] = o;
    LeaveCriticalSection(&s->outLock);
    ReleaseSRWLockExclusive(&g.lock);
    InterlockedExchange(&s->redraw, 1);
    source_update_flags(s);
    return &o->base;
}

void video_shutdown(void)
{
    AcquireSRWLockExclusive(&g.lock);
    if (g.sourceCount == 0) device_release();
    ReleaseSRWLockExclusive(&g.lock);
}

/* ================================================================ static-content presenter
 * Images and HTML pause snapshots are presented through the same D3D11 device with a
 * flip-model swap chain (required on the 24H2+ layered desktop host). Content is presented
 * once; DWM keeps showing the last frame, so no loop and no CPU copy is retained. */

BOOL gfx_texture_from_bgra(const void *bits, int w, int h, struct ID3D11Texture2D **tex, wchar_t *err, size_t cap)
{
    *tex = NULL;
    AcquireSRWLockExclusive(&g.lock);
    BOOL ok = device_init(err, cap);
    ID3D11Device *dev = g.dev;
    if (ok) ID3D11Device_AddRef(dev);
    ReleaseSRWLockExclusive(&g.lock);
    if (!ok) return FALSE;
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { bits, (UINT)w * 4, 0 };
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, &sd, tex);
    ID3D11Device_Release(dev);
    if (FAILED(hr)) {
        str_printf(err, cap, L"Could not create a %dx%d texture (0x%08lX)", w, h, (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

HRESULT gfx_present(HWND hwnd, struct IDXGISwapChain1 **swap, struct ID3D11Texture2D *tex, int w, int h)
{
    HRESULT hr = S_OK;
    AcquireSRWLockExclusive(&g.lock);
    if (!g.dev) {
        ReleaseSRWLockExclusive(&g.lock);
        return E_FAIL;
    }
    EnterCriticalSection(&g.renderLock);
    if (!*swap) {
        DXGI_SWAP_CHAIN_DESC1 d;
        ZeroMemory(&d, sizeof d);
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(g.factory, (IUnknown *)g.dev, hwnd, &d, NULL, NULL, swap);
        if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(g.factory, hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    } else {
        DXGI_SWAP_CHAIN_DESC1 cur;
        if (SUCCEEDED(IDXGISwapChain1_GetDesc1(*swap, &cur)) && (cur.Width != (UINT)w || cur.Height != (UINT)h)) {
            ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
            hr = IDXGISwapChain1_ResizeBuffers(*swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        }
    }
    if (SUCCEEDED(hr)) {
        ID3D11Texture2D *bb = NULL;
        hr = IDXGISwapChain1_GetBuffer(*swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)bb, (ID3D11Resource *)tex);
            ID3D11Texture2D_Release(bb);
            hr = IDXGISwapChain1_Present(*swap, 0, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) InterlockedExchange(&g.deviceLost, 1);
            else if (hr == DXGI_STATUS_OCCLUDED) hr = S_OK;
        }
    }
    LeaveCriticalSection(&g.renderLock);
    ReleaseSRWLockExclusive(&g.lock);
    return hr;
}

void gfx_release(struct IDXGISwapChain1 **swap)
{
    if (!*swap) return;
    AcquireSRWLockExclusive(&g.lock);
    if (g.ctx) {
        EnterCriticalSection(&g.renderLock);
        ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
        IDXGISwapChain1_Release(*swap);
        LeaveCriticalSection(&g.renderLock);
    } else {
        IDXGISwapChain1_Release(*swap);
    }
    *swap = NULL;
    ReleaseSRWLockExclusive(&g.lock);
}
