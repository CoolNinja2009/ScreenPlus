#include "engine.h"
#include "occlusion.h"
#include "audio_visualizer.h"
#include "now_playing.h"
#include "sysinfo.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/ipc.h"
#include "../settings/config.h"
#include "../monitor/monitor.h"
#include "../desktop/desktop_host.h"
#include "../renderer/renderer.h"
#include "../diagnostics/diag.h"
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>

#define WM_SP_HOST_LOST  (WM_APP + 41)
#define WM_SP_SHELL_GONE (WM_APP + 42)

enum { T_OCCLUSION = 1, T_SAFETY, T_DISPLAY, T_REHOST, T_RETRY, T_STATUS, T_WALLPAPER_DATA };

#define MAX_RETRIES 3
#define SAFETY_MS   2000

typedef struct Slot {
    int            used;
    MonitorInfo    mon;
    HWND           hwnd;
    int            shown;
    int            destroying;
    WallpaperEntry wp;            /* id[0] == 0: no wallpaper on this monitor */
    Renderer      *r;
    RenderState    state;
    int            keepFrame;
    int            covered;
    int            fullscreen;    /* the foreground app fills this display */
    int            failures;
    int            failed;        /* safe state: system wallpaper visible */
    double         retryAt;
    double         startedAt;
    wchar_t        error[SP_ERR_LEN];
    unsigned       trackSerial;   /* now-playing state last delivered to this renderer (0: none) */
} Slot;

static struct {
    HINSTANCE     inst;
    HWND          ctl;
    HWND          shell;
    int           safeMode;
    Config        cfg;
    DesktopHost   host;
    Slot          slots[SP_MAX_MONITORS];
    int           userPaused, locked, displayOff, sleeping, onBattery, energySaver, disconnected;
    int           fullscreen;     /* on any display (status only; pausing is per display) */
    HWINEVENTHOOK hooks[8];
    int           hookCount;
    HPOWERNOTIFY  pn[3];
    UINT          msgTaskbarCreated;
    uint32_t      nextRid;
    uint64_t      processStart, firstFrame;
    double        initMs, startMs;
    int           rehostAttempts;
    wchar_t       lastError[SP_ERR_LEN];
    wchar_t       webview[48];
    HANDLE        shellProcess, shellWait;
    Slot          *mouseSlot;
    int            mouseButtons;
} E;

static void apply_states(void);
static void status_dirty(void);

/* ================================================================ slots */

static Slot *slot_by_rid(uint32_t rid)
{
    for (int i = 0; i < SP_MAX_MONITORS; i++)
        if (E.slots[i].used && E.slots[i].r && E.slots[i].r->rid == rid) return &E.slots[i];
    return NULL;
}

static Slot *slot_by_monitor(const wchar_t *id)
{
    for (int i = 0; i < SP_MAX_MONITORS; i++)
        if (E.slots[i].used && str_ieq(E.slots[i].mon.id, id)) return &E.slots[i];
    return NULL;
}

static int slot_width(const Slot *s)  { return s->mon.rect.right - s->mon.rect.left; }
static int slot_height(const Slot *s) { return s->mon.rect.bottom - s->mon.rect.top; }

static Slot *audio_slot(void)
{
    Slot *best = NULL;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || (s->wp.type != WP_VIDEO && s->wp.type != WP_HTML)) continue;
        if (!best || (s->mon.primary && !best->mon.primary)) best = s;
    }
    return best;
}

static BOOL slot_wants_audio(const Slot *s)
{
    return E.cfg.s.audio && audio_slot() == s;
}

static void slot_stop_renderer(Slot *s)
{
    if (s->r) {
        s->r->vt->destroy(s->r);
        s->r = NULL;
    }
    s->trackSerial = 0;
    s->state = RS_STOPPED;
    s->retryAt = 0;
}

static void slot_destroy_window(Slot *s)
{
    if (!s->hwnd) return;
    s->destroying = 1;
    if (IsWindow(s->hwnd)) {
        SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(s->hwnd);
    }
    s->hwnd = NULL;
    s->shown = 0;
    s->destroying = 0;
}

static void slot_hide(Slot *s)
{
    if (s->hwnd && s->shown) ShowWindow(s->hwnd, SW_HIDE);
    s->shown = 0;
}

static BOOL host_layered(void)
{
    /* Progman (24H2+) has no redirection surface; GDI-drawn children need their own (layered). */
    return E.host.mode == HOSTMODE_PROGMAN;
}

static BOOL slot_ensure_window(Slot *s)
{
    if (s->hwnd && IsWindow(s->hwnd)) return TRUE;
    s->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, SP_WALLPAPER_CLASS, L"Screen+ Wallpaper", WS_POPUP,
                              s->mon.rect.left, s->mon.rect.top, slot_width(s), slot_height(s), NULL, NULL, E.inst, s);
    if (!s->hwnd) {
        str_printf(s->error, SP_COUNTOF(s->error), L"Could not create the wallpaper window (err %lu)", GetLastError());
        return FALSE;
    }
    if (!desktop_host_attach(&E.host, s->hwnd, &s->mon.rect, host_layered())) {
        str_copy(s->error, SP_COUNTOF(s->error), L"Could not attach the wallpaper behind the desktop icons");
        slot_destroy_window(s);
        return FALSE;
    }
    return TRUE;
}

