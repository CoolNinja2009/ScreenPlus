/* screenplus-engine: the Hyprland renderer. One layer-shell background window per display, showing a wallpaper
 * through libmpv (video, GIF, image) or WebKitGTK (web pages). Like the Windows engine it reads config.ini
 * itself; the service sends SIGUSR1 to reload it and SIGTERM to stop. Works on any compositor with
 * wlr-layer-shell, but pause on fullscreen reads Hyprland's socket. */
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <epoxy/gl.h>
#include <dlfcn.h>
#include <locale.h>

#include "desktop.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../renderer/html/wallpaper_api.h"

/* A fixed table: mpv's update callback can outlive a teardown, so a slot's address must stay valid. */
typedef struct Slot {
    char                monitor[64];    /* connector name, "" = free */
    GdkRectangle        geo;
    GtkWindow          *win;
    GtkWidget          *gl;
    mpv_handle         *mpv;
    mpv_render_context *rc;
    WebKitWebView      *web;
    wchar_t             wallpaper[SP_ID_LEN];
    int                 span;
    int                 paused;
    JVal               *sent;           /* the property values the page has */
    char               *uri;            /* mpv: loaded once its render context exists */
    char                defScale[32];   /* mpv's own scaler, for Upscaling Off */
    Rec                 rec[REC_SECTIONS];   /* Customise recordings playing here */
    WallpaperAdjust     recBase;
    JVal               *recProps;
} Slot;

static Slot   g_slots[SP_MAX_MONITORS];
static Config g_cfg;
static int    g_appHeld, g_micHeld;      /* app rules: a pause app is focused; a mute app is recording */
static guint  g_micSource;
static GtkApplication *g_app;

/* ------------------------------------------------------------------ mpv */

static void *mpv_proc(void *ctx, const char *name)
{
    (void)ctx;
    static void *(*get)(const char *);
    if (!get) {
        void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);   /* GTK has it loaded already */
        if (egl) *(void **)&get = dlsym(egl, "eglGetProcAddress");
    }
    return get ? get(name) : NULL;
}

static gboolean mpv_redraw(gpointer data)
{
    Slot *s = (Slot *)data;
    if (s->rc && s->gl && (mpv_render_context_update(s->rc) & MPV_RENDER_UPDATE_FRAME))
        gtk_gl_area_queue_render(GTK_GL_AREA(s->gl));
    return G_SOURCE_REMOVE;
}

static void mpv_wants_redraw(void *data)        /* any mpv thread */
{
    g_idle_add(mpv_redraw, data);
}

static void gl_realize(GtkGLArea *area, gpointer data)
{
    Slot *s = (Slot *)data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) || !s->mpv) return;
    mpv_opengl_init_params gl = { mpv_proc, NULL };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl },
        { MPV_RENDER_PARAM_INVALID, NULL }
    };
    if (mpv_render_context_create(&s->rc, s->mpv, params) < 0) {
        LOGE(L"mpv: no render context");
        s->rc = NULL;
        return;
    }
    mpv_render_context_set_update_callback(s->rc, mpv_wants_redraw, s);
    const char *cmd[] = { "loadfile", s->uri, NULL };    /* not earlier: vo=libmpv fails without the context */
    if (s->uri) mpv_command(s->mpv, cmd);
}

static void gl_unrealize(GtkGLArea *area, gpointer data)
{
    Slot *s = (Slot *)data;
    gtk_gl_area_make_current(area);
    if (s->rc) {
        mpv_render_context_set_update_callback(s->rc, NULL, NULL);
        mpv_render_context_free(s->rc);
        s->rc = NULL;
    }
}

static gboolean gl_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
    (void)ctx;
    Slot *s = (Slot *)data;
    if (!s->rc) return TRUE;
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area)), flip = 1;
    mpv_opengl_fbo target = { fbo, gtk_widget_get_width(GTK_WIDGET(area)) * scale, gtk_widget_get_height(GTK_WIDGET(area)) * scale, 0 };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &target },
        { MPV_RENDER_PARAM_FLIP_Y, &flip },
        { MPV_RENDER_PARAM_INVALID, NULL }
    };
    mpv_render_context_render(s->rc, params);
    return TRUE;
}

