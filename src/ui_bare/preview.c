/* preview.c: the Screen+ Bare window on sample data, no service needed.
 *
 *   screenplus-bare-preview [--page library|displays|customise|store|settings] [--light]
 *
 * SP_BACKDROP=gen draws a generated picture behind the glass instead of the
 * live desktop (SP_BACKDROP=<file> one of yours), for repeatable captures. */
#include "screenplus_bare.h"
#include "sp_kit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Wall { const char *id, *name, *type, *format, *file, *tags; bool fav; } Wall;

static Wall walls[] = {
    { "singularity", "Singularity", "html", "lively", "index.html", "test 3", true },
    { "aurora", "Aurora Drift", "video", "file", "aurora.mp4", "", false },
    { "koi", "Koi Pond", "image", "file", "koi.jpg", "Test2", true },
    { "neon-rain", "Neon Rain", "html", "lively", "index.html", "", false },
    { "dunes", "Dunes at Dusk", "video", "file", "dunes.webm", "test 3", false },
    { "glacier", "Glacier", "image", "file", "glacier.png", "", false },
    { "fluid", "Fluid Simulation", "html", "lively", "index.html", "Test2", false },
    { "city", "City Lights", "video", "file", "city.mp4", "", true },
    { "forest", "Misty Forest", "image", "file", "forest.jpg", "", false },
    { "particles", "Particles", "html", "lively", "index.html", "", false },
    { "ocean", "Ocean Waves", "video", "file", "ocean.mp4", "test 3", false },
    { "nebula", "Nebula", "image", "file", "nebula.png", "", false },
};
#define WALLS (int)(sizeof walls / sizeof walls[0])

typedef struct Setting { const char *key; char value[32]; bool is_bool, is_num; } Setting;
static Setting settings[] = {
    { "start_with_windows", "on", true }, { "launch_to_tray", "off", true }, { "still_wallpaper", "on", true },
    { "pause_on_fullscreen", "on", true }, { "pause_when_covered", "on", true }, { "suspend_on_lock", "on", true },
    { "pause_on_battery", "off", true }, { "auto_recover", "on", true }, { "audio", "off", true },
    { "volume", "60", false, true }, { "html_mouse", "on", true }, { "html_audio", "on", true }, { "web_fps", "60" },
    { "slideshow", "off", true }, { "slideshow_minutes", "15", false, true }, { "shuffle", "off", true },
    { "schedule", "off", true }, { "lively_auto_import", "on", true }, { "url_import", "on", true },
    { "interactive_images", "off", true }, { "screensaver_audio", "off", true }, { "screensaver_displays", "all" },
    { "screensaver_exit_on_mouse", "on", true }, { "auto_update", "on", true }, { "auto_update_samples", "on", true },
    { "theme", "dark" }, { "ui_motion", "full" }, { "accent_color", "#00D705" }, { "library_sort", "custom" },
    { "display_mode", "independent" }, { "language", "en" },
};
#define SETTINGS (int)(sizeof settings / sizeof settings[0])

typedef struct Preview {
    SpView view;
    SpModel model;
    JVal *status, *settings, *list, *look, *store;
    int on_screen;
    bool paused, audio;
    int look_values[5];
    BareImage thumbs[64];
    BareImage backdrop;
    char message[320];
} Preview;

static Setting *setting(const char *key) {
    for (int i = 0; i < SETTINGS; i++) if (!strcmp(settings[i].key, key)) return &settings[i];
    return NULL;
}

static void wall_json(StrBuf *b, int i) {
    const Wall *w = &walls[i];
    sb_printf(b, "{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"format\":\"%s\",\"path\":\"C:\\\\Wallpapers\\\\%s\\\\%s\","
                 "\"thumbnail\":\"gen:%d\",\"missing\":false,\"favourite\":%s,\"volume\":100,\"collections\":\"%s\"}",
              w->id, w->name, w->type, w->format, w->id, w->file, i, w->fav ? "true" : "false", w->tags);
}

static JVal *parse(StrBuf *b) {
    JVal *v = b->failed ? NULL : json_parse(b->p, b->len);
    sb_free(b);
    return v;
}

