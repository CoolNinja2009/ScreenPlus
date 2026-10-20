/* Page content: sidebar, Library (hero + grid), Displays, Settings/Diagnostics.
 * Immediate-mode: layout is recomputed on every paint (paints only happen on change). */
#include "ui_internal.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../renderer/renderer.h"
#include "../desktop/desktop_host.h"
#include <stdlib.h>
#include <math.h>

#define SIDEBAR_W 232.0f
#define PAD       32.0f

static const wchar_t *k_filters[] = { L"All", L"Images", L"Videos", L"HTML", L"Recent" };

/* ================================================================ helpers */

static const wchar_t *target_id(void)
{
    Ui *u = g_ui;
    return (u->target >= 0 && u->target < g_app.monCount) ? g_app.mons[u->target].id : NULL;
}

static void fmt_bytes(wchar_t *buf, size_t cap, size_t b)
{
    if (b >= (size_t)1 << 30) str_printf(buf, cap, L"%.2f GB", (double)b / (1 << 30));
    else str_printf(buf, cap, L"%.0f MB", (double)b / (1 << 20));
}

static void text_right(const wchar_t *s, IDWriteTextFormat *f, float right, float y, float h, D2D1_COLOR_F c)
{
    float w = ui_text_width(s, f);
    ui_text(s, f, rectf(right - w - 1.0f, y, w + 2.0f, h), c);
}

static const wchar_t *state_label(const IpcMonitorStatus *ms, D2D1_COLOR_F *c)
{
    Theme *t = &g_ui->t;
    *c = t->text2;
    if (!ms) return L"Waiting";
    if (!ms->wallpaperId[0]) return L"No wallpaper";
    if (ms->failed) { *c = t->danger; return L"Error"; }
    switch (ms->state) {
    case RS_ACTIVE:     *c = t->success; return L"Active";
    case RS_OCCLUDED:   return L"Covered";
    case RS_SUSPENDED:  *c = t->warning; return L"Paused";
    case RS_RECOVERING: *c = t->warning; return L"Recovering";
    default:            return L"Stopped";
    }
}

static int cmp_recent(const void *a, const void *b)
{
    const WallpaperEntry *x = &g_app.cfg.items[*(const int *)a], *y = &g_app.cfg.items[*(const int *)b];
    return x->lastUsed > y->lastUsed ? -1 : x->lastUsed < y->lastUsed ? 1 : 0;
}

void pages_rebuild_view(void)
{
    Ui *u = g_ui;
    if (!u) return;
    if (u->viewCap < g_app.cfg.count) {
        int *p = (int *)realloc(u->view, sizeof(int) * (size_t)SP_MAX(g_app.cfg.count, 16));
        if (!p) return;
        u->view = p;
        u->viewCap = SP_MAX(g_app.cfg.count, 16);
    }
    u->viewCount = 0;
    for (int i = 0; i < g_app.cfg.count; i++) {
        const WallpaperEntry *e = &g_app.cfg.items[i];
        if (u->filter == 1 && e->type != WP_IMAGE) continue;
        if (u->filter == 2 && e->type != WP_VIDEO) continue;
        if (u->filter == 3 && e->type != WP_HTML) continue;
        if (u->filter == 4 && !e->lastUsed) continue;
        if (u->search[0] && !str_icontains(e->name, u->search) && !str_icontains(path_filename(e->path), u->search)) continue;
        u->view[u->viewCount++] = i;
    }
    if (u->filter == 4) qsort(u->view, (size_t)u->viewCount, sizeof(int), cmp_recent);
    if (u->target >= g_app.monCount) u->target = -1;
}

/* ================================================================ sidebar */

static void draw_sidebar(Ui *u)
{
    Theme *t = &u->t;
    ui_fill(rectf(0, 0, SIDEBAR_W, u->h), t->sidebar);
    ui_line(SIDEBAR_W - 0.5f, 0, SIDEBAR_W - 0.5f, u->h, 1.0f, t->separator);

    /* brand */
    D2D1_RECT_F mark = rectf(24, 22, 30, 30);
    ui_rrect(mark, 9, t->accent);
    ui_rrect_stroke(rectf(31, 30, 16, 12), 3, 1.6f, t->onAccent);
    ui_line(39, 42, 39, 45.5f, 1.6f, t->onAccent);
    ui_line(35, 46, 43, 46, 1.6f, t->onAccent);
    ui_text(L"Screen+", u->fBrand, rectf(64, 22, 150, 30), t->text);

    static const wchar_t *labels[] = { L"Library", L"Displays", L"Settings" };
    static const wchar_t icons[] = { GL_LIBRARY, GL_DISPLAY, GL_SETTINGS };
    float y = 84;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int id = HIT_ID(HK_NAV, i);
        D2D1_RECT_F r = rectf(12, y, SIDEBAR_W - 24, 40);
        int sel = u->page == i, hot = ui_is_hot(id);
        if (sel) ui_rrect(r, 10, t->navActive);
        else if (hot) ui_rrect(r, 10, col_a(t->navActive, t->navActive.a * 0.55f));
        ui_icon(icons[i], u->fIcon, rectf(r.left + 10, y, 24, 40), sel ? t->accent : t->text2);
        ui_text(labels[i], sel ? u->fBodyB : u->fBody, rectf(r.left + 44, y, 150, 40), sel ? t->text : t->text2);
        ui_hit(id, r, 1, 1);
        ui_focus_ring(r, 10, id);
        y += 44;
    }

    /* engine status card */
    int sev = 0;
    const wchar_t *state = app_engine_state_text(&sev);
    D2D1_COLOR_F dot = sev == 0 ? t->success : sev == 1 ? t->warning : sev == 2 ? t->warning : t->danger;
    float ch = 112, cy = u->h - ch - 16;
    D2D1_RECT_F card = rectf(12, cy, SIDEBAR_W - 24, ch);
    ui_glass(card, 14, 0, 0);
    ui_circle(30, cy + 24, 4.5f, dot);
    ui_text(state, u->fSmallB, rectf(42, cy + 13, SIDEBAR_W - 70, 22), t->text);
    wchar_t line[128];
    int mons = g_app.haveStatus ? g_app.status.monitorCount : g_app.monCount;
    int act = g_app.haveStatus ? g_app.status.activeRenderers : 0;
    str_printf(line, SP_COUNTOF(line), L"%d display%ls · %d rendering", mons, mons == 1 ? L"" : L"s", act);
    ui_text(line, u->fSmall, rectf(24, cy + 38, SIDEBAR_W - 48, 18), t->text2);
    wchar_t mem[32];
    fmt_bytes(mem, SP_COUNTOF(mem), g_app.shellMem + g_app.engineMem);
    str_printf(line, SP_COUNTOF(line), L"CPU %.1f%% · RAM %ls", g_app.shellCpu + g_app.engineCpu, mem);
    ui_text(line, u->fSmall, rectf(24, cy + 58, SIDEBAR_W - 48, 18), t->text2);
    if (g_app.wallpaperMs > 0) str_printf(line, SP_COUNTOF(line), L"Wallpaper ready in %.0f ms", g_app.wallpaperMs);
    else str_printf(line, SP_COUNTOF(line), L"Started in %.0f ms", g_app.nativeInitMs);
    ui_text(line, u->fSmall, rectf(24, cy + 78, SIDEBAR_W - 48, 18), t->text3);
}

