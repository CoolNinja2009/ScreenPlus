#include "app.h"
#include "../ui/ui.h"
#include "../tray/tray.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

AppState g_app;
static UINT g_msgTaskbarCreated;

#define WM_SP_SHELLCMD (WM_APP + 70)
enum { T_ENGINE_RESTART = 1 };

void app_notice(int kind, const wchar_t *fmt, ...)
{
    if (kind == NOTICE_NONE || !fmt) {
        g_app.notice[0] = 0;
        g_app.noticeKind = NOTICE_NONE;
    } else {
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(g_app.notice, SP_COUNTOF(g_app.notice), _TRUNCATE, fmt, ap);
        va_end(ap);
        g_app.noticeKind = kind;
    }
    ui_on_status();
}

void app_refresh_monitors(void)
{
    g_app.monCount = monitor_enumerate(g_app.mons, SP_MAX_MONITORS);
}

const IpcMonitorStatus *app_monitor_status(const wchar_t *monitorId)
{
    if (!g_app.haveStatus || !monitorId) return NULL;
    for (int i = 0; i < g_app.status.monitorCount && i < SP_MAX_MONITORS; i++)
        if (str_ieq(g_app.status.monitors[i].monitorId, monitorId)) return &g_app.status.monitors[i];
    return NULL;
}

const wchar_t *app_engine_state_text(int *severity)
{
    int sev = 0;
    const wchar_t *t = L"Active";
    const IpcEngineStatus *st = &g_app.status;
    int anyWallpaper = 0, anyFailed = 0, anyRecovering = 0;
    if (g_app.haveStatus) {
        for (int i = 0; i < st->monitorCount; i++) {
            if (st->monitors[i].wallpaperId[0]) anyWallpaper = 1;
            if (st->monitors[i].failed) anyFailed = 1;
            if (st->monitors[i].state == RS_RECOVERING) anyRecovering = 1;
        }
    }
    if (g_app.engineGaveUp) { sev = 3; t = L"Renderer stopped"; }
    else if (g_app.restartPending || !g_app.engineProcess) { sev = 2; t = L"Restarting renderer"; }
    else if (!g_app.haveStatus) { sev = 2; t = L"Starting"; }
    else if (g_app.cfg.s.paused) { sev = 1; t = L"Paused"; }
    else if (anyFailed) { sev = 3; t = L"Renderer error"; }
    else if (anyRecovering) { sev = 2; t = L"Recovering"; }
    else if (!anyWallpaper) { sev = 1; t = L"No wallpaper"; }
    else if (st->flags & (ENGINE_FLAG_LOCKED | ENGINE_FLAG_SLEEPING | ENGINE_FLAG_DISPLAY_OFF)) { sev = 1; t = L"Suspended"; }
    else if ((st->flags & ENGINE_FLAG_BATTERY) && g_app.cfg.s.pauseOnBattery) { sev = 1; t = L"Paused on battery"; }
    else if ((st->flags & ENGINE_FLAG_FULLSCREEN) && g_app.cfg.s.pauseOnFullscreen) { sev = 1; t = L"Paused for fullscreen app"; }
    else if (st->activeRenderers == 0) { sev = 0; t = L"Idle (desktop covered)"; }
    if (severity) *severity = sev;
    return t;
}

void app_save_config(void)
{
    if (!config_save(&g_app.cfg, paths_config()))
        app_notice(NOTICE_ERROR, L"Settings could not be saved. Check that %ls is writable.", paths_root());
    supervisor_send(CMD_RELOAD_CONFIG, 0);
    tray_update();
    ui_on_status();
}

/* Customise-panel changes live in <data>\props\<id>.json, not in config.ini — pushing
 * them does not reload the config, the renderers simply re-read their files. */
void app_apply_customise(void)
{
    supervisor_send(CMD_APPLY_CUSTOMISE, 0);
    ui_on_status();
}

const wchar_t *app_wallpaper_for(const wchar_t *monitorIdOrNull)
{
    if (monitorIdOrNull) return config_assignment(&g_app.cfg, monitorIdOrNull);
    if (g_app.cfg.defaultWallpaper[0]) return g_app.cfg.defaultWallpaper;
    return g_app.monCount ? config_assignment(&g_app.cfg, g_app.mons[0].id) : L"";
}

void app_apply_wallpaper(const wchar_t *wallpaperId, const wchar_t *monitorIdOrNull)
{
    WallpaperEntry *e = config_find(&g_app.cfg, wallpaperId);
    if (!e) return;
    e->lastUsed = time_unix_now();
    config_assign(&g_app.cfg, monitorIdOrNull, wallpaperId);
    LOGI(L"apply '%ls' (%ls) to %ls", e->name, wallpaper_type_name(e->type), monitorIdOrNull ? monitorIdOrNull : L"all displays");
    if (e->missing) app_notice(NOTICE_WARN, L"'%ls' could not be found at its original location.", e->name);
    app_save_config();
}

