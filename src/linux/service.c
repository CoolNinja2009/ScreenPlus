/* `screenplus --daemon`: the Linux counterpart of the Windows shell process. It owns config.ini, answers
 * the CLI over a Unix socket (the same cli_server.c as Windows), and puts the wallpaper on screen:
 * through plasmashell on KDE, through a supervised screenplus-engine on Hyprland. No window, no tray. */
#include "service.h"
#include "desktop.h"
#include "../app/app.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../ui/ui.h"
#include <dlfcn.h>
#include <errno.h>
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

static int effective_pause(void)
{
    return g_app.cfg.s.paused || (g_app.cfg.s.pauseOnBattery && g_battery);
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

void app_refresh_monitors(void)
{
    MonitorInfo m[SP_MAX_MONITORS];
    int n = g_desk == DESK_KDE ? kde_monitors(m, SP_MAX_MONITORS) : g_desk == DESK_HYPRLAND ? hypr_monitors(m, SP_MAX_MONITORS) : 0;
    if (n <= 0) return;                 /* keep what we had: the desktop may be restarting */
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
    g_ui_mod.lib = lib;
    return 1;
}

void app_show_ui(void)
{
    if (ui_load() && g_ui_mod.show) g_ui_mod.show();
}

void app_notice(int kind, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, SP_COUNTOF(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    str_copy(g_app.notice, SP_COUNTOF(g_app.notice), buf);
    g_app.noticeKind = kind;
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
void depth_auto_all(BOOL fetchMissing) { (void)fetchMissing; }
void depth_forget(const WallpaperEntry *e) { (void)e; }
void update_schedule(BOOL soon) { (void)soon; }
BOOL update_check(BOOL install) { (void)install; return FALSE; }
void update_get(UpdateState *out)
{
    memset(out, 0, sizeof *out);
    str_copy(out->text, SP_COUNTOF(out->text), L"Update Screen+ with the release from GitHub or your package manager.");
}
void samples_schedule(BOOL soon) { (void)soon; }
BOOL samples_check(BOOL install) { (void)install; return FALSE; }
void samples_get(SamplesState *out)
{
    memset(out, 0, sizeof *out);
    str_copy(out->text, SP_COUNTOF(out->text), L"Sample updates aren't on Linux yet.");
}

BOOL startup_set_enabled(BOOL on)
{
    return sp_autostart_set(on);
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

    config_load(&g_app.cfg, paths_config());
    library_check_missing();
    library_purge();
    library_add_samples();
    g_battery = sp_on_battery();
    app_refresh_monitors();
    if (g_desk == DESK_KDE) kde_install_plugin();
    else engine_start();
    push_to_desktop();
    g_app.processStart = filetime_now();
    LOGI(L"service started (%ls)", g_desk == DESK_KDE ? L"KDE Plasma" : L"Hyprland");

    double nextPoll = time_ms() + 5000;
    while (!g_quit) {
        struct pollfd p = { ls, POLLIN, 0 };
        int open = g_ui_mod.is_open && g_ui_mod.is_open();
        int r = poll(&p, 1, open ? 15 : 500);
        if (open) g_ui_mod.pump();
        if (r > 0 && (p.revents & POLLIN)) {
            int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) serve_client(c);
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        url_import_tick();
        if (g_desk == DESK_HYPRLAND) engine_watch();
        if (time_ms() >= nextPoll) {
            nextPoll = time_ms() + 5000;
            int was = monitors_key();
            int bat = sp_on_battery();
            app_refresh_monitors();
            if (bat != g_battery || was != monitors_key()) {
                g_battery = bat;
                push_to_desktop();
            }
        }
    }

    LOGI(L"service stopping");
    ui_destroy();
    if (g_desk == DESK_KDE) kde_restore();
    else engine_stop();
    close(ls);
    char path[108];
    if (sp_socket_path(path, sizeof path)) unlink(path);
    log_shutdown();
    return 0;
}