/* ================================================================ shared page chrome */

static void draw_title(Ui *u, float x, const wchar_t *title, const wchar_t *subtitle)
{
    ui_text(title, u->fTitle, rectf(x, 16, 420, 42), u->t.text);
    ui_text(subtitle, u->fSmall, rectf(x + 1, 56, 420, 18), u->t.text2);
}

static float draw_notice(Ui *u, float x, float y, float w)
{
    if (!g_app.notice[0]) return 0.0f;
    Theme *t = &u->t;
    D2D1_COLOR_F c = g_app.noticeKind == NOTICE_ERROR ? t->danger : g_app.noticeKind == NOTICE_WARN ? t->warning : t->accent;
    wchar_t g = g_app.noticeKind == NOTICE_ERROR ? GL_ERROR : g_app.noticeKind == NOTICE_WARN ? GL_WARNING : GL_INFO;
    D2D1_RECT_F r = rectf(x, y, w, 44);
    ui_rrect(r, 12, col_a(c, t->dark ? 0.14f : 0.10f));
    ui_rrect_stroke(r, 12, 1, col_a(c, 0.35f));
    ui_icon(g, u->fIcon, rectf(x + 10, y, 28, 44), c);
    ui_text(g_app.notice, u->fBody, rectf(x + 44, y, w - 96, 44), t->text);
    if (g_app.noticeKind == NOTICE_ERROR && (g_app.engineGaveUp || g_app.engineError[0])) {
        float bw = ui_button_width(L"Restart renderer", 0);
        ui_button(HIT_ID(HK_RESTART_ENGINE, 1), rectf(x + w - 48 - bw, y + 6, bw, 32), L"Restart renderer", 0, BTN_SECONDARY);
    }
    ui_button(HIT_ID(HK_NOTICE_CLOSE, 0), rectf(x + w - 40, y + 6, 32, 32), NULL, GL_CLOSE, BTN_GHOST);
    return 56.0f;
}

static void draw_scrollbar(Ui *u, D2D1_RECT_F view, int page)
{
    float vh = view.bottom - view.top, ch = u->contentH[page];
    if (ch <= vh + 1.0f) return;
    float th = SP_MAX(36.0f, vh * vh / ch), range = vh - th;
    float pos = range * SP_CLAMP(u->scroll[page] / (ch - vh), 0.0f, 1.0f);
    ui_rrect(rectf(view.right - 8, view.top + pos + 2, 4, th - 4), 2, u->t.text3);
}

static void finish_scroll(Ui *u, int page, float contentBottom, float top)
{
    u->contentH[page] = contentBottom + u->scroll[page] - top;
    float maxScroll = SP_MAX(0.0f, u->contentH[page] - u->viewH);
    if (u->scrollTarget[page] > maxScroll) {
        u->scrollTarget[page] = maxScroll;
        ui_animate();
    }
}

/* ================================================================ library */

static void draw_search(Ui *u, D2D1_RECT_F r)
{
    Theme *t = &u->t;
    int id = HIT_ID(HK_SEARCH, 0);
    ui_rrect(r, 9, t->field);
    ui_rrect_stroke(r, 9, u->searchFocus ? 1.5f : 1.0f, u->searchFocus ? t->accent : t->border);
    ui_icon(GL_SEARCH, u->fIconS, rectf(r.left + 8, r.top, 22, r.bottom - r.top), t->text3);
    float tx = r.left + 34, tw = r.right - r.left - 34 - (u->search[0] ? 32 : 10);
    if (u->search[0]) ui_text(u->search, u->fBody, rectf(tx, r.top, tw, r.bottom - r.top), t->text);
    else ui_text(L"Search wallpapers", u->fBody, rectf(tx, r.top, tw, r.bottom - r.top), t->text3);
    if (u->searchFocus && u->caretOn && GetForegroundWindow() == u->hwnd) {
        float cx = tx + SP_MIN(ui_text_width(u->search, u->fBody), tw) + 1.0f;
        ui_line(cx, r.top + 9, cx, r.bottom - 9, 1.2f, t->accent);
    }
    ui_hit(id, r, 1, 2);
    if (u->search[0]) ui_button(HIT_ID(HK_SEARCH_CLEAR, 0), rectf(r.right - 30, r.top + 4, 26, r.bottom - r.top - 8), NULL, GL_CLOSE, BTN_GHOST);
}