static void slot_fail(Slot *s, const wchar_t *reason, BOOL transient)
{
    wchar_t msg[SP_ERR_LEN];
    str_copy(msg, SP_COUNTOF(msg), reason);   /* reason may alias s->error */
    LOGE(L"display %d (%ls): %ls renderer failed: %ls", s->mon.index, s->mon.name, wallpaper_type_name(s->wp.type), msg);
    str_copy(s->error, SP_COUNTOF(s->error), msg);
    str_copy(E.lastError, SP_COUNTOF(E.lastError), msg);
    slot_stop_renderer(s);
    slot_hide(s);   /* safe state: the regular Windows wallpaper shows through */
    if (s->startedAt > 0 && time_ms() - s->startedAt > 120000.0) s->failures = 0;   /* ran fine for a while */
    s->failures++;
    if (transient && E.cfg.s.autoRecover && s->failures <= MAX_RETRIES) {
        double delay = 2000.0 * (double)(1 << (s->failures - 1));    /* 2 s, 4 s, 8 s */
        s->retryAt = time_ms() + delay;
        s->state = RS_RECOVERING;
        SetTimer(E.ctl, T_RETRY, 500, NULL);
        LOGW(L"display %d: retry %d/%d in %.0f s", s->mon.index, s->failures, MAX_RETRIES, delay / 1000.0);
    } else {
        s->failed = 1;
        s->state = RS_STOPPED;
        LOGW(L"display %d: renderer stopped; showing the system wallpaper (safe state)", s->mon.index);
    }
    status_dirty();
}

static void slot_start_renderer(Slot *s)
{
    if (!s->wp.id[0] || s->failed || s->r) return;
    if (E.safeMode && s->wp.type != WP_IMAGE) {
        s->failed = 1;
        s->state = RS_STOPPED;
        str_copy(s->error, SP_COUNTOF(s->error), L"Paused in safe mode after repeated engine crashes. Use Restart renderer to try again.");
        slot_hide(s);
        status_dirty();
        return;
    }
    if (!slot_ensure_window(s)) {
        slot_fail(s, s->error, TRUE);
        return;
    }
    RendererContext ctx;
    ZeroMemory(&ctx, sizeof ctx);
    ctx.hwnd = s->hwnd;
    ctx.notify = E.ctl;
    ctx.rid = ++E.nextRid;
    ctx.wp = &s->wp;
    ctx.width = slot_width(s);
    ctx.height = slot_height(s);
    ctx.dpi = s->mon.dpi;
    ctx.refreshHz = s->mon.refreshHz;
    ctx.audio = slot_wants_audio(s);
    ctx.layered = host_layered();
    wchar_t err[SP_ERR_LEN] = L"";
    double t0 = time_ms();
    switch (s->wp.type) {
    case WP_IMAGE: s->r = image_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    case WP_VIDEO: s->r = video_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    case WP_HTML:  s->r = html_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    default: str_copy(err, SP_COUNTOF(err), L"Unknown wallpaper type"); break;
    }
    if (!s->r) {
        BOOL permanent = !file_exists(s->wp.path) || (s->wp.type == WP_HTML && !html_runtime_version(NULL, 0)) ||
                         (s->wp.type == WP_VIDEO && !video_ffmpeg_available(NULL, 0)) || s->wp.type == WP_NONE;
        slot_fail(s, err[0] ? err : L"Renderer could not be created", !permanent);
        return;
    }
    s->startedAt = time_ms();
    s->state = RS_RECOVERING;   /* until the first frame is ready */
    s->keepFrame = -1;
    LOGI(L"display %d: %ls renderer created for '%ls' in %.1f ms", s->mon.index, wallpaper_type_name(s->wp.type), s->wp.name,
         time_ms() - t0);
    apply_states();
}

static void slot_release(Slot *s)
{
    slot_stop_renderer(s);
    slot_destroy_window(s);
    ZeroMemory(s, sizeof *s);
}

static void resolve_wallpaper(const wchar_t *monitorId, WallpaperEntry *out)
{
    ZeroMemory(out, sizeof *out);
    WallpaperEntry *e = config_find(&E.cfg, config_assignment(&E.cfg, monitorId));
    if (e) *out = *e;
}

