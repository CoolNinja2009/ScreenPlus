/* HTML/CSS/JS wallpapers via the WebView2 COM API (C bindings).
 *
 * Isolation: each wallpaper folder is exposed through a virtual host mapping
 * (https://wallpaper.screenplus.example/...) with cross-origin access denied. Dev tools,
 * context menus, host objects, web messaging, script dialogs, popups, permissions and
 * top-level navigation away from the wallpaper are all disabled. Wallpaper scripts are
 * treated as untrusted and have no channel to Screen+.
 *
 * Suspension: OCCLUDED hides the WebView (Chromium stops producing frames and throttles
 * timers); SUSPENDED additionally calls TrySuspend (page frozen: timers, animations and
 * media stop) and lowers the memory target. When the desktop stays visible while suspended,
 * a JPEG snapshot of the page is shown in the host window instead. One environment (browser
 * process) is shared by all HTML wallpapers.
 *
 * Interactivity: desktop mouse input arrives from the engine (raw input, only while the
 * cursor is over the desktop) and is injected with the DevTools protocol
 * (Input.dispatchMouseEvent): trusted events with :hover/isTrusted semantics and no focus
 * changes. Lively / Wallpaper Engine APIs are provided by wallpaper_api.c. Keyboard input is
 * never forwarded. */
#include "../renderer.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include "../../common/paths.h"
#include "../../common/json.h"
#include "wallpaper_api.h"
#include <WebView2.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>

#define WALLPAPER_HOST L"wallpaper.screenplus.example"
#define WALLPAPER_ORIGIN L"https://" WALLPAPER_HOST L"/"

typedef struct HtmlRenderer HtmlRenderer;

/* ------------------------------------------------------------ generic COM callback objects */

typedef HRESULT (*CbFn)(HtmlRenderer *r, void *a, void *b);

typedef struct Callback {
    const void   *lpVtbl;
    volatile LONG refs;
    const IID    *iid;
    HtmlRenderer *owner;
    CbFn          fn;
} Callback;

typedef struct VtblHrPtr {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Callback *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Callback *);
    ULONG   (STDMETHODCALLTYPE *Release)(Callback *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Callback *, HRESULT, void *);
} VtblHrPtr;

typedef struct VtblPtrPtr {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Callback *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Callback *);
    ULONG   (STDMETHODCALLTYPE *Release)(Callback *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Callback *, void *, void *);
} VtblPtrPtr;

typedef struct VtblHrBool {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Callback *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Callback *);
    ULONG   (STDMETHODCALLTYPE *Release)(Callback *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Callback *, HRESULT, BOOL);
} VtblHrBool;

typedef struct VtblHr {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Callback *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Callback *);
    ULONG   (STDMETHODCALLTYPE *Release)(Callback *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Callback *, HRESULT);
} VtblHr;

static void html_addref(HtmlRenderer *r);
static void html_release(HtmlRenderer *r);