static void rebuild(Preview *p) {
    json_free(p->status); json_free(p->settings); json_free(p->list); json_free(p->look); json_free(p->store);
    StrBuf b = { 0 };
    sb_printf(&b, "{\"version\":\"2.1.0\",\"state\":\"%s\",\"severity\":0,\"paused\":%s,\"audio\":%s,"
                  "\"renderer\":{\"pid\":0},\"memoryMB\":{\"app\":38,\"renderer\":74},\"displays\":["
                  "{\"index\":1,\"name\":\"XV272U X1\",\"id\":\"d1\",\"width\":2560,\"height\":1440,\"hz\":170,"
                  "\"primary\":true,\"state\":\"Playing\",\"covered\":true,"
                  "\"renderer\":\"WebView2 \\u00b7 Lively \\u00b7 audio-reactive \\u00b7 60 fps\",\"wallpaper\":",
              p->paused ? "Paused" : "Idle (desktop covered)", p->paused ? "true" : "false", p->audio ? "true" : "false");
    wall_json(&b, p->on_screen);
    sb_puts(&b, "}]}");
    p->status = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{");
    for (int i = 0; i < SETTINGS; i++) {
        const Setting *s = &settings[i];
        if (i) sb_puts(&b, ",");
        if (s->is_bool) sb_printf(&b, "\"%s\":%s", s->key, strcmp(s->value, "on") ? "false" : "true");
        else if (s->is_num) sb_printf(&b, "\"%s\":%s", s->key, s->value);
        else sb_printf(&b, "\"%s\":\"%s\"", s->key, s->value);
    }
    sb_puts(&b, "}");
    p->settings = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "[");
    int n = 0;
    for (int rep = 0; rep < 5; rep++)
        for (int i = 0; i < WALLS && n < 59; i++, n++) {
            if (n) sb_puts(&b, ",");
            sb_printf(&b, "{\"number\":%d,\"onScreen\":%s,\"wallpaper\":", n + 1, n == p->on_screen ? "true" : "false");
            wall_json(&b, i);
            sb_puts(&b, "}");
        }
    sb_puts(&b, "]");
    p->list = parse(&b);
    b = (StrBuf){ 0 };
    sb_printf(&b, "{\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hue\":%d,\"gamma\":%d}", p->look_values[0],
              p->look_values[1], p->look_values[2], p->look_values[3], p->look_values[4]);
    p->look = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"loading\":false,\"items\":[");
    static const char *sources[] = { "MotionBGS", "Lively", "MoeWalls", "Wallhaven" };
    for (int i = 0; i < 12; i++) {
        if (i) sb_puts(&b, ",");
        sb_printf(&b, "{\"number\":%d,\"title\":\"%s\",\"source\":\"%s\",\"kind\":\"%s\",\"preview\":\"gen:%d\","
                      "\"badge\":\"%s\",\"download\":%d,\"percent\":%d}",
                  i + 1, walls[(i + 3) % WALLS].name, sources[i % 4], i % 3 ? "live" : "still", (i + 3) % WALLS,
                  i % 3 ? "4K" : "", i == 1 ? 2 : i == 4 ? 3 : 0, i == 1 ? 42 : -1);
    }
    sb_puts(&b, "]}");
    p->store = parse(&b);
    p->model.status = p->status;
    p->model.settings = p->settings;
    p->model.list = p->list;
    p->model.look = p->look;
    p->model.store = p->store;
}

static void say(Preview *p, const char *text, bool failed) {
    snprintf(p->message, sizeof p->message, "%s", text);
    p->model.message = p->message;
    p->model.message_time = bare_time();
    p->model.message_failed = failed;
}

static void command(void *user, const char *const *a) {
    Preview *p = user;
    char text[256];
    if (!strcmp(a[0], "apply") && a[1]) {
        for (int i = 0; i < WALLS; i++) if (!strcmp(walls[i].id, a[1])) p->on_screen = i;
        snprintf(text, sizeof text, "Showing %s", walls[p->on_screen].name);
        say(p, text, false);
    } else if (!strcmp(a[0], "pause") || !strcmp(a[0], "resume")) {
        p->paused = !strcmp(a[0], "pause");
        say(p, p->paused ? "Paused" : "Playing", false);
    } else if (!strcmp(a[0], "audio") && a[1]) {
        p->audio = !strcmp(a[1], "on");
        say(p, p->audio ? "Sound on" : "Muted", false);
    } else if (!strcmp(a[0], "settings") && a[1] && a[2] && a[3]) {
        Setting *s = setting(a[2]);
        if (s) snprintf(s->value, sizeof s->value, "%s", a[3]);
        say(p, "Saved", false);
    } else if (!strcmp(a[0], "favourite") && a[1] && a[2]) {
        for (int i = 0; i < WALLS; i++) if (!strcmp(walls[i].id, a[1])) walls[i].fav = !strcmp(a[2], "on");
        say(p, !strcmp(a[2], "on") ? "Added to Favourites" : "Removed from Favourites", false);
    } else if (!strcmp(a[0], "look") && a[1] && !strcmp(a[1], "reset")) {
        memset(p->look_values, 0, sizeof p->look_values);
        say(p, "Look reset", false);
    } else if (!strcmp(a[0], "look") && a[1] && a[2] && a[3]) {
        static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
        for (int i = 0; i < 5; i++) if (!strcmp(keys[i], a[2])) p->look_values[i] = atoi(a[3]);
    } else if (!strcmp(a[0], "mode") && a[1]) {
        Setting *s = setting("display_mode");
        snprintf(s->value, sizeof s->value, "%s", a[1]);
    } else {
        snprintf(text, sizeof text, "Preview: \"%s\" needs Screen+ running", a[0]);
        say(p, text, true);
    }
    rebuild(p);
}