static float draw_hero(Ui *u, float x, float y, float w)
{
    Theme *t = &u->t;
    float h = 236;
    D2D1_RECT_F card = rectf(x, y, w, h);
    ui_glass(card, 18, 0, 1);
    const wchar_t *mid = target_id();
    WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(mid));

    float ph = h - 32, pw = ph * 16.0f / 9.0f;
    if (pw > w * 0.46f) { pw = w * 0.46f; ph = pw * 9.0f / 16.0f; }
    D2D1_RECT_F prev = rectf(x + 16, y + (h - ph) * 0.5f, pw, ph);
    if (e) ui_draw_thumb(e, prev, 12, 1);
    else {
        ui_rrect(prev, 12, t->chip);
        ui_icon(GL_DISPLAY, u->fIconL, rectf(prev.left, prev.top, pw, ph - 18), t->text3);
        ui_text(L"No wallpaper", u->fCenter, rectf(prev.left, prev.top + ph * 0.5f + 14, pw, 20), t->text3);
    }
    ui_rrect_stroke(prev, 12, 1, t->border);

    float rx = prev.right + 26, rw = card.right - 22 - rx, ry = y + 22;
    wchar_t cap[96];
    if (mid) str_printf(cap, SP_COUNTOF(cap), L"NOW SHOWING  ·  DISPLAY %d", g_app.mons[u->target].index);
    else str_printf(cap, SP_COUNTOF(cap), L"NOW SHOWING  ·  %ls", g_app.monCount > 1 ? L"ALL DISPLAYS" : L"DESKTOP");
    ui_text(cap, u->fCaption, rectf(rx, ry, rw, 16), t->text3);
    ry += 20;
    ui_text(e ? e->name : L"Choose a wallpaper from your library", u->fHeading, rectf(rx, ry, rw, 30), t->text);
    ry += 36;

    /* type badge + renderer detail from the engine */
    const IpcMonitorStatus *ms = NULL;
    for (int i = 0; e && i < (g_app.haveStatus ? g_app.status.monitorCount : 0); i++) {
        const IpcMonitorStatus *m = &g_app.status.monitors[i];
        if (wcscmp(m->wallpaperId, e->id) == 0 && (!mid || str_ieq(m->monitorId, mid))) { ms = m; break; }
    }
    float bx = rx;
    if (e) {
        const wchar_t *tn = e->type == WP_VIDEO ? L"VIDEO" : e->type == WP_HTML ? L"HTML" : L"IMAGE";
        float bw = ui_text_width(tn, u->fCenterS) + 18;
        ui_pill(rectf(bx, ry, bw, 22), tn, col_a(t->accent, 0.16f), t->accent);
        bx += bw + 10;
        D2D1_COLOR_F sc;
        const wchar_t *sl = state_label(ms, &sc);
        float sw = ui_text_width(sl, u->fCenterS) + 26;
        ui_pill(rectf(bx, ry, sw, 22), L"", col_a(sc, 0.14f), sc);
        ui_circle(bx + 11, ry + 11, 3.5f, sc);
        ui_text(sl, u->fCenterS, rectf(bx + 14, ry, sw - 16, 22), sc);
        bx += sw + 10;
    }
    if (ms && ms->error[0])
        ui_text(ms->error, u->fSmall, rectf(bx, ry, rx + rw - bx, 22), ms->failed ? t->danger : t->text2);
    else if (e)
        ui_text(e->path, u->fSmall, rectf(bx, ry, rx + rw - bx, 22), t->text3);
    ry += 38;

    /* target selector */
    float cx = rx;
    {
        float cw = ui_text_width(L"All displays", u->fCenter) + 28;
        ui_chip(HIT_ID(HK_TARGET, -1), rectf(cx, ry, cw, 30), L"All displays", u->target < 0);
        cx += cw + 8;
    }
    for (int i = 0; i < g_app.monCount; i++) {
        wchar_t lbl[64];
        str_printf(lbl, SP_COUNTOF(lbl), L"%d  %.16ls", g_app.mons[i].index, g_app.mons[i].name);
        float cw = ui_text_width(lbl, u->fCenter) + 28;
        if (cx + cw > rx + rw) {
            str_printf(lbl, SP_COUNTOF(lbl), L"%d", g_app.mons[i].index);
            cw = 40;
        }
        ui_chip(HIT_ID(HK_TARGET, i), rectf(cx, ry, cw, 30), lbl, u->target == i);
        cx += cw + 8;
    }
    ry += 46;

    /* playback controls */
    int paused = g_app.cfg.s.paused;
    const wchar_t *pl = paused ? L"Resume" : L"Pause";
    float b1 = ui_button_width(pl, GL_PLAY);
    ui_button(HIT_ID(HK_PLAYPAUSE, 0), rectf(rx, ry, b1, 34), pl, paused ? GL_PLAY : GL_PAUSE, paused ? BTN_PRIMARY : BTN_SECONDARY);
    float ax = rx + b1 + 8;
    const wchar_t *al = g_app.cfg.s.audio ? L"Sound on" : L"Muted";
    float b2 = ui_button_width(al, GL_VOLUME);
    ui_button(HIT_ID(HK_AUDIO, 0), rectf(ax, ry, b2, 34), al, g_app.cfg.s.audio ? GL_VOLUME : GL_MUTE, BTN_SECONDARY);
    ax += b2 + 8;
    float b3 = ui_button_width(L"Restart", GL_REFRESH);
    ui_button(HIT_ID(HK_RESTART_CURRENT, 0), rectf(ax, ry, b3, 34), L"Restart", GL_REFRESH, BTN_SECONDARY);
    ax += b3 + 8;
    if (e && ax + 36 <= rx + rw) ui_button(HIT_ID(HK_REVEAL, 0), rectf(ax, ry, 36, 34), NULL, GL_OPEN, BTN_SECONDARY);
    return y + h;
}

static float draw_filters(Ui *u, float x, float y, float w)
{
    float cx = x;
    for (int i = 0; i < (int)SP_COUNTOF(k_filters); i++) {
        float cw = ui_text_width(k_filters[i], u->fCenter) + 30;
        ui_chip(HIT_ID(HK_FILTER, i), rectf(cx, y, cw, 30), k_filters[i], u->filter == i);
        cx += cw + 8;
    }
    wchar_t n[48];
    str_printf(n, SP_COUNTOF(n), L"%d of %d", u->viewCount, g_app.cfg.count);
    text_right(n, u->fSmall, x + w, y, 30, u->t.text3);
    return y + 30;
}

static void draw_card(Ui *u, int vi, const WallpaperEntry *e, D2D1_RECT_F r, float thumbH, int current)
{
    Theme *t = &u->t;
    int id = HIT_ID(HK_CARD, vi), hot = ui_is_hot(id);
    ui_glass(r, 14, hot, hot);
    D2D1_RECT_F th = { r.left + 6, r.top + 6, r.right - 6, r.top + 6 + thumbH };
    ui_draw_thumb(e, th, 10, 0);
    if (e->missing) {
        ui_rrect(th, 10, t->scrim);
        ui_icon(GL_WARNING, u->fIcon, rectf(th.left, th.top, th.right - th.left, (th.bottom - th.top) - 14), col(0xFFFFFF, 1));
        ui_text(L"File not found", u->fCenter, rectf(th.left, (th.top + th.bottom) * 0.5f + 4, th.right - th.left, 20), col(0xFFFFFF, 0.9f));
    }
    const wchar_t *tn = e->type == WP_VIDEO ? L"VIDEO" : e->type == WP_HTML ? L"HTML" : L"IMAGE";
    float bw = ui_text_width(tn, u->fCenterS) + 34;
    D2D1_RECT_F b = { th.left + 8, th.bottom - 30, th.left + 8 + bw, th.bottom - 8 };
    ui_rrect(b, 11, col(0x000000, 0.48f));
    ui_icon(ui_type_icon(e->type)[0], u->fIconS, rectf(b.left + 6, b.top, 16, 22), col(0xFFFFFF, 0.95f));
    ui_text(tn, u->fCenterS, rectf(b.left + 20, b.top, bw - 24, 22), col(0xFFFFFF, 0.95f));
    if (current) {
        ui_circle(th.right - 18, th.top + 18, 12, t->accent);
        ui_icon(GL_CHECK, u->fIconS, rectf(th.right - 30, th.top + 6, 24, 24), t->onAccent);
        ui_rrect_stroke(r, 14, 2, t->accent);
    }
    ui_text(e->name, u->fBodyB, rectf(r.left + 12, th.bottom + 8, r.right - r.left - 24, 20), t->text);
    wchar_t meta[96], ext[16];
    str_copy(ext, SP_COUNTOF(ext), path_ext(e->path)[0] ? path_ext(e->path) + 1 : L"file");
    CharUpperW(ext);
    if (e->type == WP_HTML) str_printf(meta, SP_COUNTOF(meta), L"HTML · %ls", path_filename(e->path));
    else str_printf(meta, SP_COUNTOF(meta), L"%ls · %ls", wallpaper_type_name(e->type), ext);
    ui_text(meta, u->fSmall, rectf(r.left + 12, th.bottom + 28, r.right - r.left - 24, 16), t->text3);
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, 14, id);
}