static HRESULT STDMETHODCALLTYPE cb_qi(Callback *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, This->iid)) {
        *ppv = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cb_addref(Callback *This) { return (ULONG)InterlockedIncrement(&This->refs); }

static ULONG STDMETHODCALLTYPE cb_release(Callback *This)
{
    LONG n = InterlockedDecrement(&This->refs);
    if (n == 0) {
        if (This->owner) html_release(This->owner);
        free(This);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE inv_hr_ptr(Callback *This, HRESULT hr, void *p) { return This->fn(This->owner, (void *)(intptr_t)hr, p); }
static HRESULT STDMETHODCALLTYPE inv_ptr_ptr(Callback *This, void *a, void *b) { return This->fn(This->owner, a, b); }
static HRESULT STDMETHODCALLTYPE inv_hr_bool(Callback *This, HRESULT hr, BOOL ok) { return This->fn(This->owner, (void *)(intptr_t)hr, (void *)(intptr_t)ok); }
static HRESULT STDMETHODCALLTYPE inv_hr(Callback *This, HRESULT hr) { return This->fn(This->owner, (void *)(intptr_t)hr, NULL); }

static const VtblHrPtr  k_vtHrPtr  = { cb_qi, cb_addref, cb_release, inv_hr_ptr };
static const VtblPtrPtr k_vtPtrPtr = { cb_qi, cb_addref, cb_release, inv_ptr_ptr };
static const VtblHrBool k_vtHrBool = { cb_qi, cb_addref, cb_release, inv_hr_bool };
static const VtblHr     k_vtHr     = { cb_qi, cb_addref, cb_release, inv_hr };

static HRESULT noop_fn(HtmlRenderer *r, void *a, void *b) { (void)r; (void)a; (void)b; return S_OK; }
/* Shared completion handlers for fire-and-forget calls; their refcount never reaches zero. */
static Callback g_noopScript = { &k_vtHrPtr, 1 << 30, &IID_ICoreWebView2ExecuteScriptCompletedHandler, NULL, noop_fn };
static Callback g_noopCdp = { &k_vtHrPtr, 1 << 30, &IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, NULL, noop_fn };
static Callback g_noopAdd = { &k_vtHrPtr, 1 << 30, &IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, NULL, noop_fn };

static Callback *cb_new(const void *vtbl, const IID *iid, HtmlRenderer *owner, CbFn fn)
{
    Callback *c = (Callback *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->lpVtbl = vtbl;
    c->refs = 1;
    c->iid = iid;
    c->owner = owner;
    c->fn = fn;
    if (owner) html_addref(owner);
    return c;
}

/* ------------------------------------------------------------ shared environment */

#define MAX_WAITERS 16

static struct {
    ICoreWebView2Environment *env;
    int                       creating;
    HtmlRenderer             *waiters[MAX_WAITERS];
    int                       waiterCount;
} g_env;

/* ------------------------------------------------------------ renderer */

struct HtmlRenderer {
    Renderer                  base;
    volatile LONG             refs;
    int                       closed;
    HWND                      hwnd;
    HWND                      notify;
    int                       w, h;
    wchar_t                   folder[SP_MAX_PATH];
    wchar_t                   url[SP_MAX_PATH + 64];
    ICoreWebView2Controller  *ctl;
    ICoreWebView2            *wv;
    EventRegistrationToken    tNavStart, tNavDone, tFail, tNewWin, tPerm, tDownload;
    int                       hasTokens, hasDownloadToken;
    WallpaperEntry            wp;          /* copy: format, flags, id, path */
    int                       isUrl;
    double                    scale;       /* physical px per CSS px */
    int                       wantMask;    /* page probe: 1 audio, 2 system info, 4 current track */
    int                       pausedNotified;
    RenderState               state;
    int                       keepFrame;
    int                       audio;
    int                       readyPosted;
    int                       suspendGen;
    HBITMAP                   snapshot;    /* GDI fallback for the pause snapshot */
    IDXGISwapChain1          *snapSwap;    /* pause snapshot presented on the host window */
    IStream                  *capture;
    IStream                  *grab;        /* html_grab in flight */
    double                    raster;      /* fixed rasterization scale (preview capture), 0: monitor */
};

static void html_addref(HtmlRenderer *r) { InterlockedIncrement(&r->refs); }

static void html_release(HtmlRenderer *r)
{
    if (InterlockedDecrement(&r->refs) == 0) {
        if (r->snapshot) DeleteObject(r->snapshot);
        SP_RELEASE(r->capture);
        SP_RELEASE(r->grab);
        free(r);
    }
}

static void fail(HtmlRenderer *r, HRESULT hr, const wchar_t *msg)
{
    if (r->closed) return;
    renderer_post(r->notify, r->base.rid, REV_FAILED, hr, L"%ls", msg);
}

#define TIMER_PROBE_1 0x5101
#define TIMER_PROBE_2 0x5102

static void run_js(HtmlRenderer *r, const wchar_t *js)
{
    if (r->wv && !r->closed)
        ICoreWebView2_ExecuteScript(r->wv, js, (ICoreWebView2ExecuteScriptCompletedHandler *)&g_noopScript);
}

static void run_js_utf8(HtmlRenderer *r, const char *js)
{
    wchar_t *w = js ? wide_from_utf8(js, -1) : NULL;
    if (w) {
        run_js(r, w);
        free(w);
    }
}

static void notify_playback(HtmlRenderer *r, int paused)
{
    if (!r->readyPosted || r->pausedNotified == paused) return;
    r->pausedNotified = paused;
    run_js(r, paused ? L"window.__sp&&__sp.playback(true)" : L"window.__sp&&__sp.playback(false)");
}

static HRESULT on_probe(HtmlRenderer *r, void *hrv, void *resultv)
{
    if (r->closed || FAILED((HRESULT)(intptr_t)hrv) || !resultv) return S_OK;
    int m = _wtoi((const wchar_t *)resultv) & 7;
    if (m != r->wantMask) {
        r->wantMask = m;
        LOGI(L"html: page APIs: audio=%d systeminfo=%d track=%d", (m & 1) != 0, (m & 2) != 0, (m & 4) != 0);
        renderer_post(r->notify, r->base.rid, REV_WANTS, S_OK, NULL);
    }
    return S_OK;
}

static void probe_apis(HtmlRenderer *r)
{
    if (!r->wv || r->closed) return;
    Callback *cb = cb_new(&k_vtHrPtr, &IID_ICoreWebView2ExecuteScriptCompletedHandler, r, on_probe);
    if (!cb) return;
    ICoreWebView2_ExecuteScript(r->wv, L"(window.__sp?window.__sp.wants():0)", (ICoreWebView2ExecuteScriptCompletedHandler *)cb);
    cb_release(cb);
}

static void apply_properties(HtmlRenderer *r)
{
    char *js = wpapi_properties_js(&r->wp);
    if (js) {
        LOGI(L"html: applying %ls properties", r->wp.format == WPF_LIVELY ? L"Lively" : L"Wallpaper Engine");
        run_js_utf8(r, js);
        free(js);
    }
}

static void apply_audio(HtmlRenderer *r)
{
    ICoreWebView2_8 *w8 = NULL;
    if (r->wv && SUCCEEDED(ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_8, (void **)&w8))) {
        ICoreWebView2_8_put_IsMuted(w8, r->audio ? FALSE : TRUE);
        ICoreWebView2_8_Release(w8);
    }
}

static void set_memory_target(HtmlRenderer *r, int low)
{
    ICoreWebView2_19 *w19 = NULL;
    if (r->wv && SUCCEEDED(ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_19, (void **)&w19))) {
        ICoreWebView2_19_put_MemoryUsageTargetLevel(w19, low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
                                                             : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        ICoreWebView2_19_Release(w19);
    }
}

static HRESULT on_suspended(HtmlRenderer *r, void *hrv, void *okv)
{
    (void)hrv;
    if (!r->closed && !(BOOL)(intptr_t)okv) LOGD(L"html: TrySuspend declined");
    return S_OK;
}

static void deep_suspend(HtmlRenderer *r)
{
    ICoreWebView2Controller_put_IsVisible(r->ctl, FALSE);
    ICoreWebView2_3 *w3 = NULL;
    if (SUCCEEDED(ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_3, (void **)&w3))) {
        Callback *cb = cb_new(&k_vtHrBool, &IID_ICoreWebView2TrySuspendCompletedHandler, r, on_suspended);
        if (cb) {
            ICoreWebView2_3_TrySuspend(w3, (ICoreWebView2TrySuspendCompletedHandler *)cb);
            cb_release(cb);
        }
        ICoreWebView2_3_Release(w3);
    }
    set_memory_target(r, 1);
}

static HRESULT on_captured(HtmlRenderer *r, void *hrv, void *unused)
{
    (void)unused;
    HRESULT hr = (HRESULT)(intptr_t)hrv;
    if (r->closed) return S_OK;
    if (SUCCEEDED(hr) && r->capture && r->state == RS_SUSPENDED) {
        LARGE_INTEGER zero = { 0 };
        IStream_Seek(r->capture, zero, STREAM_SEEK_SET, NULL);
        HRESULT dh = S_OK;
        HBITMAP bmp = image_decode_cover(NULL, r->capture, r->w, r->h, &dh);
        if (bmp) {
            /* Present the frozen frame on the host window (flip model, visible on every desktop
             * host); the WebView is hidden afterwards so this frame stays on screen. */
            DIBSECTION ds;
            ID3D11Texture2D *tex = NULL;
            wchar_t err[SP_ERR_LEN];
            if (GetObjectW(bmp, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBits &&
                gfx_texture_from_bgra(ds.dsBm.bmBits, r->w, r->h, &tex, err, SP_COUNTOF(err)) &&
                SUCCEEDED(gfx_present(r->hwnd, &r->snapSwap, tex, r->w, r->h))) {
                DeleteObject(bmp);
            } else {
                if (r->snapshot) DeleteObject(r->snapshot);
                r->snapshot = bmp;
                InvalidateRect(r->hwnd, NULL, FALSE);
            }
            SP_RELEASE(tex);
        }
    }
    SP_RELEASE(r->capture);
    if (r->state == RS_SUSPENDED && r->ctl) deep_suspend(r);
    return S_OK;
}

static void apply_state(HtmlRenderer *r)
{
    if (!r->ctl || r->closed) return;
    switch (r->state) {
    case RS_ACTIVE:
        set_memory_target(r, 0);
        ICoreWebView2Controller_put_IsVisible(r->ctl, TRUE);   /* also resumes a suspended page */
        notify_playback(r, 0);
        if (r->snapshot) {
            DeleteObject(r->snapshot);
            r->snapshot = NULL;
        }
        gfx_release(&r->snapSwap);   /* the live page covers the host window again */
        break;
    case RS_OCCLUDED:
        notify_playback(r, 1);
        ICoreWebView2Controller_put_IsVisible(r->ctl, FALSE);
        break;
    case RS_SUSPENDED:
        notify_playback(r, 1);
        if (r->keepFrame && r->readyPosted && !r->capture) {
            /* desktop stays visible: freeze on a snapshot of the current page */
            if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &r->capture))) {
                Callback *cb = cb_new(&k_vtHr, &IID_ICoreWebView2CapturePreviewCompletedHandler, r, on_captured);
                HRESULT hr = cb ? ICoreWebView2_CapturePreview(r->wv, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG,
                                                               r->capture, (ICoreWebView2CapturePreviewCompletedHandler *)cb)
                                : E_OUTOFMEMORY;
                if (cb) cb_release(cb);
                if (SUCCEEDED(hr)) return;
                SP_RELEASE(r->capture);
            }
        }
        if (!r->capture) deep_suspend(r);
        break;
    default:
        ICoreWebView2Controller_put_IsVisible(r->ctl, FALSE);
        break;
    }
}

