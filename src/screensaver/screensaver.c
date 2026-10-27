/* Screensaver mode. Windows starts ScreenPlus.scr (a copy of ScreenPlus.exe, see scrnsave.h) with
 *   /s          full screen until the user is back: a key, a click, the wheel, or a mouse move when
 *               allowed (a short grace period and a dead zone absorb jitter),
 *   /p <hwnd>   the small preview inside Screen Saver Settings (a child of that window),
 *   /c          its Settings button: opens Screen+ on the Screensaver settings.
 * Screen+'s Preview button runs ScreenPlus.exe --screensaver /s the same way.
 * The wallpaper renderers run in this process (no shell, engine or desktop host): one topmost
 * window per display, or one across all of them (span), or the primary only with the others black.
 * A video shown on several displays still has one decoder. While it runs the desktop engine is
 * suspended (it watches this process, so a crash can't leave it suspended). Started by Windows over
 * a fullscreen app or a presentation, it quits at once; it renders nothing while the display is off,
 * and freezes on battery when the wallpaper would. Web wallpapers use their own WebView2 profile. */
#include "screensaver.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/ipc.h"
#include "../settings/config.h"
#include "../monitor/monitor.h"
#include "../renderer/renderer.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <wctype.h>

#define SCR_CLASS     L"ScreenPlus.Screensaver"
#define SCR_CTL_CLASS L"ScreenPlus.ScreensaverControl"
#define GRACE_MS      600.0   /* the hand still on the mouse after Preview or a wake-up */
#define MOVE_PX       12      /* cursor dead zone */
#define MOVE_MICKEYS  48      /* relative motion, for a cursor pinned to a screen edge */

enum { T_REBUILD = 1 };

typedef struct ScrWin {
    HWND      hwnd;
    Renderer *r;
} ScrWin;

static struct {
    HINSTANCE      inst;
    HWND           ctl, parent;     /* parent: the /p preview window */
    int            run;             /* /s, else /p */
    Config         cfg;
    WallpaperEntry wp;
    ScrWin         w[SP_MAX_MONITORS];
    int            n;
    uint32_t       rid;
    double         startMs;
    int            armed;           /* grace period over: start is where the cursor rests */
    POINT          start;
    long           motion;
    int            displayOff, onBattery, quitting;
    HPOWERNOTIFY   pn[2];
} S;

static void request_exit(const wchar_t *why)
{
    if (S.quitting) return;
    S.quitting = 1;
    LOGI(L"screensaver: ending (%ls)", why);
    PostQuitMessage(0);
}

/* Over a fullscreen app (game, video, slideshow) or a presentation, or with the sign-in screen up,
 * a screensaver only gets in the way. */
static int should_not_start(void)
{
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_PRESENTATION_MODE) return 1;
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t name[64] = L"";
    if (d) {
        GetUserObjectInformationW(d, UOI_NAME, name, sizeof name, NULL);
        CloseDesktop(d);
    }
    if (!str_ieq(name, L"Default")) return 1;
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) return 0;
    wchar_t cls[64] = L"";
    GetClassNameW(fg, cls, SP_COUNTOF(cls));
    if (str_ieq(cls, L"Progman") || str_ieq(cls, L"WorkerW") || str_ieq(cls, L"Shell_TrayWnd")) return 0;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(fg, &r);
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

static void apply_state(void)
{
    BOOL hold = S.displayOff || (S.onBattery && S.cfg.s.pauseOnBattery);
    for (int i = 0; i < S.n; i++)
        if (S.w[i].r) S.w[i].r->vt->set_state(S.w[i].r, hold ? RS_SUSPENDED : RS_ACTIVE, TRUE);
}

static void destroy_windows(void)
{
    for (int i = 0; i < S.n; i++) {
        ScrWin *w = &S.w[i];
        if (w->r) w->r->vt->destroy(w->r);
        w->r = NULL;
        if (w->hwnd && IsWindow(w->hwnd)) {
            SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);   /* our own destroy: not a reason to quit */
            DestroyWindow(w->hwnd);
        }
        w->hwnd = NULL;
    }
    S.n = 0;
}

