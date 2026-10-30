/* Page content: sidebar, Library (hero + grid), Displays, Customise, Store, Settings.
 * Immediate-mode: layout is recomputed on every paint (paints only happen on change). */
#include "ui_internal.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/json.h"
#include "../renderer/renderer.h"
#include "../desktop/desktop_host.h"
#include "../common/scrnsave.h"
#include <shellapi.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <windowsx.h>
#include <commdlg.h>
#include <colordlg.h>

#define SIDEBAR_W 232.0f
#define PAD       32.0f

static const wchar_t *k_filters[] = { L"All", L"Images", L"Videos", L"HTML", L"Recent" };

/* ================================================================ helpers */

static const wchar_t *target_id(void)
{
    Ui *u = g_ui;
    if (g_app.cfg.s.displayMode != DM_INDEPENDENT) return NULL;   /* duplicate/span: one wallpaper for all */
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

    static const wchar_t *labels[] = { L"Library", L"Displays", L"Customise", L"Store", L"Settings" };
    static const wchar_t icons[] = { GL_LIBRARY, GL_DISPLAY, GL_COLOR, GL_STORE, GL_SETTINGS };
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
    float ch = 70, cy = u->h - ch - 16;
    D2D1_RECT_F card = rectf(12, cy, SIDEBAR_W - 24, ch);
    ui_glass(card, 14, 0, 0);
    ui_circle(30, cy + 24, 4.5f, dot);
    ui_text(state, u->fSmallB, rectf(42, cy + 13, SIDEBAR_W - 70, 22), t->text);
    wchar_t line[128], mem[32];
    fmt_bytes(mem, SP_COUNTOF(mem), g_app.shellMem + g_app.engineMem);
    str_printf(line, SP_COUNTOF(line), L"CPU %.1f%% · RAM %ls", g_app.shellCpu + g_app.engineCpu, mem);
    ui_text(line, u->fSmall, rectf(24, cy + 38, SIDEBAR_W - 48, 18), t->text2);
}

/* ================================================================ shared page chrome */

