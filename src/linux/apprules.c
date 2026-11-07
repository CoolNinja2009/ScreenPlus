/* App rules on Linux (config.h's pause_apps / mute_apps).
 *  - Pause: the program in front is /proc/<pid>/exe's name, or its comm (a Wine game's exe is the Wine loader,
 *    its comm is "game.exe"). Hyprland gives the pid (hypr_active_pid); Plasma's plugin matches its active
 *    task's app id itself (FullscreenWatch.qml), from the PauseApps list the service writes.
 *  - Mute: an app using the microphone is a PipeWire/PulseAudio recording stream ("source output") whose
 *    process binary or app name is on the list (pactl -f json list source-outputs). `pactl subscribe` says when
 *    those come and go, so nothing is polled; one watcher per process. */
#include "desktop.h"
#include "../common/json.h"
#include "../common/util.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int has_utf8(const wchar_t *rules, const char *name)
{
    if (!name || !name[0]) return 0;
    wchar_t *w = wide_from_utf8(name, -1);
    int hit = w && config_rule_has(rules, w);
    free(w);
    return hit;
}

int sp_pid_matches(int pid, const wchar_t *rules)
{
    char path[64], buf[4096];
    if (pid <= 0 || !rules[0]) return 0;
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        char *del = strstr(buf, " (deleted)");      /* updated while running */
        if (del) *del = 0;
        if (has_utf8(rules, buf)) return 1;
    }
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return has_utf8(rules, buf);
}

int sp_mic_busy(const wchar_t *rules)
{
    static char out[256 * 1024];
    size_t got = 0;
    if (!rules[0]) return 0;
    const char *argv[] = { "pactl", "-f", "json", "list", "source-outputs", NULL };
    if (sp_run_read(argv, out, sizeof out, &got) != 0) return 0;
    JVal *v = json_parse(out, got);
    int busy = 0, any = config_rule_has(rules, L"*");
    for (int i = 0; v && v->t == J_ARR && i < v->n && !busy; i++) {
        const JVal *p = json_get(&v->kids[i], "properties");
        const char *bin = json_str(json_get(p, "application.process.binary"), "");
        if (!strncmp(bin, "screenplus", 10) || !strcmp(bin, "pavucontrol")) continue;   /* level meters aren't calls */
        busy = any || has_utf8(rules, bin) || has_utf8(rules, json_str(json_get(p, "application.name"), ""));
    }
    json_free(v);
    return busy;
}

static pid_t g_watch;
static int   g_fd = -1;

int sp_mic_watch_start(void)
{
    int p[2];
    if (g_fd >= 0) return g_fd;
    if (!sp_on_path("pactl") || pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(p[1], 1);
        dup2(null, 2);
        execlp("pactl", "pactl", "subscribe", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    g_watch = pid;
    g_fd = p[0];
    return g_fd;
}

/* 1: a recording stream came or went; 0: other events; -1: the watcher ended (stop it). */
int sp_mic_watch_read(void)
{
    char buf[4096];
    int hit = 0;
    for (;;) {
        ssize_t n = read(g_fd, buf, sizeof buf - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return hit;
        if (n <= 0) return -1;
        buf[n] = 0;
        hit |= strstr(buf, "source-output") != NULL;
    }
}

void sp_mic_watch_stop(void)
{
    if (g_fd >= 0) close(g_fd);
    if (g_watch > 0) {
        kill(g_watch, SIGTERM);
        while (waitpid(g_watch, NULL, 0) < 0 && errno == EINTR) {}
    }
    g_fd = -1;
    g_watch = 0;
}
