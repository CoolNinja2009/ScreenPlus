/* `screenplus`: the terminal interface on Linux, and the program's entry point. Commands travel to the service
 * over a Unix socket as the same JSON the Windows CLI sends; cli_server.c answers them. A few work without
 * the service: help, version, start, logs, paths, path, tools, startup.
 *
 * Exit codes: 0 ok, 1 the command failed, 2 bad usage, 3 Screen+ is not running. */
#include "service.h"
#include "desktop.h"
#include "../common/json.h"
#include "../common/paths.h"
#include "../common/util.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_NOT_RUNNING = 3 };

static int g_json, g_quiet;

static const char k_help[] =
    "Screen+ for Linux (KDE Plasma and Hyprland)\n"
    "\n"
    "Usage: screenplus [command] [--json] [--quiet]\n"
    "With no command: an interactive prompt.\n"
    "\n"
    "Service\n"
    "  show | hide            Open or close the window\n"
    "  start                  Start Screen+ in the background\n"
    "  stop                   Stop it and give the desktop its wallpaper back\n"
    "  restart                Restart the wallpapers\n"
    "  status                 What is running\n"
    "  startup [on|off]       Start at login\n"
    "\n"
    "Wallpapers\n"
    "  list                   The library\n"
    "  import <path|url>...   Add files, folders or links (--apply, --page, --download)\n"
    "  apply <wallpaper>      Set a wallpaper (--display N for one display)\n"
    "  next                   The next wallpaper\n"
    "  remove <wallpaper>     Remove it from the library\n"
    "  move <wallpaper> <n>   Put it at place n in the library (first, last)\n"
    "  displays               Connected displays\n"
    "  mode [independent|duplicate|span]\n"
    "  store [search <words> | more | get <n>]   The Store: list, search, download\n"
    "  sharp [file.onnx]      Your own SHARP model for Make wallpaper cooler (not included)\n"
    "\n"
    "Playback\n"
    "  pause | resume | toggle\n"
    "  audio [on|off|toggle]  Wallpaper sound\n"
    "  look [set <key> <value>... | reset]   Colour grading of what's showing\n"
    "  props [set <name> <value> | reset]    Settings of the wallpaper that's showing\n"
    "  settings [get <key> | set <key> <value>]\n"
    "  rules [pause|mute add|remove <app>]   Pause while an app is in front, mute while one uses the mic (* any)\n"
    "\n"
    "Other\n"
    "  logs [-n N] [-f]       Show the log\n"
    "  paths | path <name>    Where Screen+ keeps things\n"
    "  tools                  Whether yt-dlp and curl are installed\n"
    "  version | help\n";

static int cmd_paths(void)
{
    char *r = sp_path_utf8(paths_root()), *c = sp_path_utf8(paths_config()), *l = sp_path_utf8(paths_logs()), *e = sp_path_utf8(paths_exe());
    printf("Data:   %s\nConfig: %s\nLogs:   %s\nApp:    %s\n", r, c, l, e);
    free(r); free(c); free(l); free(e);
    return EXIT_OK;
}

static int cmd_path(const char *which)
{
    const wchar_t *p = !which ? NULL : !strcmp(which, "data") ? paths_root() : !strcmp(which, "config") ? paths_config()
                     : !strcmp(which, "logs") ? paths_logs() : !strcmp(which, "cache") ? paths_cache()
                     : !strcmp(which, "downloads") ? paths_downloads() : !strcmp(which, "app") ? paths_exe() : NULL;
    if (!p) {
        fputs("Usage: screenplus path <data|config|logs|cache|downloads|app>\n", stderr);
        return EXIT_USAGE;
    }
    char *u = sp_path_utf8(p);
    puts(u ? u : "");
    free(u);
    return EXIT_OK;
}

static int cmd_tools(void)
{
    printf("yt-dlp: %s\ncurl:   %s\n", sp_on_path("yt-dlp") ? "installed" : "missing (install it for video links)",
           sp_on_path("curl") ? "installed" : "missing (install it for direct links)");
    return EXIT_OK;
}

static int cmd_startup(const char *sub)
{
    if (sub && strcmp(sub, "status")) {
        if (strcmp(sub, "on") && strcmp(sub, "off")) {
            fputs("Usage: screenplus startup [on|off]\n", stderr);
            return EXIT_USAGE;
        }
        if (!sp_autostart_set(!strcmp(sub, "on"))) {
            fputs("Could not change the autostart entry.\n", stderr);
            return EXIT_FAILED;
        }
    }
    int on = sp_autostart_get();
    printf("Start at login: %s\n", on ? "on" : "off");
    if (on && desktop_detect() == DESK_HYPRLAND)
        puts("Hyprland doesn't read autostart entries. Add this to hyprland.conf: exec-once = screenplus start");
    return EXIT_OK;
}

