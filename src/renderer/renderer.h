/* Renderer interface. One renderer instance drives one host window (one monitor slot).
 * All vtable calls happen on the engine's window thread. Renderers report asynchronous
 * results by posting WM_SP_RENDER_EVENT (lParam = heap RenderEvent*, engine frees it). */
#pragma once
#include "../common/common.h"
#include "../settings/config.h"

typedef struct Renderer Renderer;

typedef struct RendererContext {
    HWND                  hwnd;      /* host window (child of the desktop host) */
    HWND                  notify;    /* engine control window */
    uint32_t              rid;       /* unique id used to match events to slots */
    const WallpaperEntry *wp;
    int                   width;     /* client size, physical pixels */
    int                   height;
    UINT                  dpi;
    UINT                  refreshHz;
    BOOL                  audio;     /* this renderer may produce audio */
    BOOL                  layered;   /* host window is WS_EX_LAYERED (24H2+ Progman host): use blt presents */
    double                rasterScale; /* HTML only, 0: follow the monitor; preview capture renders at a fixed scale */
} RendererContext;

/* Interactive wallpapers: engine-provided data and desktop mouse input. */
enum { RENDER_WANT_AUDIO = 1, RENDER_WANT_SYSINFO = 2, RENDER_WANT_INPUT = 4, RENDER_WANT_TRACK = 8 };
enum { RENDER_FEED_AUDIO = 1, RENDER_FEED_SYSINFO = 2, RENDER_FEED_TRACK = 3 };   /* TRACK: JSON text, quoted by the renderer */
enum { RIN_MOVE = 0, RIN_DOWN, RIN_UP, RIN_WHEEL, RIN_LEAVE };

typedef struct RendererInput {
    int kind;      /* RIN_* */
    int x, y;      /* physical pixels relative to the host window */
    int button;    /* 0 left, 1 right, 2 middle */
    int buttons;   /* bitmask of pressed buttons: 1 left, 2 right, 4 middle */
    int wheel;     /* wheel delta (WHEEL_DELTA units) */
} RendererInput;

typedef struct RendererVtbl {
    /* keepFrame: the desktop stays visible while suspended, so the last frame must remain. */
    void (*set_state)(Renderer *r, RenderState st, BOOL keepFrame);
    void (*resize)(Renderer *r, int w, int h);
    void (*set_audio)(Renderer *r, BOOL on);
    BOOL (*paint)(Renderer *r, HDC dc, const RECT *rc);   /* GDI content for the host window */
    void (*describe)(Renderer *r, wchar_t *buf, size_t cap);
    void (*destroy)(Renderer *r);
    /* optional (NULL when unsupported) */
    int  (*wants)(Renderer *r);                                  /* RENDER_WANT_* */
    void (*input)(Renderer *r, const RendererInput *in);
    void (*feed)(Renderer *r, int what, const char *payload);    /* RENDER_FEED_* (UTF-8 JS literal) */
    void (*on_timer)(Renderer *r, UINT_PTR id);                  /* WM_TIMER on the host window */
    void (*apply)(Renderer *r);                                  /* customise panel changed: reload properties + grading */
} RendererVtbl;

struct Renderer {
    const RendererVtbl *vt;
    uint32_t            rid;
    WallpaperType       type;
};

#define WM_SP_RENDER_EVENT (WM_APP + 40)
#define WM_SP_HTML_GRAB    (WM_APP + 41)   /* html_grab result on the host window: wParam HRESULT, lParam IStream* (JPEG) or NULL */

typedef enum RenderEventCode {
    REV_READY  = 1,   /* first frame available: host window may be shown */
    REV_FAILED = 2,   /* renderer failed; engine decides recovery */
    REV_INFO   = 3,   /* informational (logged) */
    REV_WANTS  = 4    /* the renderer's data needs (RENDER_WANT_*) changed */
} RenderEventCode;

typedef struct RenderEvent {
    uint32_t rid;
    int      code;
    HRESULT  hr;
    wchar_t  msg[SP_ERR_LEN];
} RenderEvent;

void renderer_post(HWND notify, uint32_t rid, int code, HRESULT hr, const wchar_t *fmt, ...);

/* Factories. On failure return NULL and write a user-facing reason into err. */
Renderer *image_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap);
Renderer *video_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap);
Renderer *html_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap);

/* Process-wide subsystems (engine process only). */
BOOL video_ffmpeg_available(wchar_t *err, size_t cap);
void video_ffmpeg_version(wchar_t *buf, size_t cap);
void video_shutdown(void);
BOOL html_runtime_version(wchar_t *buf, size_t cap);
void html_shutdown(void);
/* Preview capture process: own WebView2 profile, pages keep rendering while off-screen. */
void html_capture_mode(const wchar_t *userDataFolder);
/* Screensaver process: its own WebView2 profile, so it never depends on the engine's browser. */
void html_set_profile(const wchar_t *userDataFolder);
/* Grabs the loaded page as JPEG; the result arrives as WM_SP_HTML_GRAB (receiver releases the stream). */
BOOL html_grab(Renderer *r);

/* Shared WIC helper: decode (file path or stream), apply EXIF orientation and scale-to-cover
 * into a top-down 32-bpp DIB of exactly w x h. Caller owns the HBITMAP. */
HBITMAP image_decode_cover(const wchar_t *path, IStream *stream, int w, int h, HRESULT *hrOut);
struct IWICBitmapFrameDecode;
UINT image_exif_orientation(struct IWICBitmapFrameDecode *frame);   /* 1..8 (1: upright or unknown) */
int  image_orientation_transform(UINT orientation);                /* WICBitmapTransformOptions that makes it upright */

/* Shared D3D11 presenter for static content (video_renderer.c). Flip-model swap chain on the
 * host window; the texture can be released after a successful present. Thread-safe. */
struct ID3D11Texture2D;
struct IDXGISwapChain1;
BOOL    gfx_texture_from_bgra(const void *bits, int w, int h, struct ID3D11Texture2D **tex, wchar_t *err, size_t cap);
struct WallpaperAdjust;
HRESULT gfx_present(HWND hwnd, struct IDXGISwapChain1 **swap, struct ID3D11Texture2D *tex, int w, int h,
                    const struct WallpaperAdjust *adj);   /* NULL: no colour grading */
void    gfx_release(struct IDXGISwapChain1 **swap);