static void mpv_look(Slot *s, const WallpaperEntry *e)
{
    WallpaperAdjust a = adjust_defaults();
    char v[16];
    wpapi_load_adjust(e->id, &a);
    snprintf(v, sizeof v, "%d", a.brightness); mpv_set_property_string(s->mpv, "brightness", v);
    snprintf(v, sizeof v, "%d", a.contrast);   mpv_set_property_string(s->mpv, "contrast", v);
    snprintf(v, sizeof v, "%d", a.saturation); mpv_set_property_string(s->mpv, "saturation", v);
    snprintf(v, sizeof v, "%d", a.gamma);      mpv_set_property_string(s->mpv, "gamma", v);
    snprintf(v, sizeof v, "%d", a.hue * 100 / 180); mpv_set_property_string(s->mpv, "hue", v);
}

/* Upscaling (Customise): Off keeps mpv's own scaler; Smooth is EWA Lanczos (enlarging only); Sharp is the sharper
 * EWA Lanczos plus the deblur of video.hlsl at display resolution, at any size. */
static const char k_casShader[] =
    "//!HOOK SCALED\n"
    "//!BIND HOOKED\n"
    "//!DESC Screen+ sharpen\n"
    "#define T(x, y) HOOKED_texOff(vec2(x, y)).rgb\n"
    "vec4 hook() {\n"
    "    vec4 c4 = HOOKED_tex(HOOKED_pos);\n"
    "    vec3 c = c4.rgb, n = T(0.0, -1.0), s = T(0.0, 1.0), w = T(-1.0, 0.0), e = T(1.0, 0.0);\n"
    "    vec3 nw = T(-1.0, -1.0), se = T(1.0, 1.0), ne = T(1.0, -1.0), sw = T(-1.0, 1.0);\n"
    "    vec3 n2 = T(0.0, -2.0), s2 = T(0.0, 2.0), w2 = T(-2.0, 0.0), e2 = T(2.0, 0.0);\n"
    "    const vec3 L = vec3(0.2126, 0.7152, 0.0722);\n"
    "    vec3 b1 = (c * 4.0 + (n + s + w + e) * 2.0 + (nw + ne + sw + se)) / 16.0;\n"
    "    vec3 b2 = (b1 * 4.0 + n2 + s2 + w2 + e2) / 8.0;\n"
    "    float d = dot(c - b1, L) * 1.6 + dot(b1 - b2, L) * 1.2;\n"
    "    vec3 mn = min(c, min(min(min(n, s), min(w, e)), min(min(nw, se), min(ne, sw))));\n"
    "    vec3 mx = max(c, max(max(max(n, s), max(w, e)), max(max(nw, se), max(ne, sw))));\n"
    "    return vec4(clamp(clamp(c + d, mn - 0.02, mx + 0.02), 0.0, 1.0), c4.a);\n"
    "}\n";

static void mpv_upscale(Slot *s, const WallpaperEntry *e)
{
    static char shader[1024];
    if (!shader[0]) {
        wchar_t p[SP_MAX_PATH];
        path_join(p, SP_COUNTOF(p), paths_cache(), L"sharpen.glsl");
        char *u = utf8_from_wide(p, NULL);
        FILE *f = u ? fopen(u, "wb") : NULL;
        if (f && fwrite(k_casShader, 1, sizeof k_casShader - 1, f) == sizeof k_casShader - 1) snprintf(shader, sizeof shader, "%s", u);
        if (f) fclose(f);
        free(u);
    }
    if (!s->defScale[0]) {
        char *d = mpv_get_property_string(s->mpv, "scale");
        snprintf(s->defScale, sizeof s->defScale, "%s", d ? d : "lanczos");
        mpv_free(d);
    }
    int m = wpapi_load_upscale(e->id);
    mpv_set_property_string(s->mpv, "scale", m == UPSCALE_SHARP ? "ewa_lanczossharp" : m == UPSCALE_SMOOTH ? "ewa_lanczos" : s->defScale);
    mpv_set_property_string(s->mpv, "glsl-shaders", m == UPSCALE_SHARP && shader[0] ? shader : "");
}

/* Span: each display shows its share of the picture. */
static void mpv_crop(Slot *s, const GdkRectangle *box)
{
    char vf[160];
    snprintf(vf, sizeof vf, "lavfi=[crop=iw*%.6f:ih*%.6f:iw*%.6f:ih*%.6f]",
             (double)s->geo.width / box->width, (double)s->geo.height / box->height,
             (double)(s->geo.x - box->x) / box->width, (double)(s->geo.y - box->y) / box->height);
    mpv_set_option_string(s->mpv, "vf", vf);
}