static float draw_grid(Ui *u, float x, float y, float w)
{
    float gap = 18, minW = 210;
    int cols = SP_MAX(1, (int)((w + gap) / (minW + gap)));
    u->cols = cols;
    float cw = (w - gap * (cols - 1)) / cols, thumbH = (cw - 12) * 9.0f / 16.0f, ch = thumbH + 12 + 48;
    u->gridY = y + u->scroll[PAGE_LIBRARY];
    u->rowH = ch + gap;
    const wchar_t *cur = app_wallpaper_for(target_id());
    for (int i = 0; i < u->viewCount; i++) {
        int c = i % cols, row = i / cols;
        D2D1_RECT_F r = rectf(x + c * (cw + gap), y + row * (ch + gap), cw, ch);
        if (r.bottom < u->viewTop - 8 || r.top > u->h + 8) continue;     /* culled */
        const WallpaperEntry *e = &g_app.cfg.items[u->view[i]];
        draw_card(u, i, e, r, thumbH, wcscmp(e->id, cur) == 0);
    }
    int rows = (u->viewCount + cols - 1) / cols;
    return y + rows * (ch + gap) - gap;
}

static float draw_empty(Ui *u, float x, float y, float w, int noResults)
{
    Theme *t = &u->t;
    float h = noResults ? 170.0f : 250.0f;
    D2D1_RECT_F r = rectf(x, y, w, h);
    ui_glass(r, 18, 0, 0);
    ui_icon(noResults ? GL_SEARCH : GL_LIBRARY, u->fIconL, rectf(x, y + 22, w, 50), noResults ? t->text3 : t->accent);
    const wchar_t *title = noResults ? L"No matching wallpapers" : L"Your library is empty";
    float tw = ui_text_width(title, u->fHeading);
    ui_text(title, u->fHeading, rectf(x + (w - tw) * 0.5f, y + 76, tw + 4, 30), t->text);
    if (noResults) {
        float bw = ui_button_width(L"Clear search", 0);
        ui_button(HIT_ID(HK_SEARCH_CLEAR, 1), rectf(x + (w - bw) * 0.5f, y + 118, bw, 34), L"Clear search", 0, BTN_SECONDARY);
        return y + h;
    }
    ui_text(L"For an HTML wallpaper, choose its whole folder — the one containing index.html. Screen+ automatically keeps its CSS, "
            L"JavaScript, images, fonts and media available. Files stay where they are.",
            u->fWrapC, rectf(x + w * 0.18f, y + 114, w * 0.64f, 44), t->text2);
    float b1 = ui_button_width(L"Import files", GL_ADD), b2 = ui_button_width(L"Wallpaper folder", GL_FOLDER);
    float bx = x + (w - b1 - b2 - 10) * 0.5f;
    ui_button(HIT_ID(HK_IMPORT, 1), rectf(bx, y + 178, b1, 36), L"Import files", GL_ADD, BTN_PRIMARY);
    ui_button(HIT_ID(HK_IMPORT_FOLDER, 1), rectf(bx + b1 + 10, y + 178, b2, 36), L"Wallpaper folder", GL_FOLDER, BTN_SECONDARY);
    return y + h;
}

static void draw_library(Ui *u, float cx, float cw)
{
    float x = cx + PAD, w = cw - PAD * 2;
    wchar_t sub[64];
    str_printf(sub, SP_COUNTOF(sub), L"%d wallpaper%ls", g_app.cfg.count, g_app.cfg.count == 1 ? L"" : L"s");
    draw_title(u, x, L"Library", sub);
    float by = 24, bh = 34, bx = x + w;
    float iw = ui_button_width(L"Import", GL_ADD);
    bx -= iw;
    ui_button(HIT_ID(HK_IMPORT, 0), rectf(bx, by, iw, bh), L"Import", GL_ADD, BTN_PRIMARY);
    bx -= 8 + 36;
    ui_button(HIT_ID(HK_IMPORT_FOLDER, 0), rectf(bx, by, 36, bh), NULL, GL_FOLDER, BTN_SECONDARY);
    float sw = SP_MIN(260.0f, w * 0.34f);
    bx -= 12 + sw;
    draw_search(u, rectf(bx, by, sw, bh));

    float top = 88;
    top += draw_notice(u, x, top, w);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_LIBRARY];
    y = draw_hero(u, x, y, w);
    y += 24;
    y = draw_filters(u, x, y, w);
    y += 16;
    if (!g_app.cfg.count) y = draw_empty(u, x, y, w, 0);
    else if (!u->viewCount) y = draw_empty(u, x, y, w, 1);
    else y = draw_grid(u, x, y, w);
    y += 28;
    ui_pop_clip();
    finish_scroll(u, PAGE_LIBRARY, y, top);
    draw_scrollbar(u, view, PAGE_LIBRARY);
}

/* ================================================================ displays */