static void draw_title(Ui *u, float x, const wchar_t *title, const wchar_t *subtitle)
{
    ui_text(title, u->fTitle, rectf(x, 16, 420, 42), u->t.text);
    if (subtitle) ui_text(subtitle, u->fSmall, rectf(x + 1, 56, 420, 18), u->t.text2);
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

/* Import from a web link: the link button opens this panel above the library (url_import.c does the work). */
static struct {
    int     open;
    int     mode;               /* URLMODE_* */
    int     seen;               /* UrlImportState.finished already handled */
    wchar_t url[SP_MAX_PATH];
} urlp;

static float draw_url_panel(Ui *u, float x, float y, float w);

/* A search box (the Library's, the Store's): text holds what was typed; the engine draws it while editing. */
static void draw_search(Ui *u, D2D1_RECT_F r, int id, int clearId, const wchar_t *text, const wchar_t *hint)
{
    Theme *t = &u->t;
    int editing = u->textFocus == id;
    float bh = r.bottom - r.top;
    ui_rrect(r, 9, t->field);
    ui_rrect_stroke(r, 9, editing ? 1.5f : 1.0f, editing ? t->accent : t->border);
    ui_icon(GL_SEARCH, u->fIconS, rectf(r.left + 8, r.top, 22, bh), t->text3);
    float tx = r.left + 34, tw = r.right - r.left - 34 - (text[0] ? 32 : 10);
    if (!text[0] && !editing) ui_text(hint, u->fBody, rectf(tx, r.top, tw, bh), t->text3);
    else if (editing) {
        D2D1_RECT_F tr = rectf(tx, r.top + (bh - 20) * 0.5f, tw, 20);
        te_set_layout(u->fBody, tr);
        te_draw(tr, t->text, col_a(t->accent, 0.35f), t->accent, u->caretOn && GetForegroundWindow() == u->hwnd);
    } else {
        ui_text(text, u->fBody, rectf(tx, r.top, tw, bh), t->text);
    }
    ui_hit(id, r, 1, 2);
    if (text[0]) ui_button(clearId, rectf(r.right - 30, r.top + 4, 26, bh - 8), NULL, GL_CLOSE, BTN_GHOST);
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
    else if (g_app.monCount > 1 && g_app.cfg.s.displayMode == DM_SPAN)
        str_printf(cap, SP_COUNTOF(cap), L"NOW SHOWING  ·  SPANNED ACROSS %d DISPLAYS", g_app.monCount);
    else str_printf(cap, SP_COUNTOF(cap), L"NOW SHOWING  ·  %ls", g_app.monCount > 1 ? L"ALL DISPLAYS" : L"DESKTOP");
    ui_text(cap, u->fCaption, rectf(rx, ry, rw, 16), t->text3);
    ry += 20;
    ui_text(e ? e->name : L"Choose a wallpaper from your library", u->fHeading, rectf(rx, ry, rw, 30), t->text);
    ry += 36;

    /* type badge + renderer detail from the engine */
    const IpcMonitorStatus *ms = NULL;
    for (int i = 0; e && i < (g_app.haveStatus ? g_app.status.monitorCount : 0); i++) {
        const IpcMonitorStatus *m = &g_app.status.monitors[i];
        if (wcscmp(m->wallpaperId, e->id) == 0 && (!mid || str_ieq(m->monitorId, mid) || wcscmp(m->monitorId, SP_SPAN_ID) == 0)) {
            ms = m;
            break;
        }
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
    for (int i = 0; i < g_app.monCount && g_app.cfg.s.displayMode == DM_INDEPENDENT; i++) {
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

static void draw_card_name(Ui *u, int vi, const WallpaperEntry *e, D2D1_RECT_F nr);

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
    wchar_t meta[96], ext[16];
    str_copy(ext, SP_COUNTOF(ext), path_ext(e->path)[0] ? path_ext(e->path) + 1 : L"file");
    CharUpperW(ext);
    if (wallpaper_is_url(e->path)) {               /* a live page or a stream: where it comes from */
        const wchar_t *host = wcsstr(e->path, L"://") + 3;
        if (str_istarts(host, L"www.")) host += 4;
        str_printf(meta, SP_COUNTOF(meta), L"%ls · %.*ls", e->type == WP_VIDEO ? L"Stream" : L"HTML",
                   (int)SP_MIN(wcscspn(host, L"/?#:"), 60), host);
    } else if (e->type == WP_HTML) str_printf(meta, SP_COUNTOF(meta), L"HTML · %ls", path_filename(e->path));
    else str_printf(meta, SP_COUNTOF(meta), L"%ls · %ls", wallpaper_type_name(e->type), ext);
    ui_text(meta, u->fSmall, rectf(r.left + 12, th.bottom + 28, r.right - r.left - 24, 16), t->text3);
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, 14, id);

    /* the name is its own hit, registered after the card's so it wins: double-click renames */
    draw_card_name(u, vi, e, rectf(r.left + 12, th.bottom + 8, r.right - r.left - 24, 20));
    ui_hit(HIT_ID(HK_NAME, vi), rectf(r.left + 10, th.bottom + 6, r.right - r.left - 20, 24), 0, 2);
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
    float h = 170.0f;
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
    float b1 = ui_button_width(L"Import files", GL_ADD), b2 = ui_button_width(L"Wallpaper folder", GL_FOLDER);
    float bx = x + (w - b1 - b2 - 10) * 0.5f;
    ui_button(HIT_ID(HK_IMPORT, 1), rectf(bx, y + 116, b1, 36), L"Import files", GL_ADD, BTN_PRIMARY);
    ui_button(HIT_ID(HK_IMPORT_FOLDER, 1), rectf(bx + b1 + 10, y + 116, b2, 36), L"Wallpaper folder", GL_FOLDER, BTN_SECONDARY);
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
    if (g_app.cfg.s.urlImport) {
        bx -= 8 + 36;
        ui_button(HIT_ID(HK_URL, 0), rectf(bx, by, 36, bh), NULL, GL_LINK, BTN_SECONDARY);
    }
    float sw = SP_MIN(260.0f, w * 0.34f);
    bx -= 12 + sw;
    draw_search(u, rectf(bx, by, sw, bh), HIT_ID(HK_SEARCH, 0), HIT_ID(HK_SEARCH_CLEAR, 0), u->search, L"Search wallpapers");

    float top = 88;
    top += draw_url_panel(u, x, top, w);
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

/* ================================================================ store */

/* MotionBGS wallpapers (store.c reads the site). The search box has its own buffer and searches as you type,
 * once typing pauses (TM_STORE); Enter searches at once. */
static struct {
    wchar_t text[96];
} storep;

static D2D1_RECT_F hit_rect(int id);
static void draw_choice(Ui *u, int id, D2D1_RECT_F r, const wchar_t *text);

static void store_query_changed(void)
{
    if (g_ui) SetTimer(g_ui->hwnd, TM_STORE, 250, NULL);
}

void pages_store_timer(void)
{
    Ui *u = g_ui;
    if (u) KillTimer(u->hwnd, TM_STORE);
    if (store_search(storep.text) && u) u->scrollTarget[PAGE_STORE] = u->scroll[PAGE_STORE] = 0.0f;
    ui_invalidate();
}

void pages_store_search_open(void)
{
    Ui *u = g_ui;
    int id = HIT_ID(HK_STORE_TEXT, 0);
    if (u->textFocus != id) {
        if (u->textFocus) pages_text_end(1);
        u->textFocus = u->focus = id;
        u->searchFocus = 0;
        te_open(storep.text, SP_COUNTOF(storep.text), (int)wcslen(storep.text), 0, NULL, store_query_changed);
    }
    u->caretOn = 1;                      /* clicked again while typing: the caret keeps blinking */
    SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
    ui_invalidate();
}

typedef struct StoreGrid {
    int   cols;
    float gap, cw, thumbH, ch;
} StoreGrid;

static StoreGrid store_grid(float w)
{
    StoreGrid g;
    g.gap = 18;
    g.cols = SP_MAX(1, (int)((w + g.gap) / (210 + g.gap)));
    g.cw = (w - g.gap * (g.cols - 1)) / g.cols;
    g.thumbH = (g.cw - 12) * 9.0f / 16.0f;
    g.ch = g.thumbH + 100;               /* name, resolution, Download, and the button's 10 px inset below it */
    return g;
}

static D2D1_RECT_F store_cell(const StoreGrid *g, float x, float y, int i)
{
    return rectf(x + (i % g->cols) * (g->cw + g->gap), y + (i / g->cols) * (g->ch + g->gap), g->cw, g->ch);
}

static void draw_store_card(Ui *u, const StoreItem *it, int i, D2D1_RECT_F r, float thumbH)
{
    Theme *t = &u->t;
    int id = HIT_ID(HK_STORE_CARD, i), hot = ui_is_hot(id);
    ui_glass(r, 14, hot, hot);
    D2D1_RECT_F th = { r.left + 6, r.top + 6, r.right - 6, r.top + 6 + thumbH };
    WallpaperEntry e;
    if (store_preview_entry(i, &e)) ui_draw_thumb(&e, th, 10, 0);
    else {
        ui_rrect(th, 10, t->chip);
        if (it->preview == STORE_FAILED) ui_icon(GL_PHOTO, u->fIcon, th, t->text3);
    }
    {   /* a live wallpaper or a still picture, and its quality */
        const wchar_t *label = it->badge[0] ? it->badge : it->kind == SKIND_LIVE ? L"LIVE" : L"PHOTO";
        float bw = ui_text_width(label, u->fCenterS) + 34;
        D2D1_RECT_F b = { th.left + 8, th.bottom - 30, th.left + 8 + bw, th.bottom - 8 };
        ui_rrect(b, 11, col(0x000000, 0.48f));
        ui_icon(it->kind == SKIND_LIVE ? GL_VIDEO : GL_PHOTO, u->fIconS, rectf(b.left + 6, b.top, 16, 22), col(0xFFFFFF, 0.95f));
        ui_text(label, u->fCenterS, rectf(b.left + 20, b.top, bw - 24, 22), col(0xFFFFFF, 0.95f));
    }
    if (hot) {                           /* the preview opens the wallpaper's page */
        ui_circle(th.right - 20, th.top + 20, 14, col(0x000000, 0.48f));
        ui_icon(GL_OPEN, u->fIconS, rectf(th.right - 34, th.top + 6, 28, 28), col(0xFFFFFF, 0.95f));
    }
    ui_hit(id, th, 1, 1);
    ui_focus_ring(th, 10, id);
    ui_text(it->title, u->fBodyB, rectf(r.left + 12, th.bottom + 8, r.right - r.left - 24, 20), t->text);

    wchar_t meta[128], res[48] = L"";            /* where it is from, and what is known of its file */
    const StoreVariant *v = store_best(it);
    int rw = it->w, rh = it->h;
    float mb = v && v->mb > 0 ? v->mb : it->mb;
    if (v && (int64_t)v->w * v->h > (int64_t)rw * rh) {
        rw = v->w;
        rh = v->h;
    }
    if (rw > 0 && mb > 0) str_printf(res, SP_COUNTOF(res), L" · %d × %d · %.1f MB", rw, rh, mb);
    else if (rw > 0) str_printf(res, SP_COUNTOF(res), L" · %d × %d", rw, rh);
    str_printf(meta, SP_COUNTOF(meta), L"%ls%ls", store_source_name(it->source), res);
    ui_text(meta, u->fSmall, rectf(r.left + 12, th.bottom + 28, r.right - r.left - 24, 16), t->text3);

    int pct, style = BTN_SECONDARY;
    wchar_t label[40], icon = 0;
    switch (store_dl_state(i, &pct)) {
    case STOREDL_WAIT:
        str_copy(label, SP_COUNTOF(label), it->detail == STORE_READY ? L"Queued" : L"Preparing…");
        break;
    case STOREDL_RUNNING:
        if (pct >= 0) str_printf(label, SP_COUNTOF(label), L"Downloading %d%%", pct);
        else str_copy(label, SP_COUNTOF(label), L"Downloading…");
        break;
    case STOREDL_DONE:
        str_copy(label, SP_COUNTOF(label), L"In library");
        icon = GL_CHECK;
        style = BTN_GHOST;
        break;
    case STOREDL_FAILED:
        str_copy(label, SP_COUNTOF(label), L"Try again");
        icon = GL_REFRESH;
        break;
    case STOREDL_SITE:                   /* the site hands its files out only on its own page */
        str_printf(label, SP_COUNTOF(label), L"Open on %ls", store_source_name(it->source));
        icon = GL_OPEN;
        break;
    default:
        str_copy(label, SP_COUNTOF(label), L"Download");
        icon = GL_DOWNLOAD;
        style = BTN_PRIMARY;
        break;
    }
    ui_button(HIT_ID(HK_STORE_DL, i), rectf(r.left + 10, th.bottom + 52, r.right - r.left - 20, 32), label, icon, style);
}

static float draw_store_grid(Ui *u, const StoreState *s, float x, float y, float w)
{
    StoreGrid g = store_grid(w);
    float ahead = u->viewH;              /* previews load a screen before their cards scroll in */
    int pw = (int)((g.cw - 12) * u->scale);
    /* the newest request is served first: the cards near the screen go in, then those on it, top left last */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = s->viewCount - 1; k >= 0; k--) {
            D2D1_RECT_F r = store_cell(&g, x, y, k);
            int seen = r.bottom >= u->viewTop - 8 && r.top <= u->h + 8, i = s->view[k];
            if (seen != pass || r.bottom < u->viewTop - ahead || r.top > u->h + ahead) continue;
            store_want(i, pw, seen);
            if (seen) draw_store_card(u, &s->items[i], i, r, g.thumbH);
        }
    }
    int rows = (s->viewCount + g.cols - 1) / g.cols;
    return y + rows * (g.ch + g.gap) - g.gap;
}

static float draw_store_skeleton(Ui *u, float x, float y, float w)
{
    Theme *t = &u->t;
    StoreGrid g = store_grid(w);
    for (int i = 0; i < g.cols * 2; i++) {
        D2D1_RECT_F r = store_cell(&g, x, y, i);
        ui_glass(r, 14, 0, 0);
        ui_rrect(rectf(r.left + 6, r.top + 6, g.cw - 12, g.thumbH), 10, t->chip);
        ui_rrect(rectf(r.left + 12, r.top + g.thumbH + 18, g.cw * 0.55f, 10), 5, t->chip);
        ui_rrect(rectf(r.left + 12, r.top + g.thumbH + 38, g.cw * 0.35f, 8), 4, t->chip);
        ui_rrect(rectf(r.left + 10, r.top + g.thumbH + 58, g.cw - 20, 32), 8, t->chip);
    }
    return y + 2 * (g.ch + g.gap) - g.gap;
}

static float draw_store_panel(Ui *u, float x, float y, float w, wchar_t glyph, D2D1_COLOR_F gc, const wchar_t *title,
                              const wchar_t *text, int buttonId, const wchar_t *button)
{
    Theme *t = &u->t;
    /* grows with the message, if there is one */
    float textH = text ? ui_text_height(text, u->fWrapC, w * 0.7f) + 14.0f : 0.0f, h = 170.0f + textH;
    ui_glass(rectf(x, y, w, h), 18, 0, 0);
    ui_icon(glyph, u->fIconL, rectf(x, y + 22, w, 50), gc);
    float tw = SP_MIN(ui_text_width(title, u->fHeading), w - 24);
    ui_text(title, u->fHeading, rectf(x + (w - tw) * 0.5f, y + 76, tw + 4, 30), t->text);
    if (text) ui_text(text, u->fWrapC, rectf(x + w * 0.15f, y + 110, w * 0.7f, textH), t->text2);
    float bw = ui_button_width(button, 0);
    ui_button(buttonId, rectf(x + (w - bw) * 0.5f, y + 118 + textH, bw, 34), button, 0, BTN_SECONDARY);
    return y + h;
}

static void draw_store(Ui *u, float cx, float cw)
{
    Theme *t = &u->t;
    float x = cx + PAD, w = cw - PAD * 2;
    store_open();
    const StoreState *s = store_get();
    static const wchar_t *const captions[SF_COUNT] = { L"Category", L"Resolution", L"Sort by", L"Type", L"Site" };
    const wchar_t *names[32];
    int sites = store_filter_names(SF_SOURCE, names, SP_COUNTOF(names)) - 1;
    wchar_t sub[200];
    if (s->loading && !s->viewCount) str_copy(sub, SP_COUNTOF(sub), L"Loading…");
    else if (!s->query[0]) str_printf(sub, SP_COUNTOF(sub), L"Wallpapers from %d sites", sites);
    else str_printf(sub, SP_COUNTOF(sub), L"%d%ls result%ls for '%.60ls'", s->viewCount, s->more ? L"+" : L"",
                    s->viewCount == 1 && !s->more ? L"" : L"s", s->query);
    draw_title(u, x, L"Store", sub);
    draw_search(u, rectf(x, 86, w, 38), HIT_ID(HK_STORE_TEXT, 0), HIT_ID(HK_STORE_CLEAR, 0), storep.text,
                L"Search wallpapers");
    float fw = (w - 12.0f * (SF_COUNT - 1)) / SF_COUNT;
    for (int f = 0; f < SF_COUNT; f++) {
        int n = store_filter_names(f, names, SP_COUNTOF(names)), v = s->filter[f];
        float fx = x + f * (fw + 12.0f);
        ui_text(captions[f], u->fSmall, rectf(fx + 2, 134, fw - 4, 18), t->text2);
        draw_choice(u, HIT_ID(HK_STORE_FILTER, f), rectf(fx, 154, fw, 36), v >= 0 && v < n ? names[v] : L"");
    }

    float top = 204;
    top += draw_notice(u, x, top, w);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_STORE], end = y;
    int filtered = s->filter[SF_CATEGORY] || s->filter[SF_RES] || s->filter[SF_KIND] || s->filter[SF_SOURCE];
    if (s->viewCount) {
        y = end = draw_store_grid(u, s, x, y, w);
        if (s->loading) {
            ui_text(L"Loading…", u->fCenter, rectf(x, y + 16, w, 24), t->text3);
            y += 44;
        }
    } else if (s->loading || s->more) {
        y = draw_store_skeleton(u, x, y, w);
    } else if (s->failed) {
        y = draw_store_panel(u, x, y, w, GL_ERROR, t->danger, L"Couldn't reach the wallpaper sites", s->error,
                             HIT_ID(HK_STORE_RETRY, 0), L"Try again");
    } else if (s->query[0]) {
        wchar_t title[160];
        str_printf(title, SP_COUNTOF(title), L"No results for '%.50ls'", s->query);
        y = draw_store_panel(u, x, y, w, GL_SEARCH, t->text3, title, NULL, HIT_ID(HK_STORE_CLEAR, 1), L"Clear search");
    } else if (filtered) {
        y = draw_store_panel(u, x, y, w, GL_SEARCH, t->text3, L"Nothing matches these filters", NULL,
                             HIT_ID(HK_STORE_CLEAR, 2), L"Reset filters");
    } else {
        y = draw_store_panel(u, x, y, w, GL_STORE, t->text3, L"No wallpapers found", NULL, HIT_ID(HK_STORE_RETRY, 0), L"Try again");
    }
    y += 28;
    ui_pop_clip();
    finish_scroll(u, PAGE_STORE, y, top);
    draw_scrollbar(u, view, PAGE_STORE);
    if (end < u->h + u->viewH * 1.5f) store_more();   /* the next pages are in before the end is reached */
}

/* a filter's choices in a menu under its field (Customise's choice fields do the same) */
static void store_filter_menu(int which)
{
    Ui *u = g_ui;
    const wchar_t *names[32];
    int n = store_filter_names(which, names, SP_COUNTOF(names)), cur = store_get()->filter[which];
    HMENU m = n > 0 ? CreatePopupMenu() : NULL;
    if (!m) return;
    for (int i = 0; i < n; i++) AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), (UINT_PTR)i + 1, names[i]);
    D2D1_RECT_F r = hit_rect(HIT_ID(HK_STORE_FILTER, which));
    POINT p = { (LONG)(r.left * u->scale), (LONG)((r.bottom + 4.0f) * u->scale) };
    ClientToScreen(u->hwnd, &p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, u->hwnd, NULL);
    DestroyMenu(m);
    if (cmd && (int)cmd - 1 != cur) {
        store_set_filter(which, (int)cmd - 1);
        u->scrollTarget[PAGE_STORE] = u->scroll[PAGE_STORE] = 0.0f;
    }
}