static GtkWidget *mpv_view(Slot *s, const WallpaperEntry *e, const GdkRectangle *span)
{
    s->mpv = mpv_create();
    if (!s->mpv) return NULL;
    mpv_set_option_string(s->mpv, "vo", "libmpv");
    mpv_set_option_string(s->mpv, "hwdec", span ? "auto-copy-safe" : "auto-safe");
    mpv_set_option_string(s->mpv, "loop-file", "inf");
    mpv_set_option_string(s->mpv, "image-display-duration", "inf");
    mpv_set_option_string(s->mpv, "panscan", "1.0");         /* fill the display, cropping if the shape differs */
    mpv_set_option_string(s->mpv, "keep-open", "yes");
    mpv_set_option_string(s->mpv, "idle", "yes");
    mpv_set_option_string(s->mpv, "input-default-bindings", "no");
    mpv_set_option_string(s->mpv, "audio-client-name", "Screen+");
    if (span) mpv_crop(s, span);
    if (mpv_initialize(s->mpv) < 0) {
        mpv_destroy(s->mpv);
        s->mpv = NULL;
        return NULL;
    }
    GtkWidget *gl = gtk_gl_area_new();
    gtk_gl_area_set_auto_render(GTK_GL_AREA(gl), FALSE);
    g_signal_connect(gl, "realize", G_CALLBACK(gl_realize), s);
    g_signal_connect(gl, "unrealize", G_CALLBACK(gl_unrealize), s);
    g_signal_connect(gl, "render", G_CALLBACK(gl_render), s);
    s->gl = gl;
    mpv_look(s, e);
    mpv_upscale(s, e);
    free(s->uri);
    s->uri = sp_file_uri(e->path);
    return gl;
}

/* ------------------------------------------------------------------ web */

static void web_send_properties(Slot *s, const WallpaperEntry *e)
{
    char *js = wpapi_properties_js(e, &s->sent);
    if (js) webkit_web_view_evaluate_javascript(s->web, js, -1, NULL, NULL, NULL, NULL, NULL);
    free(js);
}

/* The service's local web host for this page (webhost.c), else the file. */
static char *page_uri(const WallpaperEntry *e)
{
    wchar_t path[SP_MAX_PATH];
    path_join(path, SP_COUNTOF(path), paths_cache(), L"webhost.txt");
    char *p = sp_path_utf8(path), *text = NULL, token[32], *out = NULL;
    int port = 0;
    if (p && !wallpaper_is_url(e->path) && g_file_get_contents(p, &text, NULL, NULL) && sscanf(text, "%d %31s", &port, token) == 2) {
        char *id = utf8_from_wide(e->id, NULL), *name = utf8_from_wide(path_filename(e->path), NULL);
        char *safe = name ? g_uri_escape_string(name, NULL, FALSE) : NULL;
        if (id && safe) {
            size_t n = strlen(token) + strlen(id) + strlen(safe) + 48;
            out = (char *)malloc(n);
            if (out) snprintf(out, n, "http://%s-%s.localhost:%d/%s", token, id, port, safe);
        }
        g_free(safe);
        free(id);
        free(name);
    }
    g_free(text);
    free(p);
    return out ? out : sp_file_uri(e->path);
}

static void web_loaded(WebKitWebView *v, WebKitLoadEvent ev, gpointer data)
{
    (void)v;
    Slot *s = (Slot *)data;
    const WallpaperEntry *e = config_find(&g_cfg, s->wallpaper);
    if (ev == WEBKIT_LOAD_FINISHED && e && s->web) web_send_properties(s, e);
}