static void draw_layout(Ui *u, float x, float y, float w, float h)
{
    Theme *t = &u->t;
    ui_glass(rectf(x, y, w, h), 18, 0, 0);
    if (!g_app.monCount) return;
    RECT b = g_app.mons[0].rect;
    for (int i = 1; i < g_app.monCount; i++) UnionRect(&b, &b, &g_app.mons[i].rect);
    float bw = (float)(b.right - b.left), bh = (float)(b.bottom - b.top);
    float s = SP_MIN((w - 64) / bw, (h - 56) / bh);
    float ox = x + (w - bw * s) * 0.5f, oy = y + (h - bh * s) * 0.5f;
    for (int i = 0; i < g_app.monCount; i++) {
        const MonitorInfo *m = &g_app.mons[i];
        D2D1_RECT_F r = { ox + (m->rect.left - b.left) * s + 3, oy + (m->rect.top - b.top) * s + 3,
                          ox + (m->rect.right - b.left) * s - 3, oy + (m->rect.bottom - b.top) * s - 3 };
        int id = HIT_ID(HK_MONITOR, i), sel = u->target == i, hot = ui_is_hot(id);
        WallpaperEntry *e = config_find(&g_app.cfg, config_assignment(&g_app.cfg, m->id));
        if (e) ui_draw_thumb(e, r, 8, 0);
        else ui_rrect(r, 8, t->chip);
        ui_rrect_stroke(r, 8, sel ? 2.5f : 1.0f, sel ? t->accent : (hot ? t->text3 : t->border));
        wchar_t num[8];
        str_printf(num, SP_COUNTOF(num), L"%d", m->index);
        ui_circle(r.left + 20, r.bottom - 20, 12, sel ? t->accent : col(0x000000, 0.55f));
        ui_text(num, u->fCenterS, rectf(r.left + 8, r.bottom - 32, 24, 24), col(0xFFFFFF, 1));
        ui_hit(id, r, 1, 1);
        ui_focus_ring(r, 8, id);
    }
}

static void draw_displays(Ui *u, float cx, float cw)
{
    Theme *t = &u->t;
    float x = cx + PAD, w = cw - PAD * 2;
    draw_title(u, x, L"Displays", L"Assign a wallpaper to each display, or use one everywhere");
    float top = 88;
    top += draw_notice(u, x, top, w);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_DISPLAYS];
    draw_layout(u, x, y, w, 230);
    y += 230 + 22;
    for (int i = 0; i < g_app.monCount; i++) {
        const MonitorInfo *m = &g_app.mons[i];
        const IpcMonitorStatus *ms = app_monitor_status(m->id);
        int override = config_has_override(&g_app.cfg, m->id);
        WallpaperEntry *e = config_find(&g_app.cfg, config_assignment(&g_app.cfg, m->id));
        int hasErr = ms && ms->failed && ms->error[0];
        float rh = hasErr ? 104.0f : 84.0f;
        D2D1_RECT_F r = rectf(x, y, w, rh);
        ui_glass(r, 16, u->target == i, 0);
        if (u->target == i) ui_rrect_stroke(r, 16, 1.5f, col_a(t->accent, 0.8f));
        wchar_t num[8];
        str_printf(num, SP_COUNTOF(num), L"%d", m->index);
        ui_circle(x + 34, y + 42, 17, u->target == i ? t->accent : t->chip);
        ui_text(num, u->fCenter, rectf(x + 17, y + 25, 34, 34), u->target == i ? t->onAccent : t->text);
        ui_text(m->name, u->fBodyB, rectf(x + 66, y + 18, 260, 22), t->text);
        wchar_t det[128];
        str_printf(det, SP_COUNTOF(det), L"%ld × %ld · %u Hz · %u%%%ls", m->rect.right - m->rect.left,
                   m->rect.bottom - m->rect.top, m->refreshHz, m->dpi * 100 / 96, m->primary ? L" · Primary" : L"");
        ui_text(det, u->fSmall, rectf(x + 66, y + 42, 300, 20), t->text2);

        float mx = x + SP_MAX(330.0f, w * 0.38f);
        D2D1_RECT_F th = rectf(mx, y + 18, 85, 48);
        if (e) ui_draw_thumb(e, th, 8, 0);
        else ui_rrect(th, 8, t->chip);
        ui_text(e ? e->name : L"No wallpaper", u->fBody, rectf(mx + 98, y + 18, w * 0.24f, 22), t->text);
        D2D1_COLOR_F sc;
        const wchar_t *sl = state_label(ms, &sc);
        wchar_t st[64];
        str_printf(st, SP_COUNTOF(st), L"%ls · %ls", sl, override ? L"custom" : L"uses default");
        ui_circle(mx + 103, y + 52, 3.5f, sc);
        ui_text(st, u->fSmall, rectf(mx + 112, y + 42, w * 0.24f, 20), t->text2);

        float bx = x + w - 16;
        float b1 = ui_button_width(L"Choose…", 0);
        bx -= b1;
        ui_button(HIT_ID(HK_CHANGE, i), rectf(bx, y + 25, b1, 34), L"Choose…", 0, BTN_SECONDARY);
        if (override) {
            float b2 = ui_button_width(L"Use default", 0);
            bx -= b2 + 8;
            ui_button(HIT_ID(HK_USE_DEFAULT, i), rectf(bx, y + 25, b2, 34), L"Use default", 0, BTN_GHOST);
        }
        if (hasErr) ui_text(ms->error, u->fSmall, rectf(x + 66, y + 74, w - 90, 20), t->danger);
        ui_hit(HIT_ID(HK_MONITOR, i), r, 0, 0);
        y += rh + 12;
    }
    y += 16;
    ui_pop_clip();
    finish_scroll(u, PAGE_DISPLAYS, y, top);
    draw_scrollbar(u, view, PAGE_DISPLAYS);
}

/* ================================================================ settings */

static float section(Ui *u, float x, float y, const wchar_t *title)
{
    ui_text(title, u->fCaption, rectf(x + 4, y, 300, 18), u->t.text3);
    return y + 24;
}

static void row_label(Ui *u, float x, float y, float w, const wchar_t *label, const wchar_t *desc)
{
    ui_text(label, u->fBody, rectf(x + 18, y + 11, w, 20), u->t.text);
    if (desc) ui_text(desc, u->fSmall, rectf(x + 18, y + 32, w, 18), u->t.text2);
}

static float toggle_group(Ui *u, float x, float y, float w, const int *idx, const wchar_t *const *labels,
                          const wchar_t *const *descs, const int *values, int n)
{
    ui_glass(rectf(x, y, w, 62.0f * n), 14, 0, 0);
    for (int i = 0; i < n; i++) {
        float ry = y + i * 62.0f;
        if (i) ui_line(x + 16, ry, x + w - 16, ry, 1, u->t.separator);
        row_label(u, x, ry, w - 120, labels[i], descs[i]);
        ui_toggle(HIT_ID(HK_TOGGLE, idx[i]), x + w - 62, ry + 19, idx[i], values[i]);
    }
    return y + 62.0f * n;
}

static float info_row(Ui *u, float x, float y, float w, const wchar_t *k, const wchar_t *v, D2D1_COLOR_F vc)
{
    ui_text(k, u->fBody, rectf(x + 18, y, 160, 40), u->t.text2);
    ui_text(v, u->fBody, rectf(x + 180, y, w - 200, 40), vc);
    return y + 40;
}