/* ---- WebView events */

static HRESULT on_nav_starting(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)sender;
    ICoreWebView2NavigationStartingEventArgs *args = (ICoreWebView2NavigationStartingEventArgs *)argsv;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) && uri) {
        int allowed = str_istarts(uri, L"about:blank") ||
                      (r->isUrl ? (str_istarts(uri, L"https://") || str_istarts(uri, L"http://"))
                                : str_istarts(uri, WALLPAPER_ORIGIN));
        if (!allowed) {
            ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, TRUE);
            if (!r->closed) LOGW(L"html: blocked navigation away from the wallpaper (%.120ls)", uri);
        }
        CoTaskMemFree(uri);
    }
    return S_OK;
}

static HRESULT on_nav_completed(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)sender;
    if (r->closed) return S_OK;
    ICoreWebView2NavigationCompletedEventArgs *args = (ICoreWebView2NavigationCompletedEventArgs *)argsv;
    BOOL ok = FALSE;
    ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(args, &ok);
    if (!ok) {
        COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        ICoreWebView2NavigationCompletedEventArgs_get_WebErrorStatus(args, &st);
        if (st == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
        wchar_t msg[128];
        str_printf(msg, SP_COUNTOF(msg), L"The wallpaper page failed to load (WebView2 error %d)", (int)st);
        fail(r, E_FAIL, msg);
        return S_OK;
    }
    if (!r->readyPosted) {
        r->readyPosted = 1;
        LOGI(L"html: page loaded");
        renderer_post(r->notify, r->base.rid, REV_READY, S_OK, NULL);
        apply_state(r);
    }
    /* (Re)applied after every successful navigation, e.g. when the page reloads itself. */
    apply_properties(r);
    probe_apis(r);
    SetTimer(r->hwnd, TIMER_PROBE_1, 2500, NULL);   /* listeners registered after async init */
    SetTimer(r->hwnd, TIMER_PROBE_2, 8000, NULL);
    return S_OK;
}

