/* `screenplus --daemon`: the Linux counterpart of the Windows shell process. It owns config.ini, answers
 * the CLI over a Unix socket (the same cli_server.c as Windows), and puts the wallpaper on screen:
 * through plasmashell on KDE, through a supervised screenplus-engine on Hyprland. No window, no tray. */
#include "service.h"
#include "../app/preview.h"
#include "desktop.h"
#include "../app/app.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../ui/ui.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

AppState g_app;
uint64_t g_mainEntered;

static DesktopKind g_desk;
static pid_t       g_engine;
static int         g_battery;
static double      g_nextStart;       /* the engine's next start after a crash, 0: none */
static volatile sig_atomic_t g_quit;

static void on_signal(int s) { (void)s; g_quit = 1; }

/* ------------------------------------------------------------------ engine (Hyprland) */

static void engine_start(void)
{
    char *dir = sp_path_utf8(paths_exe_dir());
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/screenplus-engine", dir ? dir : ".");
    free(dir);
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    g_engine = pid > 0 ? pid : 0;
    g_app.engineProcess = g_engine ? (HANDLE)(intptr_t)g_engine : NULL;
    g_app.enginePid = (DWORD)g_engine;
    g_app.engineSpawnTime = filetime_now();
    LOGI(L"engine: started, pid %d", (int)g_engine);
}

static void engine_stop(void)
{
    if (!g_engine) return;
    kill(g_engine, SIGTERM);
    for (int i = 0; i < 40; i++) {
        if (waitpid(g_engine, NULL, WNOHANG) == g_engine) break;
        usleep(50000);
        if (i == 39) { kill(g_engine, SIGKILL); waitpid(g_engine, NULL, 0); }
    }
    g_engine = 0;
    g_app.engineProcess = NULL;
    g_app.enginePid = 0;
}

/* Up to five quick crashes, each waiting longer; then it stays down until `screenplus restart`. */
static void engine_watch(void)
{
    if (g_engine) {
        int st = 0;
        if (waitpid(g_engine, &st, WNOHANG) != g_engine) return;
        g_engine = 0;
        g_app.engineProcess = NULL;
        g_app.enginePid = 0;
        LOGW(L"engine: exited (status %d)", st);
        if (!g_app.cfg.s.autoRecover) {             /* the setting: stays down until `screenplus restart` */
            g_app.engineGaveUp = 1;
            return;
        }
        if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
            str_copy(g_app.engineError, SP_COUNTOF(g_app.engineError), L"screenplus-engine could not be started; are GTK 4, gtk4-layer-shell, libmpv and WebKitGTK installed?");
            g_app.engineGaveUp = 1;
            return;
        }
        if (++g_app.crashes > 5) {
            g_app.engineGaveUp = 1;
            return;
        }
        g_nextStart = time_ms() + 1000.0 * (1 << g_app.crashes);
    } else if (g_nextStart && time_ms() >= g_nextStart) {
        g_nextStart = 0;
        g_app.engineRestarts++;
        engine_start();
    }
}

/* ------------------------------------------------------------------ status and state */

static double g_noticeAt;
enum { NOTICE_INFO_MS = 10000 };

static int g_locked;

static int effective_pause(void)
{
    return g_app.cfg.s.paused || (g_app.cfg.s.pauseOnBattery && g_battery) || (g_app.cfg.s.suspendOnLock && g_locked);
}

/* Plasma's lock screen, through the freedesktop name every session has. */
static int session_locked(void)
{
    static char out[256];
    const char *argv[] = { "dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.ScreenSaver", "/ScreenSaver",
                           "org.freedesktop.ScreenSaver.GetActive", NULL };
    return !sp_run(argv, out, sizeof out) && strstr(out, "boolean true");
}

