/* sp_kit.c: Screen+'s liquid-glass controls (see sp_kit.h). */
#include "sp_kit.h"
#ifndef _WIN32
#include "../linux/ui_icons.h"
#include <unistd.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SpTheme sp;

/* ---- small memory per control: last frame's rects and press ---------------- */

typedef struct Memo {
    BareId id;
    bool pressed, moving;
    BareRect box, blob;
    BareRect rects[16];
    int n;
    double since;            /* when the selection last moved */
} Memo;

static Memo memos[96];

static Memo *memo(BareId id) {
    unsigned h = (unsigned)(id ^ (id >> 29)) % 96;
    for (int k = 0; k < 96; k++) {
        Memo *m = &memos[(h + (unsigned)k) % 96];
        if (m->id == id) return m;
        if (!m->id) { memset(m, 0, sizeof *m); m->id = id; return m; }
    }
    Memo *m = &memos[h];   /* full: reuse (only a lost glide) */
    memset(m, 0, sizeof *m);
    m->id = id;
    return m;
}

static BareId sub(BareId id, unsigned k) { return id * 1099511628211ull + k + 1; }

/* ---- theme ------------------------------------------------------------------ */

BareColor sp_mix(BareColor a, BareColor b, float t) {
    return (BareColor){ (uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                        (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t) };
}

BareColor sp_alpha(BareColor c, int a) { c.a = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a); return c; }

void sp_theme_set(bool dark, BareColor accent, int motion_level) {
    sp.dark = dark;
    sp.motion = motion_level == 2 ? 0.0f : motion_level == 1 ? 0.5f : 1.0f;
    sp.accent = accent;
    sp.accent.a = 255;
    if (dark) {
        sp.ink = BARE_RGB(243, 248, 246);
        sp.dim = BARE_RGBA(228, 238, 235, 168);
        sp.faint = BARE_RGBA(255, 255, 255, 22);
        sp.hair = BARE_RGBA(255, 255, 255, 52);
        sp.panel_tint = BARE_RGBA(10, 20, 22, 132);
        sp.card_tint = BARE_RGBA(24, 36, 38, 70);
        sp.control_tint = BARE_RGBA(255, 255, 255, 20);
        sp.chrome = BARE_RGBA(255, 255, 255, 210);
    } else {
        sp.ink = BARE_RGB(18, 26, 28);
        sp.dim = BARE_RGBA(30, 44, 48, 170);
        sp.faint = BARE_RGBA(0, 20, 20, 18);
        sp.hair = BARE_RGBA(255, 255, 255, 150);
        sp.panel_tint = BARE_RGBA(244, 248, 247, 150);
        sp.card_tint = BARE_RGBA(255, 255, 255, 80);
        sp.control_tint = BARE_RGBA(255, 255, 255, 70);
        sp.chrome = BARE_RGBA(10, 20, 22, 200);
    }
    float lum = 0.2126f * accent.r + 0.7152f * accent.g + 0.0722f * accent.b;
    sp.on_accent = lum > 150 ? BARE_RGB(8, 22, 12) : BARE_RGB(255, 255, 255);
    sp.accent_soft = sp_alpha(accent, 60);
}

/* ---- looks ------------------------------------------------------------------ */

BareGlass sp_glass_panel(void) {
    return (BareGlass){ .refraction = 0.95f, .depth = 26, .blur = 9, .dispersion = 0.9f, .shine = 1.15f,
                        .tint = sp.panel_tint };
}

BareGlass sp_glass_card(void) {
    return (BareGlass){ .refraction = 0.85f, .depth = 18, .blur = 5, .dispersion = 0.7f, .shine = 1.0f,
                        .tint = sp.card_tint };
}

BareGlass sp_glass_control(void) {
    return (BareGlass){ .refraction = 1.0f, .depth = 14, .blur = 3, .dispersion = 0.8f, .shine = 1.2f,
                        .tint = sp.control_tint };
}