/* the Download button's tooltip: what the page offers, or how to stop it */
static const wchar_t *store_dl_tip(int i)
{
    static wchar_t tip[200];
    const StoreState *s = store_get();
    if (i < 0 || i >= s->count) return NULL;
    int st = store_dl_state(i, NULL);
    if (st == STOREDL_RUNNING || st == STOREDL_WAIT) return L"Cancel";
    const StoreItem *it = &s->items[i];
    if (st == STOREDL_SITE) return L"Opens in your browser";
    if (!it->varCount) return NULL;
    str_copy(tip, SP_COUNTOF(tip), L"Available:");
    for (int k = 0; k < it->varCount; k++) {
        wchar_t one[48];
        str_printf(one, SP_COUNTOF(one), L"%ls %d × %d%ls", k ? L"," : L"", it->var[k].w, it->var[k].h,
                   strcmp(it->var[k].key, "gif") == 0 ? L" GIF" : L"");
        str_cat(tip, SP_COUNTOF(tip), one);
    }
    return tip;
}

/* ================================================================ displays */

static void row_label(Ui *u, float x, float y, float w, const wchar_t *label, const wchar_t *desc);

/* Segmented control (Appearance style). data: hit data per option (NULL: its index). */
static void segmented(Ui *u, float x, float y, int kind, const wchar_t *const *opts, int n, int sel, float segW,
                      const int *data)
{
    Theme *t = &u->t;
    ui_rrect(rectf(x - 3, y - 3, segW * n + 6, 38), 10, t->chip);
    for (int i = 0; i < n; i++) {
        int id = HIT_ID(kind, data ? data[i] : i), on = i == sel;
        D2D1_RECT_F r = rectf(x + i * segW, y, segW, 32);
        if (on) {
            ui_rrect(r, 8, t->dark ? col(0xFFFFFF, 0.14f) : col(0xFFFFFF, 1.0f));
            ui_rrect_stroke(r, 8, 1, t->border);
        } else if (ui_is_hot(id)) {
            ui_rrect(r, 8, col_a(t->chip, t->chip.a * 1.5f));
        }
        ui_text(opts[i], u->fCenter, r, on ? t->text : t->text2);
        ui_hit(id, r, 1, 1);
        ui_focus_ring(r, 8, id);
    }
}

static void draw_layout(Ui *u, float x, float y, float w, float h)
{
    Theme *t = &u->t;
    ui_glass(rectf(x, y, w, h), 18, 0, 0);
    if (!g_app.monCount) return;
    int span = g_app.cfg.s.displayMode == DM_SPAN && g_app.monCount > 1;
    WallpaperEntry *spanE = span ? config_find(&g_app.cfg, app_wallpaper_for(NULL)) : NULL;
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
        if (spanE) {                     /* each display shows its own part of the one picture */
            ui_push_clip(r);
            ui_draw_thumb(spanE, rectf(ox, oy, bw * s, bh * s), 0, 1);
            ui_pop_clip();
        } else if (e) ui_draw_thumb(e, r, 8, 0);
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
    wchar_t sub[32];
    str_printf(sub, SP_COUNTOF(sub), L"%d display%ls", g_app.monCount, g_app.monCount == 1 ? L"" : L"s");
    draw_title(u, x, L"Displays", sub);
    float top = 88;
    top += draw_notice(u, x, top, w);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_DISPLAYS];
    DisplayMode dm = g_app.cfg.s.displayMode;
    draw_layout(u, x, y, w, 230);
    if (g_app.monCount) {
        float bw = ui_button_width(L"Identify", 0);
        ui_button(HIT_ID(HK_IDENTIFY, 0), rectf(x + w - 14 - bw, y + 12, bw, 32), L"Identify", 0, BTN_SECONDARY);
    }
    y += 230 + 14;
    {
        static const wchar_t *modes[] = { L"Independent", L"Duplicate", L"Span" };
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - 3 * 112 - 40, L"Arrangement", NULL);
        segmented(u, x + w - 18 - 3 * 112, y + 15, HK_DISPMODE, modes, 3, (int)dm, 112, NULL);
        y += 62 + 22;
    }
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
        long mw = m->rect.right - m->rect.left, mh = m->rect.bottom - m->rect.top;
        str_printf(det, SP_COUNTOF(det), L"%ld × %ld · %u Hz · %u%%%ls", mw, mh, m->refreshHz, m->dpi * 100 / 96,
                   m->primary ? L" · Primary" : L"");
        ui_text(det, u->fSmall, rectf(x + 66, y + 42, SP_MAX(260.0f, w * 0.38f - 70), 20), t->text2);

        float mx = x + SP_MAX(330.0f, w * 0.38f);
        D2D1_RECT_F th = rectf(mx, y + 18, 85, 48);
        if (e) ui_draw_thumb(e, th, 8, 0);
        else ui_rrect(th, 8, t->chip);
        ui_text(e ? e->name : L"No wallpaper", u->fBody, rectf(mx + 98, y + 18, w * 0.24f, 22), t->text);
        D2D1_COLOR_F sc;
        const wchar_t *sl = state_label(ms, &sc);
        wchar_t st[64];
        str_printf(st, SP_COUNTOF(st), L"%ls · %ls", sl, override ? L"custom" : dm == DM_SPAN && g_app.monCount > 1 ? L"part of the span"
                                                           : dm == DM_DUPLICATE ? L"duplicated" : L"uses default");
        ui_circle(mx + 103, y + 52, 3.5f, sc);
        ui_text(st, u->fSmall, rectf(mx + 112, y + 42, w * 0.24f, 20), t->text2);

        float bx = x + w - 16;
        float b1 = ui_button_width(L"Choose…", 0);
        bx -= b1;
        if (dm == DM_INDEPENDENT) ui_button(HIT_ID(HK_CHANGE, i), rectf(bx, y + 25, b1, 34), L"Choose…", 0, BTN_SECONDARY);
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
    /* a label alone sits in the middle of its 62 DIP row */
    ui_text(label, u->fBody, rectf(x + 18, y + (desc ? 11 : 21), w, 20), u->t.text);
    if (desc) ui_text(desc, u->fSmall, rectf(x + 18, y + 32, w, 18), u->t.text2);
}

/* descs (or any one of them) may be NULL */
static float toggle_group(Ui *u, float x, float y, float w, const int *idx, const wchar_t *const *labels,
                          const wchar_t *const *descs, const int *values, int n)
{
    ui_glass(rectf(x, y, w, 62.0f * n), 14, 0, 0);
    for (int i = 0; i < n; i++) {
        float ry = y + i * 62.0f;
        if (i) ui_line(x + 16, ry, x + w - 16, ry, 1, u->t.separator);
        row_label(u, x, ry, w - 120, labels[i], descs ? descs[i] : NULL);
        ui_toggle(HIT_ID(HK_TOGGLE, idx[i]), x + w - 62, ry + 19, idx[i], values[i]);
    }
    return y + 62.0f * n;
}

