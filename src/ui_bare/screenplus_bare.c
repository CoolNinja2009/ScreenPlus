/* screenplus_bare.c: the Screen+ window on Bare liquid glass.
 *
 * The window is frameless and see-through: its background is the live
 * desktop behind it (bare_window_backdrop), and the sidebar, the page and
 * every control are glass that refracts it. Selections are glass drops that
 * glide (sp_kit.c). Data is the service's JSON; actions are service commands. */
#include "sp_shaders.h"
#include "screenplus_bare.h"
#include "sp_kit.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SP_SLASH '\\'
#else
#define SP_SLASH '/'
#endif
#define ID(s)      bare_id(s)
#define IDI(s, i)  bare_id_index((s), (uint32_t)(i))

static void run(const SpModel *m, const char *a0, const char *a1, const char *a2, const char *a3, const char *a4) {
    const char *argv[6] = { a0, a1, a2, a3, a4, NULL };
    if (m->run) m->run(m->user, argv);
}
#define RUN(m, ...) run_n((m), (const char *[]){ __VA_ARGS__, NULL })
static void run_n(const SpModel *m, const char *const *argv) { if (m->run) m->run(m->user, argv); }

/* ---- data helpers --------------------------------------------------------- */

const char *sp_setting(const SpModel *m, const char *key, const char *def) {
    const JVal *v = json_get(m->settings, key);
    if (!v) return def;
    if (v->t == J_BOOL) return v->b ? "on" : "off";
    if (v->t == J_NUM) {
        static char num[8][24];
        static int slot;
        char *b = num[slot++ & 7];
        snprintf(b, 24, "%g", v->num);
        return b;
    }
    return json_str(v, def);
}

/* The app lists only the settings that mean something here (a Store copy has no updates or screensaver: its
 * cli_server.c setting_listed); before the first list arrives, everything shows. */
static bool setting_listed(const SpModel *m, const char *key) {
    return !m->settings || json_get(m->settings, key);
}

static bool setting_on(const SpModel *m, const char *key) {
    const JVal *v = json_get(m->settings, key);
    return v && ((v->t == J_BOOL && v->b) || (v->t == J_STR && !strcmp(v->str, "on")));
}

static int setting_num(const SpModel *m, const char *key, int def) {
    return (int)json_num(json_get(m->settings, key), def);
}

static bool contains(const char *hay, const char *needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (!*b) return true;
    }
    return false;
}

static bool tag_has(const char *tags, const char *tag) {
    if (!tags || !tag || !*tag) return false;
    size_t n = strlen(tag);
    for (const char *p = tags; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, tag, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

bool sp_wallpaper_matches(const JVal *item, const char *search, int filter, int group, const char *collection) {
    static const char *kinds[] = { "", "image", "video", "html" };
    const JVal *w = json_get(item, "wallpaper");
    if (!w) return false;
    const char *type = json_str(json_get(w, "type"), "");
    if (filter > 0 && filter < 4 && strcmp(type, kinds[filter])) return false;
    if (group == 1 && !json_bool(json_get(w, "favourite"), 0)) return false;
    if (group >= 2 && !tag_has(json_str(json_get(w, "collections"), ""), collection)) return false;
    return contains(json_str(json_get(w, "name"), ""), search) || contains(json_str(json_get(w, "id"), ""), search);
}

static const char *type_label(const char *type) {
    return !strcmp(type, "html") ? "HTML" : !strcmp(type, "video") ? "Video" : !strcmp(type, "image") ? "Image" : !strcmp(type, "app") ? "App" : type;
}

static const char *file_name(const char *path) {
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

/* Pictures are kept at these sizes (pixels, cropped to 16:9); moving ones keep PIC_FRAMES frames. */
enum { PIC_W = 320, PIC_H = 180, PIC_BIG_W = 640, PIC_BIG_H = 360, PIC_FRAMES = 24 };

static bool has_ext(const char *path, const char *const *exts) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/') || strchr(dot, '\\')) return false;
    for (; *exts; exts++) {
        const char *a = dot, *b = *exts;
        while (*a && *b && tolower((unsigned char)*a) == *b) a++, b++;
        if (!*a && !*b) return true;
    }
    return false;
}

static const char *const k_still_ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tga", ".tif", ".tiff",
                                           ".heic", ".avif", ".jxr", NULL };
static const char *const k_gif_ext[] = { ".gif", NULL };

/* A picture file at card or big size: 0 while it loads; *failed once it can't be read. */
static BareImage picture_file(const SpModel *m, const char *path, bool big, bool *failed) {
    int w = big ? PIC_BIG_W : PIC_W, h = big ? PIC_BIG_H : PIC_H;
    *failed = false;
    if (!*path) { *failed = true; return 0; }
    if (m->picture) return m->picture(m->user, path, w, h);
    BarePictureOptions o = { .width = w, .height = h, .frames = PIC_FRAMES };
    BareImage img = bare_picture_(path, o);
    if (!img) *failed = bare_picture_failed(path, o);
    return img;
}

/* The picture for a wallpaper, moving when it can: an image's or video's own file; for a page, its package's
 * preview GIF or the one Screen+ recorded. Its still thumbnail shows while that loads, or if nothing moves. */
static BareImage wallpaper_picture(const SpModel *m, const JVal *w, bool big) {
    if (!w) return 0;
    const char *id = json_str(json_get(w, "id"), ""), *path = json_str(json_get(w, "path"), "");
    const char *thumb = json_str(json_get(w, "thumbnail"), ""), *type = json_str(json_get(w, "type"), "");
    const char *previews = json_str(json_get(m->status, "previews"), m->previews ? m->previews : "");
    if (!*previews && m->previews) previews = m->previews;
    bool failed;
    if (m->picture) return picture_file(m, thumb, big, &failed);
    bool url = !strncmp(path, "http://", 7) || !strncmp(path, "https://", 8);
    char cand[7][1100];
    int n = 0;
    if (!url && (!strcmp(type, "image") || !strcmp(type, "video"))) {
        snprintf(cand[n++], sizeof cand[0], "%s", path);
    } else {
        if (has_ext(thumb, k_gif_ext)) snprintf(cand[n++], sizeof cand[0], "%s", thumb);
        const char *slash = strrchr(path, '\\'), *fwd = strrchr(path, '/');
        if (fwd > slash) slash = fwd;
        if (!url && slash) {
            int dir = (int)(slash - path + 1);
            snprintf(cand[n++], sizeof cand[0], "%.*slively_p.gif", dir, path);
            snprintf(cand[n++], sizeof cand[0], "%.*spreview.gif", dir, path);
        }
        if (*previews && *id) snprintf(cand[n++], sizeof cand[0], "%s%c%s.gif", previews, SP_SLASH, id);
    }
    if (has_ext(thumb, k_still_ext)) snprintf(cand[n++], sizeof cand[0], "%s", thumb);
    if (*previews && *id && strcmp(type, "image") && strcmp(type, "video"))
        snprintf(cand[n++], sizeof cand[0], "%s%c%s.jpg", previews, SP_SLASH, id);
    /* the first that loads; while one loads, the ones after it get their chance */
    bool none = true;
    for (int i = 0; i < n; i++) {
        BareImage img = picture_file(m, cand[i], big, &failed);
        if (img) return img;
        if (!failed) none = false;
    }
    /* a page with no picture at all: the service records one (once; the pictures are tried again meanwhile) */
    static BareId asked[256];
    static int asked_count;
    if (none && *previews && *id && !url && strcmp(type, "image") && strcmp(type, "video") && asked_count < 256) {
        BareId h = bare_id(id);
        for (int i = 0; i < asked_count; i++) if (asked[i] == h) return 0;
        asked[asked_count++] = h;
        RUN(m, "preview", id);
    }
    return 0;
}

/* Whether a box drawn last frame at `r` is on screen (not yet drawn: no). */
static bool on_screen(BareRect r) {
    BareVec2 s = bare_window_size();
    return r.w > 0 && r.h > 0 && r.y < s.y && r.y + r.h > 0 && r.x < s.x && r.x + r.w > 0;
}

/* Collections named in the library, in first-seen order. */
static int collections(const SpModel *m, char names[][64], int cap) {
    int n = 0;
    const JVal *list = m->list;
    for (int i = 0; list && list->t == J_ARR && i < list->n; i++) {
        const char *tags = json_str(json_get(json_get(&list->kids[i], "wallpaper"), "collections"), "");
        for (const char *p = tags; *p && n < cap;) {
            const char *end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len && len < 64) {
                bool seen = false;
                for (int k = 0; k < n; k++) if (strlen(names[k]) == len && !strncmp(names[k], p, len)) seen = true;
                if (!seen) { memcpy(names[n], p, len); names[n][len] = 0; n++; }
            }
            if (!end) break;
            p = end + 1;
        }
    }
    return n;
}

static const JVal *display_of(const SpModel *m, int index) {
    const JVal *ds = json_get(m->status, "displays");
    for (int i = 0; ds && ds->t == J_ARR && i < ds->n; i++)
        if ((int)json_num(json_get(&ds->kids[i], "index"), 0) == index) return &ds->kids[i];
    return NULL;
}

/* The wallpaper the hero shows: the target display's, else the first on screen. */
static const JVal *active_wallpaper(const SpModel *m, int target) {
    const JVal *d = target ? display_of(m, target) : NULL;
    if (!d) {
        const JVal *ds = json_get(m->status, "displays");
        d = ds && ds->t == J_ARR && ds->n ? &ds->kids[0] : NULL;
    }
    const JVal *w = json_get(d, "wallpaper");
    return w && w->t == J_OBJ ? w : NULL;
}

/* ---- shared pieces ----------------------------------------------------------- */

static void page_title(const char *title, const char *subtitle) {
    bare_column(.gap = 2, .shrink = BARE_ZERO) {
        bare_text(title, .size = 40, .weight = BARE_REGULAR, .color = sp.ink, .nowrap = true, .letter_spacing = -0.5f);
        if (subtitle) bare_text(subtitle, .size = 16, .color = sp.dim, .nowrap = true);
    }
}

static void section(const char *title) {
    bare_text(title, .size = 14, .weight = BARE_SEMIBOLD, .color = sp.dim, .letter_spacing = 0.8f,
              .transform = BARE_UPPERCASE, .nowrap = true);
}

/* A row inside a settings card: icon, title (+ line under), then the controls. */
#define ROW(id_, icon_, title_, sub_) \
    for (int row_once_ = row_open((id_), (icon_), (title_), (sub_)); row_once_; row_once_ = row_close())

static int row_open(BareId id, const char *icon, const char *title, const char *sub) {
    bare_open_(&(BareBox){ .id = id, .dir = BARE_ROW, .width = BARE_GROW, .height = sub ? 76.0f : 66.0f,
                           .pad = { 0, 22, 0, 22 }, .gap = 18, .align = BARE_CENTER }, __FILE__, __LINE__);
    bare_row(.width = 28, .justify = BARE_CENTER, .shrink = BARE_ZERO) { sp_icon(icon, 20, sp.ink); }
    bare_column(.width = BARE_GROW, .gap = 3) {
        sp_label(title, 18, sp.ink, BARE_MEDIUM);
        if (sub && *sub) sp_label(sub, 14.5f, sp.dim, BARE_REGULAR);
    }
    return 1;
}
static int row_close(void) { return bare_close_(); }

static void divider(void) {
    bare_row(.width = BARE_GROW, .pad = { 0, 22, 0, 68 }) {
        bare_box(.width = BARE_GROW, .height = 1, .color = sp.dark ? BARE_RGBA(255, 255, 255, 26) : BARE_RGBA(0, 0, 0, 22));
    }
}

#define CARD(id_) bare_column(.id = (id_), .width = BARE_GROW, .radius = 26, .glass = sp_glass_card(), \
                              .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO, \
                              .shadow = { 0, 6, 24, 0, BARE_RGBA(0, 0, 0, sp.dark ? 50 : 22) })

/* ---- the logo: branding/icon.svg (the app icon), drawn at any size ------------------- */

static void logo(float size) {
    const BareColor blue = sp.accent;                          /* the icon's blue, or the accent chosen */
    float k = size / 472.0f;                                    /* the icon's square is 472 units */
    #define LOGO_PART(x_, y_, w_, h_, r_, c_)         bare_box(.position = BARE_ABSOLUTE, .x = (x_) * k, .y = (y_) * k, .width = (w_) * k, .height = (h_) * k,                  .radius = (r_) * k, .color = (c_), .passthrough = true)
    bare_stack(.width = size, .height = size, .shrink = BARE_ZERO, .radius = 118 * k,
               .shadow = size > 30 ? (BareShadow){ 0, size * 0.08f, size * 0.4f, 0, sp_alpha(sp.accent, 90) } : (BareShadow){ 0 }) {
        LOGO_PART(0, 0, 472, 472, 118, blue);
        LOGO_PART(76, 94, 320, 242, 58, BARE_WHITE);
        LOGO_PART(112, 130, 248, 170, 34, blue);
        LOGO_PART(220, 141, 32, 148, 16, BARE_WHITE);          /* the plus */
        LOGO_PART(162, 199, 148, 32, 16, BARE_WHITE);
        LOGO_PART(198, 317, 38, 75, 19, BARE_WHITE);           /* the stand */
        LOGO_PART(139, 360, 194, 38, 19, BARE_WHITE);
    }
    #undef LOGO_PART
}

/* ---- title bar and sidebar --------------------------------------------------- */

static void title_bar(void) {
    bare_row(.width = BARE_GROW, .height = 46, .align = BARE_CENTER, .pad = { 0, 6, 0, 18 }, .gap = 10,
             .window_drag = true) {
        logo(22);
        bare_text("Screen+", .size = 15, .weight = BARE_MEDIUM, .color = sp.ink, .nowrap = true,
                  .shadow = { 0, 1, 6, 0, sp.dark ? BARE_RGBA(0, 0, 0, 140) : BARE_RGBA(255, 255, 255, 200) });
        bare_box(.width = BARE_GROW);
        if (sp_button(ID("win.min"), SPI_MIN, NULL, SP_BTN_CHROME).clicked) bare_window_minimize();
        if (sp_button(ID("win.max"), bare_window_maximized() ? SPI_RESTORE : SPI_MAX, NULL, SP_BTN_CHROME).clicked)
            bare_window_maximize();
        if (sp_button(ID("win.close"), SPI_CLOSE, NULL, SP_BTN_CHROME).clicked) bare_window_close();
    }
}

/* Pour (setting ui_pour): the selected item is a vessel of plain accent colour. A solid stream shoots from the
 * side facing the new item (longer for a longer jump), the old item drains into it and the new one fills from its
 * far side, then the colour turns back into the usual glass drop. Fast, so the moving ends are smeared (motion
 * blur: fading copies behind them, as long as they move far in a frame). Drawn by hand from the seconds since the
 * click; the nav's own drop waits hidden. Clipping is a rectangle, so a level is the item's whole rounded shape
 * clipped at the surface. */
#define POUR_DRAIN 0.16f    /* seconds to empty, and to fill */
#define POUR_LINE 16.0f     /* the stream's width */
#define POUR_FADE 0.08f     /* colour into glass */
#define POUR_FILLET 11.0f   /* the curve joining stream and item */
#define POUR_BOUNCE 0.5f    /* seconds the new item wobbles after the stream lands */

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* easy ease: slow out, fast through the middle, slow in (cubic), and how fast it moves at p */
static float ease_io(float p) { return p < 0.5f ? 4 * p * p * p : 1 - (2 - 2 * p) * (2 - 2 * p) * (2 - 2 * p) * 0.5f; }
static float ease_io_rate(float p) { return p < 0.5f ? 12 * p * p : 3 * (2 - 2 * p) * (2 - 2 * p); }

static float pour_reach(BareRect a, BareRect b) { return 0.07f + fabsf(b.y - a.y) * 0.0002f; }   /* the stream's head */
static float pour_seconds(BareRect a, BareRect b) { return POUR_DRAIN + 0.02f + pour_reach(a, b) + POUR_FADE; }

static void pour_level(BareId id, BareRect r, float y0, float y1, float opacity) {
    float lo = fminf(y0, y1), hi = fmaxf(y0, y1);
    if (hi - lo < 0.5f || opacity <= 0.01f) return;
    bare_column(.id = id, .position = BARE_ABSOLUTE, .x = r.x, .y = lo, .width = r.w, .height = hi - lo, .clip = true,
                .opacity = opacity, .passthrough = true) {
        bare_box(.position = BARE_ABSOLUTE, .x = 0, .y = r.y - lo, .width = r.w, .height = r.h, .radius = 22,
                 .color = sp.accent, .passthrough = true);
    }
}

/* A smear behind an end at y moving `speed` px/s the way `dir` (+1 down): fading slices of the line. */
static void pour_blur(const char *id, float cx, float y, float dir, float speed) {
    float len = fminf(fabsf(speed) / 60.0f, 44.0f);   /* how far it moves in a 60 Hz frame */
    if (len < 2) return;
    for (int i = 0; i < 5; i++) {
        float k = (float)(i + 1) / 5;
        float y1 = y - dir * len * k, h = len / 5 + 2;
        bare_box(.id = bare_id_index(id, (uint32_t)i), .position = BARE_ABSOLUTE, .x = cx - POUR_LINE * 0.5f,
                 .y = (dir > 0 ? y1 : y1 - h), .width = POUR_LINE, .height = h, .radius = POUR_LINE * 0.5f,
                 .color = sp_alpha(sp.accent, (int)(150 * (1 - k))), .passthrough = true);
    }
}

/* Where the stream meets an item's colour it flares into it on a concave curve, so the two are one shape. The
 * curve is a small picture (one per corner, coverage supersampled: smooth edges at any scale), made again only
 * when the accent changes; at the edge y it is r wide on each side of the stream, narrowing over r the way `out`
 * (+1 down). */
#define FILLET_PX 40   /* the picture's size: POUR_FILLET at up to 4x */
static BareImage fillet_img[4];   /* right/left x down/up */
static BareColor fillet_col;

static void fillet_make(void) {
    if (fillet_img[0] && !memcmp(&fillet_col, &sp.accent, sizeof fillet_col)) return;
    fillet_col = sp.accent;
    static uint8_t px[4][FILLET_PX * FILLET_PX * 4];
    const float F = FILLET_PX;
    for (int y = 0; y < FILLET_PX; y++)
        for (int x = 0; x < FILLET_PX; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = x + (sx + 0.5f) / 4, v = y + (sy + 0.5f) / 4;   /* u from the stream, v from the item */
                    in += v <= F - sqrtf(fmaxf(F * F - (F - u) * (F - u), 0));
                }
            uint8_t a = (uint8_t)(in * 255 / 16);
            for (int k = 0; k < 4; k++) {
                int xx = k & 1 ? FILLET_PX - 1 - x : x, yy = k & 2 ? FILLET_PX - 1 - y : y;
                uint8_t *p = px[k] + (yy * FILLET_PX + xx) * 4;
                p[0] = sp.accent.r, p[1] = sp.accent.g, p[2] = sp.accent.b, p[3] = a;
            }
        }
    for (int k = 0; k < 4; k++)
        if (!fillet_img[k] || !bare_image_update(fillet_img[k], px[k]))
            fillet_img[k] = bare_image_from_pixels(FILLET_PX, FILLET_PX, px[k]);
}