static void reconcile_slots(void)
{
    MonitorInfo mons[SP_MAX_MONITORS];
    int n = monitor_enumerate(mons, SP_MAX_MONITORS);
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (s->used && !monitor_find(mons, n, s->mon.id)) {
            LOGI(L"display removed: %ls", s->mon.name);
            slot_release(s);
        }
    }
    for (int m = 0; m < n; m++) {
        WallpaperEntry wp;
        resolve_wallpaper(mons[m].id, &wp);
        Slot *s = slot_by_monitor(mons[m].id);
        if (!s) {
            for (int i = 0; i < SP_MAX_MONITORS && !s; i++)
                if (!E.slots[i].used) s = &E.slots[i];
            if (!s) break;
            ZeroMemory(s, sizeof *s);
            s->used = 1;
            s->mon = mons[m];
            s->wp = wp;
            LOGI(L"display %d: %ls %ldx%ld @%u Hz, %u dpi%ls", mons[m].index, mons[m].name,
                 mons[m].rect.right - mons[m].rect.left, mons[m].rect.bottom - mons[m].rect.top, mons[m].refreshHz,
                 mons[m].dpi, mons[m].primary ? L" (primary)" : L"");
            slot_start_renderer(s);
            continue;
        }
        BOOL geom = !EqualRect(&s->mon.rect, &mons[m].rect);
        s->mon = mons[m];
        BOOL changed = wcscmp(s->wp.id, wp.id) != 0 || s->wp.type != wp.type || !str_ieq(s->wp.path, wp.path);
        if (changed) {
            slot_stop_renderer(s);
            slot_hide(s);
            s->wp = wp;
            s->failures = 0;
            s->failed = 0;
            s->error[0] = 0;
            if (!wp.id[0]) slot_destroy_window(s);
            else slot_start_renderer(s);
        } else {
            str_copy(s->wp.name, SP_COUNTOF(s->wp.name), wp.name);
            if (s->hwnd) {
                desktop_host_position(&E.host, s->hwnd, &s->mon.rect);   /* parent origin may have moved */
                if (geom && s->r) s->r->vt->resize(s->r, slot_width(s), slot_height(s));
            }
        }
    }
    apply_states();
    status_dirty();
}

static void teardown_slots(void)
{
    for (int i = 0; i < SP_MAX_MONITORS; i++)
        if (E.slots[i].used) slot_release(&E.slots[i]);
}

static void rehost(void)
{
    LOGW(L"desktop host changed (Explorer restart or wallpaper reset); re-attaching wallpapers");
    teardown_slots();
    if (!desktop_host_discover(&E.host) && ++E.rehostAttempts < 8) {
        SetTimer(E.ctl, T_REHOST, 2000, NULL);      /* wait for Explorer to come back */
        status_dirty();
        return;
    }
    E.rehostAttempts = 0;
    reconcile_slots();
}

/* ================================================================ adaptive state */

static RenderState desired_state(const Slot *s, int *keepFrame)
{
    *keepFrame = 0;
    if (E.sleeping || E.displayOff || E.disconnected || (E.locked && E.cfg.s.suspendOnLock)) return RS_SUSPENDED;
    if (E.userPaused || (E.cfg.s.pauseOnBattery && (E.onBattery || E.energySaver))) {
        *keepFrame = 1;
        return RS_SUSPENDED;
    }
    if (E.cfg.s.pauseOnFullscreen && s->fullscreen) {
        *keepFrame = !s->covered;
        return RS_SUSPENDED;
    }
    if (E.cfg.s.pauseWhenCovered && s->covered) return RS_OCCLUDED;
    return RS_ACTIVE;
}

static void apply_states(void)
{
    int anyRunning = 0, activated = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || !s->r) continue;
        int keep = 0;
        RenderState st = desired_state(s, &keep);
        if (st != s->state || keep != s->keepFrame) {
            if (s->state != RS_RECOVERING || st != s->state)
                LOGD(L"display %d: %ls -> %ls", s->mon.index, render_state_name(s->state), render_state_name(st));
            s->r->vt->set_state(s->r, st, keep);
            activated |= st == RS_ACTIVE && s->state != RS_ACTIVE;
            s->state = st;
            s->keepFrame = keep;
            status_dirty();
        }
        if (st == RS_ACTIVE || st == RS_OCCLUDED) anyRunning = 1;
    }
    /* The low-rate safety re-check only runs while something can render. */
    if (anyRunning) SetTimer(E.ctl, T_SAFETY, SAFETY_MS, NULL);
    else KillTimer(E.ctl, T_SAFETY);
    /* The data tick only feeds ACTIVE displays and stops itself when none wants audio, system or
     * track data. A display that just became active (first frame after start, uncovered, resumed)
     * may want it while another display's wallpaper doesn't: look again. */
    if (activated) SetTimer(E.ctl, T_WALLPAPER_DATA, 1, NULL);
}

