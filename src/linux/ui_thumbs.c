/* Thumbnails for the Linux window: one worker thread, newest request first, like src/ui/thumbs.c. The system ffmpeg
 * decodes pictures and videos and crops them to the requested size; a web wallpaper uses its preview picture. */
#include "../ui/thumbs.h"
#include "../common/log.h"
#include "../common/util.h"
#include "desktop.h"
#include "win_ui.h"
#include <unistd.h>

typedef struct Job {
    struct Job *next;
    wchar_t  id[SP_ID_LEN];
    wchar_t  path[SP_MAX_PATH];
    int      w, h, large;
} Job;

static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static Job  *g_jobs;
static HWND  g_notify;
static int   g_running;

/* A web wallpaper's picture: its own thumbnail, else a preview file beside the page. */
static void source_of(const WallpaperEntry *e, wchar_t *out, size_t cap)
{
    static const wchar_t *const names[] = { L"preview.gif", L"preview.jpg", L"preview.png", L"thumbnail.jpg", L"thumbnail.png" };
    out[0] = 0;
    if (e->thumb[0] && file_exists(e->thumb)) {
        str_copy(out, cap, e->thumb);
        return;
    }
    if (e->type != WP_HTML) {
        if (!wallpaper_is_url(e->path)) str_copy(out, cap, e->path);
        return;
    }
    wchar_t dir[SP_MAX_PATH], p[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), e->path);
    for (size_t i = 0; i < SP_COUNTOF(names); i++) {
        path_join(p, SP_COUNTOF(p), dir, names[i]);
        if (file_exists(p)) {
            str_copy(out, cap, p);
            return;
        }
    }
}

static uint8_t *decode(const wchar_t *path, int w, int h)
{
    char *p = sp_path_utf8(path), vf[128];
    size_t need = (size_t)w * h * 4;
    uint8_t *px = (uint8_t *)malloc(need + 1);
    if (!p || !px) {
        free(p); free(px);
        return NULL;
    }
    snprintf(vf, sizeof vf, "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d", w, h, w, h);
    const char *argv[] = { "ffmpeg", "-v", "quiet", "-nostdin", "-i", p, "-frames:v", "1", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "bgra", "pipe:1", NULL };
    int rc = sp_run(argv, (char *)px, need + 1);
    free(p);
    if (rc) {
        free(px);
        return NULL;
    }
    return px;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_running && !g_jobs) pthread_cond_wait(&g_wake, &g_lock);
        Job *j = g_jobs;
        if (j) g_jobs = j->next;
        int run = g_running;
        pthread_mutex_unlock(&g_lock);
        if (!run) {
            free(j);
            return NULL;
        }
        ThumbResult *r = (ThumbResult *)calloc(1, sizeof *r);
        if (r) {
            str_copy(r->id, SP_ID_LEN, j->id);
            r->large = j->large;
            r->w = j->w;
            r->h = j->h;
            r->frames = 1;
            if (j->path[0]) r->bgra = decode(j->path, j->w, j->h);
            if (!PostMessageW(g_notify, WM_SP_THUMB, 0, (LPARAM)r)) thumbs_free(r);
        }
        free(j);
    }
}

void thumbs_start(HWND notify)
{
    if (g_running) return;
    g_notify = notify;
    g_running = 1;
    if (pthread_create(&g_thread, NULL, worker, NULL)) g_running = 0;
}

void thumbs_request(const WallpaperEntry *e, int w, int h, int large)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    if (!j) return;
    str_copy(j->id, SP_ID_LEN, e->id);
    source_of(e, j->path, SP_COUNTOF(j->path));
    j->w = w;
    j->h = h;
    j->large = large;
    pthread_mutex_lock(&g_lock);
    j->next = g_jobs;
    g_jobs = j;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

void thumbs_stop(void)
{
    if (!g_running) return;
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    while (g_jobs) {
        Job *n = g_jobs->next;
        free(g_jobs);
        g_jobs = n;
    }
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
}

void thumbs_free(ThumbResult *r)
{
    if (!r) return;
    free(r->bgra);
    free(r->delays);
    free(r);
}