static void pour_fillet(const char *id, float cx, float y, float out, float r, float opacity) {
    if (r < 0.5f || opacity <= 0.01f) return;
    fillet_make();
    float top = out > 0 ? y : y - r, half = POUR_LINE * 0.5f;
    int up = out > 0 ? 0 : 2;
    bare_box(.id = bare_id_index(id, 0), .position = BARE_ABSOLUTE, .x = cx + half, .y = top, .width = r, .height = r,
             .image = fillet_img[up], .opacity = opacity, .passthrough = true);
    bare_box(.id = bare_id_index(id, 1), .position = BARE_ABSOLUTE, .x = cx - half - r, .y = top, .width = r, .height = r,
             .image = fillet_img[up + 1], .opacity = opacity, .passthrough = true);
    bare_box(.id = bare_id_index(id, 2), .position = BARE_ABSOLUTE, .x = cx - half, .y = top, .width = POUR_LINE,
             .height = r, .color = sp.accent, .opacity = opacity, .passthrough = true);
}

static void pour(BareRect a, BareRect b, BareRect box, float s) {
    a.x -= box.x, a.y -= box.y, b.x -= box.x, b.y -= box.y;
    float dir = b.y > a.y ? 1.0f : -1.0f;   /* 1: the new item is below */
    float cx = a.x + a.w * 0.5f, reach = pour_reach(a, b), total = pour_seconds(a, b);
    float a_out = dir > 0 ? a.y + a.h : a.y, b_far = dir > 0 ? b.y + b.h : b.y;
    float drained = ease_io(clamp01(s / POUR_DRAIN)), filled = ease_io(clamp01((s - reach) / POUR_DRAIN));
    float a_surface = lerpf(a_out - dir * a.h, a_out, drained), b_surface = b_far - dir * b.h * filled;
    float fade = 1 - clamp01((s - (total - POUR_FADE)) / POUR_FADE);
    pour_level(ID("nav.pour.src"), a, a_surface, a_out, 1);
    pour_level(ID("nav.pour.dst"), b, b_far, b_surface, fade);
    /* the stream: its head runs (eased in and out) to the new item's surface; once the old one is empty its tail
     * follows the same way */
    float span = b_far - a_out, hp = clamp01(s / reach), tp = clamp01((s - POUR_DRAIN - 0.02f) / reach);
    float head = a_out + span * ease_io(hp), tail = a_out - dir * 2 + (span + dir * 2) * ease_io(tp);
    if (dir * (head - b_surface) > 0) head = b_surface;
    if (dir * (tail - b_surface) > 0) tail = b_surface;
    float len = dir * (head - tail);
    if (len > 0.5f) {
        bare_box(.id = ID("nav.pour.stream"), .position = BARE_ABSOLUTE, .x = cx - POUR_LINE * 0.5f, .y = fminf(head, tail),
                 .width = POUR_LINE, .height = len, .radius = POUR_LINE * 0.5f, .color = sp.accent, .passthrough = true);
        /* joined to the old item while the tail is still in it, to the new one once the head lands */
        float rmax = fminf(POUR_FILLET, len);
        if (tp <= 0 && drained < 1)
            pour_fillet("nav.pour.join.a", cx, a_out, dir, rmax, clamp01((1 - drained) * 4));
        if (head == b_surface)
            pour_fillet("nav.pour.join.b", cx, b_surface, -dir, rmax, fade);
        if (hp < 1 && dir * (head - b_surface) < 0) pour_blur("nav.pour.blur.head", cx, head, dir, span * ease_io_rate(hp) / reach);
        if (tp > 0 && tp < 1) pour_blur("nav.pour.blur.tail", cx, tail, dir, span * ease_io_rate(tp) / reach);
    }
}

static void sidebar(SpView *v, const SpModel *m) {
    static const char *names[] = { "Library", "Displays", "Customise", "Store", "Settings" };
    static const char *icons[] = { SPI_LIBRARY, SPI_DISPLAY, SPI_PALETTE, SPI_STORE, SPI_SETTINGS };
    static BareRect rects[SP_PAGE_COUNT], box;
    static BareRect blob_was;
    bare_column(.id = ID("sidebar"), .width = 272, .height = BARE_GROW, .radius = 34, .padding = 18, .gap = 6,
                .glass = sp_glass_panel(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO,
                .shadow = { 0, 18, 48, -6, BARE_RGBA(0, 0, 0, sp.dark ? 90 : 40) }) {
        bare_row(.height = 84, .align = BARE_CENTER, .gap = 14, .pad = { 0, 0, 10, 8 }) {
            logo(48);
            bare_text("Screen+", .size = 25, .weight = BARE_SEMIBOLD, .color = sp.ink, .nowrap = true);
        }
        /* The nav: a glass drop under the current page glides between items. */
        bare_column(.id = ID("nav"), .gap = 6, .width = BARE_GROW) {
            box = bare_this().rect;
            BareRect r = rects[v->page];
            bool have = r.w > 0;
            static int shown = -1, pour_from = -1;
            static double pour_at, land_at = -1;
            static float pour_len, land_dir, land_amp;
            if ((int)v->page != shown) {
                bool can = shown >= 0 && have && rects[shown].w > 0 && sp.motion >= 1 && setting_on(m, "ui_pour");
                pour_from = can ? shown : -1;
                pour_at = bare_time();
                land_at = -1;
                if (can) {
                    pour_len = pour_seconds(rects[shown], r);
                    land_at = pour_at + pour_reach(rects[shown], r);
                    land_dir = r.y > rects[shown].y ? 1.0f : -1.0f;
                    land_amp = fminf(4.0f + fabsf(r.y - rects[shown].y) * 0.012f, 8.0f);   /* a longer fall hits harder */
                }
                shown = (int)v->page;
            }
            float pt = pour_from >= 0 ? (float)((bare_time() - pour_at) / pour_len) : 1.0f;
            bool pouring = pour_from >= 0 && pt < 1;
            /* The stream landing knocks the new item along the fall, and it springs back: a damped spring's
             * response to a hit (about 5.5 wobbles a second, gone within half a second), the item, its colour,
             * label and then its glass drop all together. */
            float since = land_at > 0 ? (float)(bare_time() - land_at) : -1;
            bool bouncing = since >= 0 && since < POUR_BOUNCE;
            float bounce = bouncing ? land_dir * land_amp * expf(-10.0f * since) * sinf(34.5f * since) : 0;
            if (bouncing) bare_wake_in(0.0f);
            else if (since >= POUR_BOUNCE) land_at = -1;
            if (pouring) {
                pour(rects[pour_from], (BareRect){ r.x, r.y + bounce, r.w, r.h }, box, pt * pour_len);
                bare_wake_in(0.0f);
            } else {
                pour_from = -1;
            }
            static bool poured;   /* the frame the pour ends, the drop takes over in place, not fading in */
            bool handoff = poured && !pouring;
            poured = pouring;
            /* the poured colour turns back into the glass drop over its last POUR_FADE seconds */
            float blob_in = pouring ? clamp01((pt * pour_len - (pour_len - POUR_FADE)) / POUR_FADE) : 1.0f;
            bool moving = !pouring && have && blob_was.w > 0 && fabsf(blob_was.y - r.y) > 3;
            /* In flight the drop clears into a lens and stretches along its path, like a drop of water. */
            static float last_y;
            float speed = fabsf(blob_was.y - last_y);
            last_y = blob_was.y;
            float stretch = moving ? fminf(speed * 1.4f, 26.0f) * sp.motion : 0;
            BareGlass g = moving && sp.motion > 0 ? sp_glass_lens() : sp_glass_accent();
            if (moving && sp.motion > 0) { g.zoom = 1.12f; g.tint = sp_alpha(sp.accent, 46); }
            BareState blob = bare_box(.id = ID("nav.blob"), .position = BARE_ABSOLUTE, .x = r.x - box.x + stretch * 0.15f,
                     .y = r.y - box.y - stretch * 0.5f,
                     .width = have ? r.w - stretch * 0.3f : 10, .height = have ? r.h + stretch : 10, .radius = 22, .glass = g,
                     .opacity = have && blob_in > 0.001f ? blob_in : BARE_ZERO, .border = 1,
                     .border_color = sp_alpha(sp_mix(sp.accent, BARE_WHITE, 0.55f), moving ? 110 : 160),
                     .shadow = { 0, 4, 22, 0, sp_alpha(sp.accent, moving ? 30 : 110) },
                     .offset = { 0, bounce },
                     .passthrough = true, .animate = pouring || handoff || bouncing ? BARE_INSTANT : sp_liquid());
            blob_was = blob.rect;
            if (moving) bare_wake_in(0.0f);
            for (int i = 0; i < SP_PAGE_COUNT; i++) {
                bool on = (int)v->page == i;
                bare_row(.id = IDI("nav.item", i), .height = 58, .width = BARE_GROW, .pad = { 0, 16, 0, 20 },
                         .gap = 18, .align = BARE_CENTER, .radius = 22, .cursor = BARE_CURSOR_HAND, .focusable = true,
                         .hover = { .color = on ? BARE_RGBA(255, 255, 255, 0) : sp.faint },
                         .press = { .scale = 1.0f - 0.03f * sp.motion }, .animate = sp_spring()) {
                    BareState st = bare_this();
                    rects[i] = st.rect;
                    BareColor c = on && !moving && !(pouring && pt < 0.85f) ? sp.on_accent : sp.ink;
                    bare_row(.gap = 18, .align = BARE_CENTER, .offset = { 0, on ? bounce : 0 }, .animate = BARE_INSTANT) {
                        sp_icon(icons[i], 21, c);
                        bare_text(names[i], .size = 18, .weight = on ? BARE_SEMIBOLD : BARE_MEDIUM, .color = c,
                                  .nowrap = true, .animate = sp_fade());
                    }
                    if (st.clicked && !on) { v->page = (SpPage)i; v->page_time = bare_time(); v->popover = 0; sp_sound(SP_SOUND_PAGE); }
                }
            }
        }
        bare_box(.height = BARE_GROW);
        /* What the renderer is doing */
        int sev = (int)json_num(json_get(m->status, "severity"), 0);
        const char *state = m->connected ? json_str(json_get(m->status, "state"), "Ready") : "Screen+ isn't running";
        BareColor dot = !m->connected || sev >= 2 ? BARE_RGB(255, 84, 84) : sev == 1 ? BARE_RGB(255, 196, 60) : sp.accent;
        bare_column(.width = BARE_GROW, .radius = 24, .pad = { 16, 18, 16, 18 }, .gap = 6, .glass = sp_glass_card(),
                    .border = 1, .border_color = sp.hair) {
            bare_row(.gap = 10, .align = BARE_CENTER) {
                bare_box(.width = 11, .height = 11, .radius = 6, .color = dot, .shadow = { 0, 0, 8, 0, sp_alpha(dot, 160) });
                sp_label(state, 16, sp.ink, BARE_MEDIUM);
            }
            const JVal *mem = json_get(m->status, "memoryMB");
            double ram = json_num(json_get(mem, "app"), 0) + json_num(json_get(mem, "renderer"), 0);
            char line[64];
            if (m->cpu >= 0) snprintf(line, sizeof line, "CPU %.1f%%  \xC2\xB7  RAM %.0f MB", m->cpu, ram);
            else snprintf(line, sizeof line, "RAM %.0f MB", ram);
            if (m->connected) sp_label(line, 14, sp.dim, BARE_REGULAR);
        }
    }
}

/* ---- Library ------------------------------------------------------------------- */

static void search_field(BareId id, char *buf, int cap, const char *placeholder, float width) {
    static const BareBox field = { .color = { 255, 255, 255, 0 }, .border = BARE_ZERO, .height = 26,
                                   .pad = { 1, 4, 1, 0 }, .cursor = BARE_CURSOR_TEXT };
    bare_row(.id = id, .width = width, .height = 52, .radius = 26, .pad = { 0, 18, 0, 20 }, .gap = 12,
             .align = BARE_CENTER, .glass = sp_glass_control(), .border = 1, .border_color = sp.hair,
             .shrink = BARE_ZERO) {
        sp_icon(SPI_SEARCH, 17, sp.dim);
        bare_input(buf, cap, .id = id + 1, .style = &field, .width = BARE_GROW, .placeholder = placeholder,
                   .size = 17, .color = sp.ink, .caret = sp.accent, .selection = sp_alpha(sp.accent, 90));
    }
}

static void hero(SpView *v, const SpModel *m) {
    const JVal *w = active_wallpaper(m, v->target_display);
    const JVal *d = display_of(m, v->target_display ? v->target_display : 1);
    bool paused = json_bool(json_get(m->status, "paused"), 0);
    bool muted = !json_bool(json_get(m->status, "audio"), 0);
    bare_row(.id = ID("hero"), .width = BARE_GROW, .height = 312, .radius = 32, .padding = 20, .gap = 30,
             .glass = sp_glass_card(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO,
             .shadow = { 0, 10, 30, 0, BARE_RGBA(0, 0, 0, sp.dark ? 60 : 25) }) {
        BareImage pic = wallpaper_picture(m, w, true);
        bare_box(.id = IDI("hero.pic", (uint32_t)bare_id(json_str(json_get(w, "id"), ""))), .width = 488, .height = 272,
                 .radius = 22, .image = pic, .color = sp.dark ? BARE_RGBA(0, 0, 0, 70) : BARE_RGBA(0, 0, 0, 25),
                 .border = 1, .border_color = BARE_RGBA(255, 255, 255, 40), .shrink = BARE_ZERO,
                 .shadow = { 0, 8, 26, 0, BARE_RGBA(0, 0, 0, 90) }, .animate = sp_fade());
        bare_column(.width = BARE_GROW, .gap = 14, .pad = { 14, 0, 0, 0 }) {
            bare_text(v->target_display ? "NOW SHOWING  \xC2\xB7  DISPLAY" : "NOW SHOWING  \xC2\xB7  DESKTOP",
                      .size = 14, .weight = BARE_MEDIUM, .color = sp.dim, .letter_spacing = 0.8f, .nowrap = true);
            bare_text(w ? json_str(json_get(w, "name"), "") : "Nothing on screen", .size = 34, .weight = BARE_SEMIBOLD,
                      .color = sp.ink, .nowrap = true, .ellipsis = true);
            bare_row(.gap = 10, .align = BARE_CENTER) {
                if (w) sp_chip(ID("hero.kind"), NULL, type_label(json_str(json_get(w, "type"), "")), true, sp.accent);
                bool covered = json_bool(json_get(d, "covered"), 0);
                const char *st = paused ? "Paused" : covered ? "Covered" : json_str(json_get(d, "state"), "Playing");
                sp_chip(ID("hero.state"), SPI_DOT, st, false, paused ? BARE_RGB(255, 196, 60) : (BareColor){ 0 });
                const char *desc = json_str(json_get(d, "renderer"), "");
                if (*desc) bare_text(desc, .size = 15, .color = sp.dim, .nowrap = true, .ellipsis = true);
            }
            /* Which display the cards apply to: a glass drop glides between them. */
            const JVal *ds = json_get(m->status, "displays");
            int n = ds && ds->t == J_ARR ? ds->n : 0;
            if (n >= 1) {
                const char *labels[9];
                static char names[8][64];
                int sel = 0;
                labels[0] = "All displays";
                for (int i = 0; i < n && i < 8; i++) {
                    snprintf(names[i], 64, "%d  %s", (int)json_num(json_get(&ds->kids[i], "index"), i + 1),
                             json_str(json_get(&ds->kids[i], "name"), "Display"));
                    labels[i + 1] = names[i];
                    if ((int)json_num(json_get(&ds->kids[i], "index"), i + 1) == v->target_display) sel = i + 1;
                }
                int c = sp_choices(ID("hero.displays"), labels, n < 8 ? n + 1 : 9, sel, false, 46);
                if (c == 0) v->target_display = 0;
                else if (c > 0) v->target_display = (int)json_num(json_get(&ds->kids[c - 1], "index"), c);
            }
            bare_row(.gap = 12, .align = BARE_CENTER) {
                if (sp_button(ID("hero.pause"), paused ? SPI_PLAY : SPI_PAUSE, paused ? "Play" : "Pause", SP_BTN_GLASS).clicked)
                    run(m, paused ? "resume" : "pause", NULL, NULL, NULL, NULL);
                if (sp_button(ID("hero.audio"), muted ? SPI_MUTE : SPI_VOLUME, muted ? "Muted" : "Sound on", SP_BTN_GLASS).clicked)
                    run(m, "audio", muted ? "on" : "off", NULL, NULL, NULL);
                if (sp_button(ID("hero.restart"), SPI_REFRESH, "Restart", SP_BTN_GLASS).clicked)
                    run(m, "restart", NULL, NULL, NULL, NULL);
                if (w && sp_button(ID("hero.open"), SPI_OPEN, NULL, SP_BTN_ICON).clicked && m->open_path)
                    m->open_path(m->user, json_str(json_get(w, "path"), ""));
            }
        }
    }
}

static void wallpaper_card(SpView *v, const SpModel *m, const JVal *item, int index, float width) {
    const JVal *w = json_get(item, "wallpaper");
    const char *id = json_str(json_get(w, "id"), "");
    const char *type = json_str(json_get(w, "type"), "");
    bool on = json_bool(json_get(item, "onScreen"), 0);
    bool fav = json_bool(json_get(w, "favourite"), 0);
    bool missing = json_bool(json_get(w, "missing"), 0);
    BareId cid = IDI("card", (uint32_t)index * 8u);
    bare_column(.id = cid, .width = width, .radius = 28, .padding = 10, .gap = 4, .shrink = BARE_ZERO,
                .glass = sp_glass_card(), .border = on ? 2.0f : 1.0f,
                .border_color = on ? sp_mix(sp.accent, BARE_WHITE, 0.15f) : sp.hair,
                .shadow = on ? (BareShadow){ 0, 0, 26, 0, sp_alpha(sp.accent, 120) }
                             : (BareShadow){ 0, 8, 24, 0, BARE_RGBA(0, 0, 0, sp.dark ? 55 : 22) },
                .hover = { .scale = 1.0f + 0.025f * sp.motion, .offset = { 0, -3 * sp.motion } },
                .press = { .scale = 1.0f - 0.02f * sp.motion },
                .cursor = BARE_CURSOR_HAND, .focusable = true, .animate = sp_spring()) {
        BareState st = bare_this();
        float ph = (width - 20) * 9.0f / 16.0f;
        bare_stack(.width = BARE_GROW, .height = ph, .radius = 20, .clip = true) {
            BareImage pic = on_screen(st.rect) ? wallpaper_picture(m, w, false) : 0;
            bare_box(.id = cid + 1, .width = BARE_GROW, .height = ph, .radius = 20, .image = pic,
                     .color = sp.dark ? BARE_RGBA(255, 255, 255, 14) : BARE_RGBA(0, 0, 0, 16),
                     .opacity = pic || missing ? 1.0f : 0.6f, .passthrough = true, .animate = sp_fade());
            /* badges: the kind, on screen, favourite */
            bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM_LEFT, .x = 12, .y = -12, .height = 32,
                     .pad = { 0, 13, 0, 11 }, .gap = 7, .align = BARE_CENTER, .radius = 16, .passthrough = true,
                     .glass = { .refraction = 0.9f, .depth = 10, .blur = 6, .shine = 1.1f, .tint = BARE_RGBA(10, 16, 18, 120) },
                     .border = 1, .border_color = BARE_RGBA(255, 255, 255, 50)) {
                const char *glyph = !strcmp(type, "video") ? SPI_FILM : !strcmp(type, "image") ? SPI_PICTURE : NULL;
                if (glyph) sp_icon(glyph, 13, BARE_WHITE);
                else bare_text("{ }", .size = 13, .weight = BARE_SEMIBOLD, .color = BARE_WHITE, .nowrap = true);
                bare_text(type_label(type), .size = 14, .weight = BARE_MEDIUM, .color = BARE_WHITE, .nowrap = true);
            }
            if (on)
                bare_stack(.id = cid + 2, .position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = -12, .y = 12,
                           .width = 34, .height = 34, .radius = 17, .align = BARE_CENTER, .justify = BARE_CENTER,
                           .glass = sp_glass_accent(), .border = 1, .border_color = BARE_RGBA(255, 255, 255, 150),
                           .passthrough = true, .animate = sp_spring()) {
                    sp_icon(SPI_CHECK, 15, sp.on_accent);
                }
            if (fav || st.hovered) {
                bare_stack(.id = cid + 3, .position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM_RIGHT, .x = -12, .y = -12,
                           .width = 34, .height = 34, .radius = 17, .align = BARE_CENTER, .justify = BARE_CENTER,
                           .glass = { .refraction = 0.9f, .depth = 10, .blur = 6, .shine = 1.1f, .tint = BARE_RGBA(10, 16, 18, 110) },
                           .border = 1, .border_color = BARE_RGBA(255, 255, 255, 50), .cursor = BARE_CURSOR_HAND,
                           .hover = { .scale = 1.15f }, .press = { .scale = 0.9f }, .animate = sp_spring()) {
                    if (bare_this().clicked) { RUN(m, "favourite", id, fav ? "off" : "on"); st.clicked = false; }
                    sp_icon(fav ? SPI_STAR : SPI_STAR_LINE, 15, fav ? BARE_RGB(255, 204, 40) : BARE_WHITE);
                }
            }
            if (missing)
                bare_text("File missing", .position = BARE_ABSOLUTE, .anchor = BARE_MIDDLE, .size = 15, .color = sp.ink);
        }
        bare_column(.pad = { 10, 10, 8, 10 }, .gap = 3, .width = BARE_GROW) {
            const char *name = json_str(json_get(w, "name"), "Untitled");
            if (v->renaming == index + 1) {
                /* the name in place: Enter keeps it, Escape or a click elsewhere leaves it as it was */
                static const BareBox field = { .color = { 255, 255, 255, 0 }, .border = BARE_ZERO, .height = 25,
                                               .pad = { 0, 4, 0, 0 } };
                BareId fid = cid + 4;
                BareState s = bare_input(v->rename, sizeof v->rename, .id = fid, .style = &field, .width = BARE_GROW,
                                         .size = 19, .color = sp.ink, .caret = sp.accent,
                                         .selection = sp_alpha(sp.accent, 90));
                if (v->rename_frames++ == 0) {
                    bare_focus(fid);
                    bare_edit(fid, BARE_EDIT_SELECT_ALL);
                } else if (s.submitted) {
                    if (v->rename[0] && strcmp(v->rename, name)) RUN(m, "rename", id, v->rename);
                    v->renaming = 0;
                } else if (!s.focused) {
                    v->renaming = 0;
                }
                st.clicked = false;
            } else {
                BareState ns = bare_text(name, .size = 19, .weight = BARE_SEMIBOLD, .color = sp.ink, .nowrap = true,
                                         .ellipsis = true);
                if (ns.clicked) {   /* rename, not apply */
                    v->renaming = index + 1;
                    v->rename_frames = 0;
                    snprintf(v->rename, sizeof v->rename, "%s", name);
                    st.clicked = false;
                }
            }
            char sub[300];
            snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", type_label(type), file_name(json_str(json_get(w, "path"), "")));
            bare_text(sub, .size = 14.5f, .color = sp.dim, .nowrap = true, .ellipsis = true);
        }
        if (st.clicked && !missing) {
            char display[16];
            snprintf(display, sizeof display, "%d", v->target_display);
            if (v->target_display) RUN(m, "apply", id, "--display", display);
            else RUN(m, "apply", id);
        }
        if (st.right_clicked) {
            v->popover = 3;
            v->menu_card = index;
            v->popover_at = bare_mouse();
        }
    }
}

static void library(SpView *v, const SpModel *m) {
    const JVal *list = m->list;
    int total = list && list->t == J_ARR ? list->n : 0;
    char count[48];
    snprintf(count, sizeof count, "%d wallpaper%s", total, total == 1 ? "" : "s");
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Library", count);
        bare_box(.width = BARE_GROW);
        search_field(ID("lib.search"), v->search, sizeof v->search, "Search wallpapers", 340);
        if (sp_button(ID("lib.link"), SPI_LINK, NULL, SP_BTN_ICON).clicked) v->popover = v->popover == 1 ? 0 : 1;
        if (sp_button(ID("lib.folder"), SPI_FOLDER, NULL, SP_BTN_ICON).clicked)
            bare_file_dialog(ID("dlg.folder"), BARE_FILE_FOLDER, NULL, NULL);
        if (sp_button(ID("lib.import"), SPI_ADD, "Import", SP_BTN_PRIMARY).clicked)
            bare_file_dialog(ID("dlg.import"), BARE_FILE_OPEN,
                             "Wallpapers:mp4;webm;mkv;mov;avi;gif;jpg;jpeg;png;bmp;webp;html;htm;zip;pptx", NULL);
    }
    const char *f = bare_file_chosen(ID("dlg.import"));
    if (f && *f) RUN(m, "import", f);
    f = bare_file_chosen(ID("dlg.folder"));
    if (f && *f) RUN(m, "import", f);

    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 18, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 24, 6 }) {
        hero(v, m);
        /* filters */
        char names[16][64];
        int nc = collections(m, names, 14);
        int shown = 0;
        const char *coll = v->group >= 2 && v->group - 2 < nc ? names[v->group - 2] : NULL;
        if (v->group >= 2 && !coll) v->group = 0;
        for (int i = 0; i < total; i++) shown += sp_wallpaper_matches(&list->kids[i], v->search, v->filter, v->group, coll);
        bare_row(.width = BARE_GROW, .height = 56, .gap = 18, .align = BARE_CENTER, .shrink = BARE_ZERO) {
            static const char *types[] = { "All", "Images", "Videos", "HTML" };
            int c = sp_choices(ID("lib.types"), types, 4, v->filter, false, 46);
            if (c >= 0) v->filter = c;
            const char *groups[16];
            groups[0] = "Favourites";
            for (int i = 0; i < nc; i++) groups[i + 1] = names[i];
            c = sp_choices(ID("lib.groups"), groups, nc + 1, v->group - 1, false, 46);
            if (c >= 0) v->group = v->group == c + 1 ? 0 : c + 1;
            bare_box(.width = BARE_GROW);
            static const char *sorts[] = { "custom", "name", "added", "type", "size" };
            static const char *sort_names[] = { "Custom order", "Name", "Recently added", "Type", "Size" };
            const char *sort = sp_setting(m, "library_sort", "custom");
            int si = 0;
            for (int i = 0; i < 5; i++) if (!strcmp(sort, sorts[i])) si = i;
            if (sp_button(ID("lib.sort"), SPI_SORT, sort_names[si], SP_BTN_PLAIN).clicked) {
                v->popover = v->popover == 2 ? 0 : 2;
                v->popover_at = bare_mouse();
            }
            char of[32];
            snprintf(of, sizeof of, "%d of %d", shown, total);
            sp_label(of, 16, sp.dim, BARE_REGULAR);
        }
        /* the cards, three or four to a row */
        BareVec2 win = bare_window_size();
        float avail = win.x - 272 - 18 * 3 - 30 * 2 - 12;
        int cols = avail > 1500 ? 4 : avail > 820 ? 3 : avail > 520 ? 2 : 1;
        float cw = (avail - (cols - 1) * 20) / cols;
        int i = 0;
        while (i < total) {
            bare_row(.id = IDI("lib.row", i), .width = BARE_GROW, .gap = 20, .shrink = BARE_ZERO) {
                for (int c = 0; c < cols && i < total; i++) {
                    if (!sp_wallpaper_matches(&list->kids[i], v->search, v->filter, v->group, coll)) continue;
                    wallpaper_card(v, m, &list->kids[i], i, cw);
                    c++;
                }
            }
        }
        if (!shown)
            bare_column(.width = BARE_GROW, .height = 200, .align = BARE_CENTER, .justify = BARE_CENTER, .gap = 10) {
                sp_icon(SPI_LIBRARY, 40, sp.dim);
                sp_label(total ? "No wallpapers match" : "Your library is empty: Import a video, picture or web page",
                         18, sp.dim, BARE_REGULAR);
            }
    }
}