static int cmd_logs(int argc, char **argv)
{
    int lines = 40, follow = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--lines")) && i + 1 < argc) lines = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--follow")) follow = 1;
    }
    wchar_t lp[SP_MAX_PATH];
    path_join(lp, SP_COUNTOF(lp), paths_logs(), L"screenplus.log");
    char *p = sp_path_utf8(lp), n[16];
    snprintf(n, sizeof n, "%d", lines);
    if (!file_exists(lp)) {
        puts("Nothing logged yet.");
        free(p);
        return EXIT_OK;
    }
    fflush(stdout);
    if (follow) execlp("tail", "tail", "-n", n, "-f", p, (char *)NULL);
    else execlp("tail", "tail", "-n", n, p, (char *)NULL);
    fprintf(stderr, "Could not run tail.\n");
    free(p);
    return EXIT_FAILED;
}

static int connect_service(void)
{
    char path[108];
    if (!sp_socket_path(path, sizeof path)) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int cmd_start(void)
{
    int fd = connect_service();
    if (fd >= 0) {
        close(fd);
        puts("Screen+ is already running.");
        return EXIT_OK;
    }
    DesktopKind k = desktop_detect();
    if (k == DESK_UNKNOWN) {
        fputs("This desktop isn't supported yet. Screen+ for Linux runs on KDE Plasma and Hyprland.\n", stderr);
        return EXIT_FAILED;
    }
    pid_t pid = fork();
    if (pid < 0) return EXIT_FAILED;
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        char *exe = sp_path_utf8(paths_exe());
        execl(exe, exe, "--daemon", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        usleep(100000);
        if ((fd = connect_service()) >= 0) {
            close(fd);
            if (!g_quiet) printf("Screen+ started on %s.\n", desktop_name(k));
            return EXIT_OK;
        }
    }
    fputs("Screen+ did not start. See: screenplus logs\n", stderr);
    return EXIT_FAILED;
}

static int send_command(int argc, char **argv)
{
    int fd = connect_service();
    if (fd < 0) {
        fputs("Screen+ is not running. Start it: screenplus start\n", stderr);
        return EXIT_NOT_RUNNING;
    }
    StrBuf q = { 0 };
    sb_puts(&q, "{\"argv\":[");
    for (int i = 0; i < argc; i++) {
        if (i) sb_puts(&q, ",");
        char full[MAX_PATH];
        /* a relative path means something only here: the service has its own working directory */
        const char *arg = realpath(argv[i], full) && argv[i][0] != '-' && i > 0 && (!strcmp(argv[0], "import") || !strcmp(argv[0], "add")
                          || !strcmp(argv[0], "apply") || !strcmp(argv[0], "set")) ? full : argv[i];
        json_write_string(&q, arg);
    }
    sb_puts(&q, "]}");
    size_t off = 0;
    while (!q.failed && off < q.len) {
        ssize_t w = write(fd, q.p + off, q.len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    sb_free(&q);
    shutdown(fd, SHUT_WR);

    StrBuf in = { 0 };
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) sb_append(&in, buf, (size_t)n);
    close(fd);
    JVal *j = in.len ? json_parse(in.p, in.len) : NULL;
    if (!j) {
        sb_free(&in);
        fputs("Screen+ gave no answer. See: screenplus logs\n", stderr);
        return EXIT_FAILED;
    }
    int ok = json_bool(json_get(j, "ok"), 0);
    if (!g_quiet) {
        if (g_json) {
            StrBuf d = { 0 };
            const JVal *data = json_get(j, "data");
            if (data) json_write(&d, data);
            puts(d.p ? d.p : "null");
            sb_free(&d);
        } else {
            fputs(json_str(json_get(j, "text"), ""), ok ? stdout : stderr);
        }
    }
    json_free(j);
    sb_free(&in);
    return ok ? EXIT_OK : EXIT_FAILED;
}

static int connect_probe(void)
{
    int fd = connect_service();
    if (fd < 0) return EXIT_NOT_RUNNING;
    close(fd);
    return EXIT_OK;
}

static int dispatch(int argc, char **argv)
{
    const char *c = argv[0];
    if (!strcmp(c, "help") || !strcmp(c, "-h") || !strcmp(c, "--help")) { fputs(k_help, stdout); return EXIT_OK; }
    if (!strcmp(c, "version") || !strcmp(c, "--version") || !strcmp(c, "-v")) { puts("Screen+ " SP_VERSION_STR); return EXIT_OK; }
    if (!strcmp(c, "start")) return cmd_start();
    if (!strcmp(c, "logs") || !strcmp(c, "log")) return cmd_logs(argc, argv);
    if (!strcmp(c, "paths")) return cmd_paths();
    if (!strcmp(c, "path")) return cmd_path(argc > 1 ? argv[1] : NULL);
    if (!strcmp(c, "tools")) return cmd_tools();
    if (!strcmp(c, "startup")) return cmd_startup(argc > 1 ? argv[1] : NULL);
    if (!strcmp(c, "show") && connect_probe() != EXIT_OK && cmd_start() != EXIT_OK) return EXIT_FAILED;
    if (!strcmp(c, "screensaver")) {
        fputs("Not available on Linux yet.\n", stderr);
        return EXIT_FAILED;
    }
    return send_command(argc, argv);
}

/* No arguments: read commands until EOF. */
static int prompt(void)
{
    char line[2048];
    puts("Screen+ for Linux. Type help, or exit to leave.");
    for (;;) {
        fputs("screenplus> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        char *args[64], *ctx = NULL;
        int n = 0;
        for (char *t = strtok_r(line, " \t\r\n", &ctx); t && n < 64; t = strtok_r(NULL, " \t\r\n", &ctx)) args[n++] = t;
        if (!n) continue;
        if (!strcmp(args[0], "exit") || !strcmp(args[0], "quit") || !strcmp(args[0], "q")) break;
        dispatch(n, args);
    }
    return EXIT_OK;
}

int cli_main(int argc, char **argv)
{
    char *args[64];
    int n = 0;
    paths_init();
    for (int i = 1; i < argc && n < 64; i++) {
        if (!strcmp(argv[i], "--json")) g_json = 1;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else args[n++] = argv[i];
    }
    return n ? dispatch(n, args) : prompt();
}

/* ------------------------------------------------------------------ autostart */

static int autostart_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    int n = x && x[0] ? snprintf(out, cap, "%s/autostart/screenplus.desktop", x)
                      : snprintf(out, cap, "%s/.config/autostart/screenplus.desktop", h ? h : "");
    return n > 0 && (size_t)n < cap;
}

int sp_autostart_get(void)
{
    char p[MAX_PATH];
    return autostart_path(p, sizeof p) && access(p, F_OK) == 0;
}

/* Plasma starts its systemd units before the XDG autostart ones, so on a systemd session the service comes up beside
 * plasmashell (it waits for it, service.c). The autostart entry stays for sessions without that target. */
static int unit_paths(char *unit, char *link, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    char base[MAX_PATH];
    if (x && x[0]) snprintf(base, sizeof base, "%s/systemd/user", x);
    else snprintf(base, sizeof base, "%s/.config/systemd/user", h ? h : "");
    int a = snprintf(unit, cap, "%s/screenplus.service", base), b = snprintf(link, cap, "%s/plasma-core.target.wants/screenplus.service", base);
    return a > 0 && (size_t)a < cap && b > 0 && (size_t)b < cap;
}

int sp_autostart_set(int on)
{
    char p[MAX_PATH], unit[MAX_PATH + 64], link_[MAX_PATH + 64];
    if (!autostart_path(p, sizeof p) || !unit_paths(unit, link_, sizeof unit)) return 0;
    if (!on) {
        unlink(link_);
        unlink(unit);
        return unlink(p) == 0 || errno == ENOENT;
    }
    wchar_t *w = wide_from_utf8(p, -1), dir[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), w);
    dir_create_recursive(dir);
    free(w);
    FILE *f = fopen(p, "w");
    if (!f) return 0;
    char *exe = sp_path_utf8(paths_exe());
    fprintf(f, "[Desktop Entry]\nType=Application\nName=Screen+\nExec=\"%s\" --daemon --startup\nX-KDE-autostart-phase=1\nNoDisplay=true\n", exe ? exe : "screenplus");
    int ok = fclose(f) == 0;
    if (desktop_detect() == DESK_KDE) {
        w = wide_from_utf8(link_, -1);
        path_dirname(dir, SP_COUNTOF(dir), w);
        dir_create_recursive(dir);
        free(w);
        if ((f = fopen(unit, "w"))) {
            fprintf(f, "[Unit]\nDescription=Screen+\nPartOf=graphical-session.target\n\n[Service]\nExecStart=\"%s\" --daemon --startup\n", exe ? exe : "screenplus");
            ok = fclose(f) == 0 && ok;
            unlink(link_);
            if (symlink(unit, link_)) ok = 0;
        }
    }
    free(exe);
    return ok;
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    if (argc > 1 && !strcmp(argv[1], "--daemon")) {
        g_startupLaunch = argc > 2 && !strcmp(argv[2], "--startup");
        return service_main();
    }
    if (argc > 1 && !strcmp(argv[1], "--depth")) {
        char mod[MAX_PATH];
        paths_init();
        char *dir = sp_path_utf8(paths_exe_dir());
        snprintf(mod, sizeof mod, "%s/screenplus-depth.so", dir ? dir : ".");
        free(dir);
        void *lib = dlopen(mod, RTLD_NOW | RTLD_LOCAL);
        int (*run)(int, char **) = NULL;
        if (lib) *(void **)&run = dlsym(lib, "depth_child_main");
        if (!run) {
            fprintf(stderr, "%s\n", dlerror());
            return 3;
        }
        return run(argc, argv);
    }
    return cli_main(argc, argv);
}
