/* Screen- projects (.spx): the pack runs in the Screen- runtime (a Godot build from the Screen- repository) as its
 * own process, and its window is adopted into the host window like an application wallpaper.
 *
 * Start: the imported pack.spx is scanned again (third_party/spxscan; blocked = never started), then project.pck and
 * manifest.json are copied out to <data>\spx\<id>\run and the runtime is started in a job object (killed with us, no
 * child processes, no clipboard or system settings) with --sp-pipe <name>.
 *
 * Bus: JSON-RPC 2.0 over a duplex named pipe, each message a 4-byte little-endian length and UTF-8 JSON
 * (ScreenMinus docs/design.md §4). A reader thread hands whole messages to a message-only window, so everything
 * else runs on the engine thread. Writes wait at most 250 ms: a runtime that stops reading fails the renderer instead
 * of stalling the engine.
 *
 * In this first version the bus carries hello, pause/resume, properties, mute, display, mouse and the audio
 * spectrum; every other call is answered "not available yet" (-32601) or "not granted" (-32001). Grants: only
 * capabilities with no prompt (audio) until the permission prompt exists. */
#include "../renderer.h"
#include "../html/wallpaper_api.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include "../../common/paths.h"
#include "../../common/json.h"
#include "spx_scan.h"

#include <stdlib.h>
#include <string.h>

#define SPX_MAX_FRAME   (16u << 20)
#define SPX_WRITE_MS    250
#define SPX_START_MS    20000       /* first frame, or the runtime is reported as failed */
#define SPX_STOP_MS     1500        /* sp.stop, then the job is terminated */
#define WM_SPX_CONNECTED (WM_APP + 1)
#define WM_SPX_MSG       (WM_APP + 2)   /* lParam: malloc'd NUL-terminated JSON, freed by the receiver */
#define WM_SPX_GONE      (WM_APP + 3)
#define TIMER_SPX_START  1

typedef struct SpxR {
    Renderer        base;
    RendererContext ctx;
    HWND            msg;            /* message-only window on the engine thread */
    HANDLE          pipe, stopEv, writeEv, thread, job, proc;
    DWORD           pid;
    SRWLOCK         wlock;
    volatile LONG   closing;
    BOOL            connected, ready, broken, audioOn, grantAudio;
    RenderState     state;
    HWND            child;          /* the runtime's window, once adopted */
    wchar_t         title[SP_NAME_LEN];
    wchar_t         runDir[SP_MAX_PATH];
    JVal           *manifest;
} SpxR;

/* ---------------------------------------------------------------- runtime location */

/* SCREENPLUS_SPX_RUNTIME while Screen- is in development, else <data>\runtime\screen-runtime.exe (the download). */
static BOOL runtime_path(wchar_t *out, size_t cap)
{
    DWORD n = GetEnvironmentVariableW(L"SCREENPLUS_SPX_RUNTIME", out, (DWORD)cap);
    if (n && n < cap && file_exists(out)) return TRUE;
    wchar_t dir[SP_MAX_PATH];
    path_join(dir, SP_COUNTOF(dir), paths_root(), L"runtime");
    path_join(out, cap, dir, L"screen-runtime.exe");
    return file_exists(out);
}

BOOL spx_runtime_available(void)
{
    wchar_t p[SP_MAX_PATH];
    return runtime_path(p, SP_COUNTOF(p));
}

/* ---------------------------------------------------------------- writing */

static void fail(SpxR *r, const wchar_t *why)
{
    if (r->broken || r->closing) return;
    r->broken = TRUE;
    LOGW(L"spx: %ls: %ls", r->title, why);
    renderer_post(r->ctx.notify, r->ctx.rid, REV_FAILED, E_FAIL, L"%ls", why);
}

static BOOL send_raw(SpxR *r, const char *json, size_t len)
{
    if (!r->connected || r->broken || len > SPX_MAX_FRAME) return FALSE;
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    BOOL ok = TRUE;
    AcquireSRWLockExclusive(&r->wlock);
    const void *parts[2] = { hdr, json };
    DWORD sizes[2] = { 4, (DWORD)len };
    for (int i = 0; i < 2 && ok; i++) {
        OVERLAPPED ov = { 0 };
        ov.hEvent = r->writeEv;
        ResetEvent(r->writeEv);
        DWORD done = 0;
        if (!WriteFile(r->pipe, parts[i], sizes[i], NULL, &ov) && GetLastError() != ERROR_IO_PENDING) { ok = FALSE; break; }
        if (WaitForSingleObject(r->writeEv, SPX_WRITE_MS) != WAIT_OBJECT_0) {
            CancelIoEx(r->pipe, &ov);
            GetOverlappedResult(r->pipe, &ov, &done, TRUE);
            ok = FALSE;
            break;
        }
        ok = GetOverlappedResult(r->pipe, &ov, &done, FALSE) && done == sizes[i];
    }
    ReleaseSRWLockExclusive(&r->wlock);
    if (!ok) fail(r, L"The Screen- runtime stopped answering");
    return ok;
}