/* ---- Displays ------------------------------------------------------------------- */

static void displays(SpView *v, const SpModel *m) {
    const JVal *ds = json_get(m->status, "displays");
    int n = ds && ds->t == J_ARR ? ds->n : 0;
    char sub[48];
    snprintf(sub, sizeof sub, "%d display%s", n, n == 1 ? "" : "s");
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Displays", sub);
        bare_box(.width = BARE_GROW);
        static const char *modes[] = { "Independent", "Duplicate", "Span" };
        static const char *keys[] = { "independent", "duplicate", "span" };
        const char *mode = sp_setting(m, "display_mode", "independent");
        int sel = !strcmp(mode, "duplicate") ? 1 : !strcmp(mode, "span") ? 2 : 0;
        int c = sp_choices(ID("disp.mode"), modes, 3, sel, true, 52);
        if (c >= 0 && c != sel) RUN(m, "mode", keys[c]);
    }
    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 18, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 24, 6 }) {
        /* The arrangement: each display drawn to scale with its wallpaper. */
        float maxh = 1;
        float totalw = 0;
        for (int i = 0; i < n; i++) {
            float h = (float)json_num(json_get(&ds->kids[i], "height"), 1080);
            float w = (float)json_num(json_get(&ds->kids[i], "width"), 1920);
            if (h > maxh) maxh = h;
            totalw += w;
        }
        bare_row(.width = BARE_GROW, .height = 300, .radius = 32, .gap = 26, .align = BARE_END, .justify = BARE_CENTER,
                 .padding = 28, .glass = sp_glass_card(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO) {
            float scale = 230.0f / maxh;
            BareVec2 win = bare_window_size();
            float room = win.x - 272 - 160 - 26.0f * n;
            if (totalw * scale > room && totalw > 0) scale = room / totalw;
            for (int i = 0; i < n; i++) {
                const JVal *d = &ds->kids[i];
                float w = (float)json_num(json_get(d, "width"), 1920) * scale;
                float h = (float)json_num(json_get(d, "height"), 1080) * scale;
                bare_stack(.id = IDI("disp.screen", i), .width = w, .height = h, .radius = 14, .clip = true,
                           .border = 3, .border_color = sp.dark ? BARE_RGBA(10, 14, 16, 230) : BARE_RGBA(30, 34, 36, 230),
                           .shadow = { 0, 10, 26, 0, BARE_RGBA(0, 0, 0, 90) },
                           .hover = { .scale = 1.03f }, .cursor = BARE_CURSOR_HAND, .animate = sp_spring()) {
                    if (bare_this().clicked) { v->target_display = (int)json_num(json_get(d, "index"), i + 1); v->page = SP_LIBRARY; v->page_time = bare_time(); }
                    bare_box(.width = w, .height = h, .image = wallpaper_picture(m, json_get(d, "wallpaper"), false),
                             .color = BARE_RGBA(0, 0, 0, 120), .passthrough = true);
                    char num[8];
                    snprintf(num, sizeof num, "%d", (int)json_num(json_get(d, "index"), i + 1));
                    bare_stack(.position = BARE_ABSOLUTE, .anchor = BARE_MIDDLE, .width = 54, .height = 54, .radius = 27,
                               .align = BARE_CENTER, .justify = BARE_CENTER, .passthrough = true,
                               .glass = sp_glass_lens(), .border = 1, .border_color = BARE_RGBA(255, 255, 255, 120)) {
                        bare_text(num, .size = 24, .weight = BARE_SEMIBOLD, .color = BARE_WHITE,
                                  .shadow = { 0, 1, 4, 0, BARE_RGBA(0, 0, 0, 160) });
                    }
                }
            }
        }
        for (int i = 0; i < n; i++) {
            const JVal *d = &ds->kids[i];
            const JVal *w = json_get(d, "wallpaper");
            int index = (int)json_num(json_get(d, "index"), i + 1);
            CARD(IDI("disp.card", i)) {
                bare_row(.width = BARE_GROW, .padding = 18, .gap = 22, .align = BARE_CENTER) {
                    bare_box(.width = 210, .height = 118, .radius = 16, .image = wallpaper_picture(m, w, false),
                             .color = BARE_RGBA(0, 0, 0, 80), .shrink = BARE_ZERO);
                    bare_column(.width = BARE_GROW, .gap = 6) {
                        bare_row(.gap = 10, .align = BARE_CENTER) {
                            char title[96];
                            snprintf(title, sizeof title, "%d  %s", index, json_str(json_get(d, "name"), "Display"));
                            sp_label(title, 21, sp.ink, BARE_SEMIBOLD);
                            if (json_bool(json_get(d, "primary"), 0)) sp_chip(IDI("disp.primary", i), NULL, "Primary", true, sp.accent);
                        }
                        char spec[96];
                        snprintf(spec, sizeof spec, "%d \xC3\x97 %d  \xC2\xB7  %d Hz  \xC2\xB7  %s",
                                 (int)json_num(json_get(d, "width"), 0), (int)json_num(json_get(d, "height"), 0),
                                 (int)json_num(json_get(d, "hz"), 0), json_str(json_get(d, "state"), ""));
                        sp_label(spec, 15, sp.dim, BARE_REGULAR);
                        sp_label(w ? json_str(json_get(w, "name"), "") : "No wallpaper", 17, sp.ink, BARE_REGULAR);
                    }
                    if (sp_button(IDI("disp.choose", i), SPI_LIBRARY, "Choose wallpaper", SP_BTN_GLASS).clicked) {
                        v->target_display = index;
                        v->page = SP_LIBRARY;
                        v->page_time = bare_time();
                    }
                }
            }
        }
    }
}


static const BareColor accents[] = { { 0, 215, 5, 255 }, { 10, 132, 255, 255 }, { 94, 92, 230, 255 }, { 191, 90, 242, 255 },
                                     { 255, 55, 95, 255 }, { 255, 159, 10, 255 }, { 255, 214, 10, 255 }, { 100, 210, 255, 255 } };

/* ---- Customise ---------------------------------------------------------------------- */
/* Every Customise command goes to the wallpaper on the chosen display (`--display N`), as the classic window's
 * display picker does. Recordings: `record` commands; the service notes the look / volume / props commands sent
 * while a take runs, so the sliders send at 30 Hz then. Played values come from the recording's samples, their
 * playhead moves on from the position the last answer gave. */

enum { REC_SEC_LOOK, REC_SEC_PROPS, REC_SEC_VOLUME };
static const char *const k_rec_sections[] = { "look", "props", "volume" };

static void crun(const SpView *v, const SpModel *m, const char *const *argv) {
    const char *all[16];
    int n = 0;
    while (argv[n] && n < 12) { all[n] = argv[n]; n++; }
    char num[16];
    if (v->target_display) {
        snprintf(num, sizeof num, "%d", v->target_display);
        all[n++] = "--display";
        all[n++] = num;
    }
    all[n] = NULL;
    if (m->run) m->run(m->user, all);
}
#define CRUN(...) crun(v, m, (const char *[]){ __VA_ARGS__, NULL })

static const JVal *recording_of(const SpModel *m, int sec) {
    const JVal *r = json_get(m->record, k_rec_sections[sec]);
    return r && r->t == J_OBJ ? r : NULL;
}

/* The section being recorded: -1 none. */
static int recording_section(const SpModel *m) {
    const char *s = json_str(json_get(json_get(m->record, "recording"), "section"), "");
    for (int i = 0; i < 3; i++) if (!strcmp(s, k_rec_sections[i])) return i;
    return -1;
}

/* The playhead, 0..1 through the loop. */
static float rec_position(const SpModel *m, const JVal *r) {
    double pos = json_num(json_get(r, "position"), 0);
    if (!json_bool(json_get(r, "paused"), 0)) {
        double n = json_num(json_get(r, "samples"), 1), rate = json_num(json_get(r, "rate"), 30);
        pos += (bare_time() - m->record_time) * json_num(json_get(r, "speed"), 1) * rate / (n > 0 ? n : 1);
    }
    pos -= floor(pos);
    return (float)pos;
}

static const JVal *rec_track_of(const JVal *r, const char *key) {
    const JVal *ts = json_get(r, "tracks");
    for (int i = 0; ts && ts->t == J_ARR && i < ts->n; i++)
        if (!strcmp(json_str(json_get(&ts->kids[i], "key"), ""), key)) return &ts->kids[i];
    return NULL;
}

/* A track's height (0..1) or colour at f. */
static float track_height(const JVal *t, float f) {
    const JVal *vals = json_get(t, "values");
    if (!vals || vals->t != J_ARR || !vals->n) return 0;
    int i = (int)(f * vals->n);
    if (i >= vals->n) i = vals->n - 1;
    return (float)json_num(&vals->kids[i], 0);
}

/* While a section's recording plays (not paused, not being re-recorded), its controls show the played value. */
static bool played_value(const SpModel *m, int sec, const char *key, float *out) {
    const JVal *r = recording_of(m, sec);
    if (!r || json_bool(json_get(r, "paused"), 0) || recording_section(m) == sec) return false;
    const JVal *t = rec_track_of(r, key);
    if (!t || !strcmp(json_str(json_get(t, "kind"), ""), "colour")) return false;
    float lo = (float)json_num(json_get(t, "lo"), 0), hi = (float)json_num(json_get(t, "hi"), 1);
    if (!strcmp(json_str(json_get(t, "kind"), ""), "bool")) lo = 0, hi = 1;
    *out = lo + track_height(t, rec_position(m, r)) * (hi - lo);
    bare_wake_in(1.0f / 30.0f);
    return true;
}

/* A slider that sends `cmd... value` while dragged (8 times a second, 30 while recording) and on release.
 * slot: which of v->drag_look holds it while held. */
static void live_slider(SpView *v, const SpModel *m, BareId id, int slot, float value, float lo, float hi, float width,
                        const char *a0, const char *a1, const char *a2, const char *a3) {
    float val = v->dragging_look == slot + 1 ? v->drag_look[slot] : value;
    int r = sp_slider(id, &val, lo, hi, width);
    if (r) {
        v->dragging_look = r == 1 ? slot + 1 : 0;
        v->drag_look[slot] = val;
        double now = bare_time(), every = recording_section(m) >= 0 ? 1.0 / 30.0 : 0.12;
        if (r == 2 || now - v->look_sent > every) {
            char num[16];
            snprintf(num, sizeof num, "%d", (int)lroundf(val));
            if (!strcmp(a0, "volume")) RUN(m, a0, a1, num);          /* by wallpaper id, not display */
            else if (a3) CRUN(a0, a1, a2, a3, num);
            else if (a2) CRUN(a0, a1, a2, num);
            else CRUN(a0, a1, num);
            v->look_sent = now;
        }
    }
}

/* The value a live slider shows: the dragged one while held. */
static float slider_value(const SpView *v, int slot, float value) {
    return v->dragging_look == slot + 1 ? v->drag_look[slot] : value;
}