static void draw_settings(Ui *u, float cx, float cw)
{
    Settings *s = &g_app.cfg.s;
    float x = cx + PAD, w = SP_MIN(cw - PAD * 2, 860.0f);
    draw_title(u, x, L"Settings", NULL);
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
        const wchar_t *l[] = { L"Start with Windows", L"Start minimised to the tray" };
        int v[] = { s->startWithWindows, s->launchToTray };
        y = toggle_group(u, x, y, w, idx, l, NULL, v, 2);
    }
    y += 10;
    {
        UpdateState us;
        update_get(&us);
        float bw = ui_button_width(L"Check now", GL_REFRESH);
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - bw - 120, L"Auto update", us.text);
        ui_button(HIT_ID(HK_UPDATE_CHECK, 0), rectf(x + w - 82 - bw, y + 14, bw, 34), L"Check now", GL_REFRESH, BTN_SECONDARY);
        ui_toggle(HIT_ID(HK_TOGGLE, TOG_AUTO_UPDATE), x + w - 62, y + 19, TOG_AUTO_UPDATE, s->autoUpdate);
    }
    y += 62 + 10;
    ui_glass(rectf(x, y, w, 62), 14, 0, 0);
    row_label(u, x, y, w - 300, L"Appearance", NULL);
    {
        static const wchar_t *opts[] = { L"System", L"Light", L"Dark" };
        segmented(u, x + w - 18 - 78 * 3, y + 15, HK_THEME, opts, 3, (int)s->theme, 78, NULL);
    }
    y += 62 + 26;

    y = section(u, x, y, L"SCREENSAVER");
    {
        ScrState st;
        scr_query(&st);
        int on = st.ours && st.active;
        int idx[] = { TOG_SCR_ENABLE };
        const wchar_t *l[] = { L"Use Screen+ as the screensaver" };
        const wchar_t *d[] = { st.policy ? L"Managed by your organisation" : !on && st.active ? L"Another screensaver is in use" : NULL };
        int v[] = { on };
        y = toggle_group(u, x, y, w, idx, l, d, v, 1);
        y += 10;

        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        WallpaperEntry *se = config_find(&g_app.cfg, config_screensaver(&g_app.cfg));
        const wchar_t *d1 = !se ? L"None" : g_app.cfg.screensaver[0] ? se->name : L"Same as the desktop";
        float bp = ui_button_width(L"Preview", GL_PLAY), bc = ui_button_width(L"Choose…", 0);
        row_label(u, x, y, w - bp - bc - 60, L"Wallpaper", d1);
        ui_button(HIT_ID(HK_SCR_PREVIEW, 0), rectf(x + w - 18 - bp, y + 14, bp, 34), L"Preview", GL_PLAY, BTN_SECONDARY);
        ui_button(HIT_ID(HK_SCR_CHOOSE, 0), rectf(x + w - 26 - bp - bc, y + 14, bc, 34), L"Choose…", 0, BTN_SECONDARY);
        y += 62 + 10;

        static const wchar_t *mins[] = { L"1", L"3", L"5", L"10", L"15", L"30" };
        static const int minv[] = { 1, 3, 5, 10, 15, 30 };
        int cur = -1;
        for (int i = 0; i < 6; i++) if (st.timeoutSec == minv[i] * 60) cur = i;
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - 6 * 46 - 40, L"Start after (minutes)", NULL);
        segmented(u, x + w - 18 - 6 * 46, y + 15, HK_SCR_TIMEOUT, mins, 6, cur, 46, minv);
        y += 62 + 10;

        static const wchar_t *dopts[] = { L"All", L"Span", L"Primary" };
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - 3 * 84 - 40, L"Displays", NULL);
        segmented(u, x + w - 18 - 3 * 84, y + 15, HK_SCR_DISPLAYS, dopts, 3, (int)s->scrDisplays, 84, NULL);
        y += 62 + 10;

        int idx2[] = { TOG_SCR_AUDIO, TOG_SCR_MOUSE, TOG_SCR_LOCK };
        const wchar_t *l2[] = { L"Play sound", L"End on mouse movement", L"Show the sign-in screen on resume" };
        int v2[] = { s->scrAudio, s->scrExitOnMouse, st.secure };
        y = toggle_group(u, x, y, w, idx2, l2, NULL, v2, 3);
        y += 10;
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        float bw = ui_button_width(L"Open", GL_OPEN);
        row_label(u, x, y, w - bw - 60, L"Windows screensaver settings", NULL);
        ui_button(HIT_ID(HK_SCR_WINDOWS, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Open", GL_OPEN, BTN_SECONDARY);
    }
    y += 62 + 26;

    y = section(u, x, y, L"PERFORMANCE");
    {
        int idx[] = { TOG_FULLSCREEN, TOG_COVERED, TOG_LOCK, TOG_BATTERY };
        const wchar_t *l[] = { L"Pause during fullscreen apps", L"Pause when covered by windows", L"Pause while locked",
                               L"Pause on battery power" };
        int v[] = { s->pauseOnFullscreen, s->pauseWhenCovered, s->suspendOnLock, s->pauseOnBattery };
        y = toggle_group(u, x, y, w, idx, l, NULL, v, 4);
    }
    y += 26;

    y = section(u, x, y, L"AUDIO & RECOVERY");
    {
        int idx[] = { TOG_AUDIO, TOG_RECOVER };
        const wchar_t *l[] = { L"Play wallpaper audio", L"Restart failed renderers automatically" };
        int v[] = { s->audio, s->autoRecover };
        y = toggle_group(u, x, y, w, idx, l, NULL, v, 2);
    }
    y += 10;
    y = section(u, x, y, L"HTML WALLPAPERS");
    {
        DepthState ds;
        depth_get(&ds);
        int idx[] = { TOG_HTML_MOUSE, TOG_HTML_SPECTRUM, TOG_LIVELY, TOG_URL, TOG_DEPTH_ALL };
        const wchar_t *l[] = { L"Allow desktop mouse interaction", L"Enable audio-reactive spectrum",
                               L"Import Lively wallpapers automatically", L"Import wallpapers from web links",
                               L"Make image wallpapers interactive" };
        const wchar_t *d[] = { NULL, NULL, NULL, NULL, ds.busy && !ds.id[0] ? ds.text : NULL };
        int v[] = { s->htmlMouse, s->htmlAudio, s->livelyImport, s->urlImport, s->depthAll };
        y = toggle_group(u, x, y, w, idx, l, d, v, 5);
    }
    y += 26;
    y = section(u, x, y, L"TROUBLESHOOTING");
    {
        const wchar_t *state = g_app.engineGaveUp ? L"Stopped after repeated failures"
                             : !g_app.engineProcess ? L"Starting"
                             : g_app.safeMode ? L"Running in safe mode" : L"Running";
        float bw = ui_button_width(L"Restart renderer", GL_REFRESH);
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - bw - 60, L"Renderer", state);
        ui_button(HIT_ID(HK_RESTART_ENGINE, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Restart renderer", GL_REFRESH, BTN_SECONDARY);
        y += 62 + 10;
    }
    if (g_app.haveStatus && !g_app.status.webviewVersion[0]) {    /* web wallpapers can't run without it */
        float bw = ui_button_width(L"Get WebView2", 0);
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - bw - 60, L"WebView2 Runtime", L"Not installed. Web wallpapers need it.");
        ui_button(HIT_ID(HK_GET_WEBVIEW, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Get WebView2", 0, BTN_SECONDARY);
        y += 62 + 10;
    }
    {
        float b1 = ui_button_width(L"Open logs", GL_DOC), b2 = ui_button_width(L"Open data folder", GL_FOLDER);
        ui_glass(rectf(x, y, w, 62), 14, 0, 0);
        row_label(u, x, y, w - b1 - b2 - 60, L"Logs and data", NULL);
        ui_button(HIT_ID(HK_OPEN_DATA, 0), rectf(x + w - 18 - b2, y + 14, b2, 34), L"Open data folder", GL_FOLDER, BTN_SECONDARY);
        ui_button(HIT_ID(HK_OPEN_LOGS, 0), rectf(x + w - 26 - b2 - b1, y + 14, b1, 34), L"Open logs", GL_DOC, BTN_SECONDARY);
    }
    y += 62 + 26;

    y = section(u, x, y, L"ABOUT");
    ui_glass(rectf(x, y, w, 62), 14, 0, 0);
    row_label(u, x, y, w - 240, L"Screen+ " SP_VERSION_W, NULL);
    {
        float bw = ui_button_width(L"Third-party notices", 0);
        ui_button(HIT_ID(HK_LICENSES, 0), rectf(x + w - 18 - bw, y + 14, bw, 34), L"Third-party notices", 0, BTN_GHOST);
    }
    y += 62 + 32;
    ui_pop_clip();
    finish_scroll(u, PAGE_SETTINGS, y, top);
    draw_scrollbar(u, view, PAGE_SETTINGS);
}

/* ================================================================ customise */

/* Look controls: order matches the WallpaperAdjust fields the renderers consume. */
enum { SL_BRIGHT = 0, SL_CONTRAST, SL_SAT, SL_HUE, SL_GAMMA, SL_TINTLIFT, SL_TINTV, SL_COUNT };
static const struct { const wchar_t *label; int min, max; } k_look[SL_COUNT] = {
    { L"Brightness", -100, 100 },
    { L"Contrast",   -100, 100 },
    { L"Saturation", -100, 100 },
    { L"Hue shift",  -180, 180 },
    { L"Gamma",      -100, 100 },
    { L"Strength",      0, 100 },   /* tint: how far the blacks are lifted towards the colour */
    { L"Brightness",    0, 100 },   /* tint: brightness of the colour */
};

#define LOOK_ROW  50.0f
#define PROP_ROW  56.0f
#define LABEL_W   150.0f
#define DISC_R    68.0f
#define LIVE_MS   40.0     /* live preview while dragging: at most one engine round trip per 40 ms */

static struct {
    wchar_t         wpId[SP_ID_LEN];
    WallpaperAdjust adj;               /* working copy: edits land here and are saved on commit */
    CustomProp     *props;
    int             propCount;
    wchar_t         edit[SP_MAX_PATH]; /* text of the field being typed into (u->textFocus) */
    int             forceReload;       /* re-resolve property values (after a reset) */
    double          lastSend;          /* time_ms of the last commit */
} cus;

/* The card's name line: plain text, or the rename editor while typing (double-click starts one). */
static void draw_card_name(Ui *u, int vi, const WallpaperEntry *e, D2D1_RECT_F nr)
{
    if (u->textFocus == HIT_ID(HK_NAME, vi)) {
        te_set_layout(u->fBodyB, nr);
        te_draw(nr, u->t.text, col_a(u->t.accent, 0.35f), u->t.accent, u->caretOn);
    } else {
        ui_text(e->name, u->fBodyB, nr, u->t.text);
    }
}

/* Look or properties changed outside the page (CLI): re-read them on the next paint. */
void pages_customise_reload(void)
{
    cus.forceReload = 1;
}

static WallpaperEntry *customise_entry(void)
{
    return config_find(&g_app.cfg, app_wallpaper_for(target_id()));
}

static void customise_refresh(void)
{
    Ui *u = g_ui;
    WallpaperEntry *e = customise_entry();
    if (!e || !e->id[0]) {
        wpapi_props_free(cus.props, cus.propCount);
        cus.props = NULL;
        cus.propCount = 0;
        cus.wpId[0] = 0;
        u->textFocus = 0;
        te_close();
        return;
    }
    if (str_ieq(cus.wpId, e->id) && !cus.forceReload) return;
    str_copy(cus.wpId, SP_COUNTOF(cus.wpId), e->id);
    if (u->textFocus) pages_text_end(0);   /* the edited field belonged to the previous wallpaper */
    u->textFocus = 0;
    cus.forceReload = 0;
    wpapi_load_adjust(e->id, &cus.adj);
    wpapi_props_free(cus.props, cus.propCount);
    cus.props = wpapi_props_list(e, &cus.propCount);
}

static int *look_field(int def)
{
    WallpaperAdjust *a = &cus.adj;
    switch (def) {
    case SL_BRIGHT:   return &a->brightness;
    case SL_CONTRAST: return &a->contrast;
    case SL_SAT:      return &a->saturation;
    case SL_HUE:      return &a->hue;
    case SL_GAMMA:    return &a->gamma;
    case SL_TINTLIFT: return &a->tintLift;
    case SL_TINTV:    return &a->tintBrightness;
    default:          return NULL;
    }
}

/* Slider geometry shared with ui_slider (track inset 11 on both sides). */
static float slider_norm_from_x(D2D1_RECT_F r, float x)
{
    float inset = 11.0f, span = SP_MAX((r.right - r.left) - inset * 2.0f, 1.0f);
    return SP_CLAMP((x - (r.left + inset)) / span, 0.0f, 1.0f);
}

static uint32_t rgb_of(D2D1_COLOR_F c)
{
    return ((uint32_t)lroundf(SP_CLAMP(c.r, 0.0f, 1.0f) * 255.0f) << 16) |
           ((uint32_t)lroundf(SP_CLAMP(c.g, 0.0f, 1.0f) * 255.0f) << 8) |
           (uint32_t)lroundf(SP_CLAMP(c.b, 0.0f, 1.0f) * 255.0f);
}

/* The tint as the renderers apply it: colour scaled by the tint brightness. */
static uint32_t tint_effective(void)
{
    D2D1_COLOR_F c = col(cus.adj.tint, 1.0f);
    float v = cus.adj.tintBrightness / 100.0f;
    return rgb_of((D2D1_COLOR_F){ c.r * v, c.g * v, c.b * v, 1.0f });
}

/* Saves and pushes the look; `live` commits (mid-drag) are throttled, the release always sends. */
static void look_commit(int live)
{
    ui_invalidate();
    WallpaperEntry *e = customise_entry();
    double now = time_ms();
    if (!e || (live && now - cus.lastSend < LIVE_MS)) return;
    cus.lastSend = now;
    wpapi_save_adjust(e->id, &cus.adj);
    app_apply_customise();
}

/* Replaces the value (nv == NULL keeps it), saves the override and pushes it to the wallpaper. */
static void prop_commit(CustomProp *cp, JVal *nv, int live)
{
    if (nv) {
        json_free(cp->value);
        cp->value = nv;
        cp->overridden = 1;
    }
    ui_invalidate();
    WallpaperEntry *e = customise_entry();
    double now = time_ms();
    if (!e || (live && now - cus.lastSend < LIVE_MS)) return;
    cus.lastSend = now;
    if (wpapi_save_override(e, cp->name, cp->value)) app_apply_customise();
}

/* ---- choice properties: Lively dropdown (index), WE combo (option value), Lively folderDropdown (file) */

static void prop_choice_text(const CustomProp *cp, wchar_t *out, size_t cap)
{
    int i = wpapi_choice_current(cp);
    if (i >= 0) wpapi_choice_label(cp, i, out, cap);
    else if (cp->value && cp->value->t == J_STR) json_str_w(cp->value, out, cap);
    else str_printf(out, cap, L"%.6g", json_num(cp->value, 0.0));
    if (!out[0]) str_copy(out, cap, L"(none)");
}

static D2D1_RECT_F hit_rect(int id)
{
    Ui *u = g_ui;
    for (int i = 0; i < u->hitCount; i++)
        if (u->hits[i].id == id) return u->hits[i].r;
    return u->pressedRect;
}

static void prop_menu(int idx, int id)
{
    Ui *u = g_ui;
    CustomProp *cp = &cus.props[idx];
    int n = wpapi_choice_count(cp);
    if (n <= 0) return;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    int cur = wpapi_choice_current(cp);
    for (int i = 0; i < n && i < 1000; i++) {
        wchar_t l[160];
        wpapi_choice_label(cp, i, l, SP_COUNTOF(l));
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), (UINT_PTR)i + 1, l[0] ? l : L"(unnamed)");
    }
    D2D1_RECT_F r = hit_rect(id);
    POINT p = { (LONG)(r.left * u->scale), (LONG)((r.bottom + 4.0f) * u->scale) };
    ClientToScreen(u->hwnd, &p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, u->hwnd, NULL);
    DestroyMenu(m);
    if (cmd && (int)cmd - 1 != cur) prop_commit(cp, wpapi_choice_value(cp, (int)cmd - 1), 0);
}

/* ---- colour properties: "#RRGGBB" / "0xRRGGBB" (Lively) or "r g b" floats 0..1 (Wallpaper Engine) */