static void update_occlusion(void)
{
    MonitorInfo mons[SP_MAX_MONITORS];
    Slot *map[SP_MAX_MONITORS];
    int n = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        if (!E.slots[i].used) continue;
        mons[n] = E.slots[i].mon;
        map[n++] = &E.slots[i];
    }
    OcclusionResult o;
    occlusion_compute(mons, n, &o);
    int changed = 0, any = 0;
    for (int i = 0; i < n; i++) {
        if (map[i]->fullscreen != o.fullscreen[i])
            LOGI(L"display %d: fullscreen application %ls", map[i]->mon.index, o.fullscreen[i] ? L"detected" : L"closed");
        if (map[i]->covered != o.covered[i] || map[i]->fullscreen != o.fullscreen[i]) changed = 1;
        map[i]->covered = o.covered[i];
        map[i]->fullscreen = o.fullscreen[i];
        any |= o.fullscreen[i];
    }
    E.fullscreen = any;
    if (changed) apply_states();
}

/* Wallpaper input is intentionally opt-in and restricted to the real desktop
 * surface.  The host windows remain HTTRANSPARENT, so icons and taskbar keep
 * normal behaviour; raw input gives the wallpaper trusted hover/click events
 * only when the pointer is actually over Progman/WorkerW. */
static BOOL cursor_is_desktop(void)
{
    POINT p;
    if (!GetCursorPos(&p)) return FALSE;
    HWND h = WindowFromPoint(p);
    HWND root = h ? GetAncestor(h, GA_ROOT) : NULL;
    wchar_t cls[32] = L"";
    if (root) GetClassNameW(root, cls, SP_COUNTOF(cls));
    return str_ieq(cls, L"Progman") || str_ieq(cls, L"WorkerW");
}

static Slot *slot_at_cursor(void)
{
    POINT p;
    if (!GetCursorPos(&p)) return NULL;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || !s->r || s->state != RS_ACTIVE || !PtInRect(&s->mon.rect, p)) continue;
        if (!s->r->vt->wants || !s->r->vt->input || !(s->r->vt->wants(s->r) & RENDER_WANT_INPUT)) continue;
        return s;
    }
    return NULL;
}

static void dispatch_mouse(Slot *s, int kind, int button, int wheel)
{
    if (!s || !s->r || !s->r->vt->input) return;
    POINT p;
    if (!GetCursorPos(&p)) return;
    RendererInput in = { kind, p.x - s->mon.rect.left, p.y - s->mon.rect.top, button, E.mouseButtons, wheel };
    s->r->vt->input(s->r, &in);
}

static void handle_raw_mouse(HRAWINPUT raw)
{
    if (!E.cfg.s.htmlMouse || !cursor_is_desktop()) {
        if (E.mouseSlot) { dispatch_mouse(E.mouseSlot, RIN_LEAVE, 0, 0); E.mouseSlot = NULL; }
        return;
    }
    UINT bytes = 0;
    if (GetRawInputData(raw, RID_INPUT, NULL, &bytes, sizeof(RAWINPUTHEADER)) != 0 || bytes > sizeof(RAWINPUT)) return;
    RAWINPUT ri;
    if (GetRawInputData(raw, RID_INPUT, &ri, &bytes, sizeof(RAWINPUTHEADER)) != bytes || ri.header.dwType != RIM_TYPEMOUSE) return;
    Slot *s = slot_at_cursor();
    if (s != E.mouseSlot) {
        if (E.mouseSlot) dispatch_mouse(E.mouseSlot, RIN_LEAVE, 0, 0);
        E.mouseSlot = s;
    }
    USHORT f = ri.data.mouse.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) E.mouseButtons |= 1;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) E.mouseButtons |= 2;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) E.mouseButtons |= 4;
    if (s && (ri.data.mouse.lLastX || ri.data.mouse.lLastY)) dispatch_mouse(s, RIN_MOVE, 0, 0);
    if (s && (f & RI_MOUSE_LEFT_BUTTON_DOWN)) dispatch_mouse(s, RIN_DOWN, 0, 0);
    if (s && (f & RI_MOUSE_RIGHT_BUTTON_DOWN)) dispatch_mouse(s, RIN_DOWN, 1, 0);
    if (s && (f & RI_MOUSE_MIDDLE_BUTTON_DOWN)) dispatch_mouse(s, RIN_DOWN, 2, 0);
    if (s && (f & RI_MOUSE_BUTTON_4_DOWN)) dispatch_mouse(s, RIN_DOWN, 3, 0);
    if (s && (f & RI_MOUSE_WHEEL)) dispatch_mouse(s, RIN_WHEEL, 0, (short)ri.data.mouse.usButtonData);
    if (s && (f & RI_MOUSE_LEFT_BUTTON_UP)) dispatch_mouse(s, RIN_UP, 0, 0);
    if (s && (f & RI_MOUSE_RIGHT_BUTTON_UP)) dispatch_mouse(s, RIN_UP, 1, 0);
    if (s && (f & RI_MOUSE_MIDDLE_BUTTON_UP)) dispatch_mouse(s, RIN_UP, 2, 0);
    if (f & RI_MOUSE_LEFT_BUTTON_UP) E.mouseButtons &= ~1;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP) E.mouseButtons &= ~2;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP) E.mouseButtons &= ~4;
}