static BareColor hex_color(const char *hex) {
    unsigned c = hex && hex[0] == 0x23 ? (unsigned)strtoul(hex + 1, NULL, 16) : 0;
    return BARE_RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

/* A colour: the palette's dots, the colour itself (click: a hue / saturation wheel and a brightness slider
 * open under the row's controls). Returns a new #RRGGBB (throttled while dragging), else NULL. `bright`: offer
 * the brightness slider (the tint has its own). */
static const char *colour_control(SpView *v, BareId id, const char *current, bool bright) {
    static char picked[16];
    const char *out = NULL;
    BareColor cur = hex_color(current);
    bool open = v->wheel_open == id;
    bare_column(.gap = 14, .align = BARE_END, .shrink = BARE_ZERO) {
        bare_row(.gap = 10, .align = BARE_CENTER) {
            for (int i = 0; i < 8; i++) {
                BareColor c = accents[i];
                bool on = c.r == cur.r && c.g == cur.g && c.b == cur.b;
                BareState s = bare_box(.id = id + 1 + (BareId)i, .width = 28, .height = 28, .radius = 14, .color = c,
                                       .border = on ? 3.0f : 1.0f, .border_color = on ? BARE_WHITE : BARE_RGBA(255, 255, 255, 80),
                                       .shadow = on ? (BareShadow){ 0, 0, 14, 0, sp_alpha(c, 180) } : (BareShadow){ 0 },
                                       .hover = { .scale = 1.18f }, .press = { .scale = 0.9f }, .cursor = BARE_CURSOR_HAND,
                                       .animate = sp_spring());
                if (s.clicked && !on) {
                    snprintf(picked, sizeof picked, "#%02X%02X%02X", c.r, c.g, c.b);
                    out = picked;
                }
            }
            /* the colour itself: opens the wheel */
            BareState sw = { 0 };
            bare_row(.id = id + 20, .height = 34, .radius = 17, .pad = { 0, 12, 0, 6 }, .gap = 8,
                                    .align = BARE_CENTER, .glass = sp_glass_control(), .border = 1,
                                    .border_color = open ? sp.accent : sp.hair, .cursor = BARE_CURSOR_HAND,
                                    .hover = { .scale = 1.03f }, .animate = sp_spring()) {
                sw = bare_this();
                bare_box(.width = 24, .height = 24, .radius = 12, .color = cur, .border = 1,
                         .border_color = BARE_RGBA(255, 255, 255, 120), .passthrough = true);
                bare_text(current && *current ? current : "#000000", .size = 14.5f, .weight = BARE_MEDIUM, .color = sp.ink,
                          .nowrap = true);
            }
            if (sw.clicked) {
                v->wheel_open = open ? 0 : id;
                sp_rgb_hsv(cur.r, cur.g, cur.b, &v->wheel_h, &v->wheel_s, &v->wheel_v);
                if (!bright) v->wheel_v = 1;
            }
        }
        if (open) {
            bare_row(.gap = 22, .align = BARE_CENTER) {
                if (v->wheel_drag != id) {
                    float vv;
                    sp_rgb_hsv(cur.r, cur.g, cur.b, &v->wheel_h, &v->wheel_s, &vv);
                    if (bright) v->wheel_v = vv;
                }
                int r = sp_wheel(id + 30, 150, &v->wheel_h, &v->wheel_s);
                int rv = 0;
                if (bright) {
                    bare_column(.gap = 6) {
                        sp_label("Brightness", 14.5f, sp.dim, BARE_MEDIUM);
                        float pct = v->wheel_v * 100;
                        rv = sp_slider(id + 31, &pct, 0, 100, 200);
                        if (rv) v->wheel_v = pct / 100;
                    }
                }
                int any = r ? r : rv;
                v->wheel_drag = any == 1 ? id : 0;
                double now = bare_time();
                if (any == 2 || (any == 1 && now - v->wheel_sent > 0.12)) {
                    int rr, gg, bb;
                    sp_hsv_rgb(v->wheel_h, v->wheel_s, bright ? v->wheel_v : 1.0f, &rr, &gg, &bb);
                    snprintf(picked, sizeof picked, "#%02X%02X%02X", rr, gg, bb);
                    out = picked;
                    v->wheel_sent = now;
                }
            }
        }
    }
    return out;
}

/* A slider's value as text; click it to type one. Returns true with *typed (kept within lo..hi) when one was
 * entered: Enter or clicking away takes it, Escape doesn't. */
static bool value_field(SpView *v, BareId id, const char *shown, float val, float lo, float hi, float *typed) {
    static const BareBox field = { .color = { 255, 255, 255, 0 }, .border = BARE_ZERO, .height = 26,
                                   .pad = { 1, 0, 1, 0 }, .cursor = BARE_CURSOR_TEXT };
    bool done = false;
    if (v->value_edit == id) {
        bare_row(.width = 76, .height = 34, .radius = 10, .pad = { 0, 8, 0, 8 }, .align = BARE_CENTER,
                 .glass = sp_glass_control(), .border = 1, .border_color = sp.accent, .shrink = BARE_ZERO) {
            BareState s = bare_input(v->value_text, sizeof v->value_text, .id = id + 7, .style = &field, .width = BARE_GROW,
                                     .size = 16, .color = sp.ink, .caret = sp.accent, .selection = sp_alpha(sp.accent, 90));
            if (s.focused) v->value_focused = true;
            if (s.focused && bare_key_pressed(BARE_KEY_ESCAPE)) {
                v->value_edit = 0;
                bare_focus(0);
            } else if (s.submitted || (v->value_focused && !s.focused)) {
                char *end;
                float f = strtof(v->value_text, &end);
                if (end != v->value_text) {
                    *typed = f < lo ? lo : f > hi ? hi : f;
                    done = true;
                }
                v->value_edit = 0;
                if (s.focused) bare_focus(0);
            }
        }
        return done;
    }
    BareState s = { 0 };
    bare_row(.id = id, .width = 76, .height = 34, .radius = 10, .pad = { 0, 8, 0, 8 }, .align = BARE_CENTER,
             .justify = BARE_END, .cursor = BARE_CURSOR_TEXT, .hover = { .color = sp_alpha(sp.ink, 16) },
             .shrink = BARE_ZERO) {
        s = bare_this();
        sp_label(shown, 17, sp.ink, BARE_MEDIUM);
    }
    if (s.clicked) {
        v->value_edit = id;
        v->value_focused = false;
        if (fabsf(val - roundf(val)) < 0.001f) snprintf(v->value_text, sizeof v->value_text, "%d", (int)lroundf(val));
        else snprintf(v->value_text, sizeof v->value_text, "%.2f", val);
        bare_focus(id + 7);
        bare_edit(id + 7, BARE_EDIT_SELECT_ALL);
    }
    return false;
}

/* value_field for a live slider: a typed value is sent as `cmd... value`. */
static void live_value(SpView *v, const SpModel *m, BareId id, const char *fmt, int slot, float value, float lo, float hi,
                       const char *a0, const char *a1, const char *a2, const char *a3) {
    char t[24], num[24];
    float shown = slider_value(v, slot, value), typed;
    snprintf(t, sizeof t, fmt, (int)lroundf(shown));
    if (!value_field(v, id, t, shown, lo, hi, &typed)) return;
    snprintf(num, sizeof num, "%d", (int)lroundf(typed));
    if (!strcmp(a0, "volume")) RUN(m, a0, a1, num);
    else if (a3) CRUN(a0, a1, a2, a3, num);
    else if (a2) CRUN(a0, a1, a2, num);
    else CRUN(a0, a1, num);
}

/* ---- recordings ---------------------------------------------------------------------- */

static void small_icon_button(BareId id, const char *icon, BareColor c, bool *clicked) {
    BareState s = { 0 };
    bare_stack(.id = id, .width = 40, .height = 40, .radius = 20, .align = BARE_CENTER,
                             .justify = BARE_CENTER, .glass = sp_glass_control(), .border = 1, .border_color = sp.hair,
                             .cursor = BARE_CURSOR_HAND, .hover = { .scale = 1.08f }, .press = { .scale = 0.92f },
                             .animate = sp_spring()) {
        s = bare_this();
        sp_icon(icon, 17, c);
    }
    *clicked = s.clicked;
}

/* A section's title with its record controls: record; while recording, the clock and stop; with a recording,
 * pause / play, the graphs and delete. */
static void rec_bar(SpView *v, const SpModel *m, int sec, const char *title) {
    const JVal *r = recording_of(m, sec);
    int now_rec = recording_section(m);
    BareColor red = BARE_RGB(255, 69, 58);
    bare_row(.width = BARE_GROW, .height = 48, .align = BARE_CENTER, .gap = 8, .shrink = BARE_ZERO) {
        section(title);
        bare_box(.width = BARE_GROW);
        bool click = false;
        if (now_rec == sec) {
            /* recording: the clock and a big red Stop (no return in here: the row must close) */
            double secs = json_num(json_get(json_get(m->record, "recording"), "seconds"), 0) + (bare_time() - m->record_time);
            char t[24];
            snprintf(t, sizeof t, "%d:%02d", (int)secs / 60, (int)secs % 60);
            float pulse = sp.motion > 0 ? 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)bare_time() * 5.0f)) : 1.0f;
            BareState st = { 0 };
            bare_row(.id = IDI("rec.stop", sec), .height = 42, .radius = 21, .pad = { 0, 18, 0, 14 }, .gap = 10,
                     .align = BARE_CENTER, .color = red, .shrink = BARE_ZERO, .cursor = BARE_CURSOR_HAND,
                     .shadow = { 0, 4, 18, 0, sp_alpha(red, (int)(150 * pulse)) },
                     .hover = { .scale = 1.05f }, .press = { .scale = 0.95f }, .animate = sp_spring()) {
                st = bare_this();
                bare_box(.width = 12, .height = 12, .radius = 3, .color = BARE_WHITE, .passthrough = true);
                bare_text("Stop", .size = 15.5f, .weight = BARE_SEMIBOLD, .color = BARE_WHITE, .nowrap = true);
                bare_text(t, .size = 15.5f, .weight = BARE_MEDIUM, .color = BARE_RGBA(255, 255, 255, 210), .nowrap = true);
            }
            bare_wake_in(0.25f);
            if (st.clicked) {
                sp_sound(SP_SOUND_STOP);
                CRUN("record", k_rec_sections[sec], "stop");
            }
        } else {
            if (r) {
                bool paused = json_bool(json_get(r, "paused"), 0);
                small_icon_button(IDI("rec.pause", sec), paused ? SPI_PLAY : SPI_PAUSE, paused ? sp.accent : sp.ink, &click);
                if (click) CRUN("record", k_rec_sections[sec], paused ? "play" : "pause");
                small_icon_button(IDI("rec.graphs", sec), SPI_GRAPH, v->graphs[sec] ? sp.accent : sp.ink, &click);
                if (click) v->graphs[sec] = !v->graphs[sec];
                small_icon_button(IDI("rec.del", sec), SPI_DELETE, sp.ink, &click);
                if (click) {
                    sp_sound(SP_SOUND_DELETE);
                    CRUN("record", k_rec_sections[sec], "delete");
                    v->graphs[sec] = false;
                }
            }
            /* record: a red dot (another section recording: wait for it) */
            BareState s = { 0 };
            bare_stack(.id = IDI("rec.start", sec), .width = 40, .height = 40, .radius = 20, .align = BARE_CENTER,
                       .justify = BARE_CENTER, .glass = sp_glass_control(), .border = 1, .border_color = sp.hair,
                       .opacity = now_rec >= 0 ? 0.4f : 1.0f,
                       .cursor = now_rec >= 0 ? BARE_CURSOR_NOT_ALLOWED : BARE_CURSOR_HAND, .hover = { .scale = 1.08f },
                       .press = { .scale = 0.92f }, .animate = sp_spring()) {
                s = bare_this();
                bare_box(.width = 14, .height = 14, .radius = 7, .color = red, .shadow = { 0, 0, 8, 0, sp_alpha(red, 140) },
                         .passthrough = true);
            }
            if (s.clicked && now_rec < 0) {
                sp_sound(SP_SOUND_RECORD);
                CRUN("record", k_rec_sections[sec], "start");
            }
        }
    }
}

/* ---- graphs: shaders/graph.glsl draws each into a canvas the size of its box, in screen pixels ---------------- */

typedef struct GraphParams {
    float size[4], c0[4], c1[4], bg[4], misc[4];   /* see graph.glsl */
    float v[240];
} GraphParams;

typedef struct GraphCanvas { int key; BareImage image; int w, h; float lw, lh; double used; } GraphCanvas;
static GraphCanvas g_graphs[64];

/* Graph `key`'s canvas, sized to its box as laid out last frame (no image before that). */
static GraphCanvas *graph_canvas(int key) {
    GraphCanvas *c = NULL, *oldest = &g_graphs[0];
    for (int i = 0; i < 64 && !c; i++) {
        if (g_graphs[i].key == key) c = &g_graphs[i];
        else if (g_graphs[i].used < oldest->used) oldest = &g_graphs[i];
    }
    if (!c) {
        c = oldest;
        if (c->image) bare_image_free(c->image);
        *c = (GraphCanvas){ .key = key };
    }
    c->used = bare_time();
    float sc = bare_pixel_scale();
    int w = (int)ceilf(c->lw * sc), h = (int)ceilf(c->lh * sc);
    if (w > 0 && h > 0 && (!c->image || w != c->w || h != c->h)) {
        if (c->image) bare_image_free(c->image);
        c->image = bare_canvas(.width = w, .height = h);
        c->w = w;
        c->h = h;
    }
    return c;
}

/* Draws gp into c, for the box laid out at r (a new size shows from the next frame). */
static void graph_render(GraphCanvas *c, GraphParams *gp, BareRect r) {
    static BareShader shader;
    if (r.w != c->lw || r.h != c->lh) {
        c->lw = r.w;
        c->lh = r.h;
        bare_wake();
    }
    if (!c->image) return;
    if (!shader) shader = bare_shader(&graph_shader);
    gp->size[0] = (float)c->w;
    gp->size[1] = (float)c->h;
    gp->size[3] = bare_pixel_scale();
    bare_pass(.target = c->image, .shader = shader, BARE_PARAMS(*gp));
}

static void rgba4(float out[4], BareColor c, float a) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
    out[3] = a;
}

/* The graphs' colours: the accent at the left, turning a third of the way round the wheel by the right. */
static void graph_colours(GraphParams *gp, float mode, float play) {
    float h, s, val;
    int r, g, b;
    sp_rgb_hsv(sp.accent.r, sp.accent.g, sp.accent.b, &h, &s, &val);
    sp_hsv_rgb(fmodf(h + 105.0f, 360.0f), s < 0.6f ? 0.6f : s, val < 0.85f ? 0.85f : val, &r, &g, &b);
    rgba4(gp->c0, sp.accent, mode);
    rgba4(gp->c1, BARE_RGB(r, g, b), play);
}

/* The pen's stroke from (x0, y0) to (x1, y1) into the drawn heights. */
static void pen_line(SpView *v, float x0, float y0, float x1, float y1) {
    int n = v->draw_n;
    if (n <= 0) return;
    if (x1 < x0) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    int a = (int)floorf(x0 * n), b = (int)floorf(x1 * n);
    a = a < 0 ? 0 : a >= n ? n - 1 : a;
    b = b < 0 ? 0 : b >= n ? n - 1 : b;
    for (int i = a; i <= b; i++) {
        float f = x1 - x0 > 1e-6f ? ((i + 0.5f) / n - x0) / (x1 - x0) : 1.0f;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        v->draw_vals[i] = y0 + (y1 - y0) * f;
    }
}

static void send_stroke(SpView *v, const SpModel *m, int sec, int ti, float x1, float y1) {
    char tn[16], a[4][16];
    snprintf(tn, sizeof tn, "%d", ti + 1);
    snprintf(a[0], 16, "%.4f", v->draw_x);
    snprintf(a[1], 16, "%.4f", v->draw_y);
    snprintf(a[2], 16, "%.4f", x1);
    snprintf(a[3], 16, "%.4f", y1);
    CRUN("record", k_rec_sections[sec], "draw", tn, a[0], a[1], a[2], a[3]);
    v->draw_x = x1;
    v->draw_y = y1;
    v->draw_sent = bare_time();
}

/* One track's graph: its curve (colours: a strip), the playhead and the value under the pointer. Drawing over it
 * rewrites the loop: the curve follows the pen at once, the strokes go to the service as they're drawn. */
static void track_graph(SpView *v, const SpModel *m, int sec, int ti, const JVal *t, float pos) {
    const JVal *vals = json_get(t, "values");
    int n = vals && vals->t == J_ARR ? vals->n : 0;
    bool colour = !strcmp(json_str(json_get(t, "kind"), ""), "colour");
    bool flag = !strcmp(json_str(json_get(t, "kind"), ""), "bool");
    if (n > (colour ? 60 : 240)) n = colour ? 60 : 240;
    if (!n) return;
    int key = 1 + sec * 64 + ti;
    const float H = colour ? 40.0f : 132.0f, INSET = 10;
    float lo = flag ? 0 : (float)json_num(json_get(t, "lo"), 0), hi = flag ? 1 : (float)json_num(json_get(t, "hi"), 1);
    GraphParams gp = { 0 };
    graph_colours(&gp, colour ? 1.0f : 0.0f, pos);
    rgba4(gp.bg, sp.dark ? BARE_RGB(10, 12, 22) : BARE_RGB(248, 249, 253), sp.dark ? 0.62f : 0.8f);
    gp.size[2] = (float)n;
    gp.misc[0] = -1;
    gp.misc[1] = sp.dark ? 0.055f : -0.06f;
    gp.misc[2] = 2.5f;
    gp.misc[3] = INSET;
    /* the drawn heights stay until a copy from the service arrives that was made after the pen came up */
    if (v->draw_key == key && v->draw_track != key && m->record_time > v->draw_release + 0.4) v->draw_key = 0;
    bool drawn = v->draw_key == key && v->draw_n == n;
    GraphCanvas *c = graph_canvas(key);
    BareState st = { 0 };
    bare_stack(.id = IDI("rec.graph", key), .width = BARE_GROW, .height = H, .radius = colour ? 12.0f : 16.0f, .clip = true,
               .image = c->image, .border = 1, .border_color = sp.hair,
               .cursor = colour ? BARE_CURSOR_DEFAULT : BARE_CURSOR_CROSSHAIR) {
        st = bare_this();
        BareVec2 p = bare_mouse();
        float fx = st.rect.w > 0 ? (p.x - st.rect.x) / st.rect.w : 0;
        float fy = st.rect.h > 2 * INSET ? 1.0f - (p.y - st.rect.y - INSET) / (st.rect.h - 2 * INSET) : 0;
        fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
        fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
        if (!colour && st.pressed && st.rect.w > 0) {
            if (v->draw_track != key) {                    /* the pen goes down */
                if (!drawn) {
                    for (int i = 0; i < n; i++) v->draw_vals[i] = (float)json_num(&vals->kids[i], 0);
                    v->draw_n = n;
                }
                v->draw_track = v->draw_key = key;
                v->draw_x = v->pen_x = fx;
                v->draw_y = v->pen_y = fy;
                v->draw_sent = 0;
                drawn = true;
            }
            pen_line(v, v->pen_x, v->pen_y, fx, fy);
            v->pen_x = fx;
            v->pen_y = fy;
            if (bare_time() - v->draw_sent > 1.0 / 30.0 && (fx != v->draw_x || fy != v->draw_y))
                send_stroke(v, m, sec, ti, fx, fy);
        } else if (v->draw_track == key) {                 /* the pen comes up */
            if (!v->draw_sent || v->pen_x != v->draw_x || v->pen_y != v->draw_y) send_stroke(v, m, sec, ti, v->pen_x, v->pen_y);
            v->draw_track = 0;
            v->draw_release = bare_time();
        }
        if (colour) {
            for (int i = 0; i < n; i++) rgba4(gp.v + i * 4, hex_color(json_str(&vals->kids[i], "#000000")), 1);
        } else if (drawn) {
            memcpy(gp.v, v->draw_vals, sizeof(float) * (size_t)n);
        } else {
            for (int i = 0; i < n; i++) gp.v[i] = (float)json_num(&vals->kids[i], 0);
        }
        /* the value under the pointer */
        if (!colour && (st.hovered || st.pressed) && st.rect.w > 0) {
            float at = fx * n - 0.5f;
            int i0 = (int)floorf(at);
            float f = at - (float)i0, h0 = gp.v[i0 < 0 ? 0 : i0 >= n ? n - 1 : i0], h1 = gp.v[i0 + 1 < 0 ? 0 : i0 + 1 >= n ? n - 1 : i0 + 1];
            float h = st.pressed ? fy : h0 + (h1 - h0) * f, val = lo + h * (hi - lo);
            char txt[24];
            if (flag) snprintf(txt, sizeof txt, "%s", h >= 0.5f ? "On" : "Off");
            else if (hi - lo > 20) snprintf(txt, sizeof txt, "%d", (int)lroundf(val));
            else snprintf(txt, sizeof txt, "%.2f", val);
            gp.misc[0] = fx;
            float x = fx * st.rect.w + 12, y = (1 - h) * (st.rect.h - 2 * INSET) + INSET - 34;
            if (x > st.rect.w - 70) x = fx * st.rect.w - 70;
            if (y < 4) y = (1 - h) * (st.rect.h - 2 * INSET) + INSET + 10;
            bare_row(.position = BARE_ABSOLUTE, .x = x, .y = y, .height = 24, .pad = { 0, 9, 0, 9 }, .radius = 12,
                     .align = BARE_CENTER, .color = sp.dark ? BARE_RGBA(16, 18, 28, 225) : BARE_RGBA(255, 255, 255, 240),
                     .border = 1, .border_color = sp.hair, .passthrough = true) {
                bare_text(txt, .size = 13, .weight = BARE_SEMIBOLD, .color = sp.ink, .nowrap = true);
            }
        }
    }
    graph_render(c, &gp, st.rect);
}