static void refresh_status(void)
{
    IpcEngineStatus *st = &g_app.status;
    memset(st, 0, sizeof *st);
    st->version = SP_IPC_VERSION;
    st->monitorCount = g_app.monCount;
    st->hostMode = g_desk == DESK_KDE ? HOSTMODE_PLASMA : HOSTMODE_NONE;
    if (g_app.cfg.s.paused) st->flags |= ENGINE_FLAG_PAUSED;
    if (g_battery) st->flags |= ENGINE_FLAG_BATTERY;
    for (int i = 0; i < g_app.monCount && i < SP_MAX_MONITORS; i++) {
        IpcMonitorStatus *m = &st->monitors[i];
        const WallpaperEntry *e = config_find(&g_app.cfg, config_assignment(&g_app.cfg, g_app.mons[i].id));
        str_copy(m->monitorId, SP_COUNTOF(m->monitorId), g_app.mons[i].id);
        if (e) {
            str_copy(m->wallpaperId, SP_COUNTOF(m->wallpaperId), e->id);
            m->type = e->type;
            st->activeRenderers++;
        }
        m->state = !e ? RS_STOPPED : effective_pause() ? RS_SUSPENDED : RS_ACTIVE;
    }
    g_app.haveStatus = 1;
}

static void push_to_desktop(void)
{
    refresh_status();
    feed_set_paused(effective_pause());
    feed_set_audio(g_app.cfg.s.htmlAudio);
    webhost_sync(&g_app.cfg);
    if (g_desk == DESK_KDE) {
        kde_apply(&g_app.cfg, g_app.mons, g_app.monCount, effective_pause());
    } else if (g_desk == DESK_HYPRLAND && g_engine) {
        kill(g_engine, SIGUSR1);        /* the engine re-reads config.ini */
    }
}

static int monitors_key(void)
{
    uint32_t h = (uint32_t)g_app.monCount;
    for (int i = 0; i < g_app.monCount; i++) h = h * 31 + hash_fnv1a_w(g_app.mons[i].id) + (uint32_t)g_app.mons[i].rect.right;
    return (int)h;
}

static int g_desktopGone, g_desktopBack;

void app_refresh_monitors(void)
{
    MonitorInfo m[SP_MAX_MONITORS];
    int n = g_desk == DESK_KDE ? kde_monitors(m, SP_MAX_MONITORS) : g_desk == DESK_HYPRLAND ? hypr_monitors(m, SP_MAX_MONITORS) : 0;
    if (n <= 0) {                       /* keep what we had: the desktop may be restarting */
        g_desktopGone = 1;
        return;
    }
    if (g_desktopGone) g_desktopBack = 1;
    g_desktopGone = 0;
    memcpy(g_app.mons, m, sizeof(MonitorInfo) * (size_t)n);
    g_app.monCount = n;
}

/* ------------------------------------------------------------------ the shell's API (app.h) */

void app_save_config(void)
{
    if (!config_save(&g_app.cfg, paths_config())) LOGE(L"config: could not save");
    push_to_desktop();
}

void app_apply_customise(void) { push_to_desktop(); }

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
    if (monitorIdOrNull && g_app.cfg.s.displayMode != DM_INDEPENDENT) g_app.cfg.s.displayMode = DM_INDEPENDENT;
    config_assign(&g_app.cfg, monitorIdOrNull, wallpaperId);
    depth_auto(wallpaperId);        /* "interactive images" setting */
    LOGI(L"apply '%ls' (%ls) to %ls", e->name, wallpaper_type_name(e->type), monitorIdOrNull ? monitorIdOrNull : L"all displays");
    app_save_config();
}

void app_set_paused(BOOL paused)
{
    g_app.cfg.s.paused = paused ? 1 : 0;
    app_save_config();
}

void app_set_audio(BOOL on)
{
    g_app.cfg.s.audio = on ? 1 : 0;
    app_save_config();
}

void app_set_display_mode(int mode)
{
    if (mode < DM_INDEPENDENT || mode > DM_SPAN || (int)g_app.cfg.s.displayMode == mode) return;
    g_app.cfg.s.displayMode = (DisplayMode)mode;
    app_save_config();
}