static void wallpaper_data_tick(void)
{
    int wantSystem = 0, wantAudio = 0, wantTrack = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || !s->r || s->state != RS_ACTIVE || !s->r->vt->wants) continue;
        int w = s->r->vt->wants(s->r);
        wantSystem |= (w & RENDER_WANT_SYSINFO) != 0;
        wantAudio |= (w & RENDER_WANT_AUDIO) != 0;
        wantTrack |= (w & RENDER_WANT_TRACK) != 0;
    }
    if (!wantSystem && !wantTrack && !(wantAudio && E.cfg.s.htmlAudio)) {
        KillTimer(E.ctl, T_WALLPAPER_DATA); audio_visualizer_stop(); now_playing_stop(); return;
    }
    if (wantTrack) now_playing_start(); else now_playing_stop();
    if (wantAudio && E.cfg.s.htmlAudio) audio_visualizer_start(); else audio_visualizer_stop();
    /* Lively system data (cpu/gpu/ram/net) only when a page asks for it; the
     * sampling windows in sysinfo expect a steady tick. */
    char sys[2048] = "";
    if (wantSystem) sysinfo_sample(sys, sizeof sys);
    float bands[64]; BOOL spectrum = wantAudio && E.cfg.s.htmlAudio && audio_visualizer_spectrum(bands);
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || !s->r || s->state != RS_ACTIVE || !s->r->vt->wants || !s->r->vt->feed) continue;
        int w = s->r->vt->wants(s->r);
        if ((w & RENDER_WANT_SYSINFO) && sys[0]) s->r->vt->feed(s->r, RENDER_FEED_SYSINFO, sys);
        char *track = (w & RENDER_WANT_TRACK) ? now_playing_get(&s->trackSerial) : NULL;
        if (track) s->r->vt->feed(s->r, RENDER_FEED_TRACK, track);
        free(track);
        if ((w & RENDER_WANT_AUDIO) && spectrum) {
            StrBuf b = { 0 }; sb_puts(&b, "[");
            for (int x = 0; x < 64; x++) { if (x) sb_puts(&b, ","); sb_printf(&b, "%.5f", bands[x]); }
            sb_puts(&b, "]"); if (!b.failed) s->r->vt->feed(s->r, RENDER_FEED_AUDIO, b.p); sb_free(&b);
        }
    }
}

static void CALLBACK win_event(HWINEVENTHOOK hook, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD tid, DWORD time)
{
    (void)hook; (void)tid; (void)time;
    if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_CLOAKED || ev == EVENT_OBJECT_UNCLOAKED) {
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    }
    SetTimer(E.ctl, T_OCCLUSION, 150, NULL);   /* re-armed on every event: natural debounce */
}

/* ================================================================ status / IPC */

static void status_dirty(void) { if (E.ctl) SetTimer(E.ctl, T_STATUS, 120, NULL); }

static void status_send(void)
{
    if (!E.shell) return;
    IpcEngineStatus *st = (IpcEngineStatus *)calloc(1, sizeof *st);
    if (!st) return;
    st->version = SP_IPC_VERSION;
    st->hostMode = E.host.mode;
    st->processStart = E.processStart;
    st->firstFrame = E.firstFrame;
    st->initMs = E.initMs;
    st->flags = (E.userPaused ? ENGINE_FLAG_PAUSED : 0) | (E.locked ? ENGINE_FLAG_LOCKED : 0) |
                (E.onBattery ? ENGINE_FLAG_BATTERY : 0) | (E.fullscreen ? ENGINE_FLAG_FULLSCREEN : 0) |
                (E.displayOff ? ENGINE_FLAG_DISPLAY_OFF : 0) | (E.safeMode ? ENGINE_FLAG_SAFE_MODE : 0) |
                (E.webview[0] ? ENGINE_FLAG_WEBVIEW2 : 0) | (E.sleeping ? ENGINE_FLAG_SLEEPING : 0);
    str_copy(st->webviewVersion, SP_COUNTOF(st->webviewVersion), E.webview);
    str_copy(st->lastError, SP_COUNTOF(st->lastError), E.lastError);
    int n = 0, active = 0;
    for (int i = 0; i < SP_MAX_MONITORS && n < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used) continue;
        IpcMonitorStatus *m = &st->monitors[n++];
        str_copy(m->monitorId, SP_COUNTOF(m->monitorId), s->mon.id);
        str_copy(m->wallpaperId, SP_COUNTOF(m->wallpaperId), s->wp.id);
        m->type = s->wp.type;
        m->state = s->r ? s->state : (s->state == RS_RECOVERING ? RS_RECOVERING : RS_STOPPED);
        m->failed = s->failed;
        m->failures = s->failures;
        m->covered = s->covered;
        if (s->r) {
            s->r->vt->describe(s->r, m->error, SP_COUNTOF(m->error));   /* renderer detail when healthy */
            if (s->state == RS_ACTIVE) active++;
        } else {
            str_copy(m->error, SP_COUNTOF(m->error), s->error);
        }
        if (s->failed || (!s->r && s->error[0])) str_copy(m->error, SP_COUNTOF(m->error), s->error);
    }
    st->monitorCount = n;
    st->activeRenderers = active;
    ipc_send(E.ctl, E.shell, IPC_ENGINE_STATUS, st, sizeof *st, 1000);
    free(st);
}