void app_set_paused(BOOL paused)
{
    g_app.cfg.s.paused = paused ? 1 : 0;
    config_save(&g_app.cfg, paths_config());
    supervisor_send(CMD_SET_PAUSED, g_app.cfg.s.paused);
    tray_update();
    ui_on_status();
}

void app_set_audio(BOOL on)
{
    g_app.cfg.s.audio = on ? 1 : 0;
    config_save(&g_app.cfg, paths_config());
    supervisor_send(CMD_SET_AUDIO, g_app.cfg.s.audio);
    tray_update();
    ui_on_status();
}

void app_next_wallpaper(void)
{
    Config *c = &g_app.cfg;
    if (!c->count) return;
    int cur = config_index_of(c, app_wallpaper_for(NULL));
    for (int k = 1; k <= c->count; k++) {
        WallpaperEntry *e = &c->items[(cur + k + c->count) % c->count];
        if (!e->missing) {
            app_apply_wallpaper(e->id, NULL);
            return;
        }
    }
}

void app_restart_renderer(void)
{
    app_notice(NOTICE_NONE, NULL);
    supervisor_restart();
}

void app_show_ui(void)
{
    ui_show();
}

void app_exit(void)
{
    if (g_app.exiting) return;
    g_app.exiting = 1;
    LOGI(L"exit requested");
    ui_destroy();
    supervisor_stop();
    tray_remove();
    DestroyWindow(g_app.shellWnd);
}

static void handle_shell_command(uint32_t cmd)
{
    switch (cmd) {
    case CMD_SHOW_UI:        app_show_ui(); break;
    case CMD_TOGGLE_PAUSE:   app_set_paused(!g_app.cfg.s.paused); break;
    case CMD_PAUSE:          app_set_paused(TRUE); break;
    case CMD_RESUME:         app_set_paused(FALSE); break;
    case CMD_NEXT_WALLPAPER: app_next_wallpaper(); break;
    case CMD_EXIT:           app_exit(); break;
    default: break;
    }
}

static LRESULT CALLBACK shell_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && msg) {
        tray_on_taskbar_created();
        return 0;
    }
    switch (msg) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds->dwData == IPC_ENGINE_HELLO && cds->cbData == sizeof(IpcHello)) {
            supervisor_on_hello((const IpcHello *)cds->lpData);
            return TRUE;
        }
        if (cds->dwData == IPC_ENGINE_STATUS && cds->cbData == sizeof(IpcEngineStatus)) {
            memcpy(&g_app.status, cds->lpData, sizeof g_app.status);
            g_app.haveStatus = 1;
            if (!g_app.wallpaperMs && g_app.status.firstFrame && g_app.engineRestarts == 0 &&
                g_app.status.firstFrame > g_app.processStart)
                g_app.wallpaperMs = (double)(g_app.status.firstFrame - g_app.processStart) / 10000.0;
            PostMessageW(h, WM_SP_STATUS, 0, 0);
            return TRUE;
        }
        if (cds->dwData == IPC_SHELL_CMD && cds->cbData == sizeof(IpcCommand)) {
            PostMessageW(h, WM_SP_SHELLCMD, ((const IpcCommand *)cds->lpData)->cmd, 0);
            return TRUE;
        }
        return FALSE;
    }
    case WM_SP_SHELLCMD:
        handle_shell_command((uint32_t)wp);
        return 0;
    case WM_SP_STATUS:
        tray_update();
        ui_on_status();
        return 0;
    case WM_SP_ENGINE_EXITED:
        supervisor_on_exit((DWORD)wp, (DWORD)lp);
        return 0;
    case WM_SP_TRAY:
        tray_on_message(wp, lp);
        return 0;
    case WM_COMMAND:
        tray_on_command(LOWORD(wp));
        return 0;
    case WM_TIMER:
        if (wp == T_ENGINE_RESTART) {
            KillTimer(h, T_ENGINE_RESTART);
            supervisor_on_timer();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        app_refresh_monitors();
        ui_on_status();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            LOGI(L"session ending");
            g_app.exiting = 1;
            supervisor_stop();
            tray_remove();
            log_shutdown();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void first_run(void)
{
    /* Seed the library with the bundled sample so the app is useful immediately. */
    wchar_t sample[SP_MAX_PATH];
    path_join(sample, SP_COUNTOF(sample), paths_exe_dir(), L"samples\\Aurora\\index.html");
    if (file_exists(sample)) {
        WallpaperEntry *e = config_add(&g_app.cfg, WP_HTML, sample, L"Aurora");
        if (e) LOGI(L"first run: added sample wallpaper");
    }
    config_save(&g_app.cfg, paths_config());
}

/* Setup records the installation type the user picked ("default" or "clean") in
 * HKCU\Software\ScreenPlus\SetupMode; it is applied once, on the next launch. */
static void apply_setup_choice(void)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenPlus", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    wchar_t mode[16] = L"";
    DWORD type = 0, size = sizeof mode - sizeof mode[0];
    if (RegQueryValueExW(key, L"SetupMode", NULL, &type, (BYTE *)mode, &size) == ERROR_SUCCESS && type == REG_SZ) {
        if (str_ieq(mode, L"clean")) {
            /* Empty library, no automatic Lively import; the previous configuration is kept as a copy. */
            wchar_t copy[SP_MAX_PATH];
            path_dirname(copy, SP_COUNTOF(copy), paths_config());
            path_join(copy, SP_COUNTOF(copy), copy, L"config.before-clean-install.ini");
            if (file_exists(paths_config())) CopyFileW(paths_config(), copy, FALSE);
            g_app.cfg.count = 0;
            g_app.cfg.assignCount = 0;
            g_app.cfg.defaultWallpaper[0] = 0;
            g_app.cfg.s.livelyImport = 0;
        } else {
            g_app.cfg.s.livelyImport = 1;
        }
        config_save(&g_app.cfg, paths_config());
        LOGI(L"applied the %ls installation chosen in setup", mode);
    }
    RegDeleteValueW(key, L"SetupMode");
    RegCloseKey(key);
}