static HRESULT on_process_failed(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)sender;
    if (r->closed) return S_OK;
    ICoreWebView2ProcessFailedEventArgs *args = (ICoreWebView2ProcessFailedEventArgs *)argsv;
    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
    ICoreWebView2ProcessFailedEventArgs_get_ProcessFailedKind(args, &kind);
    switch (kind) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
        SP_RELEASE(g_env.env);    /* environment is dead: the next renderer creates a new one */
        fail(r, E_FAIL, L"The WebView2 browser process exited");
        break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        fail(r, E_FAIL, L"The wallpaper page crashed (renderer process exited)");
        break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
        fail(r, E_FAIL, L"The wallpaper page stopped responding");
        break;
    default:
        LOGW(L"html: WebView2 helper process failed (kind %d); WebView2 recovers it automatically", (int)kind);
        break;
    }
    return S_OK;
}

static HRESULT on_new_window(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)r; (void)sender;
    ICoreWebView2NewWindowRequestedEventArgs_put_Handled((ICoreWebView2NewWindowRequestedEventArgs *)argsv, TRUE);
    return S_OK;
}

static HRESULT on_permission(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)r; (void)sender;
    ICoreWebView2PermissionRequestedEventArgs_put_State((ICoreWebView2PermissionRequestedEventArgs *)argsv,
                                                        COREWEBVIEW2_PERMISSION_STATE_DENY);
    return S_OK;
}

static HRESULT on_download(HtmlRenderer *r, void *sender, void *argsv)
{
    (void)sender;
    ICoreWebView2DownloadStartingEventArgs_put_Cancel((ICoreWebView2DownloadStartingEventArgs *)argsv, TRUE);
    if (!r->closed) LOGW(L"html: blocked a download started by the wallpaper");
    return S_OK;
}