static void draw_settings(Ui *u, float cx, float cw)
{
    Theme *t = &u->t;
    Settings *s = &g_app.cfg.s;
    float x = cx + PAD, w = SP_MIN(cw - PAD * 2, 860.0f);
    draw_title(u, x, L"Settings", L"Behaviour, performance and diagnostics");
    float top = 88;
    top += draw_notice(u, x, top, cw - PAD * 2);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_SETTINGS];

    y = section(u, x, y, L"GENERAL");
    {
        int idx[] = { TOG_STARTUP, TOG_TRAY };
        const wchar_t *l[] = { L"Start with Windows", L"Open in the background" };
        const wchar_t *d[] = { L"Launch Screen+ in the notification area when you sign in",
                               L"Keep the window closed at startup; wallpapers still load" };
        int v[] = { s->startWithWindows, s->launchToTray };
        y = toggle_group(u, x, y, w, idx, l, d, v, 2);
    }
    y += 10;
    ui_glass(rectf(x, y, w, 62), 14, 0, 0);
    row_label(u, x, y, w - 300, L"Appearance", L"Follows Windows by default");
    {
        static const wchar_t *opts[] = { L"System", L"Light", L"Dark" };
        float segW = 78, sx = x + w - 18 - segW * 3, sy = y + 15;
        ui_rrect(rectf(sx - 3, sy - 3, segW * 3 + 6, 38), 10, t->chip);
        for (int i = 0; i < 3; i++) {
            int id = HIT_ID(HK_THEME, i), sel = (int)s->theme == i;
            D2D1_RECT_F r = rectf(sx + i * segW, sy, segW, 32);
            if (sel) {
                ui_rrect(r, 8, t->dark ? col(0xFFFFFF, 0.14f) : col(0xFFFFFF, 1.0f));
                ui_rrect_stroke(r, 8, 1, t->border);
            } else if (ui_is_hot(id)) {
                ui_rrect(r, 8, col_a(t->chip, t->chip.a * 1.5f));
            }
            ui_text(opts[i], u->fCenter, r, sel ? t->text : t->text2);
            ui_hit(id, r, 1, 1);
            ui_focus_ring(r, 8, id);
        }
    }
    y += 62 + 26;

    y = section(u, x, y, L"PERFORMANCE");
    {
        int idx[] = { TOG_FULLSCREEN, TOG_COVERED, TOG_LOCK, TOG_BATTERY };
        const wchar_t *l[] = { L"Pause during fullscreen apps", L"Pause hidden wallpapers", L"Suspend while locked",
                               L"Pause on battery power" };
        const wchar_t *d[] = { L"Games and videos get the GPU to themselves",
                               L"Stop rendering on displays covered by maximized or fullscreen windows",
                               L"Freeze rendering on the lock screen and while the display is off",
                               L"Also applies when battery saver is on" };
        int v[] = { s->pauseOnFullscreen, s->pauseWhenCovered, s->suspendOnLock, s->pauseOnBattery };
        y = toggle_group(u, x, y, w, idx, l, d, v, 4);
    }
    y += 26;

    y = section(u, x, y, L"AUDIO & RECOVERY");
    {
        int idx[] = { TOG_AUDIO, TOG_RECOVER };
        const wchar_t *l[] = { L"Play wallpaper audio", L"Restart failed renderers automatically" };
        const wchar_t *d[] = { L"Video and HTML wallpapers are muted by default (primary display only)",
                               L"Up to 3 attempts with backoff, then the Windows wallpaper is shown" };
        int v[] = { s->audio, s->autoRecover };
        y = toggle_group(u, x, y, w, idx, l, d, v, 2);
    }
    y += 10;
    y = section(u, x, y, L"HTML WALLPAPERS");
    {
        int idx[] = { TOG_HTML_MOUSE, TOG_HTML_SPECTRUM, TOG_LIVELY };
        const wchar_t *l[] = { L"Allow desktop mouse interaction", L"Enable audio-reactive spectrum",
                               L"Import Lively wallpapers automatically" };
        const wchar_t *d[] = { L"Forwards mouse activity only while the pointer is over the desktop; keyboard is never shared",
                               L"Captures the default output device only when an HTML wallpaper requests it",
                               L"Adds newly installed Lively Wallpaper packages each time Screen+ starts" };
        int v[] = { s->htmlMouse, s->htmlAudio, s->livelyImport };
        y = toggle_group(u, x, y, w, idx, l, d, v, 3);
    }
    y += 26;
    y = section(u, x, y, L"RENDERER & DIAGNOSTICS");
    ui_glass(rectf(x, y, w, 62), 14, 0, 0);
    {
        wchar_t desc[160];
        if (g_app.engineGaveUp) str_copy(desc, SP_COUNTOF(desc), L"Stopped after repeated failures");
        else if (g_app.engineProcess)
            str_printf(desc, SP_COUNTOF(desc), L"Running (pid %lu)%ls · %d restart%ls", g_app.enginePid,
                       g_app.safeMode ? L" in safe mode" : L"", g_app.engineRestarts, g_app.engineRestarts == 1 ? L"" : L"s");
        else str_copy(desc, SP_COUNTOF(desc), L"Starting");
        row_label(u, x, y, w - 220, L"Renderer process", desc);
        float bw = ui_button_width(L"Restart renderer", GL_REFRESH);
        ui_button(HIT_ID(HK_RESTART_ENGINE, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Restart renderer", GL_REFRESH, BTN_SECONDARY);
    }
    y += 62 + 26;

    y = section(u, x, y, L"DIAGNOSTICS");
    {
        float rows = 7;
        ui_glass(rectf(x, y, w, rows * 40 + 64), 14, 0, 0);
        float ry = y + 6;
        wchar_t v[256], a[32], b[32];
        str_printf(v, SP_COUNTOF(v), L"Native %.0f ms · UI %ls · Wallpaper %ls", g_app.nativeInitMs,
                   g_app.uiReadyMs > 0 ? L"" : L"–", g_app.wallpaperMs > 0 ? L"" : L"–");
        if (g_app.uiReadyMs > 0 || g_app.wallpaperMs > 0) {
            wchar_t ui[24] = L"–", wp[24] = L"–";
            if (g_app.uiReadyMs > 0) str_printf(ui, SP_COUNTOF(ui), L"%.0f ms", g_app.uiReadyMs);
            if (g_app.wallpaperMs > 0) str_printf(wp, SP_COUNTOF(wp), L"%.0f ms", g_app.wallpaperMs);
            str_printf(v, SP_COUNTOF(v), L"Native %.0f ms · UI %ls · Wallpaper %ls", g_app.nativeInitMs, ui, wp);
        }
        ry = info_row(u, x, ry, w, L"Startup", v, t->text);
        fmt_bytes(a, SP_COUNTOF(a), g_app.shellMem);
        fmt_bytes(b, SP_COUNTOF(b), g_app.engineMem);
        str_printf(v, SP_COUNTOF(v), L"Shell %.1f%% CPU, %ls · Renderer %.1f%% CPU, %ls (%d process%ls)", g_app.shellCpu, a,
                   g_app.engineCpu, b, g_app.engineProcs, g_app.engineProcs == 1 ? L"" : L"es");
        ry = info_row(u, x, ry, w, L"Resources", v, t->text);
        str_printf(v, SP_COUNTOF(v), L"%d active renderer%ls · desktop host: %ls",
                   g_app.haveStatus ? g_app.status.activeRenderers : 0,
                   g_app.haveStatus && g_app.status.activeRenderers == 1 ? L"" : L"s",
                   g_app.haveStatus ? desktop_host_mode_name(g_app.status.hostMode) : L"–");
        ry = info_row(u, x, ry, w, L"Rendering", v, t->text);
        wchar_t ff[64];
        video_ffmpeg_version(ff, SP_COUNTOF(ff));
        str_printf(v, SP_COUNTOF(v), L"%ls · Direct3D 11 · D3D11VA hardware decoding when supported", ff);
        ry = info_row(u, x, ry, w, L"Video", v, t->text);
        if (g_app.haveStatus && g_app.status.webviewVersion[0]) {
            str_printf(v, SP_COUNTOF(v), L"Runtime %ls", g_app.status.webviewVersion);
            ry = info_row(u, x, ry, w, L"WebView2", v, t->text);
        } else {
            ry = info_row(u, x, ry, w, L"WebView2", g_app.haveStatus ? L"Not installed — required for HTML wallpapers" : L"–",
                          g_app.haveStatus ? t->warning : t->text2);
            if (g_app.haveStatus) {
                float bw = ui_button_width(L"Get WebView2", 0);
                ui_button(HIT_ID(HK_GET_WEBVIEW, 0), rectf(x + w - 18 - bw, ry - 37, bw, 32), L"Get WebView2", 0, BTN_SECONDARY);
            }
        }
        const wchar_t *err = g_app.engineError[0] ? g_app.engineError : (g_app.haveStatus && g_app.status.lastError[0] ? g_app.status.lastError : L"None");
        ry = info_row(u, x, ry, w, L"Last failure", err, wcscmp(err, L"None") == 0 ? t->text2 : t->danger);
        {
            wchar_t flags[160] = L"";
            uint32_t f = g_app.haveStatus ? g_app.status.flags : 0;
            str_printf(flags, SP_COUNTOF(flags), L"%ls%ls%ls%ls%ls", f & ENGINE_FLAG_BATTERY ? L"On battery  " : L"On AC power  ",
                       f & ENGINE_FLAG_FULLSCREEN ? L"· Fullscreen app  " : L"", f & ENGINE_FLAG_LOCKED ? L"· Locked  " : L"",
                       f & ENGINE_FLAG_DISPLAY_OFF ? L"· Display off  " : L"", f & ENGINE_FLAG_SAFE_MODE ? L"· Safe mode" : L"");
            ry = info_row(u, x, ry, w, L"Conditions", flags, t->text2);
        }
        float bx = x + 18;
        float b1 = ui_button_width(L"Open logs", GL_DOC);
        ui_button(HIT_ID(HK_OPEN_LOGS, 0), rectf(bx, ry + 10, b1, 34), L"Open logs", GL_DOC, BTN_SECONDARY);
        bx += b1 + 8;
        float b2 = ui_button_width(L"Open data folder", GL_FOLDER);
        ui_button(HIT_ID(HK_OPEN_DATA, 0), rectf(bx, ry + 10, b2, 34), L"Open data folder", GL_FOLDER, BTN_SECONDARY);
        y += rows * 40 + 64;
    }
    y += 26;

    y = section(u, x, y, L"ABOUT");
    ui_glass(rectf(x, y, w, 62), 14, 0, 0);
    row_label(u, x, y, w - 240, L"Screen+ " SP_VERSION_W, L"Native C wallpaper engine · Win32, Direct3D 11, FFmpeg, WebView2");
    {
        float bw = ui_button_width(L"Third-party notices", 0);
        ui_button(HIT_ID(HK_LICENSES, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Third-party notices", 0, BTN_GHOST);
    }
    y += 62 + 32;
    ui_pop_clip();
    finish_scroll(u, PAGE_SETTINGS, y, top);
    draw_scrollbar(u, view, PAGE_SETTINGS);
}

/* ================================================================ dispatch */

void pages_draw(void)
{
    Ui *u = g_ui;
    draw_sidebar(u);
    float cx = SIDEBAR_W, cw = u->w - SIDEBAR_W;
    float p = u->pageAnim;
    u->alpha = p * p * (3.0f - 2.0f * p);     /* smoothstep fade-in on page switch */
    switch (u->page) {
    case PAGE_DISPLAYS: draw_displays(u, cx, cw); break;
    case PAGE_SETTINGS: draw_settings(u, cx, cw); break;
    default:            draw_library(u, cx, cw); break;
    }
    u->alpha = 1.0f;
}

static void toggle_setting(int idx)
{
    Settings *s = &g_app.cfg.s;
    switch (idx) {
    case TOG_STARTUP:
        if (startup_set_enabled(!s->startWithWindows)) s->startWithWindows = !s->startWithWindows;
        else app_notice(NOTICE_ERROR, L"Could not update the Windows startup entry.");
        break;
    case TOG_TRAY:       s->launchToTray = !s->launchToTray; break;
    case TOG_FULLSCREEN: s->pauseOnFullscreen = !s->pauseOnFullscreen; break;
    case TOG_COVERED:    s->pauseWhenCovered = !s->pauseWhenCovered; break;
    case TOG_LOCK:       s->suspendOnLock = !s->suspendOnLock; break;
    case TOG_BATTERY:    s->pauseOnBattery = !s->pauseOnBattery; break;
    case TOG_RECOVER:    s->autoRecover = !s->autoRecover; break;
    case TOG_HTML_MOUSE: s->htmlMouse = !s->htmlMouse; break;
    case TOG_HTML_SPECTRUM: s->htmlAudio = !s->htmlAudio; break;
    case TOG_LIVELY:     s->livelyImport = !s->livelyImport; break;
    case TOG_AUDIO:      app_set_audio(!s->audio); ui_animate(); return;
    default: return;
    }
    app_save_config();
    ui_animate();
}

void pages_activate(int id, int dbl)
{
    Ui *u = g_ui;
    int kind = HIT_KIND(id), data = HIT_DATA(id);
    (void)dbl;
    switch (kind) {
    case HK_NAV:
        if (u->page != data) {
            u->page = data;
            u->pageAnim = 0.0f;
            ui_animate();
        }
        u->searchFocus = 0;
        break;
    case HK_IMPORT:        library_import_dialog(u->hwnd, FALSE); break;
    case HK_IMPORT_FOLDER: library_import_dialog(u->hwnd, TRUE); break;
    case HK_SEARCH_CLEAR:
        u->search[0] = 0;
        pages_rebuild_view();
        break;
    case HK_FILTER:
        u->filter = data;
        pages_rebuild_view();
        break;
    case HK_CARD:
        if (data >= 0 && data < u->viewCount) {
            const WallpaperEntry *e = &g_app.cfg.items[u->view[data]];
            if (e->missing) app_notice(NOTICE_WARN, L"'%ls' can't be shown because the file is missing.", e->name);
            else app_apply_wallpaper(e->id, target_id());
        }
        break;
    case HK_TARGET:
    case HK_MONITOR:
        u->target = data;
        break;
    case HK_PLAYPAUSE:     app_set_paused(!g_app.cfg.s.paused); break;
    case HK_AUDIO:         app_set_audio(!g_app.cfg.s.audio); break;
    case HK_RESTART_CURRENT:
    case HK_RESTART_ENGINE: app_restart_renderer(); break;
    case HK_REVEAL: {
        WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(target_id()));
        if (e) shell_reveal(e->path);
        break;
    }
    case HK_TOGGLE:        toggle_setting(data); break;
    case HK_THEME:
        g_app.cfg.s.theme = (ThemeMode)data;
        app_save_config();
        ui_refresh_theme();
        break;
    case HK_OPEN_LOGS:     shell_open(paths_logs()); break;
    case HK_OPEN_DATA:     shell_open(paths_root()); break;
    case HK_NOTICE_CLOSE:  app_notice(NOTICE_NONE, NULL); break;
    case HK_CHANGE:
        u->target = data;
        u->page = PAGE_LIBRARY;
        u->pageAnim = 0.0f;
        ui_animate();
        break;
    case HK_USE_DEFAULT:
        if (data >= 0 && data < g_app.monCount) {
            config_clear_override(&g_app.cfg, g_app.mons[data].id);
            app_save_config();
        }
        break;
    case HK_GET_WEBVIEW:   shell_open(L"https://go.microsoft.com/fwlink/p/?LinkId=2124703"); break;
    case HK_LICENSES: {
        wchar_t p[SP_MAX_PATH];
        path_join(p, SP_COUNTOF(p), paths_exe_dir(), L"licenses");
        shell_open(p);
        break;
    }
    default: break;
    }
    ui_invalidate();
}

void pages_context(int id, int sx, int sy)
{
    Ui *u = g_ui;
    if (HIT_KIND(id) != HK_CARD) return;
    int vi = HIT_DATA(id);
    if (vi < 0 || vi >= u->viewCount) return;
    WallpaperEntry e = g_app.cfg.items[u->view[vi]];   /* copy: the library may change */
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 1, L"Apply to all displays");
    for (int i = 0; i < g_app.monCount && i < 16; i++) {
        wchar_t l[96];
        str_printf(l, SP_COUNTOF(l), L"Apply to display %d (%.40ls)", g_app.mons[i].index, g_app.mons[i].name);
        AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 100 + (UINT)i, l);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 2, L"Show in File Explorer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Remove from library (keeps the file)");
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, u->hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) app_apply_wallpaper(e.id, NULL);
    else if (cmd >= 100 && cmd < 116) app_apply_wallpaper(e.id, g_app.mons[cmd - 100].id);
    else if (cmd == 2) shell_reveal(e.path);
    else if (cmd == 3) library_remove(e.id);
    ui_invalidate();
}

