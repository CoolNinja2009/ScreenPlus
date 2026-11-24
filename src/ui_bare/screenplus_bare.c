/* screenplus_bare.c: the Screen+ window on Bare liquid glass.
 *
 * The window is frameless and see-through: its background is the live
 * desktop behind it (bare_window_backdrop), and the sidebar, the page and
 * every control are glass that refracts it. Selections are glass drops that
 * glide (sp_kit.c). Data is the service's JSON; actions are service commands. */
#include "screenplus_bare.h"
#include "sp_kit.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return !strcmp(type, "html") ? "HTML" : !strcmp(type, "video") ? "Video" : !strcmp(type, "image") ? "Image" : type;
}

static const char *file_name(const char *path) {
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

/* The picture for a wallpaper: its thumbnail, an image wallpaper's own file. */
static BareImage wallpaper_picture(const SpModel *m, const JVal *w) {
    if (!w || !m->thumbnail) return 0;
    const char *thumb = json_str(json_get(w, "thumbnail"), "");
    const char *type = json_str(json_get(w, "type"), "");
    if (!*thumb && !strcmp(type, "image")) thumb = json_str(json_get(w, "path"), "");
    return *thumb ? m->thumbnail(m->user, thumb) : 0;
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

/* ---- title bar and sidebar --------------------------------------------------- */

static void title_bar(void) {
    bare_row(.width = BARE_GROW, .height = 46, .align = BARE_CENTER, .pad = { 0, 6, 0, 18 }, .gap = 10,
             .window_drag = true) {
        bare_box(.width = 22, .height = 22, .radius = 6, .color = sp.accent,
                 .shadow = { 0, 0, 10, 0, sp_alpha(sp.accent, 120) });
        bare_text("Screen+", .size = 15, .weight = BARE_MEDIUM, .color = sp.ink, .nowrap = true,
                  .shadow = { 0, 1, 6, 0, sp.dark ? BARE_RGBA(0, 0, 0, 140) : BARE_RGBA(255, 255, 255, 200) });
        bare_box(.width = BARE_GROW);
        if (sp_button(ID("win.min"), SPI_MIN, NULL, SP_BTN_CHROME).clicked) bare_window_minimize();
        if (sp_button(ID("win.max"), bare_window_maximized() ? SPI_RESTORE : SPI_MAX, NULL, SP_BTN_CHROME).clicked)
            bare_window_maximize();
        if (sp_button(ID("win.close"), SPI_CLOSE, NULL, SP_BTN_CHROME).clicked) bare_window_close();
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
            bare_stack(.width = 48, .height = 48, .radius = 14, .align = BARE_CENTER, .justify = BARE_CENTER,
                       .glass = sp_glass_accent(), .border = 1, .border_color = sp_alpha(BARE_WHITE, 120),
                       .shadow = { 0, 4, 20, 0, sp_alpha(sp.accent, 120) }) {
                sp_icon(SPI_DISPLAY, 24, sp.on_accent);
            }
            bare_text("Screen+", .size = 25, .weight = BARE_SEMIBOLD, .color = sp.ink, .nowrap = true);
        }
        /* The nav: a glass drop under the current page glides between items. */
        bare_column(.id = ID("nav"), .gap = 6, .width = BARE_GROW) {
            box = bare_this().rect;
            BareRect r = rects[v->page];
            bool have = r.w > 0;
            bool moving = have && blob_was.w > 0 && fabsf(blob_was.y - r.y) > 3;
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
                     .opacity = have ? 1.0f : BARE_ZERO, .border = 1,
                     .border_color = sp_alpha(sp_mix(sp.accent, BARE_WHITE, 0.55f), moving ? 110 : 160),
                     .shadow = { 0, 4, 22, 0, sp_alpha(sp.accent, moving ? 30 : 110) },
                     .passthrough = true, .animate = sp_liquid());
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
                    BareColor c = on && !moving ? sp.on_accent : sp.ink;
                    sp_icon(icons[i], 21, c);
                    bare_text(names[i], .size = 18, .weight = on ? BARE_SEMIBOLD : BARE_MEDIUM, .color = c,
                              .nowrap = true, .animate = sp_fade());
                    if (st.clicked && !on) { v->page = (SpPage)i; v->page_time = bare_time(); v->popover = 0; }
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
        BareImage pic = wallpaper_picture(m, w);
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
            BareImage pic = wallpaper_picture(m, w);
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
            bare_text(json_str(json_get(w, "name"), "Untitled"), .size = 19, .weight = BARE_SEMIBOLD, .color = sp.ink,
                      .nowrap = true, .ellipsis = true);
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
                    bare_box(.width = w, .height = h, .image = wallpaper_picture(m, json_get(d, "wallpaper")),
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
                    bare_box(.width = 210, .height = 118, .radius = 16, .image = wallpaper_picture(m, w),
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

/* ---- Customise ---------------------------------------------------------------------- */

static void customise(SpView *v, const SpModel *m) {
    static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
    static const char *names[] = { "Brightness", "Contrast", "Saturation", "Hue", "Gamma" };
    static const char *icons[] = { SPI_SPEED, SPI_PALETTE, SPI_DROPPER, SPI_BRUSH, SPI_EYE };
    static const float lo[] = { -100, -100, -100, -180, -100 }, hi[] = { 100, 100, 100, 180, 100 };
    const JVal *w = active_wallpaper(m, v->target_display);
    bare_row(.width = BARE_GROW, .height = 84, .align = BARE_CENTER, .gap = 14, .shrink = BARE_ZERO) {
        page_title("Customise", w ? json_str(json_get(w, "name"), "") : "Nothing on screen");
        bare_box(.width = BARE_GROW);
        if (m->look && sp_button(ID("cus.reset"), SPI_REFRESH, "Reset look", SP_BTN_GLASS).clicked)
            RUN(m, "look", "reset");
    }
    bare_column(.width = BARE_GROW, .height = BARE_GROW, .gap = 18, .scroll = BARE_SCROLL_Y, .pad = { 4, 6, 24, 6 }) {
        bare_row(.width = BARE_GROW, .height = 280, .radius = 32, .padding = 18, .gap = 26, .glass = sp_glass_card(),
                 .border = 1, .border_color = sp.hair, .shrink = BARE_ZERO) {
            bare_box(.width = 435, .height = 244, .radius = 22, .image = wallpaper_picture(m, w),
                     .color = BARE_RGBA(0, 0, 0, 80), .shrink = BARE_ZERO, .shadow = { 0, 8, 26, 0, BARE_RGBA(0, 0, 0, 90) });
            bare_column(.width = BARE_GROW, .gap = 10, .pad = { 12, 0, 0, 0 }) {
                section("Live preview");
                bare_text("Changes show on the desktop as you drag. The look is kept per wallpaper.",
                          .size = 16, .color = sp.dim);
                bare_text("More controls (author properties, layout, upscaling, recordings) are in the classic window for now.",
                          .size = 14.5f, .color = sp.dim);
                if (sp_button(ID("cus.classic"), SPI_OPEN, "Open the classic Customise", SP_BTN_GLASS).clicked)
                    RUN(m, "ui", "classic");
            }
        }
        section("Colour grading");
        if (!m->look) {
            sp_label("Apply a wallpaper to adjust its look.", 17, sp.dim, BARE_REGULAR);
        } else {
            CARD(ID("cus.card")) {
                for (int i = 0; i < 5; i++) {
                    if (i) divider();
                    float val = v->dragging_look == i + 1 ? v->drag_look[i] : (float)json_num(json_get(m->look, keys[i]), 0);
                    ROW(IDI("cus.row", i), icons[i], names[i], NULL) {
                        char t[16];
                        snprintf(t, sizeof t, "%+d", (int)lroundf(val));
                        bare_row(.width = 60, .justify = BARE_END) { sp_label(t, 17, sp.ink, BARE_MEDIUM); }
                        int r = sp_slider(IDI("cus.slider", i), &val, lo[i], hi[i], 380);
                        if (r) {
                            v->dragging_look = r == 1 ? i + 1 : 0;
                            v->drag_look[i] = val;
                            double now = bare_time();
                            if (r == 2 || now - v->look_sent > 0.12) {
                                char num[16];
                                snprintf(num, sizeof num, "%d", (int)lroundf(val));
                                RUN(m, "look", "set", keys[i], num);
                                v->look_sent = now;
                            }
                        }
                    }
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
                        BareImage pic = *preview && m->thumbnail ? m->thumbnail(m->user, preview) : 0;
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

static const BareColor accents[] = { { 0, 215, 5, 255 }, { 10, 132, 255, 255 }, { 94, 92, 230, 255 }, { 191, 90, 242, 255 },
                                     { 255, 55, 95, 255 }, { 255, 159, 10, 255 }, { 255, 214, 10, 255 }, { 100, 210, 255, 255 } };

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
                    if (strcmp(rows[i].section, "General") || !want(search, rows[i].title, "General")) continue;
                    if (!first) divider();
                    first = false;
                    ROW(IDI("set.row", i), rows[i].icon, rows[i].title, NULL) { setting_control(m, &rows[i], i); }
                }
            }
        }
        if (want(search, "Auto update", "General")) CARD(ID("set.update")) {
            ROW(ID("set.update.row"), SPI_SYNC, "Auto update", "Installs new releases by itself") {
                if (sp_button(ID("set.update.check"), SPI_REFRESH, "Check now", SP_BTN_GLASS).clicked) RUN(m, "update", "check");
                bool on = setting_on(m, "auto_update");
                if (sp_toggle(ID("set.update.tog"), on)) RUN(m, "settings", "set", "auto_update", on ? "off" : "on");
            }
        }
        if (want(search, "Sample wallpaper updates", "General")) CARD(ID("set.samples")) {
            ROW(ID("set.samples.row"), SPI_LAYERS, "Sample wallpaper updates", "Checked when you press Check now") {
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
        if (want(search, "Animations motion", "General")) CARD(ID("set.motion")) {
            ROW(ID("set.motion.row"), SPI_FILM, "Animations", NULL) {
                static const char *labels[] = { "Full", "Reduced", "Off" }, *values[] = { "full", "reduced", "off" };
                choice_row(m, ID("set.motion.c"), "ui_motion", labels, values, 3, 50);
            }
        }
        if (want(search, "Accent colour color", "General")) CARD(ID("set.accent")) {
            ROW(ID("set.accent.row"), SPI_DROPPER, "Accent colour", "Changes highlights throughout Screen+") {
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
            if (!strcmp(r->section, "General") || !want(search, r->title, r->section)) continue;
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
                ROW(ID("set.about.row"), SPI_INFO, ver, "The classic window has every option while this one is in beta") {
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

void sp_draw(SpView *v, const SpModel *m) {
    if (!v || !m) return;
    BareVec2 win = bare_window_size();
    BareColor base = sp.dark ? BARE_RGB(14, 22, 24) : BARE_RGB(214, 224, 222);
    bare_stack(.id = ID("root"), .width = win.x, .height = win.y, .image = m->backdrop, .color = base,
               .text_color = sp.ink) {
        if (!m->backdrop)   /* no live desktop: a soft field so the glass still has something to bend */
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