BareGlass sp_glass_accent(void) {
    return (BareGlass){ .refraction = 1.0f, .depth = 14, .blur = 2, .dispersion = 0.6f, .shine = 1.35f,
                        .tint = sp_alpha(sp.accent, 178) };
}

BareGlass sp_glass_lens(void) {
    return (BareGlass){ .refraction = 1.1f, .depth = 12, .blur = BARE_ZERO, .shine = 1.5f, .zoom = 1.18f,
                        .tint = BARE_RGBA(255, 255, 255, 16) };
}

BareAnim sp_spring(void) { return sp.motion <= 0 ? BARE_INSTANT : sp.motion < 1 ? BARE_EASE(0.14f, BARE_EASE_OUT) : BARE_SPRING(0.32f, 0.78f); }
BareAnim sp_liquid(void) { return sp.motion <= 0 ? BARE_INSTANT : sp.motion < 1 ? BARE_EASE(0.18f, BARE_EASE_OUT) : BARE_SPRING(0.46f, 0.64f); }
BareAnim sp_fade(void) { return sp.motion <= 0 ? BARE_INSTANT : BARE_EASE(0.22f, BARE_EASE_OUT); }

float sp_enter(double since) {
    if (sp.motion <= 0 || since >= 0.42 || since < 0) return 1.0f;
    float t = (float)(since / 0.42);
    bare_wake_in(0.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* ---- text and icons ----------------------------------------------------------- */

static BareFont icons;
static int icons_state;   /* 0 not tried, 1 loaded, 2 none */

BareFont sp_icon_font(void) {
    if (!icons_state) {
        icons_state = 2;
#ifdef _WIN32
        static const char *paths[] = { "C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf" };
        const char *windir = getenv("WINDIR");
        char path[300];
        for (int i = 0; i < 2 && icons_state == 2; i++) {
            const char *p = paths[i];
            if (windir) {
                snprintf(path, sizeof path, "%.200s%s", windir, strrchr(paths[i], '\\') - 6);   /* "\Fonts\..." */
                p = path;
            }
            icons = bare_font_load(p);
            if (icons) icons_state = 1;
        }
#else
        /* Fluent System Icons, shipped beside the program (as the classic UI uses it) or the installed app's */
        char dir[1024] = "", path[1100];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        if (n > 0) {
            dir[n] = 0;
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
        }
        const char *home = getenv("HOME");
        for (int i = 0; i < 2 && icons_state == 2; i++) {
            if (i == 0) snprintf(path, sizeof path, "%s/fonts/FluentSystemIcons-Regular.ttf", dir);
            else snprintf(path, sizeof path, "%s/.local/share/screenplus-app/fonts/FluentSystemIcons-Regular.ttf",
                          home ? home : "");
            if (access(path, R_OK)) continue;
            icons = bare_font_load(path);
            if (icons) icons_state = 1;
        }
#endif
    }
    return icons_state == 1 ? icons : 0;
}

void sp_icon(const char *glyph, float size, BareColor color) {
    if (!glyph || !sp_icon_font()) return;
#ifndef _WIN32
    /* the Segoe code points (SPI_*) to Fluent's */
    const unsigned char *g = (const unsigned char *)glyph;
    char mapped[5];
    if ((g[0] & 0xF0) == 0xE0 && g[1] && g[2]) {
        uint32_t c = ui_icon_map(((uint32_t)(g[0] & 15) << 12) | ((uint32_t)(g[1] & 63) << 6) | (g[2] & 63));
        if (c < 0x10000) {
            mapped[0] = (char)(0xE0 | (c >> 12)), mapped[1] = (char)(0x80 | ((c >> 6) & 63));
            mapped[2] = (char)(0x80 | (c & 63)), mapped[3] = 0;
        } else {
            mapped[0] = (char)(0xF0 | (c >> 18)), mapped[1] = (char)(0x80 | ((c >> 12) & 63));
            mapped[2] = (char)(0x80 | ((c >> 6) & 63)), mapped[3] = (char)(0x80 | (c & 63)), mapped[4] = 0;
        }
        glyph = mapped;
    }
#endif
    bare_text(glyph, .font = sp_icon_font(), .size = size, .color = color, .nowrap = true, .line_height = 1.0f);
}

void sp_label(const char *text, float size, BareColor color, int weight) {
    bare_text(text ? text : "", .size = size, .color = color, .weight = weight, .nowrap = true, .ellipsis = true);
}

/* ---- buttons -------------------------------------------------------------------- */

BareState sp_button(BareId id, const char *icon, const char *label, int kind) {
    Memo *m = memo(id);
    bool held = m->pressed;
    bool icon_only = !label || !*label;
    float h = kind == SP_BTN_CHROME ? 40.0f : 50.0f;
    BareColor fg = kind == SP_BTN_PRIMARY ? sp.on_accent : kind == SP_BTN_CHROME ? sp.chrome : sp.ink;
    BareGlass glass = kind == SP_BTN_PRIMARY ? sp_glass_accent() : sp_glass_control();
    if (held && kind != SP_BTN_PLAIN && kind != SP_BTN_CHROME) { glass.zoom = 1.08f; glass.shine = 1.6f; }
    bool glassy = kind == SP_BTN_GLASS || kind == SP_BTN_PRIMARY || kind == SP_BTN_ICON || kind == SP_BTN_DANGER;
    if (kind == SP_BTN_DANGER) glass.tint = BARE_RGBA(232, 52, 64, 170);
    BareState st = { 0 };
    bare_row(.id = id, .height = h, .width = icon_only ? (kind == SP_BTN_CHROME ? 48.0f : 60.0f) : BARE_FIT,
             .pad = { 0, icon_only ? 0.0f : 24.0f, 0, icon_only ? 0.0f : (icon ? 20.0f : 24.0f) }, .gap = 10,
             .align = BARE_CENTER, .justify = BARE_CENTER, .shrink = BARE_ZERO,
             .radius = kind == SP_BTN_CHROME ? 12 : icon_only ? 20 : h / 2,
             .glass = glassy ? glass : (BareGlass){ 0 },
             .border = glassy ? 1 : BARE_ZERO,
             .border_color = kind == SP_BTN_PRIMARY ? sp_alpha(sp_mix(sp.accent, BARE_WHITE, 0.45f), 170) : sp.hair,
             .shadow = kind == SP_BTN_PRIMARY ? (BareShadow){ 0, 4, 22, 0, sp_alpha(sp.accent, 110) } : (BareShadow){ 0 },
             .hover = { .scale = 1.0f + 0.035f * sp.motion,
                        .color = kind == SP_BTN_CHROME && label == NULL && icon && !strcmp(icon, SPI_CLOSE)
                                     ? BARE_RGBA(232, 17, 35, 220) : sp.faint },
             .press = { .scale = 1.0f - 0.05f * sp.motion },
             .cursor = BARE_CURSOR_HAND, .focusable = kind != SP_BTN_CHROME, .animate = sp_spring()) {
        st = bare_this();
        if (icon) sp_icon(icon, kind == SP_BTN_CHROME ? 13.0f : 18.0f, fg);
        if (!icon_only) sp_label(label, 17, fg, BARE_MEDIUM);
    }
    m->pressed = st.pressed;
    return st;
}

BareState sp_chip(BareId id, const char *icon, const char *label, bool on, BareColor tint) {
    BareState st = { 0 };
    BareColor c = tint.a ? tint : sp.accent;
    bare_row(.id = id, .height = 34, .pad = { 0, 14, 0, 14 }, .gap = 8, .align = BARE_CENTER, .radius = 17,
             .shrink = BARE_ZERO,
             .glass = on ? (BareGlass){ .refraction = 0.8f, .depth = 10, .blur = 2, .shine = 1.1f, .tint = sp_alpha(c, 70) }
                         : (BareGlass){ .refraction = 0.8f, .depth = 10, .blur = 2, .shine = 0.9f, .tint = sp.control_tint },
             .border = 1, .border_color = on ? sp_alpha(c, 200) : sp.hair, .animate = sp_fade()) {
        st = bare_this();
        if (icon && !strcmp(icon, SPI_DOT))
            bare_box(.width = 8, .height = 8, .radius = 4, .color = tint.a ? tint : sp.dim,
                     .shadow = { 0, 0, 6, 0, sp_alpha(tint.a ? tint : sp.dim, 150) });
        else if (icon) sp_icon(icon, 13, on ? sp_mix(c, sp.ink, sp.dark ? 0.25f : 0.0f) : sp.dim);
        sp_label(label, 15, on ? sp_mix(c, sp.ink, sp.dark ? 0.25f : 0.0f) : sp.dim, BARE_MEDIUM);
    }
    return st;
}

/* ---- switch ---------------------------------------------------------------------- */

bool sp_toggle(BareId id, bool on) {
    const float W = 62, H = 34, K = 28;
    Memo *m = memo(id);
    bool held = m->pressed;
    bool clicked = false;
    float kw = held ? 40 : K, kh = held ? 34 : K;
    float travel = (W - K) / 2 - 3;
    float x = (on ? travel : -travel) + (on ? -(kw - K) / 2 : (kw - K) / 2);
    bare_stack(.id = id, .width = W, .height = H, .radius = H / 2, .align = BARE_CENTER, .justify = BARE_CENTER,
               .color = on ? sp.accent : (sp.dark ? BARE_RGBA(255, 255, 255, 40) : BARE_RGBA(0, 0, 0, 40)),
               .border = 1, .border_color = on ? sp_alpha(sp_mix(sp.accent, BARE_WHITE, 0.5f), 150) : sp.hair,
               .shadow = on ? (BareShadow){ 0, 0, 16, 0, sp_alpha(sp.accent, 90) } : (BareShadow){ 0 },
               .cursor = BARE_CURSOR_HAND, .focusable = true, .shrink = BARE_ZERO, .animate = sp_fade()) {
        BareState st = bare_this();
        clicked = st.clicked;
        m->pressed = st.pressed;
        bare_box(.id = sub(id, 1), .width = kw, .height = kh, .radius = kh / 2, .offset = { x, 0 },
                 .color = held ? BARE_RGBA(255, 255, 255, 0) : BARE_WHITE,
                 .glass = held ? sp_glass_lens() : (BareGlass){ 0 },
                 .shadow = { 0, 2, held ? 14.0f : 6.0f, 0, BARE_RGBA(0, 0, 0, held ? 50 : 70) },
                 .passthrough = true, .animate = sp_liquid());
    }
    return clicked;
}

/* ---- choices with a gliding glass drop ------------------------------------------- */

int sp_choices(BareId id, const char *const *labels, int count, int selected, bool track, float height) {
    Memo *m = memo(id);
    int clicked = -1;
    if (count > 16) count = 16;
    float pad = track ? 4.0f : 0.0f;
    float h = height > 0 ? height : 44;
    bool have = selected >= 0 && selected < count && selected < m->n && m->rects[selected].w > 0;
    BareRect r = have ? m->rects[selected] : (BareRect){ 0 };
    /* In flight: the drop lifts into a clear lens, then settles back into colour. */
    bool moving = have && m->blob.w > 0 && (fabsf(m->blob.x - r.x) > 3 || fabsf(m->blob.y - r.y) > 3);
    if (moving && !m->moving) m->since = bare_time();
    m->moving = moving;
    bare_row(.id = id, .height = h, .padding = pad, .gap = track ? 0.0f : 6.0f, .radius = h / 2, .align = BARE_CENTER,
             .shrink = BARE_ZERO,
             .glass = track ? sp_glass_control() : (BareGlass){ 0 }, .border = track ? 1 : BARE_ZERO,
             .border_color = sp.hair) {
        BareState box = bare_this();
        m->box = box.rect;
        BareGlass g = moving && sp.motion > 0 ? sp_glass_lens() : sp_glass_accent();
        if (moving) { g.zoom = 1.12f; g.tint = sp_alpha(sp.accent, 60); }
        BareState blob = bare_box(.id = sub(id, 99), .position = BARE_ABSOLUTE,
                 .x = have ? r.x - box.rect.x : 0, .y = have ? r.y - box.rect.y : 0,
                 .width = have ? r.w : 10, .height = have ? r.h : 10, .radius = (have ? r.h : 10) / 2,
                 .opacity = have ? 1.0f : BARE_ZERO, .glass = g, .border = 1,
                 .border_color = sp_alpha(sp_mix(sp.accent, BARE_WHITE, 0.55f), moving ? 90 : 150),
                 .shadow = { 0, 3, 18, 0, sp_alpha(sp.accent, moving ? 40 : 95) },
                 .passthrough = true, .animate = sp_liquid());
        m->blob = blob.rect;
        for (int i = 0; i < count; i++) {
            bool on = i == selected;
            bare_row(.id = sub(id, (unsigned)i + 1), .height = h - pad * 2, .pad = { 0, 20, 0, 20 },
                     .align = BARE_CENTER, .justify = BARE_CENTER, .radius = (h - pad * 2) / 2,
                     .shrink = BARE_ZERO, .cursor = BARE_CURSOR_HAND, .focusable = true,
                     .hover = { .color = on ? BARE_RGBA(255, 255, 255, 0) : sp.faint },
                     .press = { .scale = 1.0f - 0.04f * sp.motion }, .animate = sp_fade()) {
                BareState st = bare_this();
                m->rects[i] = st.rect;
                if (st.clicked) clicked = i;
                bare_text(labels[i], .size = 16, .weight = BARE_MEDIUM, .nowrap = true,
                          .color = on ? sp.on_accent : sp.ink, .animate = sp_fade());
            }
        }
    }
    m->n = count;
    if (moving) bare_wake_in(0.0f);
    return clicked;
}

/* ---- slider ------------------------------------------------------------------------ */

int sp_slider(BareId id, float *value, float lo, float hi, float width) {
    const float K = 24, H = 34;
    Memo *m = memo(id);
    int result = 0;
    float t = hi > lo ? (*value - lo) / (hi - lo) : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    bool held = m->pressed;
    bare_stack(.id = id, .width = width, .height = H, .cursor = BARE_CURSOR_HAND, .shrink = BARE_ZERO) {
        BareState st = bare_this();
        if (st.pressed && st.rect.w > K) {
            float nt = (bare_mouse().x - st.rect.x - K / 2) / (st.rect.w - K);
            nt = nt < 0 ? 0 : nt > 1 ? 1 : nt;
            *value = lo + nt * (hi - lo);
            t = nt;
            result = 1;
        } else if (m->pressed) {
            result = 2;
        }
        m->pressed = st.pressed;
        float w = st.rect.w > 0 ? st.rect.w : width;
        float kw = held ? 38 : K, kh = held ? 30 : K;
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_LEFT, .x = K / 2, .width = w - K, .height = 6, .radius = 3,
                 .color = sp.dark ? BARE_RGBA(255, 255, 255, 45) : BARE_RGBA(0, 0, 0, 45), .passthrough = true);
        bare_box(.id = sub(id, 1), .position = BARE_ABSOLUTE, .anchor = BARE_LEFT, .x = K / 2,
                 .width = t * (w - K) + 0.5f, .height = 6, .radius = 3, .color = sp.accent,
                 .passthrough = true, .animate = held ? BARE_INSTANT : sp_spring());
        bare_box(.id = sub(id, 2), .position = BARE_ABSOLUTE, .anchor = BARE_LEFT,
                 .x = K / 2 + t * (w - K) - kw / 2, .width = kw, .height = kh, .radius = kh / 2,
                 .color = held ? BARE_RGBA(255, 255, 255, 0) : BARE_WHITE,
                 .glass = held ? sp_glass_lens() : (BareGlass){ 0 },
                 .shadow = { 0, 2, held ? 14.0f : 6.0f, 0, BARE_RGBA(0, 0, 0, held ? 50 : 80) },
                 .passthrough = true, .animate = held ? BARE_SPRING(0.18f, 0.9f) : sp_liquid());
    }
    return result;
}