/* The system dialog keeps the luminance when a colour is picked from its spectrum, so from black (or
 * white) every pick stays black: lift it to the middle just before such a click lands. */
static UINT_PTR CALLBACK colour_dialog_hook(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg == WM_INITDIALOG) return TRUE;   /* default focus */
    if (msg == WM_LBUTTONDOWN) {
        HWND spectrum = GetDlgItem(dlg, COLOR_RAINBOW);
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        BOOL ok = FALSE;
        UINT lum = GetDlgItemInt(dlg, COLOR_LUM, &ok, FALSE);
        ClientToScreen(dlg, &p);
        if (spectrum && GetWindowRect(spectrum, &r) && PtInRect(&r, p) && ok && (lum == 0 || lum >= 240))
            SetDlgItemInt(dlg, COLOR_LUM, 120, FALSE);
    }
    return 0;
}

static void prop_pick_colour(int idx)
{
    CustomProp *cp = &cus.props[idx];
    int style;
    uint32_t rgb = wpapi_color_get(cp, &style);
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof cc };
    cc.hwndOwner = g_ui->hwnd;
    cc.lpCustColors = custom;
    cc.rgbResult = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ENABLEHOOK;
    cc.lpfnHook = colour_dialog_hook;
    if (!ChooseColorW(&cc)) return;
    COLORREF r = cc.rgbResult;
    prop_commit(cp, wpapi_color_value(((uint32_t)GetRValue(r) << 16) | ((uint32_t)GetGValue(r) << 8) | GetBValue(r), style), 0);
}

/* A slider property's value clamped to its range and snapped to its step. */
static double prop_snap(const CustomProp *cp, double v)
{
    double step = cp->step > 0 ? cp->step : 1.0;
    return SP_CLAMP(cp->min + floor((v - cp->min) / step + 0.5) * step, cp->min, cp->max);
}

/* ---- dragging (sliders, colour wheel): called on press, move and release */

void pages_drag(int id, float x, float y, int commit)
{
    int kind = HIT_KIND(id), data = HIT_DATA(id);
    D2D1_RECT_F r = g_ui->pressedRect;
    if (kind == HK_SLIDER) {
        int *f = look_field(data);
        if (!f) return;
        int v = k_look[data].min + (int)lroundf((k_look[data].max - k_look[data].min) * slider_norm_from_x(r, x));
        if (v == *f && !commit) return;
        *f = v;
        look_commit(!commit);
        return;
    }
    if (kind == HK_DISC) {
        float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
        float radius = SP_MAX((r.right - r.left) * 0.5f, 1.0f), dx = x - cx, dy = y - cy;
        float sat = SP_MIN(sqrtf(dx * dx + dy * dy) / radius, 1.0f);
        uint32_t tint = rgb_of(ui_hsv(atan2f(dy, dx) * 180.0f / 3.14159265f, sat, 1.0f, 1.0f));
        if (tint == cus.adj.tint && !commit) return;
        cus.adj.tint = tint;
        if (!cus.adj.tintLift) cus.adj.tintLift = 30;   /* picking a colour turns the tint on */
        look_commit(!commit);
        return;
    }
    if (kind == HK_PSLIDER && data >= 0 && data < cus.propCount) {
        CustomProp *cp = &cus.props[data];
        double step = cp->step > 0 ? cp->step : 1.0;
        double v = prop_snap(cp, cp->min + (cp->max - cp->min) * slider_norm_from_x(r, x));
        if (fabs(v - json_num(cp->value, cp->min)) < step * 1e-3 && !commit) return;
        prop_commit(cp, wpapi_num_value(v, step), !commit);
    }
}

/* Arrow keys on a focused slider: one step (Shift: ten). */
static int customise_key(UINT vk, int shift)
{
    Ui *u = g_ui;
    int kind = HIT_KIND(u->focus), data = HIT_DATA(u->focus);
    if ((kind != HK_SLIDER && kind != HK_PSLIDER) || (vk != VK_LEFT && vk != VK_RIGHT)) return 0;
    int dir = vk == VK_RIGHT ? 1 : -1;
    if (kind == HK_SLIDER) {
        int *f = look_field(data);
        if (!f) return 1;
        *f = SP_CLAMP(*f + dir * (shift ? 10 : 1), k_look[data].min, k_look[data].max);
        look_commit(0);
    } else if (data >= 0 && data < cus.propCount) {
        CustomProp *cp = &cus.props[data];
        double step = cp->step > 0 ? cp->step : 1.0;
        double v = SP_CLAMP(json_num(cp->value, cp->min) + dir * step * (shift ? 10 : 1), cp->min, cp->max);
        prop_commit(cp, wpapi_num_value(v, step), 0);
    }
    u->focusVisible = 1;
    return 1;
}

/* ---- typing: text properties, and a precise number for a slider or colour (click its value) */

static int edit_is_hex(int id)
{
    int kind = HIT_KIND(id), data = HIT_DATA(id);
    return (kind == HK_LOOKVAL && data == SL_COUNT) ||
           (kind == HK_PVAL && data >= 0 && data < cus.propCount && cus.props[data].type == CP_COLOR);
}

/* The engine's filter; it may also rewrite the character (decimal comma becomes a dot). */
static int te_field_accepts(wchar_t *ch)
{
    int id = g_ui->textFocus;
    if (HIT_KIND(id) == HK_PTEXT || HIT_KIND(id) == HK_URL_TEXT || HIT_KIND(id) == HK_NAME) return *ch >= 32;
    if (*ch == L',' && !edit_is_hex(id)) *ch = L'.';
    return *ch && wcschr(edit_is_hex(id) ? L"0123456789abcdefABCDEF#" : L"0123456789+-.", *ch);
}

/* What a value field holds when typing starts: the bare number, or the colour as #RRGGBB. */
static int value_edit_text(int id, wchar_t *out, size_t cap)
{
    int kind = HIT_KIND(id), data = HIT_DATA(id), style;
    if (kind == HK_LOOKVAL) {
        int *f = look_field(data);
        if (data == SL_COUNT) str_printf(out, cap, L"#%06X", tint_effective());
        else if (f) str_printf(out, cap, L"%d", *f);
        return data == SL_COUNT || f;
    }
    if (kind != HK_PVAL || data < 0 || data >= cus.propCount) return 0;
    CustomProp *cp = &cus.props[data];
    if (cp->type == CP_COLOR) str_printf(out, cap, L"#%06X", wpapi_color_get(cp, &style));
    else str_printf(out, cap, L"%.6g", json_num(cp->value, cp->min));
    return cp->type == CP_COLOR || cp->type == CP_SLIDER;
}

static void edit_begin(int id, int selectAll)
{
    Ui *u = g_ui;
    if (u->textFocus == id) {           /* clicked again while typing: keep the caret blinking */
        u->caretOn = 1;
        SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
        return;
    }
    if (u->textFocus) pages_text_end(1);
    u->textFocus = id;
    u->searchFocus = 0;
    u->caretOn = 1;
    SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
    te_open(cus.edit, SP_COUNTOF(cus.edit), (int)wcslen(cus.edit), selectAll, te_field_accepts, NULL);
}

static int parse_number(const wchar_t *s, double *v)
{
    wchar_t *end = NULL;
    *v = wcstod(s, &end);
    return end && end != s && !*end;
}

/* Applies what was typed; anything that isn't a number (or colour) leaves the value as it was. */
static void edit_commit(int id)
{
    int kind = HIT_KIND(id), data = HIT_DATA(id), style;
    uint32_t rgb;
    double v;
    if (kind == HK_URL_TEXT) {
        str_copy(urlp.url, SP_COUNTOF(urlp.url), cus.edit);
        str_trim(urlp.url);
    } else if (kind == HK_NAME) {                        /* the typed text becomes the card's name */
        WallpaperEntry *e = data >= 0 && data < g_ui->viewCount ? &g_app.cfg.items[g_ui->view[data]] : NULL;
        if (e) {
            wchar_t nm[SP_NAME_LEN];
            str_copy(nm, SP_COUNTOF(nm), cus.edit);
            str_trim(nm);
            str_sanitize_line(nm);
            if (nm[0] && !str_ieq(nm, e->name)) {
                str_copy(e->name, SP_COUNTOF(e->name), nm);
                app_save_config();
            }
        }
    } else if (kind == HK_LOOKVAL && data == SL_COUNT) {   /* tint: the colour's hue/saturation + its brightness */
        if (!wpapi_parse_rgb(cus.edit, &rgb)) return;
        float h, s, val;
        ui_rgb_hsv(col(rgb, 1.0f), &h, &s, &val);
        cus.adj.tint = rgb_of(ui_hsv(h, s, 1.0f, 1.0f));
        cus.adj.tintBrightness = (int)lroundf(val * 100.0f);
        if (!cus.adj.tintLift) cus.adj.tintLift = 30;
        look_commit(0);
    } else if (kind == HK_LOOKVAL) {
        int *f = look_field(data);
        if (!f || !parse_number(cus.edit, &v)) return;
        *f = (int)lround(SP_CLAMP(v, (double)k_look[data].min, (double)k_look[data].max));
        look_commit(0);
    } else if (data >= 0 && data < cus.propCount) {
        CustomProp *cp = &cus.props[data];
        if (kind == HK_PTEXT)
            prop_commit(cp, wpapi_str_value(cus.edit), 0);
        else if (kind == HK_PVAL && cp->type == CP_COLOR && wpapi_parse_rgb(cus.edit, &rgb)) {
            wpapi_color_get(cp, &style);
            prop_commit(cp, wpapi_color_value(rgb, style), 0);
        } else if (kind == HK_PVAL && cp->type == CP_SLIDER && parse_number(cus.edit, &v))
            prop_commit(cp, wpapi_num_value(prop_snap(cp, v), cp->step > 0 ? cp->step : 1.0), 0);
    }
}

int pages_text_char(wchar_t ch)
{
    Ui *u = g_ui;
    if (!u->textFocus) return 0;
    if (te_char(ch)) {
        u->caretOn = 1;                 /* typing shows a solid caret until the blink timer flips it */
        return 1;
    }
    return 0;
}

static void url_go(void);

static void search_notify(void);

int pages_text_key(UINT vk, int ctrl, int shift)
{
    Ui *u = g_ui;
    if (!u->textFocus) return 0;
    int kind = HIT_KIND(u->textFocus);
    int url = kind == HK_URL_TEXT, search = kind == HK_SEARCH, store = kind == HK_STORE_TEXT;
    if (te_key(vk, ctrl, shift)) return 1;
    if (vk == VK_ESCAPE) {
        if (te_has_sel()) {              /* the first Escape just drops the selection */
            te_clear_sel();
            return 1;
        }
        if (search) {
            if (u->search[0]) {
                u->search[0] = 0;
                search_notify();          /* empty it but keep the focus */
                return 1;
            }
            return 0;                    /* nothing to clear: the shell closes the search */
        }
        if (store && storep.text[0]) {   /* empty it, keep the focus, and list the latest again */
            te_close();
            storep.text[0] = 0;
            te_open(storep.text, SP_COUNTOF(storep.text), 0, 0, NULL, store_query_changed);
            pages_store_timer();
            return 1;
        }
        pages_text_end(url || store);    /* the link field keeps what was typed and closes; the rename cancels */
        if (url) urlp.open = 0;
    } else if (vk == VK_RETURN) {
        if (search) return 1;
        if (store) {                     /* without waiting for the pause */
            pages_store_timer();
            return 1;
        }
        pages_text_end(1);
        if (url) url_go();
    } else if (vk == VK_TAB) {
        pages_text_end(1);               /* commit, then the shell moves focus */
        return 0;
    }
    return 1;                            /* characters arrive via WM_CHAR; consume the rest */
}

