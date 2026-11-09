/* Thumbnails for the Linux window: one worker thread, newest request first, like src/ui/thumbs.c. The system ffmpeg
 * decodes pictures and videos and crops them to the requested size; a web wallpaper uses its preview picture.
 * Wallpapers that move then get a second, animated result, once every still is done. */
#include "../ui/thumbs.h"
#include "../app/preview.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../common/util.h"
#include "desktop.h"
#include "win_ui.h"
#include <unistd.h>

#define ANIM_FRAMES   40     /* the limits of src/ui/thumbs.c */
#define ANIM_MAX_W    240
#define ANIM_MAX_WL   480
#define ANIM_SECONDS  4.0

typedef struct Job {
    struct Job *next;
    wchar_t  id[SP_ID_LEN];
    wchar_t  path[SP_MAX_PATH];
    wchar_t  anim[SP_MAX_PATH];   /* where a moving preview comes from, if any */
    int      w, h, large;
} Job;

static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static Job  *g_jobs, *g_anims;     /* stills always go first */
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
    preview_paths(e->id, NULL, p, SP_COUNTOF(p));       /* recorded, or to be */
    if (file_exists(p)) str_copy(out, cap, p);
    else preview_request(e->id, g_notify);
}

static int is_gif(const wchar_t *path)
{
    size_t n = wcslen(path);
    return n > 4 && str_ieq(path + n - 4, L".gif");
}

/* The GIF a package names in its manifest (Lively "Preview", Wallpaper Engine "preview"). */
static int manifest_gif(const wchar_t *dir, const wchar_t *manifest, const char *key, wchar_t *out, size_t cap)
{
    wchar_t p[SP_MAX_PATH], name[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), dir, manifest);
    if (!file_exists(p)) return 0;
    JVal *j = json_parse_file(p);
    json_str_w(json_geti(j, key), name, SP_COUNTOF(name));
    json_free(j);
    if (!name[0]) return 0;
    path_join(out, cap, dir, name);
    return is_gif(out) && file_exists(out);
}

/* As in src/ui/thumbs.c: the package's preview GIF, a GIF wallpaper, or the video itself. Else a recorded one (preview.c). */
static int anim_source(const WallpaperEntry *e, wchar_t *out, size_t cap)
{
    if (e->flags & WPFLAG_DEPTH) return 0;
    if (e->type == WP_IMAGE) {
        if (!is_gif(e->path) || !file_exists(e->path)) return 0;
        str_copy(out, cap, e->path);
        return 1;
    }
    wchar_t dir[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), e->path);
    if (wallpaper_is_url(e->path)) {
        /* web address: no folder of its own */
    } else if (e->type == WP_HTML || e->format != WPF_PLAIN) {     /* the folder belongs to this wallpaper */
        if (manifest_gif(dir, L"LivelyInfo.json", "Preview", out, cap)) return 1;
        if (manifest_gif(dir, L"project.json", "preview", out, cap)) return 1;
        if (is_gif(e->thumb) && file_exists(e->thumb)) {
            str_copy(out, cap, e->thumb);
            return 1;
        }
        static const wchar_t *const names[] = { L"preview.gif", L"lively_p.gif" };
        for (size_t i = 0; i < SP_COUNTOF(names); i++) {
            path_join(out, cap, dir, names[i]);
            if (file_exists(out)) return 1;
        }
    }
    if (e->type == WP_VIDEO && file_exists(e->path)) {
        str_copy(out, cap, e->path);
        return 1;
    }
    if (e->type == WP_HTML) {
        /* No animation of its own, even with a still picture: record one. A recording that made only the still
         * means the page does not move. */
        wchar_t jpg[SP_MAX_PATH];
        preview_paths(e->id, NULL, jpg, SP_COUNTOF(jpg));
        preview_paths(e->id, out, NULL, cap);
        if (file_exists(out)) return 1;
        if (!file_exists(jpg)) preview_request(e->id, g_notify);
    }
    out[0] = 0;
    return 0;
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