static void configure(HtmlRenderer *r)
{
    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(r->wv, &st))) {
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreHostObjectsAllowed(st, FALSE);
        ICoreWebView2Settings_put_IsWebMessageEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsBuiltInErrorPageEnabled(st, FALSE);
        ICoreWebView2Settings8 *s8 = NULL;
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(st, &IID_ICoreWebView2Settings8, (void **)&s8))) {
            ICoreWebView2Settings8_put_IsReputationCheckingRequired(s8, FALSE);   /* no SmartScreen calls */
            ICoreWebView2Settings8_Release(s8);
        }
        ICoreWebView2Settings_Release(st);
    }
    ICoreWebView2Controller2 *c2 = NULL;
    if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(r->ctl, &IID_ICoreWebView2Controller2, (void **)&c2))) {
        COREWEBVIEW2_COLOR black = { 255, 0, 0, 0 };
        ICoreWebView2Controller2_put_DefaultBackgroundColor(c2, black);
        ICoreWebView2Controller2_Release(c2);
    }
    struct { CbFn fn; int kind; } evs[] = {
        { on_nav_starting, 0 }, { on_nav_completed, 1 }, { on_process_failed, 2 }, { on_new_window, 3 }, { on_permission, 4 }
    };
    for (size_t i = 0; i < SP_COUNTOF(evs); i++) {
        static const IID *iids[] = { &IID_ICoreWebView2NavigationStartingEventHandler, &IID_ICoreWebView2NavigationCompletedEventHandler,
                                     &IID_ICoreWebView2ProcessFailedEventHandler, &IID_ICoreWebView2NewWindowRequestedEventHandler,
                                     &IID_ICoreWebView2PermissionRequestedEventHandler };
        Callback *cb = cb_new(&k_vtPtrPtr, iids[i], r, evs[i].fn);
        if (!cb) continue;
        switch (evs[i].kind) {
        case 0: ICoreWebView2_add_NavigationStarting(r->wv, (ICoreWebView2NavigationStartingEventHandler *)cb, &r->tNavStart); break;
        case 1: ICoreWebView2_add_NavigationCompleted(r->wv, (ICoreWebView2NavigationCompletedEventHandler *)cb, &r->tNavDone); break;
        case 2: ICoreWebView2_add_ProcessFailed(r->wv, (ICoreWebView2ProcessFailedEventHandler *)cb, &r->tFail); break;
        case 3: ICoreWebView2_add_NewWindowRequested(r->wv, (ICoreWebView2NewWindowRequestedEventHandler *)cb, &r->tNewWin); break;
        case 4: ICoreWebView2_add_PermissionRequested(r->wv, (ICoreWebView2PermissionRequestedEventHandler *)cb, &r->tPerm); break;
        }
        cb_release(cb);
    }
    r->hasTokens = 1;
    ICoreWebView2_4 *w4 = NULL;
    if (SUCCEEDED(ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_4, (void **)&w4))) {
        Callback *cb = cb_new(&k_vtPtrPtr, &IID_ICoreWebView2DownloadStartingEventHandler, r, on_download);
        if (cb) {
            r->hasDownloadToken = SUCCEEDED(ICoreWebView2_4_add_DownloadStarting(w4, (ICoreWebView2DownloadStartingEventHandler *)cb, &r->tDownload));
            cb_release(cb);
        }
        ICoreWebView2_4_Release(w4);
    }
    /* Lively / Wallpaper Engine API shim, injected before any page script runs. */
    ICoreWebView2_AddScriptToExecuteOnDocumentCreated(r->wv, wpapi_shim_js(),
        (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *)&g_noopAdd);
}

static HRESULT on_controller(HtmlRenderer *r, void *hrv, void *ctlv)
{
    HRESULT hr = (HRESULT)(intptr_t)hrv;
    if (r->closed) return S_OK;
    if (FAILED(hr) || !ctlv) {
        wchar_t msg[96];
        str_printf(msg, SP_COUNTOF(msg), L"Could not create the WebView2 control (0x%08lX)", (unsigned long)hr);
        fail(r, hr, msg);
        return S_OK;
    }
    r->ctl = (ICoreWebView2Controller *)ctlv;
    ICoreWebView2Controller_AddRef(r->ctl);
    if (FAILED(ICoreWebView2Controller_get_CoreWebView2(r->ctl, &r->wv)) || !r->wv) {
        fail(r, E_FAIL, L"WebView2 returned no web view");
        return S_OK;
    }
    configure(r);
    if (r->raster > 0) {   /* preview capture: lay out like a 1080p desktop, render smaller */
        ICoreWebView2Controller3 *c3 = NULL;
        if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(r->ctl, &IID_ICoreWebView2Controller3, (void **)&c3))) {
            ICoreWebView2Controller3_put_ShouldDetectMonitorScaleChanges(c3, FALSE);
            ICoreWebView2Controller3_put_RasterizationScale(c3, r->raster);
            ICoreWebView2Controller3_Release(c3);
        }
    }
    LOGI(L"html: controller ready, navigating to %ls", r->url);
    RECT rc = { 0, 0, r->w, r->h };
    ICoreWebView2Controller_put_Bounds(r->ctl, rc);
    ICoreWebView2Controller_put_IsVisible(r->ctl, TRUE);
    apply_audio(r);

    ICoreWebView2_3 *w3 = NULL;
    hr = r->isUrl ? S_FALSE : ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_3, (void **)&w3);
    if (hr == S_OK) {
        hr = ICoreWebView2_3_SetVirtualHostNameToFolderMapping(w3, WALLPAPER_HOST, r->folder,
                                                               COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        ICoreWebView2_3_Release(w3);
    }
    if (FAILED(hr)) {
        fail(r, hr, L"This WebView2 Runtime is too old (virtual host mapping unsupported). Update the WebView2 Runtime.");
        return S_OK;
    }
    hr = ICoreWebView2_Navigate(r->wv, r->url);
    if (FAILED(hr)) fail(r, hr, L"Could not navigate to the wallpaper page");
    return S_OK;
}