/* A preset's shape, small, for its tile. */
static void preset_shape(int i, const JVal *shape) {
    int sn = shape && shape->t == J_ARR ? shape->n : 0;
    if (sn > 240) sn = 240;
    GraphCanvas *c = graph_canvas(4096 + i);
    BareState s = bare_box(.width = BARE_GROW, .height = 42, .radius = 8, .image = c->image, .passthrough = true);
    GraphParams gp = { 0 };
    graph_colours(&gp, 0, -1);
    gp.size[2] = (float)(sn ? sn : 1);
    gp.misc[0] = -1;
    gp.misc[2] = 2;
    gp.misc[3] = 5;
    for (int k = 0; k < sn; k++) gp.v[k] = (float)json_num(&shape->kids[k], 0);
    graph_render(c, &gp, s.rect);
}

/* The preset shapes for one track: a click fills its loop with that shape, repeated `times`. */
static void preset_strip(SpView *v, const SpModel *m, int sec, int ti) {
    const JVal *ps = json_get(m->record, "presets");
    int n = ps && ps->t == J_ARR ? ps->n : 0;
    if (v->preset_times < 1) v->preset_times = 1;
    bare_column(.width = BARE_GROW, .gap = 10, .pad = { 4, 0, 6, 0 }) {
        bare_row(.gap = 10, .align = BARE_CENTER) {
            sp_label("Repeat", 15, sp.dim, BARE_MEDIUM);
            if (sp_button(IDI("rec.times.minus", sec * 64 + ti), NULL, "\xE2\x88\x92", SP_BTN_GLASS).clicked && v->preset_times > 1)
                v->preset_times--;
            char t[16];
            snprintf(t, sizeof t, "\xC3\x97%d", v->preset_times);
            bare_row(.width = 44, .justify = BARE_CENTER) { sp_label(t, 16, sp.ink, BARE_SEMIBOLD); }
            if (sp_button(IDI("rec.times.plus", sec * 64 + ti), NULL, "+", SP_BTN_GLASS).clicked && v->preset_times < 20)
                v->preset_times++;
        }
        for (int row = 0; row * 7 < n; row++) {
            bare_row(.width = BARE_GROW, .gap = 8) {
                for (int i = row * 7; i < n && i < row * 7 + 7; i++) {
                    const JVal *p = &ps->kids[i];
                    const JVal *shape = json_get(p, "shape");
                    BareState s = { 0 };
                    bare_column(.id = IDI("rec.preset", (sec * 64 + ti) * 32 + i), .width = BARE_GROW, .height = 76,
                                              .radius = 12, .padding = 8, .gap = 4, .glass = sp_glass_control(), .border = 1,
                                              .border_color = sp.hair, .cursor = BARE_CURSOR_HAND,
                                              .hover = { .scale = 1.04f }, .press = { .scale = 0.96f }, .animate = sp_spring()) {
                        s = bare_this();
                        preset_shape(i, shape);
                        bare_text(json_str(json_get(p, "name"), ""), .size = 12.5f, .color = sp.dim, .nowrap = true,
                                  .ellipsis = true);
                    }
                    if (s.clicked) {
                        char tn[16], times[16];
                        snprintf(tn, sizeof tn, "%d", ti + 1);
                        snprintf(times, sizeof times, "%d", v->preset_times);
                        CRUN("record", k_rec_sections[sec], "preset", tn, json_str(json_get(p, "name"), ""), times);
                    }
                }
            }
        }
    }
}

static const char *track_label(const JVal *t) {
    static const struct { const char *key, *name; } names[] = {
        { "brightness", "Brightness" }, { "contrast", "Contrast" }, { "saturation", "Saturation" }, { "hue", "Hue" },
        { "gamma", "Gamma" }, { "tint", "Tint colour" }, { "tint-strength", "Tint strength" },
        { "tint-brightness", "Tint brightness" }, { "volume", "Volume" } };
    const char *l = json_str(json_get(t, "label"), "");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(l, names[i].key)) return names[i].name;
    return *l ? l : json_str(json_get(t, "key"), "");
}

/* The graph view of a section's recording: speed, smoothing, then each track. */
static void rec_graphs(SpView *v, const SpModel *m, int sec) {
    const JVal *r = recording_of(m, sec);
    if (!r || !v->graphs[sec]) return;
    const JVal *ts = json_get(r, "tracks");
    float pos = rec_position(m, r);
    if (!json_bool(json_get(r, "paused"), 0)) bare_wake_in(1.0f / 60.0f);
    CARD(IDI("rec.card", sec)) {
        /* speed: 0.25x..4x on a log scale; smoothing 0..100 */
        for (int k = 0; k < 2; k++) {
            if (k) divider();
            int slot = 1 + sec * 2 + k;
            float now = k ? (float)json_num(json_get(r, "smooth"), 0)
                          : (float)(log(json_num(json_get(r, "speed"), 1) / 0.25) / log(16.0));
            float val = v->rec_slider == slot ? v->rec_value : now;
            ROW(IDI("rec.set", slot), k ? SPI_HEAL : SPI_SPEED, k ? "Smoothing" : "Speed", NULL) {
                char t[24];
                if (k) snprintf(t, sizeof t, "%d%%", (int)lroundf(val));
                else snprintf(t, sizeof t, "%.2f\xC3\x97", 0.25 * pow(16.0, val));
                float shown = k ? val : (float)(0.25 * pow(16.0, val)), typed;
                if (value_field(v, IDI("rec.val", slot), t, shown, k ? 0.0f : 0.25f, k ? 100.0f : 4.0f, &typed)) {
                    char num[24];
                    snprintf(num, sizeof num, k ? "%.0f" : "%.2f", typed);
                    CRUN("record", k_rec_sections[sec], k ? "smooth" : "speed", num);
                }
                int res = sp_slider(IDI("rec.slider", slot), &val, 0, k ? 100.0f : 1.0f, 380);
                if (res == 1) { v->rec_slider = slot; v->rec_value = val; }
                if (res == 2) {
                    char num[24];
                    if (k) snprintf(num, sizeof num, "%d", (int)lroundf(val));
                    else snprintf(num, sizeof num, "%.2f", round(0.25 * pow(16.0, val) * 20.0) / 20.0);
                    CRUN("record", k_rec_sections[sec], k ? "smooth" : "speed", num);
                    v->rec_slider = 0;
                }
            }
        }
        for (int i = 0; ts && ts->t == J_ARR && i < ts->n; i++) {
            const JVal *t = &ts->kids[i];
            bool colour = !strcmp(json_str(json_get(t, "kind"), ""), "colour");
            int key = 1 + sec * 64 + i;
            divider();
            bare_column(.width = BARE_GROW, .pad = { 12, 22, 14, 22 }, .gap = 8) {
                bare_row(.width = BARE_GROW, .align = BARE_CENTER, .gap = 10) {
                    sp_label(track_label(t), 17, sp.ink, BARE_MEDIUM);
                    bare_box(.width = BARE_GROW);
                    if (!colour && sp_button(IDI("rec.presets", key), SPI_TILES, "Presets",
                                             v->presets_for == key ? SP_BTN_PRIMARY : SP_BTN_GLASS).clicked)
                        v->presets_for = v->presets_for == key ? 0 : key;
                }
                track_graph(v, m, sec, i, t, pos);
                if (v->presets_for == key) preset_strip(v, m, sec, i);
            }
        }
    }
}

/* ---- sections -------------------------------------------------------------------------- */

static void customise_look(SpView *v, const SpModel *m) {
    static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
    static const char *names[] = { "Brightness", "Contrast", "Saturation", "Hue", "Gamma" };
    static const char *icons[] = { SPI_SPEED, SPI_PALETTE, SPI_DROPPER, SPI_BRUSH, SPI_EYE };
    static const float lo[] = { -100, -100, -100, -180, -100 }, hi[] = { 100, 100, 100, 180, 100 };
    static const char *tkeys[] = { "tintStrength", "tintBrightness" }, *targs[] = { "tint-strength", "tint-brightness" };
    static const char *trec[] = { "tintLift", "tintBrightness" };
    static const char *tnames[] = { "Strength", "Brightness" };
    bool web = !strcmp(json_str(json_get(json_get(m->look, "wallpaper"), "type"), ""), "html");
    CARD(ID("cus.card")) {
        for (int i = 0; i < 5; i++) {
            if (web && i == 4) continue;                       /* CSS has no gamma */
            if (i) divider();
            ROW(IDI("cus.row", i), icons[i], names[i], NULL) {
                float now = (float)json_num(json_get(m->look, keys[i]), 0);
                played_value(m, REC_SEC_LOOK, keys[i], &now);
                live_value(v, m, IDI("cus.val", i), "%+d", i, now, lo[i], hi[i], "look", "set", keys[i], NULL);
                live_slider(v, m, IDI("cus.slider", i), i, now, lo[i], hi[i], 380, "look", "set", keys[i], NULL);
            }
        }
        divider();
        /* tint: the wheel (hue, saturation) beside strength and brightness */
        const char *tint = json_str(json_get(m->look, "tint"), "#000000");
        BareColor tc = hex_color(tint);
        bare_row(.width = BARE_GROW, .pad = { 16, 22, 18, 22 }, .gap = 30, .align = BARE_CENTER) {
            float h = v->wheel_h, s = v->wheel_s, vv;
            if (v->wheel_drag != ID("cus.tint")) sp_rgb_hsv(tc.r, tc.g, tc.b, &h, &s, &vv);
            int r = sp_wheel(ID("cus.tint.wheel"), 170, &h, &s);
            if (r) {
                v->wheel_h = h;
                v->wheel_s = s;
                v->wheel_drag = r == 1 ? ID("cus.tint") : 0;
                double now = bare_time(), every = recording_section(m) >= 0 ? 1.0 / 30.0 : 0.12;
                if (r == 2 || now - v->wheel_sent > every) {
                    int rr, gg, bb;
                    char hex[16];
                    sp_hsv_rgb(h, s, 1.0f, &rr, &gg, &bb);
                    snprintf(hex, sizeof hex, "#%02X%02X%02X", rr, gg, bb);
                    CRUN("look", "set", "tint", hex);
                    v->wheel_sent = now;
                }
            }
            bare_column(.width = BARE_GROW, .gap = 14) {
                bare_row(.gap = 12, .align = BARE_CENTER) {
                    sp_icon(SPI_BRUSH, 20, sp.ink);
                    sp_label("Tint", 18, sp.ink, BARE_MEDIUM);
                    bare_box(.width = 22, .height = 22, .radius = 11, .color = tc, .border = 1,
                             .border_color = BARE_RGBA(255, 255, 255, 120));
                    sp_label(tint, 15, sp.dim, BARE_MEDIUM);
                }
                for (int i = 0; i < 2; i++) {
                    bare_row(.width = BARE_GROW, .gap = 12, .align = BARE_CENTER) {
                        bare_row(.width = 110) { sp_label(tnames[i], 16, sp.dim, BARE_MEDIUM); }
                        float now = (float)json_num(json_get(m->look, tkeys[i]), 0);
                        played_value(m, REC_SEC_LOOK, trec[i], &now);
                        live_value(v, m, IDI("cus.tval", i), "%d%%", 5 + i, now, 0, 100, "look", "set", targs[i], NULL);
                        live_slider(v, m, IDI("cus.tslider", i), 5 + i, now, 0, 100, 300, "look", "set", targs[i], NULL);
                    }
                }
            }
        }
    }
}

static void picture_rect(const char *mode, float da, float pa, float out[4]) {
    float w = 1, h = 1;
    if (!strcmp(mode, "fill")) { if (pa > da) w = pa / da; else h = da / pa; }
    else if (!strcmp(mode, "fit") || !strcmp(mode, "original")) { if (pa > da) h = da / pa; else w = pa / da; }
    else if (!strcmp(mode, "fit-width")) h = da / pa;
    else if (!strcmp(mode, "fit-height")) w = pa / da;
    out[0] = (1 - w) / 2; out[1] = (1 - h) / 2; out[2] = w; out[3] = h;
}

/* Where the picture sits now (display fractions), as the service works it out. */
static void layout_now(const SpModel *m, float out[4]) {
    const JVal *r = json_get(m->layout, "rect");
    if (r && r->t == J_ARR && r->n == 4) {
        for (int k = 0; k < 4; k++) out[k] = (float)json_num(&r->kids[k], k < 2 ? 0 : 1);
        return;
    }
    picture_rect(json_str(json_get(m->layout, "layout"), "fill"), (float)json_num(json_get(m->layout, "display"), 16.0 / 9.0),
                 (float)json_num(json_get(m->layout, "picture"), 16.0 / 9.0), out);
}

static void layout_send(SpView *v, const SpModel *m, const char *mode, const float r[4]) {
    if (strcmp(mode, "custom")) {
        CRUN("layout", mode);
        return;
    }
    char a[4][16];
    for (int k = 0; k < 4; k++) snprintf(a[k], 16, "%.3f", r[k] * 100);
    CRUN("layout", "custom", a[0], a[1], a[2], a[3]);
}

/* Undo / redo keep every settled layout (not each step of a drag), per wallpaper. */
static void layout_push(SpView *v, const char *mode, const float r[4]) {
    int at = v->layout_hist_at;
    if (v->layout_hist_n && !strcmp(v->layout_hist_mode[at], mode) &&
        (strcmp(mode, "custom") || !memcmp(v->layout_hist[at], r, sizeof v->layout_hist[0])))
        return;
    int n = v->layout_hist_n ? at + 1 : 0;
    if (n == 40) {
        memmove(v->layout_hist, v->layout_hist + 1, sizeof v->layout_hist[0] * 39);
        memmove(v->layout_hist_mode, v->layout_hist_mode + 1, sizeof v->layout_hist_mode[0] * 39);
        n--;
    }
    memcpy(v->layout_hist[n], r, sizeof v->layout_hist[0]);
    snprintf(v->layout_hist_mode[n], sizeof v->layout_hist_mode[0], "%s", mode);
    v->layout_hist_n = n + 1;
    v->layout_hist_at = n;
}

static void layout_step(SpView *v, const SpModel *m, int step) {
    int at = v->layout_hist_at + step;
    if (at < 0 || at >= v->layout_hist_n) return;
    v->layout_hist_at = at;
    memcpy(v->layout_rect, v->layout_hist[at], sizeof v->layout_rect);
    v->layout_hold = bare_time() + 1.5;
    layout_send(v, m, v->layout_hist_mode[at], v->layout_hist[at]);
}

static float lay_snap(float x, float d) {
    static const float k[] = { 0.0f, 0.5f, 1.0f };
    for (int i = 0; i < 3; i++) if (fabsf(x - k[i]) < d) return k[i];
    return x;
}

/* A drag from layout_start by (dx, dy) display fractions. op 0 moves (edges and centre snap); else its bits are the
 * edges that move (1 left, 2 right, 4 top, 8 bottom). Shift: no snapping, and a resize keeps the shape. */
static void layout_drag_by(SpView *v, int op, float dx, float dy, float sx, float sy, bool shift) {
    const float *s = v->layout_start;
    float L = s[0], T = s[1], R = s[0] + s[2], B = s[1] + s[3];
    if (!op) {
        float w = s[2], h = s[3];
        L = s[0] + dx;
        T = s[1] + dy;
        if (!shift) {
            float a = lay_snap(L, sx), b = lay_snap(L + w, sx) - w, c = lay_snap(L + w * 0.5f, sx) - w * 0.5f;
            L = a != L ? a : b != L ? b : c;
            a = lay_snap(T, sy), b = lay_snap(T + h, sy) - h, c = lay_snap(T + h * 0.5f, sy) - h * 0.5f;
            T = a != T ? a : b != T ? b : c;
        }
        R = L + w;
        B = T + h;
    } else {
        if (op & 1) L = shift ? L + dx : lay_snap(L + dx, sx);
        if (op & 2) R = shift ? R + dx : lay_snap(R + dx, sx);
        if (op & 4) T = shift ? T + dy : lay_snap(T + dy, sy);
        if (op & 8) B = shift ? B + dy : lay_snap(B + dy, sy);
        const float mn = 0.02f;
        if (R - L < mn) { if (op & 1) L = R - mn; else R = L + mn; }
        if (B - T < mn) { if (op & 4) T = B - mn; else B = T + mn; }
        if (shift && s[2] > 0 && s[3] > 0) {
            float a = s[3] / s[2], w = R - L, h = B - T;
            if ((op & 3) && (op & 12)) {
                float k = fmaxf(w / s[2], h / s[3]);
                w = s[2] * k;
                h = s[3] * k;
                if (op & 1) L = R - w; else R = L + w;
                if (op & 4) T = B - h; else B = T + h;
            } else if (op & 3) {
                float cy = s[1] + s[3] * 0.5f;
                h = w * a;
                T = cy - h * 0.5f;
                B = T + h;
            } else {
                float cx = s[0] + s[2] * 0.5f;
                w = h / a;
                L = cx - w * 0.5f;
                R = L + w;
            }
        }
    }
    v->layout_rect[0] = L;
    v->layout_rect[1] = T;
    v->layout_rect[2] = R - L;
    v->layout_rect[3] = B - T;
}

/* The layout editor: the display and the whole picture (zoomed out when the picture runs past the display, what
 * falls off it faint), held still while a drag lasts. Drag the picture to move it, its edges and corners to size it
 * (snapping to the display's edges and middle; Shift: free, and keeping its shape). Undo / redo: Ctrl+Z / Ctrl+Y. */