/* {"jsonrpc":"2.0","method":m,"params":<params JSON or omitted>} */
static void notify(SpxR *r, const char *method, const char *params)
{
    StrBuf b = { 0 };
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_write_string(&b, method);
    if (params) { sb_puts(&b, ",\"params\":"); sb_puts(&b, params); }
    sb_puts(&b, "}");
    if (!b.failed) send_raw(r, b.p, b.len);
    sb_free(&b);
}

static void reply_error(SpxR *r, const JVal *id, int code, const char *message)
{
    StrBuf b = { 0 };
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_write(&b, id);
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    json_write_string(&b, message);
    sb_puts(&b, "}}");
    if (!b.failed) send_raw(r, b.p, b.len);
    sb_free(&b);
}

/* ---------------------------------------------------------------- messages to the pack */

static void display_json(SpxR *r, StrBuf *b)
{
    sb_printf(b, "{\"w\":%d,\"h\":%d,\"hz\":%u,\"scale\":%.3f}", r->ctx.width, r->ctx.height, r->ctx.refreshHz,
              r->ctx.dpi ? r->ctx.dpi / 96.0 : 1.0);
}

/* The manifest's property values, with the user's Customise overrides (props\<id>.json) on top. */
static void props_json(SpxR *r, StrBuf *b)
{
    wchar_t over[SP_MAX_PATH];
    wpapi_overrides_path(r->ctx.wp->id, over, SP_COUNTOF(over));
    JVal *ov = file_exists(over) ? json_parse_file(over) : NULL;
    const JVal *defs = r->manifest ? json_get(r->manifest, "properties") : NULL;
    sb_puts(b, "{");
    int first = 1;
    for (int i = 0; defs && defs->t == J_OBJ && i < defs->n; i++) {
        const JVal *v = json_get(ov, defs->keys[i]);
        if (!v) v = json_get(&defs->kids[i], "value");
        if (!v) continue;
        if (!first) sb_puts(b, ",");
        first = 0;
        json_write_string(b, defs->keys[i]);
        sb_puts(b, ":");
        json_write(b, v);
    }
    sb_puts(b, "}");
    json_free(ov);
}

static void send_hello(SpxR *r)
{
    StrBuf b = { 0 };
    sb_puts(&b, "{\"version\":\"" SP_VERSION_STR "\",\"display\":");
    display_json(r, &b);
    sb_puts(&b, ",\"props\":");
    props_json(r, &b);
    sb_printf(&b, ",\"granted\":{%s}}", r->grantAudio ? "\"audio\":true" : "");
    if (!b.failed) notify(r, "sp.hello", b.p);
    sb_free(&b);
    BOOL run = r->state == RS_ACTIVE;
    notify(r, run ? "sp.resume" : "sp.pause", run ? NULL : "{\"reason\":\"start\"}");
    notify(r, "sp.mute", r->audioOn ? "{\"on\":false}" : "{\"on\":true}");
}

/* ---------------------------------------------------------------- the runtime's window */