static GtkWidget *web_view(Slot *s, const WallpaperEntry *e)
{
    s->web = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *st = webkit_web_view_get_settings(s->web);
    webkit_settings_set_enable_webgl(st, TRUE);
    webkit_settings_set_enable_webaudio(st, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(st, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(st, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(st, FALSE);
    webkit_settings_set_hardware_acceleration_policy(st, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);

    char *shim = utf8_from_wide(wpapi_shim_js(), NULL);
    if (shim) {
        WebKitUserScript *us = webkit_user_script_new(shim, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(s->web), us);
        webkit_user_script_unref(us);
        free(shim);
    }
    char limit[640];
    sp_fps_js(g_cfg.s.webFps, limit, sizeof limit);
    if (limit[0]) {
        WebKitUserScript *us = webkit_user_script_new(limit, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(s->web), us);
        webkit_user_script_unref(us);
    }
    gtk_widget_set_can_target(GTK_WIDGET(s->web), g_cfg.s.htmlMouse);
    wchar_t feedPath[SP_MAX_PATH];
    path_join(feedPath, SP_COUNTOF(feedPath), paths_cache(), L"feed.js");
    char *fp = sp_path_utf8(feedPath), *feed = NULL;
    if (fp && g_file_get_contents(fp, &feed, NULL, NULL)) {
        WebKitUserScript *us = webkit_user_script_new(feed, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
        webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(s->web), us);
        webkit_user_script_unref(us);
        g_free(feed);
    }
    free(fp);
    g_signal_connect(s->web, "load-changed", G_CALLBACK(web_loaded), s);
    char *uri = page_uri(e);
    if (uri) webkit_web_view_load_uri(s->web, uri);
    free(uri);
    return GTK_WIDGET(s->web);
}

/* ------------------------------------------------------------------ slots */

/* ------------------------------------------------------------------ recordings (played from memory) */

static guint g_recSource;

static void rec_unload(Slot *s)
{
    for (int k = 0; k < REC_SECTIONS; k++) wprec_free(&s->rec[k]);
    json_free(s->recProps);
    s->recProps = NULL;
    if (s->wallpaper[0]) wpapi_live_set(s->wallpaper, NULL, NULL);
}

static void rec_load(Slot *s)
{
    rec_unload(s);
    if (!s->wallpaper[0]) return;
    wpapi_load_adjust(s->wallpaper, &s->recBase);
    for (int k = 0; k < REC_SECTIONS; k++)
        if (wprec_load(s->wallpaper, k, &s->rec[k]) && s->rec[k].paused > 0) wprec_free(&s->rec[k]);   /* paused: own settings hold */
}

static int rec_playing(const Slot *s) { return s->win && !s->paused && (s->rec[0].n || s->rec[1].n); }

static gboolean rec_tick(gpointer data)
{
    static unsigned ticks;
    (void)data;
    double now = wprec_now();
    int props = ++ticks % 3 == 0, any = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &g_slots[i];
        if (!rec_playing(s)) continue;
        any = 1;
        Rec *look = &s->rec[REC_LOOK], *pr = &s->rec[REC_PROPS];
        WallpaperAdjust a = s->recBase;
        if (look->n) wprec_look(look, wprec_pos(look, now), &a);
        int sendProps = pr->n && (props || !s->recProps);
        if (sendProps) {
            StrBuf b = { 0 };
            double pos = wprec_pos(pr, now);
            sb_puts(&b, "{");
            for (int k = 0; k < pr->count; k++) {
                JVal *v = wprec_value(&pr->t[k], pr->n, pos);
                if (!v) continue;
                if (b.len > 1) sb_puts(&b, ",");
                json_write_string(&b, pr->t[k].key);
                sb_puts(&b, ":");
                json_write(&b, v);
                json_free(v);
            }
            sb_puts(&b, "}");
            json_free(s->recProps);
            s->recProps = b.failed ? NULL : json_parse(b.p, b.len);
            sb_free(&b);
        }
        if (!look->n && !sendProps) continue;
        wpapi_live_set(s->wallpaper, look->n ? &a : NULL, s->recProps);
        WallpaperEntry *e = config_find(&g_cfg, s->wallpaper);
        if (!e) continue;
        if (s->mpv && look->n) mpv_look(s, e);
        if (s->web && sendProps) web_send_properties(s, e);
    }
    if (!any) g_recSource = 0;
    return any ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* The ~30 Hz playback tick runs only while a shown, unpaused display has a recording. */
static void rec_schedule(void)
{
    int any = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) any |= rec_playing(&g_slots[i]);
    if (any && !g_recSource) g_recSource = g_timeout_add(33, rec_tick, NULL);
}

static void slot_clear(Slot *s)
{
    rec_unload(s);
    if (s->win) gtk_window_destroy(s->win);     /* unrealizes the GL area, which frees mpv's context */
    if (s->mpv) mpv_terminate_destroy(s->mpv);
    json_free(s->sent);
    free(s->uri);
    s->uri = NULL;
    s->win = NULL;
    s->gl = NULL;
    s->mpv = NULL;
    s->web = NULL;
    s->sent = NULL;
    s->wallpaper[0] = 0;
    s->paused = 0;
}

static Slot *slot_for(const char *monitor)
{
    Slot *free_slot = NULL;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        if (!strcmp(g_slots[i].monitor, monitor)) return &g_slots[i];
        if (!g_slots[i].monitor[0] && !free_slot) free_slot = &g_slots[i];
    }
    if (free_slot) snprintf(free_slot->monitor, sizeof free_slot->monitor, "%s", monitor);
    return free_slot;
}

static void slot_open(Slot *s, GdkMonitor *mon, const WallpaperEntry *e, const GdkRectangle *span)
{
    GtkWidget *view = e->type == WP_HTML ? web_view(s, e) : mpv_view(s, e, span);
    if (!view) {
        LOGE(L"engine: could not show '%ls'", e->name);
        return;
    }
    GtkWindow *w = GTK_WINDOW(gtk_application_window_new(g_app));
    gtk_layer_init_for_window(w);
    gtk_layer_set_namespace(w, "screenplus");
    gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_monitor(w, mon);
    gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(w, -1);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_window_set_child(w, view);
    gtk_window_present(w);
    s->win = w;
    str_copy(s->wallpaper, SP_COUNTOF(s->wallpaper), e->id);
    s->span = span != NULL;
    LOGI(L"engine: '%ls' on %s", e->name, s->monitor);
}

static void slot_set_paused(Slot *s, int paused)
{
    if (s->paused == paused) return;
    s->paused = paused;
    if (s->mpv) mpv_set_property_string(s->mpv, "pause", paused ? "yes" : "no");
    if (s->web) gtk_widget_set_visible(GTK_WIDGET(s->web), !paused);   /* a hidden page stops drawing */
}

static void slot_set_sound(Slot *s, int on)
{
    if (s->mpv) mpv_set_property_string(s->mpv, "mute", on ? "no" : "yes");
    if (s->web) webkit_web_view_set_is_muted(s->web, !on);
}

/* ------------------------------------------------------------------ state */

static int should_pause(const Slot *s)
{
    static int battery;
    static gint64 checked;
    gint64 now = g_get_monotonic_time();
    if (now - checked > 2000000) {
        battery = sp_on_battery();
        checked = now;
    }
    return g_cfg.s.paused || g_appHeld || (g_cfg.s.pauseOnBattery && battery)
        || (g_cfg.s.pauseOnFullscreen && hypr_fullscreen_on(s->monitor));
}

static gboolean tick(gpointer data)
{
    (void)data;
    int held = g_cfg.s.pauseApps[0] && sp_pid_matches(hypr_active_pid(), g_cfg.s.pauseApps);
    if (held != g_appHeld) LOGI(L"app rules: %ls", held ? L"a pause app is focused; pausing" : L"pause app left; resuming");
    g_appHeld = held;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        Slot *s = &g_slots[i];
        if (!s->win) continue;
        slot_set_paused(s, should_pause(s));
        for (mpv_event *ev; s->mpv && (ev = mpv_wait_event(s->mpv, 0))->event_id != MPV_EVENT_NONE; ) {
            if (ev->event_id != MPV_EVENT_END_FILE) continue;
            mpv_event_end_file *end = (mpv_event_end_file *)ev->data;
            if (end->reason == MPV_END_FILE_REASON_ERROR) LOGE(L"mpv: %s: playback failed", s->monitor);
        }
    }
    rec_schedule();                                  /* a display came back from a pause */
    return G_SOURCE_CONTINUE;
}