static void add_window(const MonitorInfo *m, const RECT *rc, int content)
{
    if (S.n >= SP_MAX_MONITORS) return;
    ScrWin *w = &S.w[S.n];
    int cx = rc->right - rc->left, cy = rc->bottom - rc->top;
    w->hwnd = S.run ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SCR_CLASS, L"Screen+ Screensaver", WS_POPUP,
                                      rc->left, rc->top, cx, cy, NULL, NULL, S.inst, w)
                    : CreateWindowExW(0, SCR_CLASS, L"Screen+ Screensaver", WS_CHILD | WS_VISIBLE, 0, 0, cx, cy, S.parent,
                                      NULL, S.inst, w);
    if (!w->hwnd) {
        LOGE(L"screensaver: could not create a window (err %lu)", GetLastError());
        return;
    }
    S.n++;
    if (S.run) ShowWindow(w->hwnd, S.n == 1 ? SW_SHOW : SW_SHOWNA);   /* black at once; content when ready */
    if (!content || !S.wp.id[0]) return;
    RendererContext ctx;
    ZeroMemory(&ctx, sizeof ctx);
    ctx.hwnd = w->hwnd;
    ctx.notify = S.ctl;
    ctx.rid = ++S.rid;
    ctx.wp = &S.wp;
    ctx.width = SP_MAX(cx, 1);
    ctx.height = SP_MAX(cy, 1);
    ctx.dpi = m ? m->dpi : 96;
    ctx.refreshHz = m ? m->refreshHz : 60;
    ctx.audio = S.run && S.cfg.s.scrAudio && m && m->primary;
    if (!S.run && S.wp.type == WP_HTML) ctx.rasterScale = SP_MAX(0.1, cx / 1920.0);   /* lay out like a desktop */
    wchar_t err[SP_ERR_LEN] = L"";
    switch (S.wp.type) {
    case WP_IMAGE: w->r = image_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    case WP_VIDEO: w->r = video_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    case WP_HTML:  w->r = html_renderer_create(&ctx, err, SP_COUNTOF(err)); break;
    default: break;
    }
    if (!w->r) LOGW(L"screensaver: '%ls' can't be shown: %ls", S.wp.name, err[0] ? err : L"unsupported");
}

static void build_windows(void)
{
    destroy_windows();
    if (!S.run) {
        RECT rc;
        GetClientRect(S.parent, &rc);
        add_window(NULL, &rc, 1);
        return;
    }
    MonitorInfo mons[SP_MAX_MONITORS];
    int n = monitor_enumerate(mons, SP_MAX_MONITORS);
    if (S.cfg.s.scrDisplays == SCRD_SPAN && n > 1) {
        MonitorInfo span = mons[0];      /* the primary: dpi, audio */
        for (int i = 1; i < n; i++) {
            UnionRect(&span.rect, &span.rect, &mons[i].rect);
            span.refreshHz = SP_MAX(span.refreshHz, mons[i].refreshHz);
        }
        add_window(&span, &span.rect, 1);
    } else {
        for (int i = 0; i < n; i++) add_window(&mons[i], &mons[i].rect, S.cfg.s.scrDisplays != SCRD_PRIMARY || mons[i].primary);
    }
    if (S.n) SetForegroundWindow(S.w[0].hwnd);
    LOGI(L"screensaver: '%ls' on %d window(s) for %d display(s)", S.wp.id[0] ? S.wp.name : L"(blank)", S.n, n);
}

static void on_raw_input(HRAWINPUT h)
{
    RAWINPUT ri;
    UINT bytes = sizeof ri;
    if (GetRawInputData(h, RID_INPUT, &ri, &bytes, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        if (!(ri.data.keyboard.Flags & RI_KEY_BREAK)) request_exit(L"key");
        return;
    }
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    if (ri.data.mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                                       RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
        request_exit(L"mouse button");
        return;
    }
    if (!S.cfg.s.scrExitOnMouse || time_ms() - S.startMs < GRACE_MS) return;
    POINT p;
    GetCursorPos(&p);
    if (!S.armed) {                      /* the cursor came to rest during the grace period */
        S.armed = 1;
        S.start = p;
        return;
    }
    if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) S.motion += labs(ri.data.mouse.lLastX) + labs(ri.data.mouse.lLastY);
    if (abs(p.x - S.start.x) > MOVE_PX || abs(p.y - S.start.y) > MOVE_PX || S.motion > MOVE_MICKEYS) request_exit(L"mouse move");
}

static LRESULT CALLBACK win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ScrWin *w = (ScrWin *)GetWindowLongPtrW(h, GWLP_USERDATA);
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
        if (!w || !w->r || !w->r->vt->paint(w->r, dc, &rc)) FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:                       /* renderer timers (HTML API probes) */
        if (w && w->r && w->r->vt->on_timer) w->r->vt->on_timer(w->r, wp);
        return 0;
    case WM_SETCURSOR:
        if (S.run) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE) return 0;   /* already running */
        break;
    case WM_DESTROY:
        if (w) request_exit(S.run ? L"window closed" : L"preview closed");   /* not by destroy_windows */
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        break;                           /* DefWindowProc does the raw input cleanup */
    case WM_SP_RENDER_EVENT: {
        RenderEvent *ev = (RenderEvent *)lp;
        if (!ev) return 0;
        for (int i = 0; i < S.n; i++) {
            ScrWin *w = &S.w[i];
            if (!w->r || w->r->rid != ev->rid) continue;
            if (ev->code == REV_READY) {
                InvalidateRect(w->hwnd, NULL, FALSE);
                apply_state();
            } else if (ev->code == REV_FAILED) {
                LOGW(L"screensaver: renderer failed: %ls", ev->msg);
                w->r->vt->destroy(w->r);
                w->r = NULL;
                InvalidateRect(w->hwnd, NULL, FALSE);   /* black */
            }
        }
        free(ev);
        return 0;
    }
    case WM_TIMER:
        if (wp == T_REBUILD) {
            KillTimer(h, T_REBUILD);
            build_windows();
            apply_state();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (S.run) SetTimer(h, T_REBUILD, 500, NULL);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)lp;
            DWORD v = ps->DataLength >= sizeof(DWORD) ? *(const DWORD *)ps->Data : 0;
            if (IsEqualGUID(&ps->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE)) S.displayOff = v == 0;
            else if (IsEqualGUID(&ps->PowerSetting, &GUID_ACDC_POWER_SOURCE)) S.onBattery = v != 0;
            apply_state();
        }
        return TRUE;
    case WM_ENDSESSION:
        if (wp) request_exit(L"session ending");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open_settings(void)
{
    wchar_t exe[SP_MAX_PATH];
    path_join(exe, SP_COUNTOF(exe), paths_exe_dir(), L"ScreenPlus.exe");
    ShellExecuteW(NULL, NULL, exe, L"--screensaver-settings", NULL, SW_SHOWNORMAL);
}