static void layout_editor(SpView *v, const SpModel *m, const JVal *w, bool page) {
    static float area_w = 640;
    const char *id = json_str(json_get(json_get(m->look, "wallpaper"), "id"), "");
    const char *mode = json_str(json_get(m->layout, "layout"), "fill");
    const char *border = json_str(json_get(m->layout, "border"), "black");
    float now[4], r[4];
    layout_now(m, now);
    if (strcmp(v->layout_for, id)) {                           /* another wallpaper: its history starts here */
        snprintf(v->layout_for, sizeof v->layout_for, "%s", id);
        v->layout_hist_n = 0;
        v->layout_drag = 0;
        v->layout_hold = 0;
        layout_push(v, mode, now);
    }
    if (v->layout_drag) {
        memcpy(r, v->layout_rect, sizeof r);
    } else if (v->layout_hold > bare_time()) {
        float d = 0;
        for (int k = 0; k < 4; k++) d += fabsf(now[k] - v->layout_rect[k]);
        if (d < 0.004f) v->layout_hold = 0;
        memcpy(r, v->layout_hold ? v->layout_rect : now, sizeof r);
    } else {
        memcpy(r, now, sizeof r);
    }
    float dw = (float)json_num(json_get(m->layout, "displayW"), 1920), dh = (float)json_num(json_get(m->layout, "displayH"), 0);
    if (dh <= 0) dh = dw / (float)json_num(json_get(m->layout, "display"), 16.0 / 9.0);
    if (!v->layout_drag) {
        v->layout_view[0] = fmaxf(fminf(0.0f, r[0]), -3.0f);
        v->layout_view[1] = fmaxf(fminf(0.0f, r[1]), -3.0f);
        v->layout_view[2] = fminf(fmaxf(1.0f, r[0] + r[2]), 4.0f);
        v->layout_view[3] = fminf(fmaxf(1.0f, r[1] + r[3]), 4.0f);
    }
    const float *vb = v->layout_view, pad = 14;
    float AH = fminf(300.0f, area_w * dh / dw);
    float S = fminf((area_w - 2 * pad) / ((vb[2] - vb[0]) * dw), (AH - 2 * pad) / ((vb[3] - vb[1]) * dh));
    float cw = dw * S, ch = dh * S;
    float ox = (area_w - (vb[2] - vb[0]) * cw) * 0.5f - vb[0] * cw, oy = (AH - (vb[3] - vb[1]) * ch) * 0.5f - vb[1] * ch;
    BareRect pic = { ox + r[0] * cw, oy + r[1] * ch, r[2] * cw, r[3] * ch };
    BareImage img = wallpaper_picture(m, w, true);
    BareColor fill = !strcmp(border, "color") ? hex_color(json_str(json_get(m->layout, "color"), "#000000")) : BARE_RGB(0, 0, 0);
    static const int ops[3][3] = { { 5, 4, 6 }, { 1, 0, 2 }, { 9, 8, 10 } };
    BareState ps = { 0 }, hs[3][3] = { { { 0 } } };
    bare_column(.width = BARE_GROW, .pad = { 4, 22, 10, 22 }, .gap = 8) {
        bare_stack(.id = ID("lay.area"), .width = BARE_GROW, .height = AH, .radius = 14, .clip = true,
                   .color = sp.dark ? BARE_RGBA(0, 0, 0, 70) : BARE_RGBA(0, 0, 0, 18)) {
            BareState area = bare_this();
            if (area.rect.w > 0 && fabsf(area.rect.w - area_w) > 0.5f) { area_w = area.rect.w; bare_wake(); }
            /* what falls off the display, faint */
            bare_box(.position = BARE_ABSOLUTE, .x = pic.x, .y = pic.y, .width = pic.w, .height = pic.h, .image = img,
                     .opacity = 0.28f, .passthrough = true);
            /* the display: its border, then the picture */
            bare_stack(.position = BARE_ABSOLUTE, .x = ox, .y = oy, .width = cw, .height = ch, .clip = true, .color = fill,
                       .passthrough = true) {
                if (!page && (!strcmp(border, "blur") || !strcmp(border, "mirror") || !strcmp(border, "edges"))) {
                    float cover[4];
                    picture_rect("fill", dw / dh, (float)json_num(json_get(m->layout, "picture"), 16.0 / 9.0), cover);
                    bare_box(.position = BARE_ABSOLUTE, .x = cover[0] * cw, .y = cover[1] * ch, .width = cover[2] * cw,
                             .height = cover[3] * ch, .image = img, .opacity = !strcmp(border, "blur") ? 0.3f : 0.5f,
                             .passthrough = true);
                }
                bare_box(.position = BARE_ABSOLUTE, .x = pic.x - ox, .y = pic.y - oy, .width = pic.w, .height = pic.h,
                         .image = img, .color = img ? BARE_WHITE : sp.hair, .passthrough = true);
            }
            bare_box(.position = BARE_ABSOLUTE, .x = ox, .y = oy, .width = cw, .height = ch, .border = 1,
                     .border_color = BARE_RGBA(255, 255, 255, 140), .passthrough = true);
            /* the picture: drag to move */
            bool hot = v->layout_drag == 1;
            ps = bare_box(.id = ID("lay.pic"), .position = BARE_ABSOLUTE, .x = pic.x, .y = pic.y, .width = pic.w,
                          .height = pic.h, .border = 1.5f, .border_color = sp_alpha(sp.accent, hot ? 255 : 190),
                          .hover = { .border_color = sp.accent }, .cursor = BARE_CURSOR_MOVE);
            /* its edges, then its corners (on top) */
            float hx[3] = { pic.x, pic.x + pic.w * 0.5f, pic.x + pic.w }, hy[3] = { pic.y, pic.y + pic.h * 0.5f, pic.y + pic.h };
            for (int pass = 0; pass < 2; pass++)
                for (int j = 0; j < 3; j++)
                    for (int i = 0; i < 3; i++) {
                        int op = ops[j][i], corner = (op & 3) && (op & 12);
                        if (!op || corner != pass) continue;
                        BareId hid = IDI("lay.h", op);
                        if (corner) {
                            hs[j][i] = bare_box(.id = hid, .position = BARE_ABSOLUTE, .x = hx[i] - 6, .y = hy[j] - 6,
                                                .width = 12, .height = 12, .radius = 3, .color = BARE_WHITE, .border = 1.5f,
                                                .border_color = sp.accent, .hover = { .scale = 1.3f },
                                                .cursor = op == 5 || op == 10 ? BARE_CURSOR_RESIZE_NWSE : BARE_CURSOR_RESIZE_NESW);
                            continue;
                        }
                        bool side = (op & 3) != 0;                 /* left or right edge */
                        bare_stack(.id = hid, .position = BARE_ABSOLUTE,
                                              .x = side ? hx[i] - 5 : pic.x + 8, .y = side ? pic.y + 8 : hy[j] - 5,
                                              .width = side ? 10 : fmaxf(pic.w - 16, 1), .height = side ? fmaxf(pic.h - 16, 1) : 10,
                                              .align = BARE_CENTER, .justify = BARE_CENTER,
                                              .cursor = side ? BARE_CURSOR_RESIZE_EW : BARE_CURSOR_RESIZE_NS) {
                            hs[j][i] = bare_this();
                            if ((side ? pic.h : pic.w) > 40)
                                bare_box(.width = side ? 8.0f : 16.0f, .height = side ? 16.0f : 8.0f, .radius = 3, .color = BARE_WHITE,
                                         .border = 1.5f, .border_color = sp.accent, .passthrough = true);
                        }
                    }
        }
        /* the drag */
        int op = ps.pressed ? 0 : -1;
        for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) if (hs[j][i].pressed) op = ops[j][i];
        if (op >= 0 && !v->layout_drag) {
            v->layout_drag = op + 1;
            memcpy(v->layout_start, r, sizeof r);
            memcpy(v->layout_rect, r, sizeof r);
            v->layout_grab = bare_mouse();
        }
        if (op >= 0 && v->layout_drag == op + 1) {
            BareVec2 p = bare_mouse();
            float before[4];
            memcpy(before, v->layout_rect, sizeof before);
            layout_drag_by(v, op, (p.x - v->layout_grab.x) / cw, (p.y - v->layout_grab.y) / ch, 8 / cw, 8 / ch,
                           bare_key_down(BARE_KEY_SHIFT));
            if (memcmp(before, v->layout_rect, sizeof before) && bare_time() - v->layout_sent > 0.1) {
                layout_send(v, m, "custom", v->layout_rect);
                v->layout_sent = bare_time();
            }
        } else if (v->layout_drag) {
            if (memcmp(v->layout_start, v->layout_rect, sizeof v->layout_start)) {
                layout_send(v, m, "custom", v->layout_rect);
                layout_push(v, "custom", v->layout_rect);
                v->layout_hold = bare_time() + 1.5;
            }
            v->layout_drag = 0;
        }
        /* the size it comes out at, undo, redo, reset */
        bare_row(.width = BARE_GROW, .align = BARE_CENTER, .gap = 8) {
            char cap[96];
            snprintf(cap, sizeof cap, "%d \xC3\x97 %d on a %d \xC3\x97 %d display", (int)lroundf(r[2] * dw), (int)lroundf(r[3] * dh),
                     (int)lroundf(dw), (int)lroundf(dh));
            sp_label(cap, 15, sp.dim, BARE_REGULAR);
            bare_box(.width = BARE_GROW);
            if (sp_button(ID("lay.undo"), SPI_UNDO, NULL, SP_BTN_GLASS).clicked) layout_step(v, m, -1);
            if (sp_button(ID("lay.redo"), SPI_REDO, NULL, SP_BTN_GLASS).clicked) layout_step(v, m, 1);
            if (sp_button(ID("lay.reset"), SPI_REFRESH, "Reset", SP_BTN_GLASS).clicked) {
                float full[4];
                picture_rect("fill", dw / dh, (float)json_num(json_get(m->layout, "picture"), 16.0 / 9.0), full);
                layout_send(v, m, "fill", full);
                layout_push(v, "fill", full);
            }
        }
    }
    if (!bare_focused() && bare_key_down(BARE_KEY_CTRL)) {
        bool shift = bare_key_down(BARE_KEY_SHIFT);
        if (bare_key_pressed(BARE_KEY_Z)) layout_step(v, m, shift ? 1 : -1);
        else if (bare_key_pressed(BARE_KEY_Y)) layout_step(v, m, 1);
    }
}

static void customise_layout(SpView *v, const SpModel *m, const JVal *w) {
    static const char *modes[] = { "fill", "fit", "fit-width", "fit-height", "stretch", "original" };
    static const char *mode_names[] = { "Fill", "Fit", "Width", "Height", "Stretch", "Original" };
    static const char *borders[] = { "black", "blur", "mirror", "edges", "color" };
    static const char *border_names[] = { "Black", "Blur", "Mirror", "Edges", "Colour" };
    static const char *page_borders[] = { "black", "color" }, *page_border_names[] = { "Black", "Colour" };
    bool page = json_bool(json_get(m->layout, "page"), 0);
    const char *mode = json_str(json_get(m->layout, "layout"), "fill");
    const char *border = json_str(json_get(m->layout, "border"), "black");
    CARD(ID("cus.layout")) {
        if (!page) {
            int sel = -1;
            for (int i = 0; i < 6; i++) if (!strcmp(mode, modes[i])) sel = i;
            ROW(ID("cus.layout.row"), SPI_TILES, "Size", sel < 0 ? "Custom" : NULL) {
                int c = sp_choices(ID("cus.layout.c"), mode_names, 6, sel, true, 44);
                if (c >= 0 && c != sel) {
                    float rc[4];
                    picture_rect(modes[c], (float)json_num(json_get(m->layout, "display"), 16.0 / 9.0),
                                 (float)json_num(json_get(m->layout, "picture"), 16.0 / 9.0), rc);
                    layout_send(v, m, modes[c], rc);
                    layout_push(v, modes[c], rc);
                }
            }
        }
        layout_editor(v, m, w, page);
        divider();
        const char *const *bv = page ? page_borders : borders, *const *bn = page ? page_border_names : border_names;
        int nb = page ? 2 : 5, bsel = -1;
        for (int i = 0; i < nb; i++) if (!strcmp(border, bv[i])) bsel = i;
        ROW(ID("cus.border.row"), SPI_PICTURE, "Border", NULL) {
            int c = sp_choices(ID("cus.border.c"), bn, nb, bsel, true, 44);
            if (c >= 0 && c != bsel) CRUN("layout", "border", bv[c]);
        }
        if (!strcmp(border, "color")) {
            divider();
            bare_row(.width = BARE_GROW, .pad = { 14, 22, 14, 22 }, .gap = 18, .align = BARE_START) {
                bare_row(.width = 28, .justify = BARE_CENTER, .shrink = BARE_ZERO, .pad = { 6, 0, 0, 0 }) { sp_icon(SPI_DROPPER, 20, sp.ink); }
                bare_row(.width = BARE_GROW, .pad = { 6, 0, 0, 0 }) { sp_label("Border colour", 18, sp.ink, BARE_MEDIUM); }
                const char *hex = colour_control(v, ID("cus.bcolor"), json_str(json_get(m->layout, "color"), "#000000"), true);
                if (hex) CRUN("layout", "border", "color", hex);
            }
        }
    }
}

static void customise_upscale(SpView *v, const SpModel *m) {
    static const char *values[] = { "off", "smooth", "sharp" }, *names[] = { "Off", "Smooth", "Sharp" };
    static const char *img_values[] = { "off", "sharp" }, *img_names[] = { "Off", "Sharp" };
    bool smooth = json_bool(json_get(m->upscale, "smooth"), 1);
    const char *const *vals = smooth ? values : img_values, *const *labels = smooth ? names : img_names;
    int n = smooth ? 3 : 2, sel = -1;
    const char *cur = json_str(json_get(m->upscale, "upscale"), "off");
    for (int i = 0; i < n; i++) if (!strcmp(cur, vals[i])) sel = i;
    CARD(ID("cus.up")) {
        ROW(ID("cus.up.row"), SPI_FULL, "Upscaling", NULL) {
            int c = sp_choices(ID("cus.up.c"), labels, n, sel, true, 44);
            if (c >= 0 && c != sel) CRUN("upscale", vals[c]);
        }
    }
}

/* Make wallpaper cooler: interactive depth, and the models to make it with. */
static void customise_cool(SpView *v, const SpModel *m) {
    const JVal *c = m->cool;
    bool cool = json_bool(json_get(c, "cool"), 0), busy = json_bool(json_get(c, "busy"), 0);
    bool video = json_bool(json_get(c, "video"), 0);
    const char *text = json_str(json_get(c, "text"), "");
    int pct = (int)json_num(json_get(c, "percent"), -1);
    CARD(ID("cus.cool")) {
        ROW(ID("cus.cool.row"), SPI_LAYERS, cool ? "Interactive depth is on" : "Interactive depth", NULL) {
            const char *label = busy ? "Cancel" : cool ? (video ? "Use plain video" : "Use plain image") : "Make wallpaper cooler";
            if (sp_button(ID("cus.cool.go"), busy ? SPI_CLOSE : SPI_LAYERS, label, busy || cool ? SP_BTN_GLASS : SP_BTN_PRIMARY).clicked)
                CRUN("cool", busy ? "cancel" : cool ? "off" : "on");
        }
        if (busy) {
            bare_column(.width = BARE_GROW, .pad = { 0, 22, 16, 68 }, .gap = 8) {
                if (*text) sp_label(text, 15, sp.dim, BARE_REGULAR);
                bare_stack(.width = BARE_GROW, .height = 6, .radius = 3, .color = sp.dark ? BARE_RGBA(255, 255, 255, 40) : BARE_RGBA(0, 0, 0, 40)) {
                    BareState st = bare_this();
                    float wbar = st.rect.w > 0 ? st.rect.w : 400;
                    bare_box(.position = BARE_ABSOLUTE, .x = 0, .y = 0, .width = pct >= 0 ? wbar * pct / 100.0f : wbar * 0.3f,
                             .height = 6, .radius = 3, .color = sp.accent, .animate = sp_spring());
                }
            }
        } else {
            const char *note = json_bool(json_get(c, "failed"), 0) ? text
                             : json_bool(json_get(c, "otherBusy"), 0) ? "Busy with another wallpaper."
                             : cool && !json_bool(json_get(c, "mouse"), 1) ? "Mouse interaction is off in Settings." : "";
            if (*note) bare_row(.width = BARE_GROW, .pad = { 0, 22, 14, 68 }) { sp_label(note, 15, sp.dim, BARE_REGULAR); }
        }
        const JVal *ms = json_get(c, "models");
        const char *chosen = json_str(json_get(c, "model"), "");
        for (int i = 0; ms && ms->t == J_ARR && i < ms->n; i++) {
            const JVal *md = &ms->kids[i];
            bool off = json_bool(json_get(md, "off"), 0), ready = json_bool(json_get(md, "ready"), 0);
            bool on = !strcmp(json_str(json_get(md, "key"), ""), chosen);
            divider();
            BareState s = { 0 };
            bare_row(.id = IDI("cus.cool.m", i), .width = BARE_GROW, .height = 70, .pad = { 0, 22, 0, 22 }, .gap = 18,
                                   .align = BARE_CENTER, .cursor = off ? BARE_CURSOR_DEFAULT : BARE_CURSOR_HAND,
                                   .hover = { .color = off ? BARE_RGBA(0, 0, 0, 0) : sp.faint }, .opacity = off ? 0.45f : 1.0f) {
                s = bare_this();
                bare_row(.width = 28, .justify = BARE_CENTER, .shrink = BARE_ZERO) {
                    bare_box(.width = 16, .height = 16, .radius = 8, .color = on ? sp.accent : BARE_RGBA(0, 0, 0, 0),
                             .border = 2, .border_color = on ? sp.accent : sp.dim);
                }
                bare_column(.width = BARE_GROW, .gap = 3) {
                    char name[160];
                    snprintf(name, sizeof name, "%s%s", json_str(json_get(md, "name"), ""),
                             json_bool(json_get(md, "used"), 0) ? "  (in use)" : "");
                    sp_label(name, 17, sp.ink, BARE_MEDIUM);
                    bare_text(json_str(json_get(md, "desc"), ""), .size = 14, .color = sp.dim, .nowrap = true, .ellipsis = true);
                }
                bare_column(.width = 210, .gap = 3, .align = BARE_END, .shrink = BARE_ZERO) {
                    int level = (int)json_num(json_get(md, "level"), 0);
                    BareColor lc = level >= 4 ? BARE_RGB(63, 185, 80) : level == 3 ? BARE_RGB(212, 167, 44)
                                 : level == 2 ? BARE_RGB(232, 131, 58) : BARE_RGB(230, 60, 60);
                    bare_row(.gap = 4, .align = BARE_CENTER) {
                        for (int k = 1; k <= 5; k++)
                            bare_box(.width = 12, .height = 8, .radius = 2, .color = k <= level ? lc : sp_alpha(sp.dim, 60));
                    }
                    char st[64];
                    double mb = json_num(json_get(md, "size"), 0) / 1048576.0;
                    if (off) snprintf(st, sizeof st, "Pictures only");
                    else if (json_bool(json_get(md, "user"), 0)) snprintf(st, sizeof st, ready ? "Your file" : "Needs your .onnx");
                    else if (ready) snprintf(st, sizeof st, "%s \xC2\xB7 Downloaded", json_str(json_get(md, "rating"), ""));
                    else snprintf(st, sizeof st, "%s \xC2\xB7 %.0f MB", json_str(json_get(md, "rating"), ""), mb);
                    sp_label(st, 13.5f, sp.dim, BARE_REGULAR);
                }
            }
            if (s.clicked && !off && !busy) CRUN("cool", "on", json_str(json_get(md, "key"), ""));
        }
    }
}

static void customise_sound(SpView *v, const SpModel *m, const JVal *lw) {
    const JVal *slides = json_get(lw, "slideSeconds");
    CARD(ID("cus.sound")) {
        if (slides) {
            static const char *labels[] = { "3 s", "5 s", "10 s", "15 s", "30 s", "60 s" }, *vals[] = { "3", "5", "10", "15", "30", "60" };
            int cur = (int)json_num(slides, 10), sel = -1;
            for (int i = 0; i < 6; i++) if (atoi(vals[i]) == cur) sel = i;
            ROW(ID("cus.slides.row"), SPI_SLIDES, "Time per slide", NULL) {
                int c = sp_choices(ID("cus.slides.c"), labels, 6, sel, true, 44);
                if (c >= 0 && c != sel) RUN(m, "slides", json_str(json_get(lw, "id"), ""), vals[c]);
            }
        } else ROW(ID("cus.vol.row"), SPI_VOLUME, "Volume", NULL) {
            float now = (float)json_num(json_get(lw, "volume"), 100);
            played_value(m, REC_SEC_VOLUME, "volume", &now);
            live_value(v, m, ID("cus.vol.val"), "%d%%", 7, now, 0, 100, "volume", json_str(json_get(lw, "id"), ""), NULL, NULL);
            live_slider(v, m, ID("cus.vol"), 7, now, 0, 100, 380, "volume", json_str(json_get(lw, "id"), ""), NULL, NULL);
        }
    }
}