void pages_scroll_into_view(int id)
{
    Ui *u = g_ui;
    int page = u->page;
    float top = 0, bottom = 0;
    if (HIT_KIND(id) == HK_CARD && page == PAGE_LIBRARY && u->cols > 0 && u->rowH > 0) {
        int row = HIT_DATA(id) / u->cols;
        float ct = u->gridY + row * u->rowH;      /* content coordinates */
        top = ct - u->scroll[page];
        bottom = top + u->rowH;
    } else {
        int found = 0;
        for (int i = 0; i < u->hitCount; i++)
            if (u->hits[i].id == id) { top = u->hits[i].r.top; bottom = u->hits[i].r.bottom; found = 1; break; }
        if (!found) return;
    }
    float maxScroll = SP_MAX(0.0f, u->contentH[page] - u->viewH);
    if (top < u->viewTop + 8) u->scrollTarget[page] = SP_CLAMP(u->scroll[page] - (u->viewTop + 8 - top), 0.0f, maxScroll);
    else if (bottom > u->h - 8) u->scrollTarget[page] = SP_CLAMP(u->scroll[page] + (bottom - (u->h - 8)), 0.0f, maxScroll);
    ui_animate();
}

int pages_key(UINT vk, int ctrl, int shift)
{
    Ui *u = g_ui;
    (void)ctrl; (void)shift;
    if (HIT_KIND(u->focus) != HK_CARD || u->page != PAGE_LIBRARY) {
        if (vk == VK_DOWN || vk == VK_UP || vk == VK_NEXT || vk == VK_PRIOR) {
            float d = vk == VK_DOWN ? 60.0f : vk == VK_UP ? -60.0f : vk == VK_NEXT ? u->viewH * 0.9f : -u->viewH * 0.9f;
            float maxScroll = SP_MAX(0.0f, u->contentH[u->page] - u->viewH);
            u->scrollTarget[u->page] = SP_CLAMP(u->scrollTarget[u->page] + d, 0.0f, maxScroll);
            ui_animate();
            return 1;
        }
        return 0;
    }
    int i = HIT_DATA(u->focus), n = u->viewCount, c = SP_MAX(u->cols, 1);
    switch (vk) {
    case VK_LEFT:  i--; break;
    case VK_RIGHT: i++; break;
    case VK_UP:    i -= c; break;
    case VK_DOWN:  i += c; break;
    case VK_HOME:  i = 0; break;
    case VK_END:   i = n - 1; break;
    case VK_DELETE:
        if (i >= 0 && i < n) library_remove(g_app.cfg.items[u->view[i]].id);
        return 1;
    default: return 0;
    }
    i = SP_CLAMP(i, 0, n - 1);
    u->focus = HIT_ID(HK_CARD, i);
    u->focusVisible = 1;
    pages_scroll_into_view(u->focus);
    ui_invalidate();
    return 1;
}