void pages_text_end(int commit)
{
    Ui *u = g_ui;
    int id = u->textFocus;
    int search = id && (HIT_KIND(id) == HK_SEARCH || HIT_KIND(id) == HK_STORE_TEXT);   /* live buffers */
    u->textFocus = 0;
    te_close();
    if (search) {
        u->searchFocus = 0;
        KillTimer(u->hwnd, TM_CARET);
    }
    if (commit && id && !search) edit_commit(id);
    ui_invalidate();
}

/* The search box shares the engine: its buffer is u->search, its change triggers a library rebuild. */
static void search_notify(void)
{
    Ui *u = g_ui;
    u->scrollTarget[PAGE_LIBRARY] = u->scroll[PAGE_LIBRARY] = 0.0f;
    pages_rebuild_view();
}

void pages_search_open(void)
{
    Ui *u = g_ui;
    int id = HIT_ID(HK_SEARCH, 0);
    if (u->textFocus != id) {
        if (u->textFocus) pages_text_end(1);
        u->searchFocus = 1;
        u->textFocus = id;
        u->caretOn = 1;
        SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
        te_open(u->search, SP_COUNTOF(u->search), (int)wcslen(u->search), 0, NULL, search_notify);
    }
    ui_invalidate();
}

/* ---- import from a web link */