int app_main(HINSTANCE inst, int argc, wchar_t **argv)
{
    uint32_t forward = CMD_SHOW_UI;
    int trayOnly = 0;
    for (int i = 1; i < argc; i++) {
        if (str_ieq(argv[i], L"--tray")) { trayOnly = 1; forward = CMD_NONE; }
        else if (str_ieq(argv[i], L"--show")) forward = CMD_SHOW_UI;
        else if (str_ieq(argv[i], L"--pause")) forward = CMD_PAUSE;
        else if (str_ieq(argv[i], L"--resume")) forward = CMD_RESUME;
        else if (str_ieq(argv[i], L"--toggle-pause")) forward = CMD_TOGGLE_PAUSE;
        else if (str_ieq(argv[i], L"--next")) forward = CMD_NEXT_WALLPAPER;
        else if (str_ieq(argv[i], L"--exit")) forward = CMD_EXIT;
    }

    /* Single instance: forward the request to the running shell instead of starting a second engine. */
    HANDLE mutex = CreateMutexW(NULL, TRUE, SP_INSTANCE_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = NULL;
        for (int i = 0; i < 30 && !(other = FindWindowW(SP_SHELL_CLASS, NULL)); i++) Sleep(100);
        if (other && forward != CMD_NONE) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            IpcCommand c = { forward, 0 };
            ipc_send(NULL, other, IPC_SHELL_CMD, &c, sizeof c, 3000);
        }
        CloseHandle(mutex);
        return 0;
    }
    if (forward == CMD_EXIT) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_app.inst = inst;
    g_app.processStart = process_creation_filetime(GetCurrentProcess());
    log_init(L"screenplus");
    diag_install_crash_handler(L"shell");
    LOGI(L"Screen+ %ls starting (pid %lu, Windows build %lu, data %ls)", SP_VERSION_W, GetCurrentProcessId(),
         windows_build_number(), paths_root());
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int existed = file_exists(paths_config());
    if (!config_load(&g_app.cfg, paths_config()) && !existed) first_run();
    int sw = startup_is_enabled();
    if (sw != g_app.cfg.s.startWithWindows) g_app.cfg.s.startWithWindows = sw;
    if (sw) startup_set_enabled(TRUE);      /* refresh the path if the executable moved */
    if (forward == CMD_PAUSE) g_app.cfg.s.paused = 1;
    if (forward == CMD_RESUME) g_app.cfg.s.paused = 0;
    apply_setup_choice();
    if (g_app.cfg.s.livelyImport)
        library_auto_import_lively(); /* non-interactive: finds newly installed Lively packages */
    library_check_missing();
    app_refresh_monitors();

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = shell_proc;
    wc.hInstance = inst;
    wc.lpszClassName = SP_SHELL_CLASS;
    RegisterClassExW(&wc);
    g_app.shellWnd = CreateWindowExW(WS_EX_TOOLWINDOW, SP_SHELL_CLASS, L"Screen+", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_app.shellWnd) {
        LOGE(L"could not create the shell window (err %lu)", GetLastError());
        return 1;
    }
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.shellWnd, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);

    supervisor_start();
    tray_init(g_app.shellWnd, inst);
    g_app.nativeInitMs = (double)(filetime_now() - g_app.processStart) / 10000.0;
    LOGI(L"native init complete: %.1f ms after process creation", g_app.nativeInitMs);

    if (!trayOnly && !g_app.cfg.s.launchToTray) app_show_ui();
    else if (forward == CMD_NEXT_WALLPAPER) app_next_wallpaper();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!ui_translate(&m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (!g_app.exiting) {
        g_app.exiting = 1;
        ui_destroy();
        supervisor_stop();
        tray_remove();
    }
    if (g_app.job) CloseHandle(g_app.job);   /* kills any leftover engine/WebView2 processes */
    config_free(&g_app.cfg);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    LOGI(L"Screen+ exited");
    log_shutdown();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