static void send_hello(void)
{
    IpcHello h = { SP_IPC_VERSION, GetCurrentProcessId(), (uint64_t)(uintptr_t)E.ctl, E.processStart };
    if (E.shell && !ipc_send(E.ctl, E.shell, IPC_ENGINE_HELLO, &h, sizeof h, 2000))
        LOGW(L"could not reach the shell window");
}

static void restart_renderers(int all)
{
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &E.slots[i];
        if (!s->used || !s->wp.id[0]) continue;
        if (!all && !s->failed && s->state != RS_RECOVERING) continue;
        slot_stop_renderer(s);
        slot_hide(s);
        s->failed = 0;
        s->failures = 0;
        s->error[0] = 0;
        slot_start_renderer(s);
    }
    E.lastError[0] = 0;
    status_dirty();
}

static void reload_config(void)
{
    Config fresh;
    ZeroMemory(&fresh, sizeof fresh);
    config_load(&fresh, paths_config());
    int oldAudio = E.cfg.s.audio;
    config_free(&E.cfg);
    E.cfg = fresh;
    E.userPaused = E.cfg.s.paused;
    reconcile_slots();
    if (oldAudio != E.cfg.s.audio) {
        for (int i = 0; i < SP_MAX_MONITORS; i++)
            if (E.slots[i].used && E.slots[i].r) E.slots[i].r->vt->set_audio(E.slots[i].r, slot_wants_audio(&E.slots[i]));
    }
    update_occlusion();
    apply_states();
}

static void handle_command(const IpcCommand *c)
{
    switch (c->cmd) {
    case CMD_RELOAD_CONFIG:
        LOGI(L"command: reload configuration");
        reload_config();
        break;
    case CMD_SET_PAUSED:
        E.userPaused = c->arg ? 1 : 0;
        E.cfg.s.paused = E.userPaused;
        LOGI(L"command: %ls", E.userPaused ? L"pause" : L"resume");
        apply_states();
        status_dirty();
        break;
    case CMD_SET_AUDIO:
        E.cfg.s.audio = c->arg ? 1 : 0;
        for (int i = 0; i < SP_MAX_MONITORS; i++)
            if (E.slots[i].used && E.slots[i].r) E.slots[i].r->vt->set_audio(E.slots[i].r, slot_wants_audio(&E.slots[i]));
        break;
    case CMD_RESTART_RENDERERS:
        LOGI(L"command: restart %ls renderers", c->arg ? L"all" : L"failed");
        restart_renderers((int)c->arg);
        break;
    case CMD_REQUEST_STATUS:
        status_send();
        break;
    case CMD_APPLY_CUSTOMISE:
        /* every live renderer, paused or covered ones included: a paused wallpaper must not miss
         * the change (video re-presents its frozen frame graded, web pages queue the script) */
        LOGD(L"command: apply customise changes");
        for (int i = 0; i < SP_MAX_MONITORS; i++) {
            Slot *s = &E.slots[i];
            if (s->used && s->r && s->r->vt && s->r->vt->apply) s->r->vt->apply(s->r);
        }
        break;
    case CMD_SHUTDOWN:
        LOGI(L"command: shutdown");
        DestroyWindow(E.ctl);
        break;
    default:
        break;
    }
}

/* ================================================================ windows */