/* Makes the windows match config.ini and the connected displays. */
static void sync_slots(void)
{
    GListModel *list = gdk_display_get_monitors(gdk_display_get_default());
    guint n = g_list_model_get_n_items(list);
    int span = g_cfg.s.displayMode == DM_SPAN && n > 1;
    GdkRectangle box = { 0, 0, 0, 0 };
    char seen[SP_MAX_MONITORS][64];
    int seenCount = 0;

    for (guint i = 0; i < n; i++) {
        GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(list, i));
        GdkRectangle g;
        gdk_monitor_get_geometry(m, &g);
        if (i == 0) box = g;
        else gdk_rectangle_union(&box, &g, &box);
        g_object_unref(m);
    }
    for (guint i = 0; i < n && seenCount < SP_MAX_MONITORS; i++) {
        GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(list, i));
        const char *name = gdk_monitor_get_connector(m);
        Slot *s = name ? slot_for(name) : NULL;
        if (s) {
            snprintf(seen[seenCount++], sizeof seen[0], "%s", name);
            wchar_t *id = wide_from_utf8(name, -1);
            const wchar_t *wp = id ? config_assignment(&g_cfg, id) : L"";
            WallpaperEntry *e = wp[0] ? config_find(&g_cfg, wp) : NULL;
            int web = e && e->type == WP_HTML;
            int useSpan = span && e && !web;            /* a page can't be cut: it repeats on each display */
            GdkRectangle g;
            gdk_monitor_get_geometry(m, &g);
            int moved = memcmp(&g, &s->geo, sizeof g) != 0;
            if (!e || !str_ieq(s->wallpaper, e->id) || s->span != useSpan || (useSpan && moved)) {
                slot_clear(s);
                s->geo = g;
                if (e) slot_open(s, m, e, useSpan ? &box : NULL);
            } else {
                s->geo = g;
                if (s->mpv) { mpv_look(s, e); mpv_upscale(s, e); }
                if (s->web) web_send_properties(s, e);
            }
            if (s->win) slot_set_sound(s, g_cfg.s.audio && !g_micHeld);
            if (s->win) rec_load(s);                 /* a recording made, edited or deleted */
            free(id);
        }
        g_object_unref(m);
    }
    for (int i = 0; i < SP_MAX_MONITORS; i++) {         /* unplugged displays */
        Slot *s = &g_slots[i];
        int found = !s->monitor[0];
        for (int k = 0; k < seenCount && !found; k++) found = !strcmp(seen[k], s->monitor);
        if (!found) {
            slot_clear(s);
            s->monitor[0] = 0;
        }
    }
    tick(NULL);
}