void app_set_screensaver(const wchar_t *wallpaperId)
{
    str_copy(g_app.cfg.screensaver, SP_ID_LEN, wallpaperId && config_find(&g_app.cfg, wallpaperId) ? wallpaperId : L"");
    app_save_config();
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
    if (g_desk == DESK_HYPRLAND) {
        engine_stop();
        g_app.crashes = 0;
        g_app.engineGaveUp = 0;
        g_nextStart = 0;
        engine_start();
    } else {
        push_to_desktop();
    }
}

void app_exit(void) { g_quit = 1; }

/* The window lives in screenplus-ui.so, loaded the first time it opens: the service itself needs only libc, and
 * a machine without SDL3 still has the wallpapers and the CLI. The module sees this program's symbols. */
static struct {
    void *lib;
    BOOL (*show)(void);
    void (*destroy)(void);
    BOOL (*is_open)(void);
    void (*on_config)(void);
    void (*on_customise)(void);
    void (*on_status)(void);
    void (*pump)(void);
    BOOL (*post)(HWND, UINT, WPARAM, LPARAM);
} g_ui_mod;

static int ui_load(void)
{
    if (g_ui_mod.lib) return 1;
    char *dir = sp_path_utf8(paths_exe_dir()), path[MAX_PATH];
    snprintf(path, sizeof path, "%s/screenplus-ui.so", dir ? dir : ".");
    free(dir);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        wchar_t *why = wide_from_utf8(dlerror(), -1);
        app_notice(NOTICE_ERROR, L"The window could not load: %ls", why ? why : L"");
        free(why);
        return 0;
    }
    *(void **)&g_ui_mod.show = dlsym(lib, "ui_show");
    *(void **)&g_ui_mod.destroy = dlsym(lib, "ui_destroy");
    *(void **)&g_ui_mod.is_open = dlsym(lib, "ui_is_open");
    *(void **)&g_ui_mod.on_config = dlsym(lib, "ui_on_config_changed");
    *(void **)&g_ui_mod.on_customise = dlsym(lib, "ui_on_customise_changed");
    *(void **)&g_ui_mod.on_status = dlsym(lib, "ui_on_status");
    *(void **)&g_ui_mod.pump = dlsym(lib, "ui_pump");
    *(void **)&g_ui_mod.post = dlsym(lib, "PostMessageW");
    g_ui_mod.lib = lib;
    return 1;
}

void app_show_ui(void)
{
    if (ui_load() && g_ui_mod.show) g_ui_mod.show();
}