static void create_controller(HtmlRenderer *r)
{
    Callback *cb = cb_new(&k_vtHrPtr, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, r, on_controller);
    if (!cb) {
        fail(r, E_OUTOFMEMORY, L"Out of memory");
        return;
    }
    HRESULT hr = ICoreWebView2Environment_CreateCoreWebView2Controller(
        g_env.env, r->hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)cb);
    cb_release(cb);
    if (FAILED(hr)) fail(r, hr, L"Could not create the WebView2 control");
}

static HRESULT on_environment(HtmlRenderer *unused, void *hrv, void *envv)
{
    (void)unused;
    HRESULT hr = (HRESULT)(intptr_t)hrv;
    g_env.creating = 0;
    if (SUCCEEDED(hr) && envv) {
        g_env.env = (ICoreWebView2Environment *)envv;
        ICoreWebView2Environment_AddRef(g_env.env);
        LOGI(L"html: WebView2 environment ready");
    } else {
        LOGE(L"html: WebView2 environment creation failed 0x%08lX", (unsigned long)hr);
    }
    int n = g_env.waiterCount;
    HtmlRenderer *waiters[MAX_WAITERS];
    memcpy(waiters, g_env.waiters, sizeof(HtmlRenderer *) * (size_t)n);
    g_env.waiterCount = 0;
    for (int i = 0; i < n; i++) {
        HtmlRenderer *r = waiters[i];
        if (!r->closed) {
            if (g_env.env) create_controller(r);
            else fail(r, hr, L"The WebView2 Runtime could not be started");
        }
        html_release(r);
    }
    return S_OK;
}

static wchar_t g_captureUdf[SP_MAX_PATH];   /* preview capture process: its own profile */

void html_capture_mode(const wchar_t *userDataFolder)
{
    str_copy(g_captureUdf, SP_COUNTOF(g_captureUdf), userDataFolder);
}

static BOOL request_environment(HtmlRenderer *r)
{
    if (g_env.env) {
        create_controller(r);
        return TRUE;
    }
    if (g_env.waiterCount >= MAX_WAITERS) return FALSE;
    /* Register as a waiter first: the completion handler may run synchronously. */
    html_addref(r);
    g_env.waiters[g_env.waiterCount++] = r;
    if (g_env.creating) return TRUE;
    g_env.creating = 1;
    /* Autoplay for wallpaper media without a user gesture. The capture window is off-screen, so
     * Chromium must not treat it as occluded or background (it would stop animating). */
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                            g_captureUdf[0] ? L"--autoplay-policy=no-user-gesture-required --disable-features=CalculateNativeWinOcclusion "
                                              L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
                                              L"--disable-background-timer-throttling"
                                            : L"--autoplay-policy=no-user-gesture-required");
    Callback *cb = cb_new(&k_vtHrPtr, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, NULL, on_environment);
    HRESULT hr = cb ? CreateCoreWebView2EnvironmentWithOptions(NULL, g_captureUdf[0] ? g_captureUdf : paths_webview(), NULL,
                                                               (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)cb)
                    : E_OUTOFMEMORY;
    if (cb) cb_release(cb);
    if (FAILED(hr)) {
        LOGE(L"html: CreateCoreWebView2EnvironmentWithOptions failed 0x%08lX", (unsigned long)hr);
        g_env.creating = 0;
        for (int i = 0; i < g_env.waiterCount; i++) {
            if (g_env.waiters[i] == r) {                  /* still queued: undo our registration */
                g_env.waiters[i] = g_env.waiters[--g_env.waiterCount];
                html_release(r);
                break;
            }
        }
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------ preview capture */

static HRESULT on_grabbed(HtmlRenderer *r, void *hrv, void *unused)
{
    (void)unused;
    HRESULT hr = (HRESULT)(intptr_t)hrv;
    IStream *st = r->grab;
    r->grab = NULL;
    if (!st) return S_OK;
    if (r->closed || FAILED(hr)) SP_RELEASE(st);
    if (!r->closed && !PostMessageW(r->hwnd, WM_SP_HTML_GRAB, (WPARAM)hr, (LPARAM)st) && st) IStream_Release(st);
    return S_OK;
}

BOOL html_grab(Renderer *base)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (!r || !r->wv || r->closed || !r->readyPosted || r->grab) return FALSE;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &r->grab))) return FALSE;
    Callback *cb = cb_new(&k_vtHr, &IID_ICoreWebView2CapturePreviewCompletedHandler, r, on_grabbed);
    HRESULT hr = cb ? ICoreWebView2_CapturePreview(r->wv, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG, r->grab,
                                                   (ICoreWebView2CapturePreviewCompletedHandler *)cb)
                    : E_OUTOFMEMORY;
    if (cb) cb_release(cb);
    if (FAILED(hr)) SP_RELEASE(r->grab);
    return SUCCEEDED(hr);
}