int screensaver_main(HINSTANCE inst, int argc, wchar_t **argv, int asScr)
{
    int mode = asScr ? 'c' : 's';        /* a bare .scr is Explorer's "Configure" */
    HWND parent = NULL;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if ((a[0] != L'/' && a[0] != L'-') || !a[1] || (a[2] && a[2] != L':')) continue;
        wchar_t c = (wchar_t)towlower(a[1]);
        if (c == L's') mode = 's';
        else if (c == L'c') mode = 'c';
        else if (c == L'a') return 0;    /* Windows 9x password change */
        else if (c == L'p' || c == L'l') {
            mode = 'p';
            const wchar_t *v = a[2] == L':' ? a + 3 : (i + 1 < argc ? argv[++i] : L"");
            parent = (HWND)(uintptr_t)wcstoull(v, NULL, 10);
        }
    }
    if (mode == 'c') {
        open_settings();
        return 0;
    }
    if (mode == 'p' && (!parent || !IsWindow(parent))) return 1;
    if (mode == 's' && FindWindowW(SCR_CLASS, NULL)) return 0;   /* one is already running */

    S.inst = inst;
    S.run = mode == 's';
    S.parent = parent;
    log_init(L"screensaver");
    LOGI(L"screensaver %ls starting (%ls, pid %lu)", SP_VERSION_W, S.run ? (asScr ? L"/s" : L"preview from Screen+") : L"/p",
         GetCurrentProcessId());
    if (S.run && asScr && should_not_start()) {
        LOGI(L"screensaver: not starting over a fullscreen app, a presentation or the sign-in screen");
        log_shutdown();
        return 0;
    }
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    config_load(&S.cfg, paths_config());
    WallpaperEntry *e = config_find(&S.cfg, config_screensaver(&S.cfg));
    if (e && !e->missing && wallpaper_source_exists(e)) S.wp = *e;
    wchar_t udf[SP_MAX_PATH];
    path_join(udf, SP_COUNTOF(udf), paths_root(), L"WebView2-Screensaver");
    html_set_profile(udf);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = ctl_proc;
    wc.hInstance = inst;
    wc.lpszClassName = SCR_CTL_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = win_proc;
    wc.lpszClassName = SCR_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    S.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, SCR_CTL_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    HWND engine = NULL;
    if (S.ctl && S.run) {
        RAWINPUTDEVICE rid[2] = { { 0x01, 0x02, RIDEV_INPUTSINK, S.ctl }, { 0x01, 0x06, RIDEV_INPUTSINK, S.ctl } };
        if (!RegisterRawInputDevices(rid, 2, sizeof rid[0])) LOGW(L"screensaver: no raw input (err %lu)", GetLastError());
        S.pn[0] = RegisterPowerSettingNotification(S.ctl, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
        S.pn[1] = RegisterPowerSettingNotification(S.ctl, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
        SYSTEM_POWER_STATUS sps;
        if (GetSystemPowerStatus(&sps)) S.onBattery = sps.ACLineStatus == 0;
        if ((engine = FindWindowW(SP_ENGINE_CLASS, NULL)) != NULL)
            PostMessageW(engine, SP_MSG_SCREENSAVER, 1, (LPARAM)GetCurrentProcessId());
        ShowCursor(FALSE);
    }
    S.startMs = time_ms();
    if (S.ctl) {
        build_windows();
        apply_state();
    }
    MSG m;
    while (S.n && GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    S.quitting = 1;
    destroy_windows();
    video_shutdown();
    html_shutdown();
    if (engine && IsWindow(engine)) PostMessageW(engine, SP_MSG_SCREENSAVER, 0, (LPARAM)GetCurrentProcessId());
    for (int i = 0; i < 2; i++) if (S.pn[i]) UnregisterPowerSettingNotification(S.pn[i]);
    if (S.ctl) DestroyWindow(S.ctl);
    if (S.run) ShowCursor(TRUE);
    config_free(&S.cfg);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    LOGI(L"screensaver: stopped after %.1f s", (time_ms() - S.startMs) / 1000.0);
    log_shutdown();
    return 0;
}
