/* Small process and system helpers shared by the service and the engine. */
#include "desktop.h"
#include "../common/util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

DesktopKind desktop_detect(void)
{
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) return DESK_HYPRLAND;
    if (d && strcasestr(d, "hyprland")) return DESK_HYPRLAND;
    if ((d && strcasestr(d, "kde")) || getenv("KDE_FULL_SESSION")) return DESK_KDE;
    return DESK_UNKNOWN;
}

const char *desktop_name(DesktopKind k)
{
    return k == DESK_KDE ? "KDE Plasma" : k == DESK_HYPRLAND ? "Hyprland" : "unsupported desktop";
}

int sp_socket_path(char *out, size_t cap)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    int n = dir && dir[0] ? snprintf(out, cap, "%s/%s", dir, SP_SOCKET_NAME)
                          : snprintf(out, cap, "/tmp/%s-%u", SP_SOCKET_NAME, (unsigned)getuid());
    return n > 0 && (size_t)n < cap;
}

int sp_run_read(const char *const *argv, char *out, size_t cap, size_t *got)
{
    int fd[2] = { -1, -1 };
    if (out && pipe2(fd, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(fd[0]); close(fd[1]); }
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(out ? fd[1] : null, 1);
        dup2(null, 2);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    size_t have = 0;
    if (out) {
        close(fd[1]);
        for (;;) {
            char skip[4096];
            size_t room = have + 1 < cap ? cap - have - 1 : 0;
            ssize_t n = room ? read(fd[0], out + have, room) : read(fd[0], skip, sizeof skip);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (room) have += (size_t)n;
        }
        close(fd[0]);
        if (cap) out[have] = 0;
    }
    if (got) *got = have;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st) == 127 ? -1 : WEXITSTATUS(st);
}

int sp_run(const char *const *argv, char *out, size_t cap) { return sp_run_read(argv, out, cap, NULL); }

int sp_on_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path), *ctx = NULL;
    int found = 0;
    for (char *d = copy ? strtok_r(copy, ":", &ctx) : NULL; d && !found; d = strtok_r(NULL, ":", &ctx)) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s/%s", d, name);
        found = access(p, X_OK) == 0;
    }
    free(copy);
    return found;
}

static int read_small(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    out[n > 0 ? n : 0] = 0;
    return n > 0;
}

int sp_on_battery(void)
{
    const char *power = getenv("SCREENPLUS_POWER_DIR");     /* tests */
    if (!power) power = "/sys/class/power_supply";
    DIR *d = opendir(power);
    if (!d) return 0;
    int mains = 0, online = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[512], v[32];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s/type", power, e->d_name);
        if (!read_small(p, v, sizeof v) || strncmp(v, "Mains", 5)) continue;
        mains = 1;
        snprintf(p, sizeof p, "%s/%s/online", power, e->d_name);
        if (read_small(p, v, sizeof v) && v[0] == '1') online = 1;
    }
    closedir(d);
    return mains && !online;
}

char *sp_file_uri(const wchar_t *path)
{
    char *p = wallpaper_is_url(path) ? utf8_from_wide(path, NULL) : sp_path_utf8(path);
    if (!p || wallpaper_is_url(path)) return p;
    size_t n = strlen(p);
    char *o = (char *)malloc(n * 3 + 8);
    if (o) {
        char *w = o + sprintf(o, "file://");
        for (const unsigned char *c = (const unsigned char *)p; *c; c++) {
            if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("/-_.~", *c)) *w++ = (char)*c;
            else w += sprintf(w, "%%%02X", *c);
        }
        *w = 0;
    }
    free(p);
    return o;
}

/* The port and token kept in `path`; 0 and the token as it was when there are none. */
int sp_keep_port(const wchar_t *path, char *token, size_t cap)
{
    char *p = sp_path_utf8(path), t[64];
    FILE *f = p ? fopen(p, "r") : NULL;
    int port = 0;
    free(p);
    if (f) {
        if (fscanf(f, "%d %63s", &port, t) == 2 && strlen(t) + 1 == cap) snprintf(token, cap, "%s", t);
        else port = 0;
        fclose(f);
    }
    return port;
}

void sp_save_port(const wchar_t *path, int port, const char *token)
{
    char *p = sp_path_utf8(path);
    int fd = p ? open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    free(p);
    if (fd < 0) return;
    dprintf(fd, "%d %s\n", port, token);
    close(fd);
}

/* The web_fps setting: requestAnimationFrame held to the limit, before the page's own scripts. */
void sp_fps_js(int fps, char *out, size_t cap)
{
    if (fps <= 0) { out[0] = 0; return; }
    snprintf(out, cap, "(function(){var r=window.requestAnimationFrame.bind(window),q={},id=0,pend=false,last=0,ms=1000/%d;"
                       "function run(t){if(t-last<ms-2){r(run);return;}pend=false;last=t;var o=q;q={};"
                       "for(var k in o){try{o[k](t);}catch(e){setTimeout(function(){throw e;});}}}"
                       "window.requestAnimationFrame=function(f){q[++id]=f;if(!pend){pend=true;r(run);}return id;};"
                       "window.cancelAnimationFrame=function(i){delete q[i];};})();\n", fps);
}