void app_notice(int kind, const wchar_t *fmt, ...)
{
    if (kind == NOTICE_NONE || !fmt) {      /* the notice's close button */
        g_app.notice[0] = 0;
        g_app.noticeKind = NOTICE_NONE;
        return;
    }
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, SP_COUNTOF(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    str_copy(g_app.notice, SP_COUNTOF(g_app.notice), buf);
    g_app.noticeKind = kind;
    g_noticeAt = time_ms();
    log_write(kind == NOTICE_ERROR ? LOG_ERROR : kind == NOTICE_WARN ? LOG_WARN : LOG_INFO, L"%ls", buf);
}

const IpcMonitorStatus *app_monitor_status(const wchar_t *monitorId)
{
    for (int i = 0; i < g_app.status.monitorCount; i++)
        if (str_ieq(g_app.status.monitors[i].monitorId, monitorId)) return &g_app.status.monitors[i];
    return NULL;
}

const wchar_t *app_engine_state_text(int *severity)
{
    int sev = 0;
    const wchar_t *t = L"Active";
    if (g_app.engineGaveUp) { sev = 3; t = L"Renderer stopped"; }
    else if (g_desk == DESK_HYPRLAND && !g_engine) { sev = 2; t = L"Restarting renderer"; }
    else if (g_app.cfg.s.paused) { sev = 1; t = L"Paused"; }
    else if (effective_pause()) { sev = 1; t = L"Paused on battery"; }
    if (severity) *severity = sev;
    return t;
}

static size_t resident_bytes(pid_t pid)
{
    char path[64];
    long pages = 0, rss = 0;
    snprintf(path, sizeof path, "/proc/%d/statm", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
        fclose(f);
    }
    return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
}

void supervisor_sample_stats(void)
{
    g_app.shellMem = resident_bytes(getpid());
    g_app.engineMem = g_engine ? resident_bytes(g_engine) : 0;
    g_app.engineProcs = g_engine ? 1 : 0;
}

/* Not on Linux yet. cli_server.c and library.c call these. */
void ui_destroy(void) { if (g_ui_mod.destroy) g_ui_mod.destroy(); }
void ui_on_config_changed(void) { if (g_ui_mod.on_config) g_ui_mod.on_config(); }
void ui_on_customise_changed(void)
{
    if (g_ui_mod.on_customise) g_ui_mod.on_customise();
    push_to_desktop();
}
void ui_on_status(void) { if (g_ui_mod.on_status) g_ui_mod.on_status(); }
/* What depth.c asks of Windows-only parts. Qt WebEngine plays what it plays: an unplayable video ends as exit code 5. */
BOOL vcruntime_ok(wchar_t *found, size_t cap) { if (cap) found[0] = 0; return TRUE; }

double ffmpeg_duration(const wchar_t *path)
{
    char *p = sp_path_utf8(path), out[64] = "";
    const char *argv[] = { "ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "csv=p=0", p, NULL };
    int rc = p ? sp_run(argv, out, sizeof out) : -1;
    free(p);
    return rc ? 0.0 : atof(out);
}

BOOL webvideo_needed(const wchar_t *path)
{
    char *p = sp_path_utf8(path), out[32] = "";
    const char *argv[] = { "ffprobe", "-v", "quiet", "-select_streams", "v:0", "-show_entries", "stream=codec_name", "-of", "csv=p=0", p, NULL };
    int rc = p ? sp_run(argv, out, sizeof out) : -1;
    free(p);
    return !rc && !strncmp(out, "hevc", 4);
}

BOOL webvideo_make(const wchar_t *src, const wchar_t *dst, HANDLE cancel, void (*progress)(int percent))
{
    wchar_t cmd[3 * SP_MAX_PATH];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    str_printf(cmd, SP_COUNTOF(cmd), L"ffmpeg -y -nostdin -v quiet -i \"%ls\" -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p -c:a aac \"%ls\"", src, dst);
    if (progress) progress(-1);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) return FALSE;
    HANDLE h[2] = { pi.hProcess, cancel };
    if (WaitForMultipleObjects(2, h, FALSE, INFINITE) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

BOOL ui_is_open(void) { return g_ui_mod.is_open && g_ui_mod.is_open(); }

void shell_open(const wchar_t *target)
{
    char *p = sp_utf8_dup(target);
    const char *argv[] = { "xdg-open", p, NULL };
    if (p) sp_run(argv, NULL, 0);
    free(p);
}
BOOL startup_set_enabled(BOOL on)
{
    return sp_autostart_set(on);
}

/* ------------------------------------------------------------------ messages and timers */

/* What the Windows shell window does for the workers: their results come back to this thread. */
typedef struct { UINT msg; WPARAM wp; LPARAM lp; } Msg;
enum { MSG_MAX = 4096, TIMER_MAX = 8 };
static Msg             g_msgs[MSG_MAX];
static unsigned        g_msgHead, g_msgTail;
static pthread_mutex_t g_msgLock = PTHREAD_MUTEX_INITIALIZER;
static int             g_wake[2] = { -1, -1 }, g_child[2] = { -1, -1 };
static struct { UINT_PTR id; double due; void (*fn)(HWND, UINT, UINT_PTR, DWORD); } g_timers[TIMER_MAX];
static UINT_PTR g_nextTimer = 1000;

BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_wake[1] < 0) {                            /* the depth child: h is the service's pipe */
        Msg m = { msg, wp, lp };
        return write((int)(intptr_t)h, &m, sizeof m) == (ssize_t)sizeof m;
    }
    pthread_mutex_lock(&g_msgLock);
    int full = g_msgTail - g_msgHead >= MSG_MAX;
    if (!full) g_msgs[g_msgTail++ % MSG_MAX] = (Msg){ msg, wp, lp };
    pthread_mutex_unlock(&g_msgLock);
    if (!full && g_wake[1] >= 0 && write(g_wake[1], "", 1) < 0) {}
    return !full;
}

UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn)
{
    (void)h;
    if (!id) id = ++g_nextTimer;                    /* Win32: no window, Windows picks */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < TIMER_MAX; i++)
            if (pass ? !g_timers[i].id : g_timers[i].id == id) {
                g_timers[i].id = id;
                g_timers[i].due = time_ms() + ms;
                *(void **)&g_timers[i].fn = fn;
                return id;
            }
    return 0;
}