static void url_focus(void)
{
    Ui *u = g_ui;
    int id = HIT_ID(HK_URL_TEXT, 0);
    if (!urlp.url[0] && OpenClipboard(u->hwnd)) {   /* a link on the clipboard is offered right away */
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        const wchar_t *p = h ? (const wchar_t *)GlobalLock(h) : NULL;
        if (p) {
            wchar_t clip[SP_MAX_PATH];
            str_copy(clip, SP_COUNTOF(clip), p);
            str_trim(clip);
            if (wcslen(p) < SP_COUNTOF(clip) && wallpaper_is_url(clip) && !wcspbrk(clip, L" \t\r\n"))
                str_copy(urlp.url, SP_COUNTOF(urlp.url), clip);
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    str_copy(cus.edit, SP_COUNTOF(cus.edit), urlp.url);
    u->focus = id;
    edit_begin(id, urlp.url[0] != 0);
    ui_invalidate();
}

static void url_go(void)
{
    UrlImportState st;
    url_import_get(&st);
    if (st.busy) {
        url_import_cancel();
        return;
    }
    if (HIT_KIND(g_ui->textFocus) == HK_URL_TEXT) pages_text_end(1);
    url_import_start(urlp.url, urlp.mode);
}

static float draw_url_panel(Ui *u, float x, float y, float w)
{
    Theme *t = &u->t;
    UrlImportState st;
    url_import_get(&st);
    if (st.finished != urlp.seen) {                 /* an import ended: a success closes the panel */
        urlp.seen = st.finished;
        if (st.lastOk) {
            urlp.open = 0;
            urlp.url[0] = 0;
            if (HIT_KIND(u->textFocus) == HK_URL_TEXT) u->textFocus = 0;
        }
    }
    if (!st.busy && (!urlp.open || !g_app.cfg.s.urlImport)) return 0.0f;
    const float h = 128.0f;
    ui_glass(rectf(x, y, w, h), 14, 0, 0);
    ui_icon(GL_LINK, u->fIconS, rectf(x + 14, y + 10, 24, 28), t->accent);
    ui_text(L"Import from a link", u->fBodyB, rectf(x + 44, y + 10, w - 100, 28), t->text);
    if (!st.busy) ui_button(HIT_ID(HK_URL_CLOSE, 0), rectf(x + w - 42, y + 8, 32, 32), NULL, GL_CLOSE, BTN_GHOST);

    const wchar_t *go = st.busy ? L"Cancel" : L"Import";
    float gw = ui_button_width(go, 0), fy = y + 46;
    D2D1_RECT_F f = rectf(x + 16, fy, w - 42 - gw, 36);
    int id = HIT_ID(HK_URL_TEXT, 0), editing = u->textFocus == id;
    ui_rrect(f, 9, t->field);
    ui_rrect_stroke(f, 9, editing ? 1.5f : 1.0f, editing ? t->accent : t->border);
    const wchar_t *s = editing ? cus.edit : urlp.url;
    float tx = f.left + 12, avail = f.right - f.left - 24;
    if (!s[0] && !editing) {
        ui_text(L"Paste a link", u->fBody, rectf(tx, f.top, avail, 36), t->text3);
    } else if (editing) {
        D2D1_RECT_F tr = rectf(tx, f.top + 7, avail, 36 - 14);
        te_set_layout(u->fBody, tr);
        te_draw(tr, st.busy ? t->text2 : t->text, col_a(t->accent, 0.35f), t->accent, u->caretOn);
    } else {
        ui_text(s, u->fBody, rectf(tx, f.top, avail, 36), t->text);
    }
    if (!st.busy) {
        ui_hit(id, f, 1, 2);
        ui_focus_ring(f, 9, id);
    }
    ui_button(HIT_ID(HK_URL_GO, 0), rectf(f.right + 10, fy + 1, gw, 34), go, 0, st.busy ? BTN_SECONDARY : BTN_PRIMARY);

    float ry = y + 92;
    if (st.busy) {
        D2D1_RECT_F bar = rectf(x + 16, ry + 22, w - 32, 4);
        ui_text(st.text, u->fSmall, rectf(x + 16, ry, w - 32, 18), t->text2);
        ui_rrect(bar, 2, t->track);
        if (st.percent > 0) ui_rrect(rectf(bar.left, bar.top, (bar.right - bar.left) * st.percent / 100.0f, 4), 2, t->accent);
        return h + 12.0f;
    }
    static const wchar_t *modes[] = { L"Automatic", L"Download", L"Web page" };
    float cx = x + 16;
    for (int i = 0; i < (int)SP_COUNTOF(modes); i++) {
        float cw = ui_text_width(modes[i], u->fCenter) + 30;
        ui_chip(HIT_ID(HK_URL_MODE, i), rectf(cx, ry, cw, 28), modes[i], urlp.mode == i);
        cx += cw + 8;
    }
    if (st.failed && st.text[0]) ui_text(st.text, u->fSmall, rectf(cx + 6, ry, x + w - 22 - cx, 28), t->danger);
    return h + 12.0f;
}

/* ---- drawing */

static void look_value_text(int def, int val, wchar_t *out, size_t cap)
{
    if (def == SL_HUE) str_printf(out, cap, L"%+d°", val);
    else if (def == SL_TINTLIFT || def == SL_TINTV) str_printf(out, cap, L"%d%%", val);
    else str_printf(out, cap, L"%+d", val);
}

static void row_separator(Ui *u, float x, float y, float w)
{
    ui_line(x + 18, y, x + w - 18, y, 1.0f, u->t.separator);
}

/* A value beside its control (slider number, hex colour); click it to type a precise one.
 * Anchored at x: its right edge, or its left edge when `left`; centred in [y, y + h]. */
static void value_field(Ui *u, int id, const wchar_t *text, float x, int left, float y, float h, D2D1_COLOR_F c)
{
    Theme *t = &u->t;
    int editing = u->textFocus == id;
    float tw = ui_text_width(editing ? cus.edit : text, u->fBody), w = SP_MAX(tw, ui_text_width(text, u->fBody));
    D2D1_RECT_F f = rectf((left ? x : x - w) - 6.0f, y + h * 0.5f - 14.0f, w + 12.0f, 28.0f);
    if (editing) {
        ui_rrect(f, 6, t->field);
        ui_rrect_stroke(f, 6, 1.5f, t->accent);
        D2D1_RECT_F tr = rectf(f.left + 7.0f, f.top + 5.0f, f.right - f.left - 14.0f, f.bottom - f.top - 10.0f);
        te_set_layout(u->fBody, tr);
        te_draw(tr, t->text, col_a(t->accent, 0.35f), t->accent, u->caretOn);
    } else {
        if (ui_is_hot(id)) ui_rrect(f, 6, t->panelHover);
        ui_text(text, u->fBody, rectf((left ? x : x - tw) - 1.0f, y, tw + 2.0f, h), c);
    }
    ui_hit(id, f, 0, 2);
}

/* label | slider | value, vertically centred in one row */
static void look_slider(Ui *u, float x, float y, float w, float labelW, int def, int enabled)
{
    Theme *t = &u->t;
    int *f = look_field(def);
    int val = f ? *f : 0;
    ui_text(k_look[def].label, u->fBody, rectf(x, y, labelW, LOOK_ROW), enabled ? t->text : t->text3);
    D2D1_RECT_F sr = rectf(x + labelW, y + (LOOK_ROW - 24.0f) * 0.5f, w - labelW - 56.0f, 24.0f);
    if (enabled) {
        ui_slider(HIT_ID(HK_SLIDER, def), sr, (float)(val - k_look[def].min) / (float)(k_look[def].max - k_look[def].min));
        wchar_t v[32];
        look_value_text(def, val, v, SP_COUNTOF(v));
        value_field(u, HIT_ID(HK_LOOKVAL, def), v, x + w, 0, y, LOOK_ROW, t->text2);
    } else {
        float cy = (sr.top + sr.bottom) * 0.5f;
        ui_rrect(rectf(sr.left + 11.0f, cy - 2.0f, (sr.right - sr.left) - 22.0f, 4.0f), 2.0f, col_a(t->track, 0.5f));
        text_right(L"n/a", u->fBody, x + w, y, LOOK_ROW, t->text3);
    }
}

static float draw_look_section(Ui *u, float x, float y, float w, const WallpaperEntry *e)
{
    Theme *t = &u->t;
    int web = e->type == WP_HTML;
    y = section(u, x, y, L"LOOK & COLOUR");
    const int rows[] = { SL_BRIGHT, SL_CONTRAST, SL_SAT, SL_HUE, SL_GAMMA };
    /* the tint band fits the wheel and the column beside it: title, two sliders, the swatch row */
    int resettable = adjust_active(&cus.adj) || cus.adj.tint != adjust_defaults().tint || cus.adj.tintBrightness != 100;
    float tintH = SP_MAX(DISC_R * 2.0f + 40.0f, 22.0f + 28.0f + LOOK_ROW * 2.0f + 22.0f + 18.0f), footH = resettable ? 52.0f : 0.0f;
    float card = (float)SP_COUNTOF(rows) * LOOK_ROW + 8.0f + tintH + footH;
    ui_glass(rectf(x, y, w, card), 14, 0, 0);

    float ry = y + 4.0f;
    for (int i = 0; i < (int)SP_COUNTOF(rows); i++) {
        if (i) row_separator(u, x, ry, w);
        look_slider(u, x + 18, ry, w - 36, LABEL_W, rows[i], !(web && rows[i] == SL_GAMMA));
        ry += LOOK_ROW;
    }
    ry += 4.0f;
    row_separator(u, x, ry, w);

    /* tint: hue/saturation wheel + strength and brightness of the colour */
    float discCx = x + 18 + DISC_R + 6.0f, discCy = ry + tintH * 0.5f;
    float hue, sat, val;
    ui_rgb_hsv(col(cus.adj.tint, 1.0f), &hue, &sat, &val);
    ui_disc(HIT_ID(HK_DISC, 0), discCx, discCy, DISC_R, hue, sat);
    float colX = discCx + DISC_R + 30.0f, colW = x + w - 18 - colX;
    float cy = ry + 22.0f;
    ui_text(L"Tint", u->fBodyB, rectf(colX, cy, colW, 20), t->text);
    look_slider(u, colX, cy + 28, colW, 96.0f, SL_TINTLIFT, 1);
    look_slider(u, colX, cy + 28 + LOOK_ROW - 6.0f, colW, 96.0f, SL_TINTV, 1);
    uint32_t eff = tint_effective();
    D2D1_RECT_F sw = rectf(colX, cy + 28 + LOOK_ROW * 2.0f - 4.0f, 26, 26);
    ui_rrect(sw, 7, col(eff, 1.0f));
    ui_rrect_stroke(sw, 7, 1, t->border);
    wchar_t hex[16];
    str_printf(hex, SP_COUNTOF(hex), L"#%06X", eff);
    value_field(u, HIT_ID(HK_LOOKVAL, SL_COUNT), hex, colX + 40, 1, sw.top, 26, t->text2);
    ry += tintH;

    /* footer, once something differs from the defaults: reset */
    if (resettable) {
        row_separator(u, x, ry, w);
        float bw = ui_button_width(L"Reset look", GL_REFRESH);
        ui_button(HIT_ID(HK_RESET_LOOK, 0), rectf(x + w - 14 - bw, ry + 10, bw, 32), L"Reset look", GL_REFRESH, BTN_GHOST);
    }
    return y + card + 26.0f;
}

static void draw_choice(Ui *u, int id, D2D1_RECT_F r, const wchar_t *text)
{
    Theme *t = &u->t;
    int hot = ui_is_hot(id);
    ui_rrect(r, 8, hot ? t->panelHover : t->field);
    ui_rrect_stroke(r, 8, 1, t->border);
    ui_text(text, u->fBody, rectf(r.left + 12, r.top, r.right - r.left - 44, r.bottom - r.top), t->text);
    ui_icon(0xE70D, u->fIconS, rectf(r.right - 34, r.top, 26, r.bottom - r.top), t->text2);   /* chevron down */
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, 8, id);
}

static void draw_prop_row(Ui *u, float x, float y, float w, CustomProp *cp, int idx)
{
    Theme *t = &u->t;
    float ctrlW = SP_MIN(300.0f, w * 0.48f), right = x + w - 18;
    float cy = y + PROP_ROW * 0.5f;
    if (cp->type == CP_LABEL) {
        ui_text(cp->label, u->fSmall, rectf(x + 18, y, w - 36, PROP_ROW), t->text2);
        return;
    }
    ui_text(cp->label, u->fBody, rectf(x + 18, y, w - ctrlW - 60, PROP_ROW), t->text);
    switch (cp->type) {
    case CP_CHECKBOX:
        ui_toggle(HIT_ID(HK_PCHECK, idx), right - 42, cy - 12, -1, json_bool(cp->value, 0));
        break;
    case CP_SLIDER: {
        float norm = (float)((json_num(cp->value, cp->min) - cp->min) / SP_MAX(cp->max - cp->min, 1e-9));
        ui_slider(HIT_ID(HK_PSLIDER, idx), rectf(right - ctrlW - 52, cy - 12, ctrlW, 24), SP_CLAMP(norm, 0.0f, 1.0f));
        wchar_t v[40];
        str_printf(v, SP_COUNTOF(v), L"%.6g", json_num(cp->value, cp->min));
        value_field(u, HIT_ID(HK_PVAL, idx), v, right, 0, y, PROP_ROW, t->text2);
        break;
    }
    case CP_DROPDOWN:
    case CP_COMBO:
    case CP_FILE: {
        wchar_t text[160];
        prop_choice_text(cp, text, SP_COUNTOF(text));
        draw_choice(u, HIT_ID(HK_PDOWN, idx), rectf(right - ctrlW, cy - 17, ctrlW, 34), text);
        break;
    }
    case CP_COLOR: {
        int id = HIT_ID(HK_PCOLOR, idx), style;
        uint32_t rgb = wpapi_color_get(cp, &style);
        D2D1_RECT_F sw = rectf(right - 64, cy - 15, 64, 30);
        ui_rrect(sw, 8, col(rgb, 1));
        ui_rrect_stroke(sw, 8, ui_is_hot(id) ? 2.0f : 1.0f, ui_is_hot(id) ? t->accent : t->border);
        wchar_t hex[16];
        str_printf(hex, SP_COUNTOF(hex), L"#%06X", rgb);
        value_field(u, HIT_ID(HK_PVAL, idx), hex, sw.left - 12, 0, y, PROP_ROW, t->text2);
        ui_hit(id, sw, 1, 1);
        ui_focus_ring(sw, 8, id);
        break;
    }
    case CP_TEXT: {
        int id = HIT_ID(HK_PTEXT, idx), focused = u->textFocus == id;
        D2D1_RECT_F f = rectf(right - ctrlW, cy - 17, ctrlW, 34);
        ui_rrect(f, 8, t->field);
        ui_rrect_stroke(f, 8, focused ? 1.5f : 1.0f, focused ? t->accent : t->border);
        D2D1_RECT_F tr = rectf(f.left + 10, f.top, f.right - f.left - 20, f.bottom - f.top);
        if (focused) {
            te_set_layout(u->fBody, tr);
            te_draw(tr, t->text, col_a(t->accent, 0.35f), t->accent, u->caretOn);
        } else {
            wchar_t val[256];
            json_str_w(cp->value, val, SP_COUNTOF(val));
            ui_text(val[0] ? val : L"(empty)", u->fBody, tr, val[0] ? t->text : t->text3);
        }
        ui_hit(id, f, 1, 2);
        ui_focus_ring(f, 8, id);
        break;
    }
    default:
        break;
    }
}

static float draw_props_section(Ui *u, float x, float y, float w)
{
    Theme *t = &u->t;
    y = section(u, x, y, L"WALLPAPER PROPERTIES");
    if (!cus.props || !cus.propCount) {
        ui_glass(rectf(x, y, w, 56), 14, 0, 0);
        ui_text(L"This wallpaper has no properties.", u->fSmall, rectf(x + 18, y, w - 36, 56), t->text2);
        return y + 56 + 26;
    }
    int anyOverride = 0;
    for (int i = 0; i < cus.propCount; i++) anyOverride |= cus.props[i].overridden;
    float footH = anyOverride ? 52.0f : 0.0f;
    float card = cus.propCount * PROP_ROW + footH;
    ui_glass(rectf(x, y, w, card), 14, 0, 0);
    float ry = y;
    for (int i = 0; i < cus.propCount; i++) {
        if (i) row_separator(u, x, ry, w);
        draw_prop_row(u, x, ry, w, &cus.props[i], i);
        ry += PROP_ROW;
    }
    if (anyOverride) {
        row_separator(u, x, ry, w);
        float bw = ui_button_width(L"Restore defaults", GL_REFRESH);
        ui_button(HIT_ID(HK_RESET_PROPS, 0), rectf(x + w - 14 - bw, ry + 10, bw, 32), L"Restore defaults", GL_REFRESH, BTN_GHOST);
    }
    return y + card + 26.0f;
}

/* Image wallpapers, and the interactive pages made from them (depth.c). */
static float draw_cooler_section(Ui *u, float x, float y, float w, const WallpaperEntry *e)
{
    Theme *t = &u->t;
    DepthState st;
    depth_get(&st);
    int cool = (e->flags & WPFLAG_DEPTH) != 0, mine = !st.id[0] || str_ieq(st.id, e->id);
    int busy = st.busy && mine, failed = !st.busy && st.failed && st.text[0] && mine;
    const wchar_t *note = NULL;
    if (failed) note = st.text;
    else if (st.busy) note = L"Busy with another wallpaper.";
    else if (cool && !g_app.cfg.s.htmlMouse) note = L"Mouse interaction is off in Settings.";
    else if (!cool && !depth_ready()) note = L"The first use downloads about 110 MB.";
    /* the text starts below the button and the card grows with it, so neither clips; no text: one row */
    float noteH = busy || !note ? 0.0f : ui_text_height(note, u->fWrap, w - 36);
    float h = busy ? 102.0f : note ? SP_MAX(96.0f, 60.0f + noteH + 18.0f) : 64.0f;
    y = section(u, x, y, L"MAKE WALLPAPER COOLER");
    ui_glass(rectf(x, y, w, h), 14, 0, 0);
    const wchar_t *label = busy ? L"Cancel" : cool ? L"Use plain image" : L"Make wallpaper cooler";
    wchar_t icon = busy ? GL_CLOSE : cool ? GL_PHOTO : GL_SPEED;
    float bw = ui_button_width(label, icon);
    ui_button(HIT_ID(HK_COOL, busy ? 2 : cool ? 1 : 0), rectf(x + w - 18 - bw, y + 14, bw, 36), label, icon,
              busy ? BTN_SECONDARY : cool ? BTN_GHOST : BTN_PRIMARY);
    ui_text(cool ? L"Interactive depth is on" : L"Interactive depth", u->fBodyB, rectf(x + 18, y + 21, w - 54 - bw, 22), t->text);
    if (busy) {
        D2D1_RECT_F bar = rectf(x + 18, y + 84, w - 36, 4);
        ui_text(st.text, u->fSmall, rectf(x + 18, y + 60, w - 36, 18), t->text2);
        ui_rrect(bar, 2, t->track);
        if (st.percent > 0) ui_rrect(rectf(bar.left, bar.top, (bar.right - bar.left) * st.percent / 100.0f, 4), 2, t->accent);
        return y + h + 26.0f;
    }
    if (note) ui_text(note, u->fWrap, rectf(x + 18, y + 60, w - 36, noteH + 4.0f), failed ? t->danger : t->text2);
    return y + h + 26.0f;
}

static void draw_customise(Ui *u, float cx, float cw)
{
    Theme *t = &u->t;
    float x = cx + PAD, w = SP_MIN(cw - PAD * 2, 860.0f);
    WallpaperEntry *e = customise_entry();
    wchar_t sub[160];
    if (e && e->id[0]) {
        const wchar_t *mid = target_id();
        str_printf(sub, SP_COUNTOF(sub), L"%.80ls · %ls", e->name[0] ? e->name : L"Wallpaper",
                   mid ? L"selected display" : L"all displays");
    } else {
        str_copy(sub, SP_COUNTOF(sub), L"No wallpaper applied");
    }
    draw_title(u, x, L"Customise", sub);
    customise_refresh();
    float top = 88;
    top += draw_notice(u, x, top, w);
    D2D1_RECT_F view = { cx, top, cx + cw, u->h };
    u->viewTop = top;
    u->viewH = u->h - top;
    ui_push_clip(view);
    float y = top + 6 - u->scroll[PAGE_CUSTOMIZE];
    if (!e || !e->id[0]) {
        D2D1_RECT_F card = rectf(x, y + 20, w, 150);
        ui_glass(card, 16, 0, 0);
        ui_icon(GL_COLOR, u->fIconL, rectf(x, y + 44, w, 48), t->text3);
        ui_text(L"Apply a wallpaper to customise it.", u->fCenter, rectf(x + 20, y + 100, w - 40, 22), t->text2);
        y += 190;
    } else {
        if (depth_can_cool(e) || (e->flags & WPFLAG_DEPTH)) y = draw_cooler_section(u, x, y, w, e);
        y = draw_look_section(u, x, y, w, e);
        y = draw_props_section(u, x, y, w);
    }
    ui_pop_clip();
    finish_scroll(u, PAGE_CUSTOMIZE, y + 8, top);
    draw_scrollbar(u, view, PAGE_CUSTOMIZE);
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
    case PAGE_DISPLAYS:  draw_displays(u, cx, cw); break;
    case PAGE_CUSTOMIZE: draw_customise(u, cx, cw); break;
    case PAGE_STORE:     draw_store(u, cx, cw); break;
    case PAGE_SETTINGS:  draw_settings(u, cx, cw); break;
    default:             draw_library(u, cx, cw); break;
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
    case TOG_URL:        s->urlImport = !s->urlImport; break;
    case TOG_DEPTH_ALL:  s->depthAll = !s->depthAll; break;
    case TOG_AUDIO:      app_set_audio(!s->audio); ui_animate(); return;
    case TOG_SCR_AUDIO:  s->scrAudio = !s->scrAudio; break;
    case TOG_SCR_MOUSE:  s->scrExitOnMouse = !s->scrExitOnMouse; break;
    case TOG_AUTO_UPDATE: s->autoUpdate = !s->autoUpdate; break;
    case TOG_SCR_ENABLE:
    case TOG_SCR_LOCK: {                 /* Windows settings: not in config.ini */
        ScrState st;
        scr_query(&st);
        wchar_t err[200] = L"";
        if (idx == TOG_SCR_LOCK) scr_set_secure(!st.secure);
        else if (!scr_enable(!(st.ours && st.active), err, SP_COUNTOF(err))) app_notice(NOTICE_ERROR, L"%ls", err);
        else if (err[0]) app_notice(NOTICE_WARN, L"%ls", err);
        ui_animate();
        return;
    }
    default: return;
    }
    app_save_config();
    if (idx == TOG_DEPTH_ALL) depth_auto_all(TRUE);     /* switching it on is the go-ahead to download and to start on the current image */
    if (idx == TOG_AUTO_UPDATE) update_schedule(TRUE);  /* on: check in a moment; off: no more checks */
    ui_animate();
}