static void prop_text_field(SpView *v, const SpModel *m, int i, const char *name, const char *shown) {
    static const BareBox field = { .color = { 255, 255, 255, 0 }, .border = BARE_ZERO, .height = 26,
                                   .pad = { 1, 4, 1, 0 }, .cursor = BARE_CURSOR_TEXT };
    static char idle[64][256];                   /* fields not being edited show the saved text */
    bool editing = v->prop_editing == i + 1;
    char *buf = editing ? v->prop_text : idle[i & 63];
    int cap = editing ? (int)sizeof v->prop_text : (int)sizeof idle[0];
    if (!editing) snprintf(buf, (size_t)cap, "%s", shown);
    bare_row(.width = 340, .height = 46, .radius = 23, .pad = { 0, 16, 0, 16 }, .align = BARE_CENTER,
             .glass = sp_glass_control(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO) {
        BareState s = bare_input(buf, cap, .id = IDI("cus.prop.txt", i), .style = &field, .width = BARE_GROW, .size = 16,
                                 .color = sp.ink, .caret = sp.accent, .selection = sp_alpha(sp.accent, 90));
        if (!editing && (s.focused || s.changed)) {
            v->prop_editing = i + 1;
            snprintf(v->prop_text, sizeof v->prop_text, "%s", buf);
        } else if (editing && s.submitted) {
            CRUN("props", "set", name, v->prop_text);
            v->prop_editing = 0;
        } else if (editing && !s.focused) {
            v->prop_editing = 0;
        }
    }
}

static void customise_props(SpView *v, const SpModel *m, const JVal *list) {
    CARD(ID("cus.props")) {
        for (int i = 0; i < list->n; i++) {
            const JVal *p = &list->kids[i];
            const char *name = json_str(json_get(p, "name"), ""), *label = json_str(json_get(p, "label"), name);
            const char *type = json_str(json_get(p, "type"), ""), *shown = json_str(json_get(p, "display"), "");
            const JVal *value = json_get(p, "value");
            if (!*label) label = name;
            if (i) divider();
            if (!strcmp(type, "colour")) {
                bare_row(.id = IDI("cus.prop", i), .width = BARE_GROW, .pad = { 14, 22, 14, 22 }, .gap = 18, .align = BARE_START) {
                    bare_row(.width = 28, .justify = BARE_CENTER, .shrink = BARE_ZERO, .pad = { 6, 0, 0, 0 }) { sp_icon(SPI_PALETTE, 20, sp.ink); }
                    bare_row(.width = BARE_GROW, .pad = { 6, 0, 0, 0 }) { sp_label(label, 18, sp.ink, BARE_MEDIUM); }
                    const char *hex = colour_control(v, IDI("cus.prop.col", i), shown, true);
                    if (hex) CRUN("props", "set", name, hex);
                }
                continue;
            }
            ROW(IDI("cus.prop", i), SPI_SETTINGS, label, NULL) {
                if (!strcmp(type, "toggle")) {
                    bool on = json_bool(value, 0);
                    float pv;
                    if (played_value(m, REC_SEC_PROPS, name, &pv)) on = pv >= 0.5f;
                    if (sp_toggle(IDI("cus.prop.tog", i), on)) CRUN("props", "set", name, on ? "off" : "on");
                } else if (!strcmp(type, "slider")) {
                    float lo = (float)json_num(json_get(p, "min"), 0), hi = (float)json_num(json_get(p, "max"), 100);
                    float step = (float)json_num(json_get(p, "step"), 1);
                    float now = (float)json_num(value, lo);
                    played_value(m, REC_SEC_PROPS, name, &now);
                    float val = v->prop_drag == i + 1 ? v->prop_drag_value : now;
                    char t[32];
                    float typed;
                    if (step >= 1) snprintf(t, sizeof t, "%d", (int)lroundf(val));
                    else snprintf(t, sizeof t, "%.2f", val);
                    if (value_field(v, IDI("cus.prop.val", i), t, val, lo, hi, &typed)) {
                        char num[32];
                        snprintf(num, sizeof num, "%g", step >= 1 ? roundf(typed) : typed);
                        CRUN("props", "set", name, num);
                    }
                    int r = sp_slider(IDI("cus.prop.sl", i), &val, lo, hi, 340);
                    if (r) {
                        v->prop_drag = r == 1 ? i + 1 : 0;
                        v->prop_drag_value = val;
                        double tnow = bare_time(), every = recording_section(m) >= 0 ? 1.0 / 30.0 : 0.12;
                        if (r == 2 || tnow - v->look_sent > every) {
                            char num[32];
                            snprintf(num, sizeof num, "%g", val);
                            CRUN("props", "set", name, num);
                            v->look_sent = tnow;
                        }
                    }
                } else if (!strcmp(type, "choice")) {
                    const JVal *ch = json_get(p, "choices");
                    int n = ch && ch->t == J_ARR ? ch->n : 0, sel = -1;
                    for (int c = 0; c < n; c++) if (!strcmp(json_str(&ch->kids[c], ""), shown)) sel = c;
                    if (n > 0 && n <= 4) {
                        const char *labels[4];
                        for (int c = 0; c < n; c++) labels[c] = json_str(&ch->kids[c], "");
                        int c = sp_choices(IDI("cus.prop.c", i), labels, n, sel, true, 44);
                        if (c >= 0 && c != sel) CRUN("props", "set", name, labels[c]);
                    } else if (n > 0) {   /* many choices: step through them */
                        if (sel < 0) sel = 0;
                        if (sp_button(IDI("cus.prop.prev", i), NULL, "\xE2\x80\xB9", SP_BTN_GLASS).clicked)
                            CRUN("props", "set", name, json_str(&ch->kids[(sel + n - 1) % n], ""));
                        bare_row(.width = 220, .justify = BARE_CENTER) {
                            bare_text(shown, .size = 17, .weight = BARE_MEDIUM, .color = sp.ink, .nowrap = true, .ellipsis = true);
                        }
                        if (sp_button(IDI("cus.prop.next", i), NULL, "\xE2\x80\xBA", SP_BTN_GLASS).clicked)
                            CRUN("props", "set", name, json_str(&ch->kids[(sel + 1) % n], ""));
                    }
                } else if (!strcmp(type, "text")) {
                    prop_text_field(v, m, i, name, shown);
                } else {
                    bare_text(shown, .size = 16, .color = sp.dim, .nowrap = true, .ellipsis = true);
                }
            }
        }
    }
}

static void customise(SpView *v, const SpModel *m) {
    const JVal *w = active_wallpaper(m, v->target_display);
    const JVal *lw = json_get(m->look, "wallpaper");
    const JVal *plist = json_get(m->props, "properties");
    bool has_props = plist && plist->t == J_ARR && plist->n > 0;
    const char *type = json_str(json_get(lw, "type"), "");
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Customise", lw ? json_str(json_get(lw, "name"), "") : "Nothing on screen");
        bare_box(.width = BARE_GROW);
        /* which display's wallpaper */
        const JVal *ds = json_get(m->status, "displays");
        int n = ds && ds->t == J_ARR ? ds->n : 0;
        if (n > 1) {
            const char *labels[9];
            static char names[8][16];
            int sel = 0;
            labels[0] = "All";
            for (int i = 0; i < n && i < 8; i++) {
                int idx = (int)json_num(json_get(&ds->kids[i], "index"), i + 1);
                snprintf(names[i], 16, "Display %d", idx);
                labels[i + 1] = names[i];
                if (idx == v->target_display) sel = i + 1;
            }
            int c = sp_choices(ID("cus.displays"), labels, n < 8 ? n + 1 : 9, sel, true, 44);
            if (c == 0) v->target_display = 0;
            else if (c > 0) v->target_display = (int)json_num(json_get(&ds->kids[c - 1], "index"), c);
        }
        if (m->layout && sp_button(ID("cus.layout.reset"), SPI_TILES, "Reset layout", SP_BTN_GLASS).clicked)
            CRUN("layout", "reset");
        if (m->look && strcmp(type, "app") && sp_button(ID("cus.reset"), SPI_REFRESH, "Reset look", SP_BTN_GLASS).clicked)
            CRUN("look", "reset");
    }
    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 14, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 24, 6 }) {
        if (!m->look) {
            sp_label("Apply a wallpaper to customise it.", 17, sp.dim, BARE_REGULAR);
        } else {
            bare_row(.width = BARE_GROW, .radius = 32, .padding = 18, .gap = 26, .glass = sp_glass_card(),
                     .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO, .align = BARE_CENTER) {
                bare_box(.width = 320, .height = 180, .radius = 22, .image = wallpaper_picture(m, w, true),
                         .color = BARE_RGBA(0, 0, 0, 80), .shrink = BARE_ZERO, .shadow = { 0, 8, 26, 0, BARE_RGBA(0, 0, 0, 90) });
                bare_column(.width = BARE_GROW, .gap = 8) {
                    bare_text(json_str(json_get(lw, "name"), ""), .size = 22, .weight = BARE_SEMIBOLD, .color = sp.ink,
                              .ellipsis = true, .nowrap = true);
                    char line[80];
                    snprintf(line, sizeof line, "%s%s", !strcmp(type, "html") ? "Web page" : !strcmp(type, "video") ? "Video" : !strcmp(type, "app") ? "Program" : "Picture",
                             v->target_display ? "  \xC2\xB7  this display" : "  \xC2\xB7  all displays");
                    sp_label(line, 15, sp.dim, BARE_REGULAR);
                }
            }
            if (m->cool && json_bool(json_get(m->cool, "can"), 0)) {
                section("Make wallpaper cooler");
                customise_cool(v, m);
            }
            if (strcmp(type, "app")) {   /* a program draws its own picture: no grading or layout */
                rec_bar(v, m, REC_SEC_LOOK, "Colour grading");
                rec_graphs(v, m, REC_SEC_LOOK);
                customise_look(v, m);
                if (m->layout) { section("Layout"); customise_layout(v, m, w); }
            }
            if (m->upscale) { section("Upscaling"); customise_upscale(v, m); }
            if (strcmp(type, "image")) {
                if (json_get(lw, "slideSeconds")) section("Slides");
                else { rec_bar(v, m, REC_SEC_VOLUME, "Sound"); rec_graphs(v, m, REC_SEC_VOLUME); }
                customise_sound(v, m, lw);
            }
            if (has_props) {
                rec_bar(v, m, REC_SEC_PROPS, "Wallpaper settings");
                rec_graphs(v, m, REC_SEC_PROPS);
                customise_props(v, m, plist);
                bare_row(.width = BARE_GROW, .justify = BARE_END) {
                    if (sp_button(ID("cus.props.reset"), SPI_REFRESH, "Reset wallpaper settings", SP_BTN_PLAIN).clicked)
                        CRUN("props", "reset");
                }
            }
        }
    }
}

/* ---- Store ------------------------------------------------------------------------------- */

static void store_page(SpView *v, const SpModel *m) {
    const JVal *items = json_get(m->store, "items");
    int n = items && items->t == J_ARR ? items->n : 0;
    bool loading = json_bool(json_get(m->store, "loading"), 0);
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Store", loading ? "Loading\xE2\x80\xA6" : "Wallpapers from MotionBGS, Lively, MoeWalls, Wallhaven and more");
        bare_box(.width = BARE_GROW);
        search_field(ID("store.search"), v->store_search, sizeof v->store_search, "Search the Store", 320);
        if (sp_button(ID("store.go"), SPI_SEARCH, "Search", SP_BTN_PRIMARY).clicked)
            RUN(m, "store", "search", v->store_search);
    }
    const char *err = json_str(json_get(m->store, "error"), "");
    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 20, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 24, 6 }) {
        if (*err) sp_label(err, 16, BARE_RGB(255, 120, 120), BARE_REGULAR);
        BareVec2 win = bare_window_size();
        float avail = win.x - 272 - 18 * 3 - 30 * 2 - 12;
        int cols = avail > 1300 ? 4 : avail > 760 ? 3 : 2;
        float cw = (avail - (cols - 1) * 20) / cols;
        for (int i = 0; i < n;) {
            bare_row(.id = IDI("store.row", i), .width = BARE_GROW, .gap = 20, .shrink = BARE_ZERO) {
                for (int c = 0; c < cols && i < n; c++, i++) {
                    const JVal *it = &items->kids[i];
                    int number = (int)json_num(json_get(it, "number"), i + 1);
                    int dl = (int)json_num(json_get(it, "download"), 0), pct = (int)json_num(json_get(it, "percent"), -1);
                    const char *preview = json_str(json_get(it, "preview"), "");
                    bare_column(.id = IDI("store.card", i), .width = cw, .radius = 26, .padding = 10, .gap = 6,
                                .glass = sp_glass_card(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO,
                                .hover = { .scale = 1.0f + 0.02f * sp.motion }, .animate = sp_spring()) {
                        float ph = (cw - 20) * 9.0f / 16.0f;
                        bool failed;
                        BareImage pic = picture_file(m, preview, false, &failed);
                        bare_stack(.width = BARE_GROW, .height = ph, .radius = 18, .clip = true) {
                            bare_box(.width = BARE_GROW, .height = ph, .radius = 18, .image = pic,
                                     .color = sp.dark ? BARE_RGBA(255, 255, 255, 14) : BARE_RGBA(0, 0, 0, 16),
                                     .animate = sp_fade());
                            const char *badge = json_str(json_get(it, "badge"), "");
                            const char *kind = json_str(json_get(it, "kind"), "");
                            bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_TOP_LEFT, .x = 10, .y = 10, .height = 28,
                                     .pad = { 0, 11, 0, 11 }, .gap = 6, .align = BARE_CENTER, .radius = 14,
                                     .glass = { .refraction = 0.9f, .depth = 9, .blur = 6, .tint = BARE_RGBA(10, 16, 18, 120) }) {
                                sp_icon(!strcmp(kind, "live") ? SPI_FILM : SPI_PICTURE, 12, BARE_WHITE);
                                bare_text(*badge ? badge : (!strcmp(kind, "live") ? "Live" : "Still"), .size = 13,
                                          .weight = BARE_MEDIUM, .color = BARE_WHITE, .nowrap = true);
                            }
                        }
                        bare_row(.pad = { 6, 8, 4, 8 }, .gap = 10, .align = BARE_CENTER, .width = BARE_GROW) {
                            bare_column(.width = BARE_GROW, .gap = 2) {
                                bare_text(json_str(json_get(it, "title"), ""), .size = 17, .weight = BARE_SEMIBOLD,
                                          .color = sp.ink, .nowrap = true, .ellipsis = true);
                                sp_label(json_str(json_get(it, "source"), ""), 14, sp.dim, BARE_REGULAR);
                            }
                            char label[24];
                            const char *l = "Get";
                            if (dl == 2) { snprintf(label, sizeof label, pct >= 0 ? "%d%%" : "\xE2\x80\xA6", pct); l = label; }
                            else if (dl == 3) l = "Added";
                            else if (dl == 4) l = "Retry";
                            if (sp_button(IDI("store.get", i), dl == 3 ? SPI_CHECK : SPI_DOWNLOAD, l,
                                          dl == 3 ? SP_BTN_GLASS : SP_BTN_PRIMARY).clicked && dl != 3) {
                                char num[16];
                                snprintf(num, sizeof num, "%d", number);
                                RUN(m, "store", "get", num);
                            }
                        }
                    }
                }
            }
        }
        if (!n && !loading && !*err) sp_label("Nothing here yet.", 17, sp.dim, BARE_REGULAR);
        if (n && sp_button(ID("store.more"), SPI_ADD, "More", SP_BTN_GLASS).clicked) RUN(m, "store", "more");
    }
}

/* ---- Settings ------------------------------------------------------------------------------ */

typedef struct SettingRow { const char *section, *key, *title, *icon; int kind; const char *opts; } SettingRow;
enum { K_TOGGLE, K_CHOICE, K_NUMBER };

static const SettingRow rows[] = {
#ifdef _WIN32
    { "General", "start_with_windows", "Start with Windows", SPI_TILES, K_TOGGLE, NULL },
    { "General", "launch_to_tray", "Start minimised to the tray", SPI_DOWNLOAD, K_TOGGLE, NULL },
    { "General", "still_wallpaper", "Show a still while Screen+ starts", SPI_PICTURE, K_TOGGLE, NULL },
#else
    { "General", "start_with_linux", "Start when you sign in", SPI_TILES, K_TOGGLE, NULL },
    { "General", "launch_to_tray", "Keep the window closed at sign-in", SPI_DOWNLOAD, K_TOGGLE, NULL },
#endif
    { "General", "bare_ui", "Bare window (beta)", SPI_PALETTE, K_TOGGLE, NULL },
    { "General", "ui_sounds", "Sounds", SPI_VOLUME, K_TOGGLE, NULL },
    { "Playback", "pause_on_fullscreen", "Pause behind fullscreen apps and games", SPI_FULL, K_TOGGLE, NULL },
    { "Playback", "pause_when_covered", "Stop rendering while windows cover the desktop", SPI_EYE, K_TOGGLE, NULL },
    { "Playback", "suspend_on_lock", "Suspend while the PC is locked", SPI_LOCK, K_TOGGLE, NULL },
    { "Playback", "pause_on_battery", "Pause on battery or energy saver", SPI_BATTERY, K_TOGGLE, NULL },
    { "Playback", "auto_recover", "Restart failed renderers", SPI_HEAL, K_TOGGLE, NULL },
    { "Sound", "audio", "Play wallpaper sound", SPI_VOLUME, K_TOGGLE, NULL },
    { "Sound", "volume", "Volume", SPI_AUDIO, K_NUMBER, "0 100" },
    { "Web wallpapers", "html_mouse", "Mouse input", SPI_MOUSE, K_TOGGLE, NULL },
    { "Web wallpapers", "html_audio", "System audio spectrum", SPI_AUDIO, K_TOGGLE, NULL },
    { "Web wallpapers", "web_fps", "Frame-rate limit", SPI_SPEED, K_CHOICE, "30|60|off" },
    { "Slideshow", "slideshow", "Change the wallpaper by itself", SPI_SLIDES, K_TOGGLE, NULL },
    { "Slideshow", "slideshow_minutes", "Minutes between changes", SPI_CLOCK, K_NUMBER, "1 120" },
    { "Slideshow", "shuffle", "Shuffle", SPI_SHUFFLE, K_TOGGLE, NULL },
    { "Slideshow", "schedule", "Day and night wallpapers", SPI_CLOCK, K_TOGGLE, NULL },
    { "Library", "lively_auto_import", "Add new Lively packages at launch", SPI_FOLDER, K_TOGGLE, NULL },
    { "Library", "url_import", "Import from web links", SPI_LINK, K_TOGGLE, NULL },
    { "Library", "interactive_images", "Pictures follow the mouse (depth)", SPI_MOUSE, K_TOGGLE, NULL },
#ifdef _WIN32
    { "Screensaver", "screensaver_audio", "Screensaver sound", SPI_VOLUME, K_TOGGLE, NULL },
    { "Screensaver", "screensaver_displays", "Screensaver displays", SPI_DISPLAY, K_CHOICE, "all|span|primary" },
    { "Screensaver", "screensaver_exit_on_mouse", "A mouse move ends the screensaver", SPI_MOUSE, K_TOGGLE, NULL },
#endif
};

