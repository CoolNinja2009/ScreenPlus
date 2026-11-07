/* Recorded previews for web wallpapers, as src/app/preview_capture.c on Windows: the page runs for a few seconds in
 * kde/org.screenplus.capture, through plasmawindowed on Qt's offscreen platform, so nothing shows and a page that
 * misbehaves can only take that process down. ffmpeg makes <cache>/previews/<id>.jpg, and <id>.gif when the page moves. */
#include "../app/preview.h"
#include "../app/app.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "desktop.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEADLINE_MS 45000
#define QUEUE_MAX   256
#define PROBE_W     64
#define PROBE_H     36

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static wchar_t g_tried[QUEUE_MAX][SP_ID_LEN], g_queue[QUEUE_MAX][SP_ID_LEN];
static int     g_triedCount, g_count, g_running;
static HWND    g_notify;

void preview_paths(const wchar_t *id, wchar_t *gif, wchar_t *jpg, size_t cap)
{
    wchar_t dir[SP_MAX_PATH], name[SP_ID_LEN + 8];
    path_join(dir, SP_COUNTOF(dir), paths_cache(), L"previews");
    if (gif) {
        str_printf(name, SP_COUNTOF(name), L"%ls.gif", id);
        path_join(gif, cap, dir, name);
    }
    if (jpg) {
        str_printf(name, SP_COUNTOF(name), L"%ls.jpg", id);
        path_join(jpg, cap, dir, name);
    }
}

/* The biggest change between two frames, in percent, like frame_change on Windows. */
static double motion_of(const char *dir)
{
    enum { FRAME = PROBE_W * PROBE_H, MOST = 41 };
    static uint8_t px[FRAME * MOST + 1];
    char in[MAX_PATH + 16], vf[64];
    size_t got = 0;
    snprintf(in, sizeof in, "%s/%%03d.png", dir);
    snprintf(vf, sizeof vf, "scale=%d:%d,format=gray", PROBE_W, PROBE_H);
    const char *argv[] = { "ffmpeg", "-v", "quiet", "-nostdin", "-i", in, "-vf", vf, "-f", "rawvideo", "pipe:1", NULL };
    if (sp_run_read(argv, (char *)px, sizeof px, &got)) return 0.0;
    int n = (int)(got / FRAME);
    double most = 0.0;
    for (int i = 1; i < n; i++) {
        const uint8_t *a = px + (size_t)(i - 1) * FRAME, *b = px + (size_t)i * FRAME, *first = px;
        double d = 0.0, e = 0.0;
        for (int k = 0; k < FRAME; k++) {
            d += abs(a[k] - b[k]);
            e += abs(first[k] - b[k]);
        }
        most = SP_MAX(most, SP_MAX(d, e) / FRAME / 2.55);
    }
    return n >= 4 ? most : 0.0;
}

static int record(const wchar_t *id)
{
    const WallpaperEntry *e = config_find(&g_app.cfg, id);
    if (!e || e->type != WP_HTML) return 0;
    wchar_t gif[SP_MAX_PATH], jpg[SP_MAX_PATH], dirW[SP_MAX_PATH], name[SP_ID_LEN + 8];
    preview_paths(id, gif, jpg, SP_MAX_PATH);
    path_dirname(dirW, SP_COUNTOF(dirW), jpg);
    dir_create_recursive(dirW);
    str_printf(name, SP_COUNTOF(name), L"tmp-%ls", id);
    path_join(dirW, SP_COUNTOF(dirW), dirW, name);
    dir_create_recursive(dirW);
    char *dir = sp_path_utf8(dirW), *url = NULL, *shim = NULL, *script = NULL;
    kde_page_args(e, &url, &shim, &script);
    int ok = 0;
    pid_t pid = dir && url ? fork() : -1;
    if (pid == 0) {
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        if (nice(10) == -1) {}
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("plasmawindowed", "plasmawindowed", "org.screenplus.capture", dir, url, shim ? shim : "", script ? script : "", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        double end = time_ms() + DEADLINE_MS;
        int st = 0;
        pid_t r;
        char mid[MAX_PATH + 16], in[MAX_PATH + 16], last[MAX_PATH + 16];
        snprintf(last, sizeof last, "%s/done.png", dir);
        while ((r = waitpid(pid, &st, WNOHANG)) == 0 && time_ms() < end && access(last, R_OK)) Sleep(100);
        if (r == 0) {
            if (access(last, R_OK)) LOGW(L"preview: '%ls' ran out of time", e->name);
            else Sleep(300);                        /* the file is complete, the process lingers */
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
        }
        snprintf(mid, sizeof mid, "%s/020.png", dir);
        snprintf(in, sizeof in, "%s/%%03d.png", dir);
        if (access(mid, R_OK) == 0) {
            char *j = sp_path_utf8(jpg), *g = sp_path_utf8(gif);
            double motion = motion_of(dir);
            const char *still[] = { "nice", "-n", "10", "ffmpeg", "-y", "-v", "quiet", "-nostdin", "-i", mid, "-q:v", "3", j, NULL };
            ok = !sp_run(still, NULL, 0);
            if (ok && motion > 0.6) {
                const char *anim[] = { "nice", "-n", "10", "ffmpeg", "-y", "-v", "quiet", "-nostdin", "-framerate", "10", "-i", in, "-vf",
                                       "split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3", g, NULL };
                sp_run(anim, NULL, 0);
            } else {
                DeleteFileW(gif);
            }
            LOGI(L"preview: '%ls', motion %.2f", e->name, motion);
            free(j);
            free(g);
        }
    }
    if (dir) {
        const char *rm[] = { "rm", "-rf", dir, NULL };
        sp_run(rm, NULL, 0);
    }
    free(dir); free(url); free(shim); free(script);
    return ok;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        wchar_t id[SP_ID_LEN];
        pthread_mutex_lock(&g_lock);
        while (!g_count) pthread_cond_wait(&g_wake, &g_lock);
        str_copy(id, SP_COUNTOF(id), g_queue[0]);
        memmove(g_queue, g_queue + 1, sizeof g_queue[0] * (size_t)--g_count);
        pthread_mutex_unlock(&g_lock);
        if (record(id)) {
            wchar_t *copy = wcsdup_sp(id);
            if (copy && !PostMessageW(g_app.shellWnd, WM_SP_PREVIEW, (WPARAM)g_notify, (LPARAM)copy)) free(copy);
        }
    }
    return NULL;
}

void preview_request(const wchar_t *id, HWND notify)
{
    if (!id || !id[0] || !sp_on_path("plasmawindowed")) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_triedCount; i++)
        if (!wcscmp(g_tried[i], id)) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    if (g_triedCount < QUEUE_MAX) str_copy(g_tried[g_triedCount++], SP_ID_LEN, id);
    if (g_count < QUEUE_MAX) str_copy(g_queue[g_count++], SP_ID_LEN, id);
    if (notify) g_notify = notify;
    if (!g_running) {
        pthread_t t;
        g_running = !pthread_create(&t, NULL, worker, NULL);
        if (g_running) pthread_detach(t);
    }
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}