static void mic_check(void)
{
    int busy = sp_mic_busy(g_cfg.s.muteApps);
    if (busy == g_micHeld) return;
    LOGI(L"app rules: %ls", busy ? L"an app is using the microphone; sound muted" : L"microphone free; sound back");
    g_micHeld = busy;
    for (int i = 0; i < SP_MAX_MONITORS; i++)
        if (g_slots[i].win) slot_set_sound(&g_slots[i], g_cfg.s.audio && !g_micHeld);
}

static gboolean on_mic(gint fd, GIOCondition c, gpointer data)
{
    (void)fd; (void)c; (void)data;
    int r = sp_mic_watch_read();
    if (r < 0) {
        LOGW(L"app rules: pactl subscribe ended");
        sp_mic_watch_stop();
        g_micSource = 0;
        return G_SOURCE_REMOVE;
    }
    if (r) mic_check();
    return G_SOURCE_CONTINUE;
}

/* The microphone is watched only while mute_apps has an entry. */
static void mic_watch(void)
{
    int want = g_cfg.s.muteApps[0] != 0, fd;
    if (want && !g_micSource && (fd = sp_mic_watch_start()) >= 0)
        g_micSource = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_mic, NULL);
    else if (!want && g_micSource) {
        g_source_remove(g_micSource);
        g_micSource = 0;
        sp_mic_watch_stop();
    }
}

static void reload(void)
{
    config_free(&g_cfg);
    memset(&g_cfg, 0, sizeof g_cfg);
    config_load(&g_cfg, paths_config());
    mic_watch();
    g_micHeld = sp_mic_busy(g_cfg.s.muteApps);
    sync_slots();
}

static gboolean on_reload(gpointer data) { (void)data; reload(); return G_SOURCE_CONTINUE; }

static gboolean on_quit(gpointer data)
{
    (void)data;
    g_application_quit(G_APPLICATION(g_app));
    return G_SOURCE_REMOVE;
}

static void on_monitors(GListModel *list, guint pos, guint removed, guint added, gpointer data)
{
    (void)list; (void)pos; (void)removed; (void)added; (void)data;
    sync_slots();
}

static void on_activate(GApplication *app, gpointer data)
{
    (void)data;
    if (!gtk_layer_is_supported()) {
        LOGE(L"engine: this compositor has no wlr-layer-shell");
        g_application_quit(app);
        return;
    }
    setlocale(LC_NUMERIC, "C");                 /* mpv needs it */
    g_application_hold(app);
    g_signal_connect(gdk_display_get_monitors(gdk_display_get_default()), "items-changed", G_CALLBACK(on_monitors), NULL);
    g_unix_signal_add(SIGUSR1, on_reload, NULL);
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGINT, on_quit, NULL);
    g_timeout_add_seconds(1, tick, NULL);
    reload();
}

int main(int argc, char **argv)
{
    gtk_disable_setlocale();
    setlocale(LC_CTYPE, "");
    paths_init();
    log_init(L"engine");
    g_app = gtk_application_new("org.screenplus.Engine", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(g_app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(g_app), argc, argv);
    for (int i = 0; i < SP_MAX_MONITORS; i++) slot_clear(&g_slots[i]);
    g_object_unref(g_app);
    log_shutdown();
    return rc;
}