static bool want(const char *search, const char *title, const char *section) {
    return !search[0] || contains(title, search) || contains(section, search);
}

static void choice_row(const SpModel *m, BareId id, const char *key, const char *const *labels, const char *const *values,
                       int n, float h) {
    const char *cur = sp_setting(m, key, values[0]);
    int sel = 0;
    for (int i = 0; i < n; i++) if (!strcmp(cur, values[i])) sel = i;
    int c = sp_choices(id, labels, n, sel, true, h);
    if (c >= 0 && c != sel) RUN(m, "settings", "set", key, values[c]);
}

static void setting_control(const SpModel *m, const SettingRow *r, int i) {
    if (r->kind == K_TOGGLE) {
        bool on = setting_on(m, r->key);
        if (sp_toggle(IDI("set.tog", i), on)) RUN(m, "settings", "set", r->key, on ? "off" : "on");
    } else if (r->kind == K_CHOICE) {
        char buf[64];
        const char *parts[6] = { 0 };
        int n = 0;
        snprintf(buf, sizeof buf, "%s", r->opts);
        for (char *p = buf; *p && n < 6;) {
            parts[n++] = p;
            char *bar = strchr(p, '|');
            if (!bar) break;
            *bar = 0;
            p = bar + 1;
        }
        const char *labels[6];
        for (int k = 0; k < n; k++) labels[k] = !strcmp(parts[k], "off") ? "Off" : !strcmp(parts[k], "all") ? "All"
                                              : !strcmp(parts[k], "span") ? "Span" : !strcmp(parts[k], "primary") ? "Primary" : parts[k];
        choice_row(m, IDI("set.choice", i), r->key, labels, parts, n, 46);
    } else {
        int lo = 0, hi = 100;
        lo = atoi(r->opts);
        hi = atoi(strchr(r->opts, ' ') + 1);
        static float drag[64];
        static int dragging = -1;
        float val = dragging == i ? drag[i] : (float)setting_num(m, r->key, lo);
        char t[16];
        snprintf(t, sizeof t, "%d", (int)lroundf(val));
        bare_row(.width = 50, .justify = BARE_END) { sp_label(t, 17, sp.ink, BARE_MEDIUM); }
        int res = sp_slider(IDI("set.slider", i), &val, (float)lo, (float)hi, 300);
        if (res == 1) { dragging = i; drag[i] = val; }
        if (res == 2) {
            dragging = -1;
            char num[16];
            snprintf(num, sizeof num, "%d", (int)lroundf(val));
            RUN(m, "settings", "set", r->key, num);
        }
    }
}


static void settings(SpView *v, const SpModel *m) {
    static char search[96];
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Settings", NULL);
        bare_box(.width = BARE_GROW);
        search_field(ID("set.search"), search, sizeof search, "Search settings", 380);
    }
    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 14, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 30, 6 }) {
        int count = (int)(sizeof rows / sizeof rows[0]);
        /* The General group first, as one card. */
        bool any = false;
        for (int i = 0; i < count; i++) any |= !strcmp(rows[i].section, "General") && want(search, rows[i].title, "General");
        if (any) {
            section("General");
            CARD(ID("set.general")) {
                bool first = true;
                for (int i = 0; i < count; i++) {
                    if (strcmp(rows[i].section, "General") || !want(search, rows[i].title, "General") ||
                        !setting_listed(m, rows[i].key))
                        continue;
                    if (!first) divider();
                    first = false;
                    ROW(IDI("set.row", i), rows[i].icon, rows[i].title, NULL) { setting_control(m, &rows[i], i); }
                }
            }
        }
        if (setting_listed(m, "auto_update") && want(search, "Auto update", "General")) CARD(ID("set.update")) {
            ROW(ID("set.update.row"), SPI_SYNC, "Auto update", NULL) {
                if (sp_button(ID("set.update.check"), SPI_REFRESH, "Check now", SP_BTN_GLASS).clicked) RUN(m, "update", "check");
                bool on = setting_on(m, "auto_update");
                if (sp_toggle(ID("set.update.tog"), on)) RUN(m, "settings", "set", "auto_update", on ? "off" : "on");
            }
        }
        if (setting_listed(m, "auto_update_samples") && want(search, "Sample wallpaper updates", "General"))
            CARD(ID("set.samples")) {
            ROW(ID("set.samples.row"), SPI_LAYERS, "Sample wallpaper updates", NULL) {
                if (sp_button(ID("set.samples.check"), SPI_REFRESH, "Check now", SP_BTN_GLASS).clicked) RUN(m, "samples", "check");
                bool on = setting_on(m, "auto_update_samples");
                if (sp_toggle(ID("set.samples.tog"), on)) RUN(m, "settings", "set", "auto_update_samples", on ? "off" : "on");
            }
        }
        if (want(search, "Appearance theme", "General")) CARD(ID("set.theme")) {
            ROW(ID("set.theme.row"), SPI_BRUSH, "Appearance", NULL) {
                static const char *labels[] = { "System", "Light", "Dark" }, *values[] = { "system", "light", "dark" };
                choice_row(m, ID("set.theme.c"), "theme", labels, values, 3, 50);
            }
        }
        if (want(search, "Animations motion selection glide pour", "General")) CARD(ID("set.motion")) {
            ROW(ID("set.motion.row"), SPI_FILM, "Animations", NULL) {
                static const char *labels[] = { "Full", "Reduced", "Off" }, *values[] = { "full", "reduced", "off" };
                choice_row(m, ID("set.motion.c"), "ui_motion", labels, values, 3, 50);
            }
            divider();
            ROW(ID("set.pour.row"), SPI_FILM, "Selection", NULL) {
                static const char *labels[] = { "Glide", "Pour" };
                bool on = setting_on(m, "ui_pour");
                int c = sp_choices(ID("set.pour.c"), labels, 2, on, true, 50);
                if (c >= 0 && c != (int)on) RUN(m, "settings", "set", "ui_pour", c ? "on" : "off");
            }
        }
        if (want(search, "Accent colour color", "General")) CARD(ID("set.accent")) {
            ROW(ID("set.accent.row"), SPI_DROPPER, "Accent colour", NULL) {
                bare_row(.gap = 10, .align = BARE_CENTER) {
                    for (int i = 0; i < 8; i++) {
                        BareColor c = accents[i];
                        bool on = c.r == sp.accent.r && c.g == sp.accent.g && c.b == sp.accent.b;
                        BareState s = bare_box(.id = IDI("set.accent.c", i), .width = 30, .height = 30, .radius = 15, .color = c,
                                 .border = on ? 3.0f : 1.0f, .border_color = on ? BARE_WHITE : BARE_RGBA(255, 255, 255, 80),
                                 .shadow = on ? (BareShadow){ 0, 0, 14, 0, sp_alpha(c, 180) } : (BareShadow){ 0 },
                                 .hover = { .scale = 1.18f }, .press = { .scale = 0.9f }, .cursor = BARE_CURSOR_HAND,
                                 .animate = sp_spring());
                        if (s.clicked && !on) {
                            char hex[16];
                            snprintf(hex, sizeof hex, "#%02X%02X%02X", c.r, c.g, c.b);
                            RUN(m, "settings", "set", "accent_color", hex);
                        }
                    }
                }
                char hex[16];
                snprintf(hex, sizeof hex, "#%02X%02X%02X", sp.accent.r, sp.accent.g, sp.accent.b);
                sp_label(hex, 16, sp.dim, BARE_MEDIUM);
            }
        }
        const char *last = "General";
        bool open = false;
        BareId card = 0;
        (void)card;
        for (int i = 0; i < count; i++) {
            const SettingRow *r = &rows[i];
            if (!strcmp(r->section, "General") || !want(search, r->title, r->section) || !setting_listed(m, r->key)) continue;
            if (strcmp(last, r->section)) {
                if (open) bare_close_();
                bare_box(.height = 8);
                section(r->section);
                bare_open_(&(BareBox){ .id = IDI("set.card", i), .dir = BARE_COLUMN, .width = BARE_GROW, .radius = 26,
                                       .glass = sp_glass_card(), .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO,
                                       .shadow = { 0, 6, 24, 0, BARE_RGBA(0, 0, 0, sp.dark ? 50 : 22) } }, __FILE__, __LINE__);
                open = true;
                last = r->section;
            } else {
                divider();
            }
            ROW(IDI("set.row", i), r->icon, r->title, NULL) { setting_control(m, r, i); }
        }
        if (open) bare_close_();
        if (want(search, "About version classic window", "About")) {
            bare_box(.height = 8);
            section("About");
            CARD(ID("set.about")) {
                char ver[64];
                snprintf(ver, sizeof ver, "Screen+ %s  \xC2\xB7  Bare UI beta", json_str(json_get(m->status, "version"), ""));
                ROW(ID("set.about.row"), SPI_INFO, ver, NULL) {
                    if (sp_button(ID("set.classic"), SPI_OPEN, "Classic window", SP_BTN_GLASS).clicked) RUN(m, "ui", "classic");
                }
            }
        }
    }
    (void)v;
}

/* ---- popovers and toasts ---------------------------------------------------------------------- */

static void popovers(SpView *v, const SpModel *m) {
    if (!v->popover) return;
    BareVec2 win = bare_window_size();
    /* a click outside closes it */
    BareState scrim = bare_box(.position = BARE_ABSOLUTE, .width = win.x, .height = win.y, .id = ID("pop.scrim2"));
    if (scrim.clicked) { v->popover = 0; return; }
    if (v->popover == 1) {
        bare_column(.id = ID("pop.link"), .position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = -180, .y = 140,
                    .width = 520, .padding = 20, .gap = 14, .radius = 28, .glass = sp_glass_panel(),
                    .border = 1, .border_color = sp.hair, .shadow = { 0, 20, 50, 0, BARE_RGBA(0, 0, 0, 110) }) {
            sp_label("Add from a link", 20, sp.ink, BARE_SEMIBOLD);
            sp_label("A video page, a direct file or a web page", 15, sp.dim, BARE_REGULAR);
            bool go = false;
            bare_row(.gap = 10, .align = BARE_CENTER, .width = BARE_GROW) {
                static const BareBox field = { .color = { 255, 255, 255, 0 }, .border = BARE_ZERO, .height = 26,
                                               .pad = { 1, 4, 1, 0 } };
                bare_row(.width = BARE_GROW, .height = 50, .radius = 25, .pad = { 0, 16, 0, 18 }, .align = BARE_CENTER,
                         .glass = sp_glass_control(), .border = 1, .border_color = sp.hair) {
                    BareState s = bare_input(v->link, sizeof v->link, .id = ID("pop.link.in"), .style = &field,
                                             .width = BARE_GROW, .placeholder = "https://", .size = 16, .color = sp.ink,
                                             .caret = sp.accent);
                    go = s.submitted;
                }
                go |= sp_button(ID("pop.link.go"), SPI_ADD, "Add", SP_BTN_PRIMARY).clicked;
            }
            if (go && v->link[0]) { RUN(m, "import", v->link); v->link[0] = 0; v->popover = 0; }
        }
    } else if (v->popover == 2) {
        static const char *sorts[] = { "custom", "name", "added", "type", "size" };
        static const char *names[] = { "Custom order", "Name", "Recently added", "Type", "Size" };
        bare_column(.id = ID("pop.sort"), .position = BARE_ABSOLUTE, .x = v->popover_at.x - 200, .y = v->popover_at.y + 24,
                    .width = 240, .padding = 8, .gap = 2, .radius = 22, .glass = sp_glass_panel(),
                    .border = 1, .border_color = sp.hair, .shadow = { 0, 18, 44, 0, BARE_RGBA(0, 0, 0, 110) }) {
            const char *cur = sp_setting(m, "library_sort", "custom");
            for (int i = 0; i < 5; i++) {
                bool on = !strcmp(cur, sorts[i]);
                bare_row(.id = IDI("pop.sort.i", i), .height = 44, .width = BARE_GROW, .pad = { 0, 14, 0, 14 }, .gap = 10,
                         .align = BARE_CENTER, .radius = 14, .cursor = BARE_CURSOR_HAND,
                         .color = on ? sp.accent_soft : BARE_RGBA(0, 0, 0, 0), .hover = { .color = sp.faint }) {
                    if (bare_this().clicked) { RUN(m, "settings", "set", "library_sort", sorts[i]); v->popover = 0; }
                    bare_row(.width = 18) { if (on) sp_icon(SPI_CHECK, 13, sp.ink); }
                    sp_label(names[i], 16, sp.ink, BARE_MEDIUM);
                }
            }
        }
    } else if (v->popover == 3 && m->list && v->menu_card >= 0 && v->menu_card < m->list->n) {
        const JVal *w = json_get(&m->list->kids[v->menu_card], "wallpaper");
        const char *id = json_str(json_get(w, "id"), "");
        bool fav = json_bool(json_get(w, "favourite"), 0);
        const JVal *ds = json_get(m->status, "displays");
        int nd = ds && ds->t == J_ARR ? ds->n : 0;
        float x = v->popover_at.x + 270 > win.x ? win.x - 280 : v->popover_at.x;
        float y = v->popover_at.y + 60 + 46 * (nd + 3) > win.y ? win.y - 70 - 46 * (nd + 3) : v->popover_at.y;
        bare_column(.id = ID("pop.card"), .position = BARE_ABSOLUTE, .x = x, .y = y, .width = 270, .padding = 8, .gap = 2,
                    .radius = 22, .glass = sp_glass_panel(), .border = 1, .border_color = sp.hair,
                    .shadow = { 0, 18, 44, 0, BARE_RGBA(0, 0, 0, 110) }) {
            int act = -1;
            const char *labels[12];
            const char *icons[12];
            static char disp[8][64];
            int n = 0;
            labels[n] = "Apply to every display"; icons[n++] = SPI_DISPLAY;
            for (int i = 0; i < nd && i < 8; i++) {
                snprintf(disp[i], 64, "Apply to display %d", (int)json_num(json_get(&ds->kids[i], "index"), i + 1));
                labels[n] = disp[i]; icons[n++] = SPI_DISPLAY;
            }
            labels[n] = fav ? "Remove from Favourites" : "Add to Favourites"; icons[n++] = fav ? SPI_STAR_LINE : SPI_STAR;
            labels[n] = "Show in folder"; icons[n++] = SPI_FOLDER;
            labels[n] = "Remove from library"; icons[n++] = SPI_DELETE;
            for (int i = 0; i < n; i++) {
                bare_row(.id = IDI("pop.card.i", i), .height = 44, .width = BARE_GROW, .pad = { 0, 14, 0, 14 }, .gap = 12,
                         .align = BARE_CENTER, .radius = 14, .cursor = BARE_CURSOR_HAND, .hover = { .color = sp.faint }) {
                    if (bare_this().clicked) act = i;
                    sp_icon(icons[i], 15, i == n - 1 ? BARE_RGB(255, 110, 110) : sp.ink);
                    sp_label(labels[i], 16, i == n - 1 ? BARE_RGB(255, 110, 110) : sp.ink, BARE_MEDIUM);
                }
            }
            if (act >= 0) {
                v->popover = 0;
                if (act == 0) RUN(m, "apply", id);
                else if (act <= nd) {
                    char num[8];
                    snprintf(num, sizeof num, "%d", (int)json_num(json_get(&ds->kids[act - 1], "index"), act));
                    RUN(m, "apply", id, "--display", num);
                } else if (act == nd + 1) RUN(m, "favourite", id, fav ? "off" : "on");
                else if (act == nd + 2) { if (m->open_path) m->open_path(m->user, json_str(json_get(w, "path"), "")); }
                else RUN(m, "remove", id);
            }
        }
    }
    if (bare_key_pressed(BARE_KEY_ESCAPE)) v->popover = 0;
}

static void toast(const SpModel *m) {
    double age = bare_time() - m->message_time;
    bool show = m->message && *m->message && age < 3.5;
    if (show) bare_wake_in((float)(3.5 - age) + 0.05f);
    /* The capsule condenses out of the glass and dissolves back into it. */
    bare_row(.id = ID("toast"), .position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM, .y = show ? -34.0f : -10.0f,
             .height = 54, .pad = { 0, 26, 0, 22 }, .gap = 12, .align = BARE_CENTER, .radius = 27,
             .opacity = show ? 1.0f : BARE_ZERO, .passthrough = true,
             .glass = show ? (BareGlass){ .refraction = 1.0f, .depth = 16, .blur = 8, .shine = 1.3f,
                                          .tint = m->message_failed ? BARE_RGBA(120, 20, 28, 170) : sp.panel_tint }
                           : (BareGlass){ 0 },
             .border = 1, .border_color = sp.hair, .shadow = { 0, 14, 36, 0, BARE_RGBA(0, 0, 0, show ? 90 : 0) },
             .animate = sp_liquid()) {
        sp_icon(m->message_failed ? SPI_INFO : SPI_CHECK, 15, m->message_failed ? BARE_RGB(255, 140, 140) : sp.accent);
        sp_label(m->message ? m->message : "", 16, sp.ink, BARE_MEDIUM);
    }
}

/* ---- the window --------------------------------------------------------------------------------- */

/* Settings > Bare window switched off (here or anywhere): the service opens the classic window; this one goes. */
static void follow_window_setting(const SpModel *m) {
    static int was = -1;
    if (!m->settings || !json_get(m->settings, "bare_ui")) return;
    int on = setting_on(m, "bare_ui");
    if (was == 1 && !on) bare_window_close();
    was = on;
}

void sp_draw(SpView *v, const SpModel *m) {
    if (!v || !m) return;
    follow_window_setting(m);
    sp_sounds = !json_get(m->settings, "ui_sounds") || setting_on(m, "ui_sounds");
    BareVec2 win = bare_window_size();
    BareColor base = sp.dark ? BARE_RGB(14, 22, 24) : BARE_RGB(214, 224, 222);
    if (m->see_through) base = BARE_RGBA(0, 0, 0, 0);
    bare_stack(.id = ID("root"), .width = win.x, .height = win.y, .image = m->backdrop, .color = base,
               .text_color = sp.ink) {
        if (!m->backdrop && !m->see_through)   /* no live desktop: a soft field so the glass still has something to bend */
            bare_box(.width = win.x, .height = win.y, .passthrough = true,
                     .color = sp.dark ? BARE_RGBA(20, 40, 38, 255) : BARE_RGBA(225, 235, 232, 255));
        bare_column(.width = win.x, .height = win.y, .pad = { 0, 18, 18, 18 }) {
            title_bar();
            bare_row(.width = BARE_GROW, .height = BARE_GROW, .gap = 18) {
                sidebar(v, m);
                float k = sp_enter(bare_time() - v->page_time);
                bare_column(.id = ID("page"), .width = BARE_GROW, .height = BARE_GROW, .radius = 34,
                            .pad = { 22, 28, 0, 28 }, .gap = 14, .glass = sp_glass_panel(), .border = 1,
                            .border_color = sp.hair, .clip = true,
                            .shadow = { 0, 18, 48, -6, BARE_RGBA(0, 0, 0, sp.dark ? 90 : 40) }) {
                    bare_column(.id = IDI("page.body", v->page), .width = BARE_GROW, .height = BARE_GROW, .gap = 14,
                                .opacity = 0.25f + 0.75f * k, .offset = { 0, (1 - k) * 26 }) {
                        switch (v->page) {
                        case SP_LIBRARY: library(v, m); break;
                        case SP_DISPLAYS: displays(v, m); break;
                        case SP_CUSTOMISE: customise(v, m); break;
                        case SP_STORE: store_page(v, m); break;
                        default: settings(v, m); break;
                        }
                    }
                }
            }
        }
        popovers(v, m);
        toast(m);
    }
}