/* ------------------------------------------------------------ vtable */

static void hr_set_state(Renderer *base, RenderState st, BOOL keepFrame)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (r->state == st && r->keepFrame == (keepFrame ? 1 : 0)) return;
    r->state = st;
    r->keepFrame = keepFrame ? 1 : 0;
    apply_state(r);
}

static void hr_resize(Renderer *base, int w, int h)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    r->w = w;
    r->h = h;
    if (r->ctl) {
        RECT rc = { 0, 0, w, h };
        ICoreWebView2Controller_put_Bounds(r->ctl, rc);
        ICoreWebView2Controller_NotifyParentWindowPositionChanged(r->ctl);
    }
}

static void hr_set_audio(Renderer *base, BOOL on)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    r->audio = on ? 1 : 0;
    apply_audio(r);
}

static BOOL hr_paint(Renderer *base, HDC dc, const RECT *rc)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (r->snapSwap) return TRUE;
    if (!r->snapshot) return FALSE;
    HDC mem = CreateCompatibleDC(dc);
    if (!mem) return FALSE;
    HGDIOBJ old = SelectObject(mem, r->snapshot);
    BitBlt(dc, 0, 0, rc->right - rc->left, rc->bottom - rc->top, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    return TRUE;
}

static void hr_describe(Renderer *base, wchar_t *buf, size_t cap)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    UINT32 pid = 0;
    if (r->wv) ICoreWebView2_get_BrowserProcessId(r->wv, &pid);
    str_printf(buf, cap, L"WebView2 \u00B7 %ls%ls%ls \u00B7 browser pid %u \u00B7 %ls",
               r->wp.format == WPF_LIVELY ? L"Lively" : r->wp.format == WPF_WE ? L"Wallpaper Engine" : L"HTML",
               r->wantMask & 1 ? L" \u00B7 audio-reactive" : L"", r->wantMask & 2 ? L" \u00B7 system info" : L"", pid,
               r->state == RS_SUSPENDED ? L"suspended" : r->state == RS_OCCLUDED ? L"hidden" : L"live");
}

static int hr_wants(Renderer *base)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    int m = RENDER_WANT_INPUT;
    if ((r->wantMask & 1) || (r->wp.flags & WPFLAG_AUDIO)) m |= RENDER_WANT_AUDIO;
    if (r->wantMask & 2) m |= RENDER_WANT_SYSINFO;
    if (r->wantMask & 4) m |= RENDER_WANT_TRACK;
    return m;
}

static void hr_input(Renderer *base, const RendererInput *in)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (!r->wv || r->closed || !r->readyPosted || r->state != RS_ACTIVE) return;
    double x = in->x / r->scale, y = in->y / r->scale, dy = 0.0;
    const char *type = "mouseMoved", *button = "none";
    int clicks = 0;
    switch (in->kind) {
    case RIN_DOWN:  type = "mousePressed"; clicks = 1; break;
    case RIN_UP:    type = "mouseReleased"; clicks = 1; break;
    case RIN_WHEEL: type = "mouseWheel"; dy = -in->wheel * 100.0 / WHEEL_DELTA; break;
    case RIN_LEAVE: x = -16.0; y = -16.0; break;   /* outside the viewport: mouseout/mouseleave */
    default: break;
    }
    if (in->kind == RIN_DOWN || in->kind == RIN_UP) button = in->button == 1 ? "right" : in->button == 2 ? "middle" : "left";
    wchar_t json[320];
    str_printf(json, SP_COUNTOF(json),
               L"{\"type\":\"%hs\",\"x\":%.2f,\"y\":%.2f,\"button\":\"%hs\",\"buttons\":%d,\"clickCount\":%d,"
               L"\"deltaX\":0,\"deltaY\":%.2f,\"pointerType\":\"mouse\"}",
               type, x, y, button, in->buttons, clicks, dy);
    ICoreWebView2_CallDevToolsProtocolMethod(r->wv, L"Input.dispatchMouseEvent", json,
                                             (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *)&g_noopCdp);
}