/* Pictures for the cards: soft gradients with a glow, a different hue each. */
static BareImage generated(int seed, int w, int h, bool blobs) {
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    float hue = (float)seed * 0.61803f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float u = (float)x / w, v = (float)y / h, t = hue * 6.2831f;
            float r = 0.5f + 0.5f * sinf(t + u * 3.0f + v * 1.5f);
            float g = 0.5f + 0.5f * sinf(t + 2.1f + u * 2.0f - v * 2.5f);
            float bl = 0.5f + 0.5f * sinf(t + 4.2f - u * 1.5f + v * 3.0f);
            float glow = expf(-((u - 0.62f) * (u - 0.62f) + (v - 0.4f) * (v - 0.4f)) * 9.0f);
            if (blobs) {
                glow += 0.8f * expf(-((u - 0.2f) * (u - 0.2f) + (v - 0.75f) * (v - 0.75f)) * 14.0f);
                glow += 0.7f * expf(-((u - 0.85f) * (u - 0.85f) + (v - 0.85f) * (v - 0.85f)) * 20.0f);
            }
            float dark = 0.35f + 0.65f * v * 0.3f;
            unsigned char *o = px + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)fminf(255, 255 * (r * dark + glow * 0.8f));
            o[1] = (unsigned char)fminf(255, 255 * (g * dark + glow * 0.6f));
            o[2] = (unsigned char)fminf(255, 255 * (bl * dark + glow * 0.9f));
            o[3] = 255;
        }
    BareImage img = bare_image_from_pixels(w, h, px);
    free(px);
    return img;
}

static BareImage thumbnail(void *user, const char *path) {
    Preview *p = user;
    if (strncmp(path, "gen:", 4)) return 0;
    int i = atoi(path + 4) & 63;
    if (!p->thumbs[i]) p->thumbs[i] = generated(i + 2, 480, 270, false);
    return p->thumbs[i];
}

static void open_path(void *user, const char *path) {
    char text[300];
    snprintf(text, sizeof text, "Would show %s", path);
    say(user, text, false);
}

static void frame(void *user) {
    Preview *p = user;
    const char *accent = setting("accent_color")->value;
    unsigned rgb = (unsigned)strtoul(accent + 1, NULL, 16);
    const char *motion = setting("ui_motion")->value;
    sp_theme_set(strcmp(setting("theme")->value, "light") != 0, BARE_RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255),
                 !strcmp(motion, "off") ? 2 : !strcmp(motion, "reduced") ? 1 : 0);
    p->model.backdrop = p->backdrop ? p->backdrop : bare_window_backdrop();
    sp_draw(&p->view, &p->model);
}

int main(int argc, char **argv) {
    static Preview p;
    static const char *pages[] = { "library", "displays", "customise", "store", "settings" };
    bool light = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--page") && i + 1 < argc)
            for (int k = 0; k < SP_PAGE_COUNT; k++) if (!strcmp(argv[i + 1], pages[k])) p.view.page = (SpPage)k;
        if (!strcmp(argv[i], "--light")) light = true;
    }
    if (light) snprintf(setting("theme")->value, 32, "light");
    p.audio = false;
    p.model.user = &p;
    p.model.cpu = 4.0f;
    p.model.connected = true;
    p.model.thumbnail = thumbnail;
    p.model.run = command;
    p.model.open_path = open_path;
    p.view.page_time = -10;
    if (!bare_window_with(.width = 1560, .height = 1000, .title = "Screen+ (preview)", .frameless = true,
                          .min_width = 1020, .min_height = 660))
        return 1;
    rebuild(&p);
    const char *bg = getenv("SP_BACKDROP");
    if (bg && !strcmp(bg, "gen")) p.backdrop = generated(7, 1560, 1000, true);
    else if (bg && *bg) p.backdrop = bare_image_load(bg);
    bare_run(frame, &p);
    bare_quit();
    return 0;
}