/* Screensaver wallpaper: the desktop's, or any wallpaper in the library. */
static void screensaver_menu(int id)
{
    Ui *u = g_ui;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (!g_app.cfg.screensaver[0] ? MF_CHECKED : 0), 1, L"Same as the desktop");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_app.cfg.count && i < 2000; i++) {
        const WallpaperEntry *e = &g_app.cfg.items[i];
        AppendMenuW(m, MF_STRING | (e->missing ? MF_GRAYED : 0) | (wcscmp(e->id, g_app.cfg.screensaver) == 0 ? MF_CHECKED : 0),
                    (UINT_PTR)i + 2, e->name[0] ? e->name : L"(unnamed)");
    }
    D2D1_RECT_F r = hit_rect(id);
    POINT p = { (LONG)(r.left * u->scale), (LONG)((r.bottom + 4.0f) * u->scale) };
    ClientToScreen(u->hwnd, &p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, u->hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) app_set_screensaver(L"");
    else if (cmd >= 2 && (int)cmd - 2 < g_app.cfg.count) app_set_screensaver(g_app.cfg.items[cmd - 2].id);
    ui_invalidate();
}

void pages_activate(int id, int dbl)
{
    Ui *u = g_ui;
    int kind = HIT_KIND(id), data = HIT_DATA(id);
    (void)dbl;                           /* double clicks are handled through the text engine */
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
    case HK_URL:
        urlp.open = !urlp.open;
        if (urlp.open) url_focus();
        else if (HIT_KIND(u->textFocus) == HK_URL_TEXT) pages_text_end(1);
        break;
    case HK_URL_TEXT:
        if (u->textFocus != id) {
            url_focus();
        } else {                         /* clicked again while typing: keep the caret blinking */
            u->caretOn = 1;
            SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
        }
        break;
    case HK_URL_MODE:      urlp.mode = SP_CLAMP(data, URLMODE_AUTO, URLMODE_PAGE); break;   /* chip order = URLMODE_* */
    case HK_URL_GO:        url_go(); break;
    case HK_URL_CLOSE:     urlp.open = 0; break;
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
    case HK_NAME:
        if (u->textFocus == id) {          /* clicked again while typing: keep the caret blinking */
            u->caretOn = 1;
            SetTimer(u->hwnd, TM_CARET, GetCaretBlinkTime(), NULL);
        } else if (data >= 0 && data < u->viewCount) {
            str_copy(cus.edit, SP_COUNTOF(cus.edit), g_app.cfg.items[u->view[data]].name);
            edit_begin(id, 0);          /* a double click selects the word through the engine */
        }
        break;
    case HK_TARGET:
    case HK_MONITOR:
        u->target = g_app.cfg.s.displayMode == DM_INDEPENDENT ? data : -1;
        break;
    case HK_DISPMODE:
        app_set_display_mode(data);
        u->target = -1;
        break;
    case HK_IDENTIFY:      identify_displays(g_app.mons, g_app.monCount); break;
    case HK_SCR_CHOOSE:    screensaver_menu(id); break;
    case HK_SCR_PREVIEW:
        app_save_config();
        if (!scr_launch(L"/s")) app_notice(NOTICE_ERROR, L"Could not start the screensaver preview.");
        break;
    case HK_SCR_TIMEOUT:   scr_set_timeout(data * 60); break;
    case HK_SCR_DISPLAYS:
        g_app.cfg.s.scrDisplays = (ScrDisplays)SP_CLAMP(data, SCRD_ALL, SCRD_PRIMARY);
        app_save_config();
        break;
    case HK_SCR_WINDOWS:   ShellExecuteW(NULL, NULL, L"control.exe", L"desk.cpl,,@screensaver", NULL, SW_SHOWNORMAL); break;
    case HK_UPDATE_CHECK:  update_check(TRUE); ui_invalidate(); break;
    case HK_PLAYPAUSE:     app_set_paused(!g_app.cfg.s.paused); break;
    case HK_AUDIO:         app_set_audio(!g_app.cfg.s.audio); break;
    case HK_RESTART_CURRENT:
    case HK_RESTART_ENGINE: app_restart_renderer(); break;
    case HK_REVEAL: {
        WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(target_id()));
        if (e && wallpaper_is_url(e->path)) shell_open(e->path);   /* a web page or stream: its link */
        else if (e) shell_reveal(e->path);
        break;
    }
    case HK_TOGGLE:        toggle_setting(data); break;
    case HK_PCHECK:
        if (data >= 0 && data < cus.propCount && cus.props[data].type == CP_CHECKBOX)
            prop_commit(&cus.props[data], wpapi_bool_value(!json_bool(cus.props[data].value, 0)), 0);
        break;
    case HK_PDOWN:
        if (data >= 0 && data < cus.propCount) prop_menu(data, id);
        break;
    case HK_PCOLOR:
        if (data >= 0 && data < cus.propCount && cus.props[data].type == CP_COLOR) prop_pick_colour(data);
        break;
    case HK_PTEXT:
        if (data >= 0 && data < cus.propCount && cus.props[data].type == CP_TEXT && u->textFocus != id) {
            json_str_w(cus.props[data].value, cus.edit, SP_COUNTOF(cus.edit));
            edit_begin(id, 0);
        }
        break;
    case HK_LOOKVAL:
    case HK_PVAL:
        if (u->textFocus != id && value_edit_text(id, cus.edit, SP_COUNTOF(cus.edit))) edit_begin(id, 1);
        break;
    case HK_RESET_LOOK:
        cus.adj = adjust_defaults();
        look_commit(0);
        break;
    case HK_COOL: {
        WallpaperEntry *e = customise_entry();
        if (!e) break;
        if (data == 2) depth_cancel();
        else if (data == 1) depth_revert(e->id);
        else {
            e->flags &= ~WPFLAG_NOCOOL;      /* asked for by hand: the "interactive images" setting may keep it cool too */
            depth_start(e->id);
        }
        ui_invalidate();
        break;
    }
    case HK_RESET_PROPS: {
        WallpaperEntry *e = customise_entry();
        if (e) {
            wpapi_clear_properties(e->id);   /* the look stays; the page gets its default values back */
            cus.forceReload = 1;
            customise_refresh();
            app_apply_customise();
        }
        break;
    }
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
    case HK_STORE_TEXT:    pages_store_search_open(); break;
    case HK_STORE_CLEAR:
        if (data == 2) {                 /* the empty page's Reset filters */
            store_set_filter(-1, 0);
            break;
        }
        storep.text[0] = 0;
        pages_store_timer();
        break;
    case HK_STORE_FILTER:  store_filter_menu(data); break;
    case HK_STORE_CARD: {
        wchar_t url[160];
        store_page_url(data, url, SP_COUNTOF(url));
        if (url[0]) shell_open(url);
        break;
    }
    case HK_STORE_DL:      store_download(data); break;
    case HK_STORE_RETRY:   store_retry(); break;
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
    if (HIT_KIND(id) != HK_CARD && HIT_KIND(id) != HK_NAME) return;
    int vi = HIT_DATA(id);
    if (vi < 0 || vi >= u->viewCount) return;
    WallpaperEntry e = g_app.cfg.items[u->view[vi]];   /* copy: the library may change */
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 1, L"Apply to all displays");
    for (int i = 0; i < g_app.monCount && i < 16 && g_app.cfg.s.displayMode == DM_INDEPENDENT; i++) {
        wchar_t l[96];
        str_printf(l, SP_COUNTOF(l), L"Apply to display %d (%.40ls)", g_app.mons[i].index, g_app.mons[i].name);
        AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 100 + (UINT)i, l);
    }
    /* what removing does: a link has no file, a download goes with its entry, anything else stays */
    int link = wallpaper_is_url(e.path), download = url_import_is_download(e.path);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0) | (wcscmp(g_app.cfg.screensaver, e.id) == 0 ? MF_CHECKED : 0), 4,
                L"Use as screensaver");
    AppendMenuW(m, MF_STRING | (e.missing ? MF_GRAYED : 0), 2, link ? L"Open link in browser" : L"Show in File Explorer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, link ? L"Remove from library" : download ? L"Remove from library and delete the download"
                                                                          : L"Remove from library (keeps the file)");
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, u->hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) app_apply_wallpaper(e.id, NULL);
    else if (cmd >= 100 && cmd < 116) app_apply_wallpaper(e.id, g_app.mons[cmd - 100].id);
    else if (cmd == 2 && link) shell_open(e.path);
    else if (cmd == 2) shell_reveal(e.path);
    else if (cmd == 3) library_remove(e.id);
    else if (cmd == 4) app_set_screensaver(wcscmp(g_app.cfg.screensaver, e.id) == 0 ? L"" : e.id);
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
    (void)ctrl;
    if (u->page == PAGE_CUSTOMIZE && customise_key(vk, shift)) return 1;
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

const wchar_t *pages_tooltip(int id)
{
    switch (HIT_KIND(id)) {
    case HK_URL:           return L"Import from a link";
    case HK_IMPORT_FOLDER: return HIT_DATA(id) == 0 ? L"Import a wallpaper folder" : NULL;
    case HK_STORE_CARD: {
        static wchar_t tip[80];
        const StoreState *s = store_get();
        if (HIT_DATA(id) < 0 || HIT_DATA(id) >= s->count) return NULL;
        str_printf(tip, SP_COUNTOF(tip), L"Open on %ls", store_source_name(s->items[HIT_DATA(id)].source));
        return tip;
    }
    case HK_STORE_DL:      return store_dl_tip(HIT_DATA(id));
    default:               return NULL;
    }
}