static void adopt(SpxR *r, HWND w)
{
    DWORD pid = 0;
    if (!IsWindow(w) || !GetWindowThreadProcessId(w, &pid) || pid != r->pid) {
        fail(r, L"The Screen- runtime named a window that isn't its own");
        return;
    }
    LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE), ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    st &= ~(LONG_PTR)(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    st |= WS_CHILD | WS_VISIBLE;
    ex &= ~(LONG_PTR)(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    ex |= WS_EX_NOACTIVATE;
    SetWindowLongPtrW(w, GWL_STYLE, st);
    SetWindowLongPtrW(w, GWL_EXSTYLE, ex);
    SetParent(w, r->ctx.hwnd);
    SetWindowPos(w, HWND_TOP, 0, 0, r->ctx.width, r->ctx.height, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    r->child = w;
    r->ready = TRUE;
    KillTimer(r->msg, TIMER_SPX_START);
    LOGI(L"spx: %ls: runtime window adopted (pid %lu)", r->title, r->pid);
    renderer_post(r->ctx.notify, r->ctx.rid, REV_READY, S_OK, NULL);
}

/* ---------------------------------------------------------------- messages from the pack */

static const char *method_ns(const char *m, char *ns, size_t cap)
{
    size_t n = strcspn(m, ".");
    if (n >= cap) n = cap - 1;
    memcpy(ns, m, n);
    ns[n] = 0;
    return ns;
}

static void on_message(SpxR *r, const char *text)
{
    JVal *m = json_parse(text, strlen(text));
    if (!m || m->t != J_OBJ) { json_free(m); return; }
    const char *method = json_str(json_get(m, "method"), NULL);
    const JVal *id = json_get(m, "id"), *params = json_get(m, "params");
    if (!method) { json_free(m); return; }               /* the engine sends no requests, so replies can't come */
    if (id) {
        char ns[32];
        method_ns(method, ns, sizeof ns);
        BOOL open = !strcmp(ns, "sp") || !strcmp(ns, "gpu") || !strcmp(ns, "quality");
        BOOL granted = open || (!strcmp(ns, "audio") && r->grantAudio);
        if (!granted) reply_error(r, id, -32001, "capability not granted");
        else reply_error(r, id, -32601, "not available in this Screen+ version yet");
    } else if (!strcmp(method, "sp.ready")) {
        double w = json_num(json_get(params, "window"), 0);
        if (!r->ready && w > 0) adopt(r, (HWND)(INT_PTR)(long long)w);
    } else if (!strcmp(method, "sp.log")) {
        wchar_t *msg = wide_from_utf8(json_str(json_get(params, "msg"), ""), -1);
        LOGI(L"spx: %ls: %ls", r->title, msg ? msg : L"");
        free(msg);
    } else if (!strcmp(method, "sp.error")) {
        wchar_t *msg = wide_from_utf8(json_str(json_get(params, "msg"), ""), -1);
        LOGW(L"spx: %ls: error: %ls", r->title, msg ? msg : L"");
        renderer_post(r->ctx.notify, r->ctx.rid, REV_INFO, S_OK, L"%ls", msg ? msg : L"");
        free(msg);
    }
    json_free(m);
}

static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    SpxR *r = (SpxR *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_SPX_CONNECTED:
        if (r && !r->closing) { r->connected = TRUE; send_hello(r); }
        return 0;
    case WM_SPX_MSG:
        if (r && !r->closing) on_message(r, (const char *)lp);
        free((void *)lp);
        return 0;
    case WM_SPX_GONE:
        if (r && !r->closing) fail(r, r->connected ? L"The Screen- runtime closed" : L"The Screen- runtime didn't connect");
        return 0;
    case WM_TIMER:
        if (r && wp == TIMER_SPX_START) { KillTimer(h, TIMER_SPX_START); if (!r->ready) fail(r, L"The Screen- runtime didn't start in time"); }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ---------------------------------------------------------------- reader thread */

static BOOL read_exact(SpxR *r, HANDLE ev, void *buf, DWORD len)
{
    char *p = (char *)buf;
    while (len) {
        OVERLAPPED ov = { 0 };
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        if (!ReadFile(r->pipe, p, len, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) return FALSE;
        HANDLE w[2] = { ev, r->stopEv };
        if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(r->pipe, &ov);
            GetOverlappedResult(r->pipe, &ov, &got, TRUE);
            return FALSE;
        }
        if (!GetOverlappedResult(r->pipe, &ov, &got, FALSE) || !got) return FALSE;
        p += got;
        len -= got;
    }
    return TRUE;
}

static DWORD WINAPI reader_main(void *arg)
{
    SpxR *r = (SpxR *)arg;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    OVERLAPPED ov = { 0 };
    ov.hEvent = ev;
    BOOL ok = ev != NULL;
    if (ok && !ConnectNamedPipe(r->pipe, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            HANDLE w[2] = { ev, r->stopEv };
            DWORD got;
            if (WaitForMultipleObjects(2, w, FALSE, SPX_START_MS) != WAIT_OBJECT_0) { CancelIoEx(r->pipe, &ov); GetOverlappedResult(r->pipe, &ov, &got, TRUE); ok = FALSE; }
            else ok = GetOverlappedResult(r->pipe, &ov, &got, FALSE);
        } else ok = e == ERROR_PIPE_CONNECTED;
    }
    if (ok) PostMessageW(r->msg, WM_SPX_CONNECTED, 0, 0);
    while (ok) {
        uint8_t hdr[4];
        if (!read_exact(r, ev, hdr, 4)) break;
        uint32_t len = hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
        if (!len || len > SPX_MAX_FRAME) break;
        char *buf = (char *)malloc((size_t)len + 1);
        if (!buf || !read_exact(r, ev, buf, len)) { free(buf); break; }
        buf[len] = 0;
        if (!PostMessageW(r->msg, WM_SPX_MSG, 0, (LPARAM)buf)) free(buf);
    }
    if (ev) CloseHandle(ev);
    if (!r->closing) PostMessageW(r->msg, WM_SPX_GONE, 0, 0);
    return 0;
}

/* ---------------------------------------------------------------- vtable */

static void spx_set_state(Renderer *base, RenderState st, BOOL keepFrame)
{
    (void)keepFrame;   /* a paused runtime stops drawing and its window keeps the last frame */
    SpxR *r = (SpxR *)base;
    BOOL wasRun = r->state == RS_ACTIVE, run = st == RS_ACTIVE;
    r->state = st;
    if (!r->connected || wasRun == run) return;
    notify(r, run ? "sp.resume" : "sp.pause",
           run ? NULL : st == RS_OCCLUDED ? "{\"reason\":\"covered\"}" : "{\"reason\":\"paused\"}");
}

static void spx_resize(Renderer *base, int w, int h)
{
    SpxR *r = (SpxR *)base;
    r->ctx.width = w;
    r->ctx.height = h;
    if (r->child) SetWindowPos(r->child, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    StrBuf b = { 0 };
    display_json(r, &b);
    if (!b.failed) notify(r, "sp.display", b.p);
    sb_free(&b);
}

static void spx_set_audio(Renderer *base, BOOL on)
{
    SpxR *r = (SpxR *)base;
    r->audioOn = on;
    notify(r, "sp.mute", on ? "{\"on\":false}" : "{\"on\":true}");
}

static BOOL spx_paint(Renderer *base, HDC dc, const RECT *rc)
{
    (void)base;
    FillRect(dc, rc, (HBRUSH)GetStockObject(BLACK_BRUSH));   /* under the runtime's window, or before it exists */
    return TRUE;
}

static void spx_describe(Renderer *base, wchar_t *buf, size_t cap)
{
    SpxR *r = (SpxR *)base;
    str_printf(buf, cap, L"Screen- runtime, pid %lu%ls", r->pid, r->ready ? L"" : L" (starting)");
}

static int spx_wants(Renderer *base)
{
    SpxR *r = (SpxR *)base;
    return RENDER_WANT_INPUT | (r->grantAudio ? RENDER_WANT_AUDIO : 0);
}

static void spx_input(Renderer *base, const RendererInput *in)
{
    SpxR *r = (SpxR *)base;
    char p[96];
    snprintf(p, sizeof p, "{\"x\":%d,\"y\":%d,\"buttons\":%d}", in->x, in->y, in->buttons);
    notify(r, "sp.mouse", p);
    if (!r->child) return;
    /* Godot's own input sees the desktop mouse too (the icons sit over the window and take the real events). */
    WPARAM mk = (in->buttons & 1 ? MK_LBUTTON : 0) | (in->buttons & 2 ? MK_RBUTTON : 0) | (in->buttons & 4 ? MK_MBUTTON : 0);
    LPARAM xy = MAKELPARAM(in->x, in->y);
    static const UINT down[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN }, up[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    switch (in->kind) {
    case RIN_MOVE: PostMessageW(r->child, WM_MOUSEMOVE, mk, xy); break;
    case RIN_DOWN: if (in->button >= 0 && in->button < 3) PostMessageW(r->child, down[in->button], mk, xy); break;
    case RIN_UP:   if (in->button >= 0 && in->button < 3) PostMessageW(r->child, up[in->button], mk, xy); break;
    case RIN_WHEEL: {
        POINT pt = { in->x, in->y };
        ClientToScreen(r->child, &pt);
        PostMessageW(r->child, WM_MOUSEWHEEL, MAKEWPARAM((WORD)mk, (WORD)(short)in->wheel), MAKELPARAM(pt.x, pt.y));
        break;
    }
    default: break;
    }
}

static void spx_feed(Renderer *base, int what, const char *payload)
{
    SpxR *r = (SpxR *)base;
    if (what != RENDER_FEED_AUDIO || !r->grantAudio || !payload) return;
    double sum = 0;
    int n = 0;
    for (const char *p = payload; *p; ) {
        char *end;
        double v = strtod(p, &end);
        if (end != p) { sum += v; n++; p = end; } else p++;
    }
    StrBuf b = { 0 };
    sb_printf(&b, "{\"level\":%.5f,\"spectrum\":", n ? sum / n : 0.0);
    sb_puts(&b, payload);
    sb_puts(&b, "}");
    if (!b.failed) notify(r, "audio.frame", b.p);
    sb_free(&b);
}

static void spx_apply(Renderer *base)
{
    SpxR *r = (SpxR *)base;
    StrBuf b = { 0 };
    props_json(r, &b);
    if (!b.failed) notify(r, "sp.props", b.p);
    sb_free(&b);
}

static void spx_destroy(Renderer *base)
{
    SpxR *r = (SpxR *)base;
    if (r->connected && !r->broken) notify(r, "sp.stop", NULL);
    InterlockedExchange(&r->closing, 1);
    if (r->proc && WaitForSingleObject(r->proc, SPX_STOP_MS) != WAIT_OBJECT_0 && r->job) TerminateJobObject(r->job, 1);
    if (r->thread) {
        SetEvent(r->stopEv);
        CancelIoEx(r->pipe, NULL);
        WaitForSingleObject(r->thread, 2000);
        CloseHandle(r->thread);
    }
    if (r->msg) {
        MSG m;   /* messages the reader posted that nobody will read */
        while (PeekMessageW(&m, r->msg, WM_SPX_MSG, WM_SPX_MSG, PM_REMOVE)) free((void *)m.lParam);
        DestroyWindow(r->msg);
    }
    if (r->job) CloseHandle(r->job);       /* KILL_ON_JOB_CLOSE: nothing of it outlives us */
    if (r->proc) CloseHandle(r->proc);
    if (r->pipe && r->pipe != INVALID_HANDLE_VALUE) CloseHandle(r->pipe);
    if (r->stopEv) CloseHandle(r->stopEv);
    if (r->writeEv) CloseHandle(r->writeEv);
    json_free(r->manifest);
    LOGI(L"spx: %ls: stopped", r->title);
    free(r);
}

static const RendererVtbl k_vt = {
    spx_set_state, spx_resize, spx_set_audio, spx_paint, spx_describe, spx_destroy,
    spx_wants, spx_input, spx_feed, NULL, spx_apply,
};

/* ---------------------------------------------------------------- create */

static HWND make_msg_window(SpxR *r)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = msg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ScreenPlusSpxBus";
        cls = RegisterClassW(&wc);
    }
    HWND h = CreateWindowExW(0, L"ScreenPlusSpxBus", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)r);
    return h;
}

static HANDLE make_job(void)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                           JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    lim.BasicLimitInformation.ActiveProcessLimit = 1;           /* a pack can't start programs */
    lim.ProcessMemoryLimit = (SIZE_T)4 << 30;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = { JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
        JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
        JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_DESKTOP };
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim) ||
        !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

static BOOL extract(const wchar_t *pack, const char *name, const wchar_t *dest, wchar_t *err, size_t cap)
{
    char *p8 = utf8_from_wide(pack, NULL), *d8 = utf8_from_wide(dest, NULL), why[256] = "";
    BOOL ok = p8 && d8 && spx_extract_file(p8, name, d8, why, sizeof why) == 0;
    if (!ok) {
        wchar_t *w = wide_from_utf8(why, -1);
        str_printf(err, cap, L"Couldn't unpack this Screen- project: %ls", w ? w : L"");
        free(w);
    }
    free(p8);
    free(d8);
    return ok;
}

Renderer *spx_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap)
{
    const WallpaperEntry *wp = ctx->wp;
    wchar_t runtime[SP_MAX_PATH];
    if (!runtime_path(runtime, SP_COUNTOF(runtime))) {
        str_copy(err, errCap, L"The Screen- runtime isn't installed");
        return NULL;
    }

    /* The pack on disk may have changed since it was imported: scan it again, every time. */
    char *pack8 = utf8_from_wide(wp->path, NULL);
    SpxReport rep = { 0 };
    int rc = pack8 ? spx_scan(pack8, &rep) : -1;
    free(pack8);
    if (rc != 0 || rep.verdict == SPX_BLOCK) {
        const SpxFinding *f = rc == 0 ? spx_report_worst(&rep) : NULL;
        wchar_t *msg = f ? wide_from_utf8(f->msg, -1) : NULL;
        str_printf(err, errCap, L"The safety check refused this Screen- project%ls%ls", msg ? L": " : L"", msg ? msg : L"");
        free(msg);
        spx_report_free(&rep);
        return NULL;
    }
    spx_report_free(&rep);

    SpxR *r = (SpxR *)calloc(1, sizeof *r);
    if (!r) { str_copy(err, errCap, L"Out of memory"); return NULL; }
    r->base.vt = &k_vt;
    r->base.rid = ctx->rid;
    r->base.type = WP_SPX;
    r->ctx = *ctx;
    r->state = RS_STOPPED;
    r->audioOn = ctx->audio;
    InitializeSRWLock(&r->wlock);
    str_copy(r->title, SP_COUNTOF(r->title), wp->name);

    wchar_t dir[SP_MAX_PATH], pck[SP_MAX_PATH], man[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), wp->path);
    path_join(r->runDir, SP_COUNTOF(r->runDir), dir, L"run");
    path_join(pck, SP_COUNTOF(pck), r->runDir, L"project.pck");
    path_join(man, SP_COUNTOF(man), r->runDir, L"manifest.json");
    if (!dir_create_recursive(r->runDir) || !extract(wp->path, "project.pck", pck, err, errCap) ||
        !extract(wp->path, "manifest.json", man, err, errCap)) {
        if (!err[0]) str_copy(err, errCap, L"Couldn't prepare this Screen- project");
        spx_destroy(&r->base);
        return NULL;
    }
    r->manifest = json_parse_file(man);
    const JVal *caps = json_get(r->manifest, "capabilities");
    r->grantAudio = json_bool(json_get(caps, "audio"), 0);

    wchar_t pipeName[96], pipePath[128];
    str_printf(pipeName, SP_COUNTOF(pipeName), L"ScreenPlus.spx.%lu.%u", GetCurrentProcessId(), ctx->rid);
    str_printf(pipePath, SP_COUNTOF(pipePath), L"\\\\.\\pipe\\%ls", pipeName);
    r->pipe = CreateNamedPipeW(pipePath, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, NULL);
    r->stopEv = CreateEventW(NULL, TRUE, FALSE, NULL);
    r->writeEv = CreateEventW(NULL, TRUE, FALSE, NULL);
    r->msg = make_msg_window(r);
    r->job = make_job();
    if (r->pipe == INVALID_HANDLE_VALUE || !r->stopEv || !r->writeEv || !r->msg || !r->job) {
        str_printf(err, errCap, L"Couldn't set up the Screen- runtime (error %lu)", GetLastError());
        spx_destroy(&r->base);
        return NULL;
    }

    /* Godot's own options, then ours after "--" (OS.get_cmdline_user_args). Off screen until adopted. */
    wchar_t cmd[SP_MAX_PATH * 3];
    str_printf(cmd, SP_COUNTOF(cmd),
               L"\"%ls\" --main-pack \"%ls\" --windowed --resolution %dx%d --position -32000,-32000 --rendering-driver d3d12 "
               L"-- --sp-pipe %ls", runtime, pck, ctx->width, ctx->height, pipeName);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(runtime, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, r->runDir, &si, &pi)) {
        str_printf(err, errCap, L"Couldn't start the Screen- runtime (error %lu)", GetLastError());
        spx_destroy(&r->base);
        return NULL;
    }
    if (!AssignProcessToJobObject(r->job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        str_printf(err, errCap, L"Couldn't contain the Screen- runtime (error %lu)", GetLastError());
        spx_destroy(&r->base);
        return NULL;
    }
    r->proc = pi.hProcess;
    r->pid = pi.dwProcessId;
    r->thread = CreateThread(NULL, 0, reader_main, r, 0, NULL);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (!r->thread) {
        str_copy(err, errCap, L"Couldn't start the Screen- bus");
        spx_destroy(&r->base);
        return NULL;
    }
    SetTimer(r->msg, TIMER_SPX_START, SPX_START_MS, NULL);
    LOGI(L"spx: '%ls' started in the Screen- runtime (pid %lu, %dx%d, audio %ls)", r->title, r->pid, ctx->width, ctx->height,
         r->grantAudio ? L"granted" : L"not asked");
    return &r->base;
}