BOOL KillTimer(HWND h, UINT_PTR id)
{
    (void)h;
    for (int i = 0; i < TIMER_MAX; i++)
        if (g_timers[i].id == id) g_timers[i].id = 0;
    return TRUE;
}

static void run_messages(void)
{
    for (;;) {
        pthread_mutex_lock(&g_msgLock);
        int any = g_msgHead != g_msgTail;
        Msg m = any ? g_msgs[g_msgHead++ % MSG_MAX] : (Msg){ 0 };
        pthread_mutex_unlock(&g_msgLock);
        if (!any) break;
        switch (m.msg) {
        case WM_SP_STORE:   store_on_event(m.wp, m.lp); break;
        case WM_SP_DEPTH:   depth_on_event(m.wp, m.lp); break;
        case WM_SP_PREVIEW:                         /* wp: the window that asked */
            if (!ui_is_open() || !g_ui_mod.post || !g_ui_mod.post((HWND)m.wp, WM_SP_PREVIEW, 0, m.lp)) free((void *)m.lp);
            break;
        case WM_SP_UPDATE:  update_on_event(m.wp, m.lp); break;
        case WM_SP_SAMPLES: samples_on_event(m.wp, m.lp); break;
        }
    }
    double now = time_ms();
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].id || now < g_timers[i].due) continue;
        UINT_PTR id = g_timers[i].id;
        g_timers[i].id = 0;                         /* the handlers set the next one */
        if (g_timers[i].fn) g_timers[i].fn(NULL, 0, id, 0);
        else if (id == T_SHELL_UPDATE) update_on_timer();
        else if (id == T_SHELL_SAMPLES) samples_on_timer();
    }
}

/* ------------------------------------------------------------------ socket */