static void hr_feed(Renderer *base, int what, const char *payload)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (!r->wv || r->closed || !r->readyPosted || r->state != RS_ACTIVE || !payload) return;
    StrBuf b = { 0 };
    if (what == RENDER_FEED_TRACK) {
        sb_puts(&b, "window.__sp&&__sp.track(");
        json_write_string(&b, payload);           /* livelyCurrentTrack receives the JSON as a string */
    } else {
        sb_puts(&b, what == RENDER_FEED_AUDIO ? "window.__sp&&__sp.audio(" : "window.__sp&&__sp.sysinfo(");
        sb_puts(&b, payload);
    }
    sb_puts(&b, ")");
    if (!b.failed) run_js_utf8(r, b.p);
    sb_free(&b);
}

static void hr_on_timer(Renderer *base, UINT_PTR id)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    if (id != TIMER_PROBE_1 && id != TIMER_PROBE_2) return;
    KillTimer(r->hwnd, id);
    probe_apis(r);
}

static void hr_destroy(Renderer *base)
{
    HtmlRenderer *r = (HtmlRenderer *)base;
    r->closed = 1;
    KillTimer(r->hwnd, TIMER_PROBE_1);
    KillTimer(r->hwnd, TIMER_PROBE_2);
    if (r->wv && r->hasDownloadToken) {
        ICoreWebView2_4 *w4 = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(r->wv, &IID_ICoreWebView2_4, (void **)&w4))) {
            ICoreWebView2_4_remove_DownloadStarting(w4, r->tDownload);
            ICoreWebView2_4_Release(w4);
        }
    }
    if (r->wv && r->hasTokens) {
        ICoreWebView2_remove_NavigationStarting(r->wv, r->tNavStart);
        ICoreWebView2_remove_NavigationCompleted(r->wv, r->tNavDone);
        ICoreWebView2_remove_ProcessFailed(r->wv, r->tFail);
        ICoreWebView2_remove_NewWindowRequested(r->wv, r->tNewWin);
        ICoreWebView2_remove_PermissionRequested(r->wv, r->tPerm);
    }
    if (r->ctl) ICoreWebView2Controller_Close(r->ctl);
    SP_RELEASE(r->wv);
    SP_RELEASE(r->ctl);
    gfx_release(&r->snapSwap);
    html_release(r);
}

static const RendererVtbl k_htmlVtbl = { hr_set_state, hr_resize, hr_set_audio, hr_paint, hr_describe, hr_destroy,
                                         hr_wants, hr_input, hr_feed, hr_on_timer };

BOOL html_runtime_version(wchar_t *buf, size_t cap)
{
    LPWSTR ver = NULL;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(NULL, &ver);
    BOOL ok = SUCCEEDED(hr) && ver && ver[0];
    if (buf) str_copy(buf, cap, ok ? ver : L"");
    CoTaskMemFree(ver);
    return ok;
}

Renderer *html_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap)
{
    if (!html_runtime_version(NULL, 0)) {
        str_copy(err, errCap, L"The Microsoft Edge WebView2 Runtime is not installed. Install it from "
                              L"https://go.microsoft.com/fwlink/p/?LinkId=2124703 to use HTML wallpapers.");
        return NULL;
    }
    int isUrl = wallpaper_is_url(ctx->wp->path);
    if (!isUrl && !file_exists(ctx->wp->path)) {
        str_copy(err, errCap, L"The wallpaper's HTML entry file was not found");
        return NULL;
    }
    HtmlRenderer *r = (HtmlRenderer *)calloc(1, sizeof *r);
    if (!r) {
        str_copy(err, errCap, L"Out of memory");
        return NULL;
    }
    r->base.vt = &k_htmlVtbl;
    r->base.rid = ctx->rid;
    r->base.type = WP_HTML;
    r->refs = 1;
    r->hwnd = ctx->hwnd;
    r->notify = ctx->notify;
    r->w = ctx->width;
    r->h = ctx->height;
    r->audio = ctx->audio ? 1 : 0;
    r->state = RS_ACTIVE;
    r->wp = *ctx->wp;
    r->isUrl = isUrl;
    r->scale = ctx->dpi ? ctx->dpi / 96.0 : 1.0;
    r->raster = ctx->rasterScale;
    r->pausedNotified = -1;
    if (isUrl) {
        str_copy(r->url, SP_COUNTOF(r->url), ctx->wp->path);
    } else {
        path_dirname(r->folder, SP_COUNTOF(r->folder), ctx->wp->path);
        char enc[SP_MAX_PATH * 3];
        if (!url_encode_path(enc, sizeof enc, path_filename(ctx->wp->path))) {
            html_release(r);
            str_copy(err, errCap, L"Invalid wallpaper file name");
            return NULL;
        }
        str_printf(r->url, SP_COUNTOF(r->url), L"%ls%hs", WALLPAPER_ORIGIN, enc);
    }
    if (!request_environment(r)) {
        r->closed = 1;
        html_release(r);
        str_copy(err, errCap, L"Could not start the WebView2 Runtime");
        return NULL;
    }
    return &r->base;
}

void html_shutdown(void)
{
    SP_RELEASE(g_env.env);
}