/* A GIF's whole loop, or a few seconds of a video from 10% in, as evenly spaced frames. */
static void make_animation(const Job *j)
{
    int w = j->w, h = j->h, cap = j->large ? ANIM_MAX_WL : ANIM_MAX_W;
    if (w > cap) {
        h = SP_MAX(1, h * cap / w);
        w = cap;
    }
    char *p = sp_path_utf8(j->anim);
    if (!p) return;
    int gif = is_gif(j->anim);
    char info[256], vf[160], start[32], length[32], most[16];
    const char *probe[] = { "ffprobe", "-v", "quiet", "-count_packets", "-select_streams", "v:0",
                            "-show_entries", "format=duration:stream=nb_read_packets", "-of", "default=nw=1", p, NULL };
    const char *quick[] = { "ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "default=nw=1", p, NULL };
    double dur = 0.0, from = 0.0;
    int n = ANIM_FRAMES;
    if (!sp_run(gif ? probe : quick, info, sizeof info)) {
        const char *d = strstr(info, "duration="), *c = strstr(info, "nb_read_packets=");
        if (d) dur = atof(d + 9);
        if (gif && c) n = SP_MIN(n, atoi(c + 16));
    }
    if (!gif && (dur > 6.0 || dur <= 0.0)) {     /* no length (ffprobe missing, stream): the first seconds */
        from = dur > 6.0 ? dur * 0.1 : 0.0;
        dur = ANIM_SECONDS;
    }
    if (n < 2 || dur <= 0.0) {          /* one frame, or no idea how long: the still stays */
        free(p);
        return;
    }
    snprintf(start, sizeof start, "%.3f", from);
    snprintf(length, sizeof length, "%.3f", dur);
    snprintf(most, sizeof most, "%d", n);
    snprintf(vf, sizeof vf, "fps=%.4f,scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d", n / dur, w, h, w, h);
    size_t frame = (size_t)w * h * 4, got = 0;
    uint8_t *px = (uint8_t *)malloc(frame * n + 1);
    uint16_t *ms = (uint16_t *)malloc(sizeof *ms * n);
    const char *argv[] = { "nice", "-n", "10", "ffmpeg", "-v", "quiet", "-nostdin", "-ss", start, "-t", length, "-i", p,
                           "-frames:v", most, "-vf", vf, "-f", "rawvideo", "-pix_fmt", "bgra", "pipe:1", NULL };
    int rc = px && ms ? sp_run_read(argv, (char *)px, frame * n + 1, &got) : -1;
    free(p);
    int frames = (int)(got / frame);
    ThumbResult *r = !rc && frames > 1 ? (ThumbResult *)calloc(1, sizeof *r) : NULL;
    if (!r) {
        free(px);
        free(ms);
        return;
    }
    for (int i = 0; i < frames; i++) ms[i] = (uint16_t)SP_MAX(20, dur * 1000.0 / frames);
    str_copy(r->id, SP_ID_LEN, j->id);
    r->large = j->large;
    r->w = w;
    r->h = h;
    r->bgra = px;
    r->frames = frames;
    r->delays = ms;
    if (!PostMessageW(g_notify, WM_SP_THUMB, 0, (LPARAM)r)) thumbs_free(r);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_running && !g_jobs && !g_anims) pthread_cond_wait(&g_wake, &g_lock);
        Job *j = g_jobs, *a = j ? NULL : g_anims;
        if (j) g_jobs = j->next;
        else if (a) g_anims = a->next;
        int run = g_running;
        pthread_mutex_unlock(&g_lock);
        if (!run) {
            free(j);
            free(a);
            return NULL;
        }
        if (a) {
            make_animation(a);
            free(a);
            continue;
        }
        ThumbResult *r = (ThumbResult *)calloc(1, sizeof *r);
        if (r) {
            str_copy(r->id, SP_ID_LEN, j->id);
            r->large = j->large;
            r->w = j->w;
            r->h = j->h;
            r->frames = 1;
            if (j->path[0]) r->bgra = decode(j->path, j->w, j->h);
            int moves = r->bgra && j->anim[0];
            if (!PostMessageW(g_notify, WM_SP_THUMB, 0, (LPARAM)r)) thumbs_free(r);
            if (moves) {
                pthread_mutex_lock(&g_lock);
                j->next = g_anims;
                g_anims = j;
                pthread_mutex_unlock(&g_lock);
                continue;
            }
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
    if (!anim_source(e, j->anim, SP_COUNTOF(j->anim))) j->anim[0] = 0;
    j->w = w;
    j->h = h;
    j->large = large;
    pthread_mutex_lock(&g_lock);
    for (Job **p = &g_anims; *p; p = &(*p)->next) {     /* its still is being redone: the animation follows that */
        if ((*p)->large != large || wcscmp((*p)->id, e->id)) continue;
        Job *old = *p;
        *p = old->next;
        free(old);
        break;
    }
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
    Job **lists[] = { &g_jobs, &g_anims };
    for (int i = 0; i < 2; i++) {
        while (*lists[i]) {
            Job *n = (*lists[i])->next;
            free(*lists[i]);
            *lists[i] = n;
        }
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