static LRESULT CALLBACK wallpaper_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Slot *s = (Slot *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (!s || !s->r || !s->r->vt->paint(s->r, dc, &rc)) FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_WINDOWPOSCHANGING:
        if (E.host.mode == HOSTMODE_FALLBACK) ((WINDOWPOS *)lp)->hwndInsertAfter = HWND_BOTTOM;
        break;
    case WM_DESTROY:
        if (s && !s->destroying) {   /* destroyed by the system: the host (Explorer) went away */
            s->hwnd = NULL;
            s->shown = 0;
            PostMessageW(E.ctl, WM_SP_HOST_LOST, 0, 0);
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void handle_render_event(RenderEvent *ev)
{
    Slot *s = slot_by_rid(ev->rid);
    if (!s) return;      /* stale event from a destroyed renderer */
    switch (ev->code) {
    case REV_READY:
        if (s->hwnd && !s->shown) {
            ShowWindow(s->hwnd, SW_SHOWNOACTIVATE);
            InvalidateRect(s->hwnd, NULL, FALSE);
            s->shown = 1;
        }
        s->error[0] = 0;
        if (!E.firstFrame) {
            E.firstFrame = filetime_now();
            LOGI(L"first wallpaper visible %.1f ms after engine start", (double)(E.firstFrame - E.processStart) / 10000.0);
        }
        /* re-push the current state: video re-presents its frame now that the window is visible */
        if (s->r && s->state != RS_RECOVERING) s->r->vt->set_state(s->r, s->state, s->keepFrame > 0);
        apply_states();
        status_dirty();
        break;
    case REV_FAILED:
        slot_fail(s, ev->msg[0] ? ev->msg : L"Renderer failure", TRUE);
        break;
    case REV_WANTS:
        SetTimer(E.ctl, T_WALLPAPER_DATA, 1, NULL);
        break;
    default:
        LOGI(L"display %d: %ls", s->mon.index, ev->msg);
        break;
    }
}

static void on_power_setting(const POWERBROADCAST_SETTING *ps)
{
    DWORD v = ps->DataLength >= sizeof(DWORD) ? *(const DWORD *)ps->Data : 0;
    if (IsEqualGUID(&ps->PowerSetting, &GUID_ACDC_POWER_SOURCE)) {
        E.onBattery = v != 0;
        LOGI(L"power source: %ls", E.onBattery ? L"battery" : L"AC");
    } else if (IsEqualGUID(&ps->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE)) {
        E.displayOff = v == 0;
        LOGI(L"display %ls", v == 0 ? L"off" : v == 2 ? L"dimmed" : L"on");
    } else if (IsEqualGUID(&ps->PowerSetting, &GUID_POWER_SAVING_STATUS)) {
        E.energySaver = v != 0;
        LOGI(L"battery saver %ls", E.energySaver ? L"on" : L"off");
    }
    apply_states();
    status_dirty();
}

static LRESULT CALLBACK control_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == E.msgTaskbarCreated && msg) {
        LOGI(L"TaskbarCreated: Explorer (re)started");
        SetTimer(h, T_REHOST, 1200, NULL);
        return 0;
    }
    switch (msg) {
    case WM_INPUT:
        handle_raw_mouse((HRAWINPUT)lp);
        return 0;
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds->dwData == IPC_ENGINE_CMD && cds->cbData == sizeof(IpcCommand)) {
            IpcCommand c = *(const IpcCommand *)cds->lpData;
            PostMessageW(h, WM_APP + 50, c.cmd, c.arg);   /* never block the sender */
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP + 50: {
        IpcCommand c = { (uint32_t)wp, (uint32_t)lp };
        handle_command(&c);
        return 0;
    }
    case WM_SP_RENDER_EVENT: {
        RenderEvent *ev = (RenderEvent *)lp;
        if (ev) {
            handle_render_event(ev);
            free(ev);
        }
        return 0;
    }
    case WM_SP_HOST_LOST:
        SetTimer(h, T_REHOST, 800, NULL);
        return 0;
    case WM_SP_SHELL_GONE:
        LOGW(L"shell process exited; stopping engine");
        DestroyWindow(h);
        return 0;
    case WM_TIMER:
        switch (wp) {
        case T_OCCLUSION:
            KillTimer(h, T_OCCLUSION);
            update_occlusion();
            break;
        case T_SAFETY:
            update_occlusion();
            if (!desktop_host_valid(&E.host)) SetTimer(h, T_REHOST, 500, NULL);
            break;
        case T_DISPLAY:
            KillTimer(h, T_DISPLAY);
            if (!desktop_host_valid(&E.host)) rehost();
            else reconcile_slots();
            update_occlusion();
            break;
        case T_REHOST:
            KillTimer(h, T_REHOST);
            rehost();
            update_occlusion();
            break;
        case T_RETRY: {
            int pending = 0;
            double now = time_ms();
            for (int i = 0; i < SP_MAX_MONITORS; i++) {
                Slot *s = &E.slots[i];
                if (!s->used || s->retryAt <= 0) continue;
                if (now >= s->retryAt) {
                    s->retryAt = 0;
                    LOGI(L"display %d: recovering renderer (attempt %d)", s->mon.index, s->failures);
                    slot_start_renderer(s);
                }
                if (s->retryAt > 0) pending = 1;
            }
            if (!pending) KillTimer(h, T_RETRY);
            break;
        }
        case T_STATUS:
            KillTimer(h, T_STATUS);
            status_send();
            break;
        case T_WALLPAPER_DATA:
            wallpaper_data_tick();
            break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        SetTimer(h, T_DISPLAY, 600, NULL);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA || wp == SPI_SETDESKWALLPAPER) SetTimer(h, T_DISPLAY, 600, NULL);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            LOGI(L"system suspending");
            E.sleeping = 1;
            apply_states();
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            if (E.sleeping) LOGI(L"system resumed");
            E.sleeping = 0;
            apply_states();
            SetTimer(h, T_DISPLAY, 1500, NULL);   /* displays/devices may have changed */
        } else if (wp == PBT_POWERSETTINGCHANGE && lp) {
            on_power_setting((const POWERBROADCAST_SETTING *)lp);
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        switch (wp) {
        case WTS_SESSION_LOCK:      E.locked = 1; break;
        case WTS_SESSION_UNLOCK:    E.locked = 0; break;
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT: E.disconnected = 1; break;
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT:    E.disconnected = 0; break;
        default: return 0;
        }
        LOGI(L"session %ls", E.locked ? L"locked" : L"unlocked");
        apply_states();
        if (!E.locked) SetTimer(h, T_OCCLUSION, 300, NULL);
        status_dirty();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            LOGI(L"session ending; stopping engine");
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void CALLBACK shell_exited(void *ctx, BOOLEAN timedOut)
{
    (void)ctx; (void)timedOut;
    if (E.ctl) PostMessageW(E.ctl, WM_SP_SHELL_GONE, 0, 0);
}

/* ================================================================ entry */

int engine_main(HINSTANCE inst, HWND shell, int safeMode)
{
    E.startMs = time_ms();
    E.inst = inst;
    E.shell = shell;
    E.safeMode = safeMode;
    E.processStart = process_creation_filetime(GetCurrentProcess());
    log_init(L"engine");
    diag_install_crash_handler(L"engine");
    LOGI(L"engine starting (pid %lu%ls, Windows build %lu)", GetCurrentProcessId(), safeMode ? L", SAFE MODE" : L"",
         windows_build_number());

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = control_proc;
    wc.hInstance = inst;
    wc.lpszClassName = SP_ENGINE_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = wallpaper_proc;
    wc.lpszClassName = SP_WALLPAPER_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    /* Hidden top-level (not message-only) window so broadcasts such as TaskbarCreated arrive. */
    E.ctl = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, SP_ENGINE_CLASS, L"Screen+ Engine", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!E.ctl) {
        LOGE(L"could not create the engine window (err %lu)", GetLastError());
        return 2;
    }
    E.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    RAWINPUTDEVICE mouse = { 0x01, 0x02, RIDEV_INPUTSINK, E.ctl };
    if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse))
        LOGW(L"could not register desktop mouse input (err %lu)", GetLastError());
    SetTimer(E.ctl, T_WALLPAPER_DATA, 250, NULL); /* self-disables until a page requests data */
    WTSRegisterSessionNotification(E.ctl, NOTIFY_FOR_THIS_SESSION);
    E.pn[0] = RegisterPowerSettingNotification(E.ctl, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    E.pn[1] = RegisterPowerSettingNotification(E.ctl, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    E.pn[2] = RegisterPowerSettingNotification(E.ctl, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);

    static const DWORD ranges[][2] = {
        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
        { EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
        { EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE },
        { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
    };
    for (size_t i = 0; i < SP_COUNTOF(ranges); i++) {
        HWINEVENTHOOK hk = SetWinEventHook(ranges[i][0], ranges[i][1], NULL, win_event, 0, 0,
                                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hk) E.hooks[E.hookCount++] = hk;
    }

    if (shell) {
        DWORD pid = 0;
        GetWindowThreadProcessId(shell, &pid);
        E.shellProcess = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        if (E.shellProcess)
            RegisterWaitForSingleObject(&E.shellWait, E.shellProcess, shell_exited, NULL, INFINITE, WT_EXECUTEONLYONCE);
    }

    config_load(&E.cfg, paths_config());
    E.userPaused = E.cfg.s.paused;
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        E.onBattery = sps.ACLineStatus == 0;
        E.energySaver = (sps.SystemStatusFlag & 1) != 0;
    }
    html_runtime_version(E.webview, SP_COUNTOF(E.webview));

    desktop_host_discover(&E.host);
    update_occlusion();
    reconcile_slots();
    update_occlusion();
    E.initMs = time_ms() - E.startMs;
    LOGI(L"engine initialized in %.1f ms (host: %ls, WebView2: %ls)", E.initMs, desktop_host_mode_name(E.host.mode),
         E.webview[0] ? E.webview : L"not installed");
    send_hello();
    status_send();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    LOGI(L"engine shutting down");
    double t0 = time_ms();
    E.shell = NULL;
    teardown_slots();
    audio_visualizer_stop();
    now_playing_stop();
    video_shutdown();
    html_shutdown();
    for (int i = 0; i < E.hookCount; i++) UnhookWinEvent(E.hooks[i]);
    for (int i = 0; i < 3; i++) if (E.pn[i]) UnregisterPowerSettingNotification(E.pn[i]);
    if (E.shellWait) UnregisterWaitEx(E.shellWait, INVALID_HANDLE_VALUE);
    if (E.shellProcess) CloseHandle(E.shellProcess);
    desktop_host_refresh(&E.host);
    config_free(&E.cfg);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    LOGI(L"engine stopped cleanly in %.1f ms", time_ms() - t0);
    log_shutdown();
    return 0;
}