static int listen_socket(void)
{
    char path[108];
    if (!sp_socket_path(path, sizeof path)) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (!connect(fd, (struct sockaddr *)&a, sizeof a)) {   /* another service is answering */
        close(fd);
        return -2;
    }
    unlink(path);                                          /* left by a crash */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || chmod(path, 0600) || listen(fd, 8)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void serve_client(int fd)
{
    StrBuf in = { 0 };
    char buf[4096];
    ssize_t n;
    struct pollfd p = { fd, POLLIN, 0 };
    while (poll(&p, 1, 2000) > 0 && (n = read(fd, buf, sizeof buf)) > 0) {   /* the client shuts down its side */
        sb_append(&in, buf, (size_t)n);
        if (in.len > 1 << 20) break;
    }
    if (in.len && !in.failed) {
        StrBuf out = { 0 };
        int quit = cli_server_run(in.p, (DWORD)in.len, &out);
        for (size_t off = 0; !out.failed && off < out.len; ) {
            ssize_t w = write(fd, out.p + off, out.len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        sb_free(&out);
        if (quit) g_quit = 1;
    }
    sb_free(&in);
    close(fd);
}

int g_startupLaunch;

int service_main(void)
{
    g_mainEntered = filetime_now();
    paths_init();
    log_init(L"screenplus");
    g_desk = desktop_detect();
    if (g_desk == DESK_UNKNOWN) {
        LOGE(L"unsupported desktop: Screen+ for Linux runs on KDE Plasma and Hyprland");
        return 1;
    }
    int ls = listen_socket();
    if (ls == -2) return 0;
    if (ls < 0) {
        LOGE(L"could not open the control socket");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    /* First of all: at sign-in the wallpaper's page is already waiting for these */
    if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) || pipe(g_child)) return 1;
    fcntl(g_child[0], F_SETFD, FD_CLOEXEC);         /* the write end is what children report through */
    g_app.shellWnd = (HWND)(intptr_t)g_child[1];
    webhost_start();
    feed_start();

    config_load(&g_app.cfg, paths_config());
    g_app.cfg.s.startWithWindows = sp_autostart_get();      /* the entry is the truth */
    webhost_sync(&g_app.cfg);
    library_check_missing();
    library_purge();
    library_add_samples();
    g_battery = sp_on_battery();
    app_refresh_monitors();
    if (g_desk == DESK_KDE) kde_install_plugin();
    else engine_start();
    if (g_desk != DESK_KDE || g_app.monCount) push_to_desktop();    /* at sign-in Plasma isn't up yet: the loop applies when it is */
    g_app.processStart = filetime_now();
    LOGI(L"service started (%ls)", g_desk == DESK_KDE ? L"KDE Plasma" : L"Hyprland");

    depth_refresh_pages();
    depth_auto_all(FALSE);
    update_init();
    samples_init();
    double nextPoll = time_ms() + (g_app.monCount ? 5000 : 200);
    int windowDue = g_startupLaunch && !g_app.cfg.s.launchToTray;
    while (!g_quit) {
        struct pollfd pf[3] = { { ls, POLLIN, 0 }, { g_wake[0], POLLIN, 0 }, { g_child[0], POLLIN, 0 } };
        int open = g_ui_mod.is_open && g_ui_mod.is_open();
        int r = poll(pf, 3, open ? 15 : 500);
        if (open) g_ui_mod.pump();
        if (pf[1].revents & POLLIN) {
            char drain[256];
            if (read(g_wake[0], drain, sizeof drain) < 0) {}
        }
        if (pf[2].revents & POLLIN) {
            Msg m;
            if (read(g_child[0], &m, sizeof m) == (ssize_t)sizeof m) PostMessageW(NULL, m.msg, m.wp, m.lp);
        }
        run_messages();
        if (r > 0 && (pf[0].revents & POLLIN)) {
            int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) serve_client(c);
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        url_import_tick();
        store_on_import();
        if (g_desk == DESK_HYPRLAND) engine_watch();
        if (windowDue && g_app.monCount) {
            windowDue = 0;
            app_show_ui();
        }
        if (time_ms() >= nextPoll) {
            nextPoll = time_ms() + (g_app.monCount ? 5000 : 200);      /* the session is still starting: be quick */
            if (g_app.notice[0] && g_app.noticeKind == NOTICE_INFO && time_ms() - g_noticeAt >= NOTICE_INFO_MS) {
                app_notice(NOTICE_NONE, NULL);      /* warnings and errors stay until closed */
                ui_on_config_changed();
            }
            int was = monitors_key();
            int bat = sp_on_battery(), lock = g_desk == DESK_KDE && session_locked();
            app_refresh_monitors();
            int back = g_desktopBack;
            g_desktopBack = 0;
            if (back && g_app.cfg.s.autoRecover) LOGI(L"desktop: plasmashell is back; applying again");
            if (bat != g_battery || lock != g_locked || was != monitors_key() || (back && g_app.cfg.s.autoRecover)) {
                g_battery = bat;
                g_locked = lock;
                push_to_desktop();
            }
        }
    }

    LOGI(L"service stopping");
    ui_destroy();
    depth_shutdown();
    store_shutdown();
    update_shutdown();
    samples_shutdown();
    if (g_desk == DESK_KDE) kde_restore();
    else engine_stop();
    close(ls);
    char path[108];
    if (sp_socket_path(path, sizeof path)) unlink(path);
    log_shutdown();
    return 0;
}
