/* Management window core: window lifecycle, Direct2D/DirectWrite resources, theme,
 * drawing primitives, widgets, hit testing, keyboard focus, scrolling and animation. */
#include "ui_internal.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../app/preview.h"
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <math.h>

#define SP_DWMWA_DARK      20
#define SP_DWMWA_CORNER    33
#define SP_DWMWA_BACKDROP  38
#define SP_DWMSBT_ACRYLIC  3

#define RT ((ID2D1RenderTarget *)g_ui->rt)
#define MAX_THUMBS 160
#define MAX_ANIMATED 24   /* animations kept resident; more than a screenful, bounded memory */



Ui *g_ui;
static const wchar_t k_class[] = L"ScreenPlus.Window";
static D2D1_RECT_F g_clip[4];
static int g_clipDepth;

/* ================================================================ colors & geometry */

D2D1_COLOR_F col(uint32_t rgb, float a)
{
    D2D1_COLOR_F c = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a };
    return c;
}

D2D1_COLOR_F col_a(D2D1_COLOR_F c, float a)
{
    c.a = a;
    return c;
}

D2D1_RECT_F rectf(float x, float y, float w, float h)
{
    D2D1_RECT_F r = { x, y, x + w, y + h };
    return r;
}

int in_rect(D2D1_RECT_F r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

/* ================================================================ theme */

static int system_dark(void)
{
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return 0;
    return v == 0;
}

static void theme_build(Ui *u)
{
    int dark = g_app.cfg.s.theme == THEME_DARK || (g_app.cfg.s.theme == THEME_SYSTEM && system_dark());
    Theme *t = &u->t;
    t->dark = dark;
    if (dark) {
        t->bg = col(0x1C1C1E, 1.0f);
        t->tint = col(0x121214, u->backdrop ? 0.60f : 1.0f);
        t->sidebar = col(0xFFFFFF, 0.035f);
        t->navActive = col(0xFFFFFF, 0.09f);
        t->panel = col(0xFFFFFF, 0.055f);
        t->panelHover = col(0xFFFFFF, 0.09f);
        t->border = col(0xFFFFFF, 0.09f);
        t->highlight = col(0xFFFFFF, 0.11f);
        t->text = col(0xF5F5F7, 1.0f);
        t->text2 = col(0xEBEBF5, 0.62f);
        t->text3 = col(0xEBEBF5, 0.36f);
        t->accent = col(0x0A84FF, 1.0f);
        t->success = col(0x32D74B, 1.0f);
        t->warning = col(0xFF9F0A, 1.0f);
        t->danger = col(0xFF453A, 1.0f);
        t->chip = col(0xFFFFFF, 0.075f);
        t->field = col(0x000000, 0.24f);
        t->separator = col(0xFFFFFF, 0.08f);
        t->scrim = col(0x000000, 0.52f);
        t->track = col(0x787880, 0.38f);
        t->shadow = col(0x000000, 0.30f);
    } else {
        t->bg = col(0xF2F2F7, 1.0f);
        t->tint = col(0xF5F5F7, u->backdrop ? 0.68f : 1.0f);
        t->sidebar = col(0xFFFFFF, 0.32f);
        t->navActive = col(0x000000, 0.06f);
        t->panel = col(0xFFFFFF, 0.64f);
        t->panelHover = col(0xFFFFFF, 0.86f);
        t->border = col(0x000000, 0.08f);
        t->highlight = col(0xFFFFFF, 0.95f);
        t->text = col(0x1D1D1F, 1.0f);
        t->text2 = col(0x3C3C43, 0.70f);
        t->text3 = col(0x3C3C43, 0.44f);
        t->accent = col(0x007AFF, 1.0f);
        t->success = col(0x28A745, 1.0f);
        t->warning = col(0xE08600, 1.0f);
        t->danger = col(0xFF3B30, 1.0f);
        t->chip = col(0x000000, 0.055f);
        t->field = col(0x000000, 0.05f);
        t->separator = col(0x000000, 0.08f);
        t->scrim = col(0x000000, 0.46f);
        t->track = col(0x787880, 0.24f);
        t->shadow = col(0x000000, 0.10f);
    }
    t->onAccent = col(0xFFFFFF, 1.0f);
    BOOL d = dark;
    if (u->hwnd) DwmSetWindowAttribute(u->hwnd, SP_DWMWA_DARK, &d, sizeof d);
}

static void setup_backdrop(Ui *u)
{
    DWORD corner = 2;   /* DWMWCP_ROUND */
    DwmSetWindowAttribute(u->hwnd, SP_DWMWA_CORNER, &corner, sizeof corner);
    u->backdrop = 0;
    if (windows_build_number() >= 22621) {   /* DWMWA_SYSTEMBACKDROP_TYPE: Windows 11 22H2+ */
        MARGINS m = { -1, -1, -1, -1 };
        DWORD type = SP_DWMSBT_ACRYLIC;
        if (SUCCEEDED(DwmExtendFrameIntoClientArea(u->hwnd, &m)) &&
            SUCCEEDED(DwmSetWindowAttribute(u->hwnd, SP_DWMWA_BACKDROP, &type, sizeof type)))
            u->backdrop = 1;
    }
}

/* ================================================================ device resources */

static IDWriteTextFormat *mkfmt(Ui *u, const wchar_t *family, float size, DWRITE_FONT_WEIGHT w,
                                DWRITE_TEXT_ALIGNMENT align, int wrap, int trim)
{
    IDWriteTextFormat *f = NULL;
    if (FAILED(u->dw->lpVtbl->CreateTextFormat(u->dw, family, NULL, w, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)) || !f)
        return NULL;
    f->lpVtbl->SetWordWrapping(f, wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    f->lpVtbl->SetParagraphAlignment(f, wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->lpVtbl->SetTextAlignment(f, align);
    if (trim) {
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject *sign = NULL;
        u->dw->lpVtbl->CreateEllipsisTrimmingSign(u->dw, f, &sign);
        f->lpVtbl->SetTrimming(f, &tr, sign);
        if (sign) sign->lpVtbl->Release(sign);
    }
    return f;
}

static BOOL create_text_formats(Ui *u)
{
    int win11 = windows_build_number() >= 22000;
    const wchar_t *disp = win11 ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *text = win11 ? L"Segoe UI Variable Text" : L"Segoe UI";
    u->iconFont = win11 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    u->fTitle   = mkfmt(u, disp, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fHeading = mkfmt(u, disp, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBrand   = mkfmt(u, disp, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBody    = mkfmt(u, text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBodyB   = mkfmt(u, text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fSmall   = mkfmt(u, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fSmallB  = mkfmt(u, text, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fCaption = mkfmt(u, text, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fCenter  = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 1);
    u->fCenterS = mkfmt(u, text, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 1);
    u->fWrap    = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 1, 0);
    u->fWrapC   = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 1, 0);
    u->fIcon    = mkfmt(u, u->iconFont, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    u->fIconS   = mkfmt(u, u->iconFont, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    u->fIconL   = mkfmt(u, u->iconFont, 36.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    return u->fTitle && u->fHeading && u->fBrand && u->fBody && u->fBodyB && u->fSmall && u->fSmallB && u->fCaption &&
           u->fCenter && u->fCenterS && u->fWrap && u->fWrapC && u->fIcon && u->fIconS && u->fIconL;
}

static void release_text_formats(Ui *u)
{
    IDWriteTextFormat **all[] = { &u->fTitle, &u->fHeading, &u->fBrand, &u->fBody, &u->fBodyB, &u->fSmall, &u->fSmallB,
                                  &u->fCaption, &u->fCenter, &u->fCenterS, &u->fWrap, &u->fWrapC, &u->fIcon, &u->fIconS,
                                  &u->fIconL };
    for (size_t i = 0; i < SP_COUNTOF(all); i++) SP_RELEASE(*all[i]);
}

static void thumb_release(Thumb *t)
{
    SP_RELEASE(t->brush);
    SP_RELEASE(t->bmp);
    for (int i = 0; i < t->frameCount; i++) SP_RELEASE(t->frames[i]);
    free(t->frames);
    free(t->delays);
    t->frames = NULL;
    t->delays = NULL;
    t->frameCount = t->cur = 0;
    t->nextAt = 0;
}

static void disc_release(void);

static void discard_device_resources(Ui *u)
{
    disc_release();
    for (int i = 0; i < u->thumbCount; i++) {
        thumb_release(&u->thumbs[i]);
        u->thumbs[i].state = 0;
    }
    SP_RELEASE(u->brush);
    SP_RELEASE(u->rt);
}

static BOOL create_device_resources(Ui *u)
{
    if (u->rt) return TRUE;
    RECT rc;
    GetClientRect(u->hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp;
    ZeroMemory(&rp, sizeof rp);
    rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rp.dpiX = rp.dpiY = 96.0f * u->scale;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp;
    hp.hwnd = u->hwnd;
    hp.pixelSize.width = (UINT32)SP_MAX(rc.right, 1);
    hp.pixelSize.height = (UINT32)SP_MAX(rc.bottom, 1);
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(u->d2d, &rp, &hp, &u->rt);
    if (FAILED(hr) && u->backdrop) {      /* no alpha support: fall back to an opaque window */
        rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        u->backdrop = 0;
        theme_build(u);
        hr = ID2D1Factory_CreateHwndRenderTarget(u->d2d, &rp, &hp, &u->rt);
    }
    if (FAILED(hr)) {
        LOGE(L"ui: CreateHwndRenderTarget failed 0x%08lX", (unsigned long)hr);
        return FALSE;
    }
    ID2D1RenderTarget_SetTextAntialiasMode(RT, u->backdrop ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    D2D1_COLOR_F white = { 1, 1, 1, 1 };
    hr = ID2D1RenderTarget_CreateSolidColorBrush(RT, &white, NULL, &u->brush);
    if (FAILED(hr)) {
        SP_RELEASE(u->rt);
        return FALSE;
    }
    return TRUE;
}

/* ================================================================ primitives */

static void set_color(D2D1_COLOR_F c)
{
    c.a *= g_ui->alpha;
    ID2D1SolidColorBrush_SetColor(g_ui->brush, &c);
}

void ui_fill(D2D1_RECT_F r, D2D1_COLOR_F c)
{
    if (c.a <= 0.0f) return;
    set_color(c);
    ID2D1RenderTarget_FillRectangle(RT, &r, (ID2D1Brush *)g_ui->brush);
}

void ui_rrect(D2D1_RECT_F r, float radius, D2D1_COLOR_F c)
{
    if (c.a <= 0.0f) return;
    D2D1_ROUNDED_RECT rr = { r, radius, radius };
    set_color(c);
    ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, (ID2D1Brush *)g_ui->brush);
}

void ui_rrect_stroke(D2D1_RECT_F r, float radius, float width, D2D1_COLOR_F c)
{
    float h = width * 0.5f;
    D2D1_ROUNDED_RECT rr = { { r.left + h, r.top + h, r.right - h, r.bottom - h }, radius, radius };
    set_color(c);
    ID2D1RenderTarget_DrawRoundedRectangle(RT, &rr, (ID2D1Brush *)g_ui->brush, width, NULL);
}

void ui_line(float x0, float y0, float x1, float y1, float width, D2D1_COLOR_F c)
{
    D2D1_POINT_2F a = { x0, y0 }, b = { x1, y1 };
    set_color(c);
    ID2D1RenderTarget_DrawLine(RT, a, b, (ID2D1Brush *)g_ui->brush, width, NULL);
}

void ui_circle(float cx, float cy, float r, D2D1_COLOR_F c)
{
    D2D1_ELLIPSE e = { { cx, cy }, r, r };
    set_color(c);
    ID2D1RenderTarget_FillEllipse(RT, &e, (ID2D1Brush *)g_ui->brush);
}

void ui_text(const wchar_t *s, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c)
{
    if (!s || !s[0] || !f) return;
    set_color(c);
    ID2D1RenderTarget_DrawText(RT, s, (UINT32)wcslen(s), f, &r, (ID2D1Brush *)g_ui->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                               DWRITE_MEASURING_MODE_NATURAL);
}

float ui_text_width(const wchar_t *s, IDWriteTextFormat *f)
{
    if (!s || !s[0] || !f) return 0.0f;
    IDWriteTextLayout *l = NULL;
    float w = 0.0f;
    if (SUCCEEDED(g_ui->dw->lpVtbl->CreateTextLayout(g_ui->dw, s, (UINT32)wcslen(s), f, 4096.0f, 200.0f, &l)) && l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->lpVtbl->GetMetrics(l, &m))) w = m.widthIncludingTrailingWhitespace;
        l->lpVtbl->Release(l);
    }
    return w;
}

float ui_text_height(const wchar_t *s, IDWriteTextFormat *f, float w)
{
    if (!s || !s[0] || !f) return 0.0f;
    IDWriteTextLayout *l = NULL;
    float h = 0.0f;
    if (SUCCEEDED(g_ui->dw->lpVtbl->CreateTextLayout(g_ui->dw, s, (UINT32)wcslen(s), f, SP_MAX(w, 1.0f), 4096.0f, &l)) && l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->lpVtbl->GetMetrics(l, &m))) h = m.height;
        l->lpVtbl->Release(l);
    }
    return h;
}

void ui_icon(wchar_t glyph, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c)
{
    wchar_t s[2] = { glyph, 0 };
    ui_text(s, f, r, c);
}

void ui_glass(D2D1_RECT_F r, float radius, int hover, int elevated)
{
    Theme *t = &g_ui->t;
    if (elevated) {
        for (int i = 0; i < 3; i++) {
            float o = 3.0f + i * 3.0f, g = (float)i * 1.5f;
            D2D1_RECT_F s = { r.left - g, r.top + o - g, r.right + g, r.bottom + o + g };
            ui_rrect(s, radius + g, col_a(t->shadow, t->shadow.a * (0.30f - i * 0.09f)));
        }
    }
    ui_rrect(r, radius, hover ? t->panelHover : t->panel);
    ui_rrect_stroke(r, radius, 1.0f, t->border);
    if (t->dark) {   /* thin specular edge: the "glass" rim */
        D2D1_RECT_F hl = { r.left + radius, r.top + 1.0f, r.right - radius, r.top + 2.0f };
        ui_fill(hl, t->highlight);
    }
}

void ui_push_clip(D2D1_RECT_F r)
{
    if (g_clipDepth < 4) g_clip[g_clipDepth] = r;
    g_clipDepth++;
    ID2D1RenderTarget_PushAxisAlignedClip(RT, &r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void ui_pop_clip(void)
{
    if (g_clipDepth > 0) g_clipDepth--;
    ID2D1RenderTarget_PopAxisAlignedClip(RT);
}

void ui_hit(int id, D2D1_RECT_F r, int focusable, int cursor)
{
    Ui *u = g_ui;
    if (g_clipDepth > 0) {
        D2D1_RECT_F c = g_clip[SP_MIN(g_clipDepth, 4) - 1];
        r.left = SP_MAX(r.left, c.left);
        r.top = SP_MAX(r.top, c.top);
        r.right = SP_MIN(r.right, c.right);
        r.bottom = SP_MIN(r.bottom, c.bottom);
        if (r.right <= r.left || r.bottom <= r.top) return;
    }
    if (u->hitCount >= UI_MAX_HITS) return;
    Hit *h = &u->hits[u->hitCount++];
    h->id = id;
    h->r = r;
    h->focusable = focusable;
    h->cursor = cursor;
}

int ui_is_hot(int id) { return g_ui->hot == id; }
int ui_is_down(int id) { return g_ui->pressed == id && g_ui->hot == id; }

void ui_focus_ring(D2D1_RECT_F r, float radius, int id)
{
    if (!g_ui->focusVisible || g_ui->focus != id) return;
    D2D1_RECT_F o = { r.left - 3, r.top - 3, r.right + 3, r.bottom + 3 };
    ui_rrect_stroke(o, radius + 3, 2.0f, g_ui->t.accent);
}

void ui_invalidate(void)
{
    if (g_ui && g_ui->hwnd) InvalidateRect(g_ui->hwnd, NULL, FALSE);
}

void ui_animate(void)
{
    Ui *u = g_ui;
    if (!u || u->animating) return;
    u->animating = 1;
    u->lastTick = time_ms();
    SetTimer(u->hwnd, TM_ANIM, 15, NULL);
}

const wchar_t *ui_type_icon(WallpaperType t)
{
    static wchar_t s[2];
    s[0] = t == WP_VIDEO ? GL_VIDEO : t == WP_HTML ? GL_CODE : GL_PHOTO;
    s[1] = 0;
    return s;
}

/* ================================================================ widgets */

float ui_button_width(const wchar_t *label, wchar_t icon)
{
    float w = label && label[0] ? ui_text_width(label, g_ui->fCenter) + 30.0f : 34.0f;
    if (icon && label && label[0]) w += 22.0f;
    return w;
}

void ui_button(int id, D2D1_RECT_F r, const wchar_t *label, wchar_t icon, int style)
{
    Theme *t = &g_ui->t;
    int hot = ui_is_hot(id), down = ui_is_down(id);
    D2D1_COLOR_F bg, fg = t->text;
    float radius = 9.0f;
    switch (style) {
    case BTN_PRIMARY:
        bg = t->accent;
        if (hot) bg = col_a(bg, 0.88f);
        if (down) bg = col_a(bg, 0.76f);
        fg = t->onAccent;
        break;
    case BTN_GHOST:
        bg = hot ? t->chip : col_a(t->chip, 0.0f);
        fg = t->text2;
        if (hot) fg = t->text;
        break;
    case BTN_DANGER:
        bg = hot ? col_a(t->danger, 0.22f) : col_a(t->danger, 0.14f);
        fg = t->danger;
        break;
    default:
        bg = hot ? t->panelHover : t->chip;
        if (down) bg = t->chip;
        break;
    }
    ui_rrect(r, radius, bg);
    if (style == BTN_SECONDARY) ui_rrect_stroke(r, radius, 1.0f, t->border);
    float cy = r.top, h = r.bottom - r.top;
    if (label && label[0]) {
        float tw = ui_text_width(label, g_ui->fCenter);
        float total = tw + (icon ? 22.0f : 0.0f);
        float x = (r.left + r.right) * 0.5f - total * 0.5f;
        if (icon) {
            ui_icon(icon, g_ui->fIconS, rectf(x, cy, 16.0f, h), fg);
            x += 22.0f;
        }
        ui_text(label, g_ui->fCenter, rectf(x - 2.0f, cy, tw + 4.0f, h), fg);
    } else if (icon) {
        ui_icon(icon, g_ui->fIcon, r, fg);
    }
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, radius, id);
}

void ui_chip(int id, D2D1_RECT_F r, const wchar_t *label, int active)
{
    Theme *t = &g_ui->t;
    int hot = ui_is_hot(id);
    float radius = (r.bottom - r.top) * 0.5f;
    if (active) ui_rrect(r, radius, t->accent);
    else ui_rrect(r, radius, hot ? t->panelHover : t->chip);
    if (!active) ui_rrect_stroke(r, radius, 1.0f, col_a(t->border, t->border.a * 0.8f));
    ui_text(label, g_ui->fCenter, r, active ? t->onAccent : (hot ? t->text : t->text2));
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, radius, id);
}

void ui_toggle(int id, float x, float y, int index, int on)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    float target = on ? 1.0f : 0.0f;
    if (index >= 0 && index < TOG_COUNT) {
        if (u->tog[index] < 0.0f) u->tog[index] = target;   /* first frame: no animation */
        u->togTarget[index] = target;
        if (fabsf(u->tog[index] - target) > 0.001f) ui_animate();
    }
    float p = index >= 0 && index < TOG_COUNT ? u->tog[index] : target;
    D2D1_RECT_F track = rectf(x, y, 42.0f, 24.0f);
    D2D1_COLOR_F off = t->track, onc = t->accent;
    D2D1_COLOR_F c = { off.r + (onc.r - off.r) * p, off.g + (onc.g - off.g) * p, off.b + (onc.b - off.b) * p,
                       off.a + (onc.a - off.a) * p };
    ui_rrect(track, 12.0f, c);
    float kx = x + 12.0f + p * 18.0f;
    ui_circle(kx, y + 12.5f, 10.0f, col(0x000000, 0.12f));
    ui_circle(kx, y + 12.0f, 10.0f, col(0xFFFFFF, 1.0f));
    D2D1_RECT_F hr = { x - 4, y - 4, x + 46, y + 28 };
    ui_hit(id, hr, 1, 1);
    ui_focus_ring(track, 12.0f, id);
}

void ui_pill(D2D1_RECT_F r, const wchar_t *label, D2D1_COLOR_F bg, D2D1_COLOR_F fg)
{
    ui_rrect(r, (r.bottom - r.top) * 0.5f, bg);
    ui_text(label, g_ui->fCenterS, r, fg);
}

/* ---------------------------------------------------------------- colour helpers */

D2D1_COLOR_F ui_hsv(float h, float s, float v, float a)
{
    h = h - floorf(h / 360.0f) * 360.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return (D2D1_COLOR_F){ r + m, g + m, b + m, a };
}

void ui_rgb_hsv(D2D1_COLOR_F c, float *hue, float *s, float *v)
{
    float mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    float mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    float d = mx - mn;
    float h = 0;
    if (d > 0) {
        if (mx == c.r) h = 60.0f * fmodf((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = 60.0f * ((c.b - c.r) / d + 2.0f);
        else h = 60.0f * ((c.r - c.g) / d + 4.0f);
    }
    if (h < 0) h += 360.0f;
    *hue = h;
    *s = mx > 0 ? d / mx : 0.0f;
    *v = mx;
}

/* ---------------------------------------------------------------- sliders + colour wheel */

/* Track + filled portion + WinUI-style thumb. While pressed the value follows the pointer via
 * pages_drag (pressed hit rect -> position), committed live and on release. */
void ui_slider(int id, D2D1_RECT_F r, float v)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    v = SP_CLAMP(v, 0.0f, 1.0f);
    int hot = ui_is_hot(id), down = u->pressed == id;
    float cy = (r.top + r.bottom) * 0.5f, inset = 11.0f;
    D2D1_RECT_F track = rectf(r.left + inset, cy - 2.0f, (r.right - r.left) - inset * 2.0f, 4.0f);
    ui_rrect(track, 2.0f, t->track);
    ui_rrect(rectf(track.left, cy - 2.0f, (track.right - track.left) * v, 4.0f), 2.0f, t->accent);
    float kx = track.left + (track.right - track.left) * v;
    ui_circle(kx, cy + 0.5f, 10.5f, col(0x000000, t->dark ? 0.40f : 0.14f));
    ui_circle(kx, cy, 10.0f, t->dark ? col(0x454545, 1.0f) : col(0xFFFFFF, 1.0f));
    ui_circle(kx, cy, down ? 4.5f : hot ? 6.5f : 5.5f, t->accent);
    ui_hit(id, r, 1, 1);
    ui_focus_ring(track, 4.0f, id);
}

static ID2D1Bitmap *g_disc;          /* hue/saturation wheel, rendered at device pixels */
static int          g_discPx;        /* its edge length in pixels: rebuilt when the size or DPI changes */

static void disc_release(void)
{
    if (g_disc) ID2D1Bitmap_Release(g_disc);
    g_disc = NULL;
    g_discPx = 0;
}

/* Hue around the circle (0 = red at 3 o'clock, clockwise like the picking maths), saturation
 * from white at the centre to full colour at the rim. Premultiplied BGRA, anti-aliased rim,
 * fully transparent outside. */
static void disc_build(Ui *u, int px)
{
    disc_release();
    BYTE *bits = (BYTE *)calloc((size_t)px * px, 4);
    if (!bits) return;
    float half = px * 0.5f;
    for (int y = 0; y < px; y++) {
        for (int x = 0; x < px; x++) {
            float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
            float len = sqrtf(dx * dx + dy * dy);
            float a = SP_CLAMP((1.0f - len) * half + 0.5f, 0.0f, 1.0f);   /* ~1 px soft edge */
            if (a <= 0.0f) continue;
            D2D1_COLOR_F c = ui_hsv(atan2f(dy, dx) * 180.0f / 3.14159265f, SP_MIN(len, 1.0f), 1.0f, 1.0f);
            BYTE *p = bits + ((size_t)y * px + x) * 4;
            p[0] = (BYTE)(c.b * a * 255.0f + 0.5f);
            p[1] = (BYTE)(c.g * a * 255.0f + 0.5f);
            p[2] = (BYTE)(c.r * a * 255.0f + 0.5f);
            p[3] = (BYTE)(a * 255.0f + 0.5f);
        }
    }
    D2D1_BITMAP_PROPERTIES pr;
    ZeroMemory(&pr, sizeof pr);
    pr.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    pr.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    pr.dpiX = pr.dpiY = 96.0f * u->scale;
    D2D1_SIZE_U size = { (UINT32)px, (UINT32)px };
    ID2D1Bitmap *bmp = NULL;
    if (SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, size, bits, (UINT32)px * 4, &pr, &bmp))) {
        g_disc = bmp;
        g_discPx = px;
    }
    free(bits);
}

void ui_disc(int id, float cx, float cy, float radius, float hue, float sat)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    int px = (int)ceilf(radius * 2.0f * u->scale);
    if (!g_disc || g_discPx != px) disc_build(u, px);
    ui_circle(cx, cy, radius + 1.5f, t->border);                 /* hairline rim */
    D2D1_RECT_F dr = rectf(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    if (g_disc) ID2D1RenderTarget_DrawBitmap(RT, g_disc, &dr, u->alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    /* marker: hue angle + saturation distance from the centre, filled with the picked colour */
    float a = hue * 3.14159265f / 180.0f, dist = SP_CLAMP(sat, 0.0f, 1.0f) * radius;
    float mx = cx + cosf(a) * dist, my = cy + sinf(a) * dist;
    float grow = ui_is_down(id) ? 1.5f : 0.0f;
    ui_circle(mx, my + 0.5f, 9.0f + grow, col(0x000000, 0.30f));
    ui_circle(mx, my, 8.0f + grow, col(0xFFFFFF, 1.0f));
    ui_circle(mx, my, 5.5f + grow, ui_hsv(hue, sat, 1.0f, 1.0f));
    ui_hit(id, dr, 1, 1);
    ui_focus_ring(rectf(cx - radius - 2, cy - radius - 2, radius * 2 + 4, radius * 2 + 4), radius + 2, id);
}

/* ================================================================ thumbnails */

static Thumb *thumb_get(const wchar_t *id, int large)
{
    Ui *u = g_ui;
    for (int i = 0; i < u->thumbCount; i++)
        if (u->thumbs[i].large == large && wcscmp(u->thumbs[i].id, id) == 0) return &u->thumbs[i];
    if (u->thumbCount >= MAX_THUMBS) {          /* evict the least recently drawn */
        int old = -1;
        for (int i = 0; i < u->thumbCount; i++)
            if (u->thumbs[i].used != u->frame && (old < 0 || u->thumbs[i].used < u->thumbs[old].used)) old = i;
        if (old < 0) return NULL;
        thumb_release(&u->thumbs[old]);
        u->thumbs[old] = u->thumbs[--u->thumbCount];
    }
    if (u->thumbCount == u->thumbCap) {
        int nc = u->thumbCap ? u->thumbCap * 2 : 32;
        Thumb *p = (Thumb *)realloc(u->thumbs, sizeof(Thumb) * (size_t)nc);
        if (!p) return NULL;
        u->thumbs = p;
        u->thumbCap = nc;
    }
    Thumb *t = &u->thumbs[u->thumbCount++];
    ZeroMemory(t, sizeof *t);
    str_copy(t->id, SP_ID_LEN, id);
    t->large = large;
    return t;
}

void ui_draw_thumb(const WallpaperEntry *e, D2D1_RECT_F r, float radius, int large)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    float rw = r.right - r.left, rh = r.bottom - r.top;
    Thumb *th = thumb_get(e->id, large);
    if (th) {
        th->used = u->frame;
        int pw = ((int)(rw * u->scale) + 15) & ~15, ph = ((int)(rh * u->scale) + 15) & ~15;
        if (th->state == 2 && (pw > th->reqW * 5 / 4 || ph > th->reqH * 5 / 4)) th->state = 0;   /* grew: reload sharper */
        if (th->state == 0) {
            th->state = 1;
            th->reqW = pw;      /* animations are stored smaller than requested: compare with the request */
            th->reqH = ph;
            thumbs_request(e, pw, ph, large);
        }
    }
    if (th && th->state == 2 && th->brush) {
        if (th->frameCount > 1) {
            ID2D1BitmapBrush_SetBitmap(th->brush, th->frames[th->cur]);
            BOOL motion = TRUE;      /* Windows "Animation effects" off: stay on the first frame */
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
            if (motion && !u->thumbTimer) {
                u->thumbTimer = 1;
                SetTimer(u->hwnd, TM_THUMBS, 33, NULL);   /* ~30 Hz: due frames advance together */
            }
        }
        float s = SP_MAX(rw / th->w, rh / th->h);
        D2D1_MATRIX_3X2_F m = { s, 0, 0, s, r.left + (rw - th->w * s) * 0.5f, r.top + (rh - th->h * s) * 0.5f };
        ID2D1BitmapBrush_SetTransform(th->brush, &m);
        ID2D1BitmapBrush_SetOpacity(th->brush, u->alpha);
        D2D1_ROUNDED_RECT rr = { r, radius, radius };
        ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, (ID2D1Brush *)th->brush);
        return;
    }
    ui_rrect(r, radius, t->chip);
    wchar_t g = e->type == WP_VIDEO ? GL_VIDEO : e->type == WP_HTML ? GL_CODE : GL_PHOTO;
    ui_icon(g, rh > 120 ? u->fIconL : u->fIcon, r, t->text3);
}

/* Replaces a thumbnail's still with its animation; on any failure the still stays. */
static void set_animation(Ui *u, Thumb *t, ThumbResult *res)
{
    if (!u->rt) return;
    ID2D1Bitmap **frames = (ID2D1Bitmap **)calloc((size_t)res->frames, sizeof *frames);
    ID2D1BitmapBrush *brush = NULL;
    D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.0f, 96.0f };
    D2D1_SIZE_U sz = { (UINT32)res->w, (UINT32)res->h };
    size_t fsz = (size_t)res->w * res->h * 4;
    int n = 0;
    while (frames && n < res->frames &&
           SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, sz, res->bgra + fsz * (size_t)n, (UINT32)res->w * 4, &bp, &frames[n])))
        n++;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
    if (n != res->frames || FAILED(ID2D1RenderTarget_CreateBitmapBrush(RT, frames[0], &bbp, NULL, &brush))) {
        for (int i = 0; i < n; i++) SP_RELEASE(frames[i]);
        free(frames);
        return;
    }
    /* Over budget: the least recently drawn other animation goes (it reloads when seen again). */
    int animated = 0, lru = -1;
    for (int i = 0; i < u->thumbCount; i++) {
        Thumb *o = &u->thumbs[i];
        if (o == t || o->frameCount < 2) continue;
        animated++;
        if (o->used != u->frame && (lru < 0 || o->used < u->thumbs[lru].used)) lru = i;
    }
    if (animated >= MAX_ANIMATED && lru >= 0) {
        thumb_release(&u->thumbs[lru]);
        u->thumbs[lru].state = 0;
    }
    thumb_release(t);
    t->frames = frames;
    t->frameCount = n;
    t->delays = res->delays;
    res->delays = NULL;
    t->brush = brush;
    t->w = res->w;
    t->h = res->h;
    t->state = 2;
    ui_invalidate();
}

/* Advances the animated thumbnails drawn in the last frame; stops when none are on screen. */
static void thumbs_tick(Ui *u)
{
    double now = time_ms();
    int onScreen = 0, changed = 0;
    if (IsWindowVisible(u->hwnd) && !IsIconic(u->hwnd)) {
        for (int i = 0; i < u->thumbCount; i++) {
            Thumb *t = &u->thumbs[i];
            if (t->frameCount < 2 || t->state != 2 || t->used != u->frame) continue;
            onScreen = 1;
            if (!t->nextAt) t->nextAt = now + t->delays[t->cur];
            if (now < t->nextAt) continue;
            t->cur = (t->cur + 1) % t->frameCount;
            double due = t->nextAt + t->delays[t->cur];
            t->nextAt = due < now ? now + t->delays[t->cur] : due;   /* fell behind (e.g. hidden): resync */
            changed = 1;
        }
    }
    if (changed) ui_invalidate();
    if (!onScreen) {
        KillTimer(u->hwnd, TM_THUMBS);
        u->thumbTimer = 0;
    }
}

static void on_thumb(Ui *u, ThumbResult *res)
{
    for (int i = 0; i < u->thumbCount; i++) {
        Thumb *t = &u->thumbs[i];
        if (t->large != res->large || wcscmp(t->id, res->id) != 0) continue;
        if (res->frames > 1) {
            if (res->bgra && res->delays) set_animation(u, t, res);
            break;
        }
        thumb_release(t);
        t->state = 3;
        if (res->bgra && u->rt) {
            D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.0f, 96.0f };
            D2D1_SIZE_U sz = { (UINT32)res->w, (UINT32)res->h };
            if (SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, sz, res->bgra, (UINT32)res->w * 4, &bp, &t->bmp))) {
                D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
                if (SUCCEEDED(ID2D1RenderTarget_CreateBitmapBrush(RT, t->bmp, &bbp, NULL, &t->brush))) {
                    t->w = res->w;
                    t->h = res->h;
                    t->state = 2;
                }
            }
        }
        ui_invalidate();
        break;
    }
    thumbs_free(res);
}

/* ================================================================ painting */

static void paint(Ui *u)
{
    if (!create_device_resources(u)) return;
    u->frame++;
    u->hitCount = 0;
    g_clipDepth = 0;
    ID2D1RenderTarget_BeginDraw(RT);
    D2D1_MATRIX_3X2_F identity = { 1, 0, 0, 1, 0, 0 };
    ID2D1RenderTarget_SetTransform(RT, &identity);
    D2D1_COLOR_F clear = u->backdrop ? col(0, 0.0f) : u->t.bg;
    ID2D1RenderTarget_Clear(RT, &clear);
    u->alpha = 1.0f;
    if (u->backdrop) ui_fill(rectf(0, 0, u->w, u->h), u->t.tint);
    pages_draw();
    HRESULT hr = ID2D1RenderTarget_EndDraw(RT, NULL, NULL);
    if (hr == D2DERR_RECREATE_TARGET) {
        LOGW(L"ui: render target lost; recreating");
        discard_device_resources(u);
        ui_invalidate();
    }
    if (!g_app.uiReadyMs) {
        g_app.uiReadyMs = (double)(filetime_now() - g_app.processStart) / 10000.0;
        LOGI(L"ui: first frame %.1f ms after process start (window open took %.1f ms)", g_app.uiReadyMs,
             time_ms() - u->showRequestedAt);
    }
}

static void anim_step(Ui *u)
{
    double now = time_ms();
    float dt = (float)SP_MIN(now - u->lastTick, 50.0) / 1000.0f;
    u->lastTick = now;
    int active = 0;
    if (u->pageAnim < 1.0f) {
        u->pageAnim = SP_MIN(1.0f, u->pageAnim + dt / 0.18f);
        active = 1;
    }
    for (int p = 0; p < PAGE_COUNT; p++) {
        float d = u->scrollTarget[p] - u->scroll[p];
        if (fabsf(d) > 0.5f) {
            u->scroll[p] += d * SP_MIN(1.0f, dt * 14.0f);
            active = 1;
        } else {
            u->scroll[p] = u->scrollTarget[p];
        }
    }
    for (int i = 0; i < TOG_COUNT; i++) {
        float d = u->togTarget[i] - u->tog[i];
        if (fabsf(d) > 0.01f) {
            u->tog[i] += d * SP_MIN(1.0f, dt * 16.0f);
            active = 1;
        } else {
            u->tog[i] = u->togTarget[i];
        }
    }
    if (!active) {
        u->animating = 0;
        KillTimer(u->hwnd, TM_ANIM);
    }
    ui_invalidate();
}

/* ================================================================ input */

static int hit_at(Ui *u, float x, float y)
{
    for (int i = u->hitCount - 1; i >= 0; i--)
        if (in_rect(u->hits[i].r, x, y)) return u->hits[i].id;
    return 0;
}

static const Hit *hit_find(Ui *u, int id)
{
    for (int i = 0; i < u->hitCount; i++)
        if (u->hits[i].id == id) return &u->hits[i];
    return NULL;
}

/* Every field that supports typing goes through the text engine (textedit.c). */
static int is_text_kind(int kind)
{
    return kind == HK_SEARCH || kind == HK_URL_TEXT || kind == HK_NAME || kind == HK_STORE_TEXT ||
           kind == HK_PTEXT || kind == HK_PVAL || kind == HK_LOOKVAL;
}

/* Click streak on the same spot: 1 caret, 2 word, 3+ the whole text. */
static int text_clicks(Ui *u, int id, float x, float y)
{
    static int lastId = 0, streak = 1;
    static DWORD lastAt = 0;
    static float lastX = 0.0f, lastY = 0.0f;
    DWORD now = GetTickCount();
    float lim = 4.0f * u->scale;
    if (id == lastId && now - lastAt < GetDoubleClickTime() && fabsf(x - lastX) <= lim && fabsf(y - lastY) <= lim)
        streak += id == lastId ? 1 : 0;
    else streak = 1;
    lastId = id;
    lastAt = now;
    lastX = x;
    lastY = y;
    return streak;
}

static void text_press(Ui *u, int id, float x, float y)
{
    pages_activate(id, 0);              /* opens (or keeps open) the field */
    te_click(text_clicks(u, id, x, y), x);
    ui_invalidate();
}

static void move_focus(Ui *u, int dir)
{
    int n = u->hitCount, cur = -1;
    for (int i = 0; i < n; i++)
        if (u->hits[i].id == u->focus) cur = i;
    for (int step = 1; step <= n; step++) {
        int i = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * step + n * 4) % n;
        if (u->hits[i].focusable) {
            u->focus = u->hits[i].id;
            u->focusVisible = 1;
            if (HIT_KIND(u->focus) == HK_SEARCH) pages_search_open();
            else if (u->textFocus && HIT_KIND(u->textFocus) == HK_SEARCH) pages_text_end(1);
            u->searchFocus = HIT_KIND(u->focus) == HK_SEARCH;
            pages_scroll_into_view(u->focus);
            ui_invalidate();
            return;
        }
    }
}

static void set_search_focus(Ui *u, int on)
{
    if (on) {
        pages_search_open();
        return;
    }
    u->searchFocus = 0;
    if (u->textFocus && HIT_KIND(u->textFocus) == HK_SEARCH) pages_text_end(0);
    else KillTimer(u->hwnd, TM_CARET);
    ui_invalidate();
}

static void on_key(Ui *u, WPARAM vk)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && vk == 'F') {
        if (u->page == PAGE_STORE) {
            pages_store_search_open();
            return;
        }
        u->page = PAGE_LIBRARY;
        u->focus = HIT_ID(HK_SEARCH, 0);
        set_search_focus(u, 1);
        return;
    }
    if (ctrl && vk == 'O') {
        library_import_dialog(u->hwnd, shift);
        return;
    }
    if (ctrl && vk >= '1' && vk < '1' + PAGE_COUNT) {
        pages_activate(HIT_ID(HK_NAV, (int)(vk - '1')), 0);
        return;
    }
    if (u->textFocus && pages_text_key((UINT)vk, ctrl, shift)) return;
    switch (vk) {
    case VK_TAB:
        move_focus(u, shift ? -1 : 1);
        return;
    case VK_ESCAPE:
        if (u->searchFocus) set_search_focus(u, 0);   /* the engine cleared the text; an empty box closes */
        return;
    case VK_RETURN:
    case VK_SPACE:
        if (u->searchFocus && vk == VK_SPACE) return;   /* typed via WM_CHAR */
        if (u->focus && !u->searchFocus) {
            u->focusVisible = 1;
            pages_activate(u->focus, vk == VK_RETURN);
        }
        return;
    case VK_APPS:
        if (u->focus) {
            const Hit *h = hit_find(u, u->focus);
            if (h) {
                POINT p = { (LONG)(h->r.left * u->scale), (LONG)(h->r.bottom * u->scale) };
                ClientToScreen(u->hwnd, &p);
                pages_context(u->focus, p.x, p.y);
            }
        }
        return;
    case VK_F5:
        app_restart_renderer();
        return;
    default:
        if (!u->searchFocus) pages_key((UINT)vk, ctrl, shift);
        return;
    }
}

static void on_char(Ui *u, wchar_t ch)
{
    pages_text_char(ch);
}

/* One tooltip, moved onto whichever icon-only button is hovered (texts: pages_tooltip). */
static void tip_update(Ui *u)
{
    const wchar_t *text = u->hot && !u->pressed ? pages_tooltip(u->hot) : NULL;
    const Hit *h = text ? hit_find(u, u->hot) : NULL;
    int id = h ? u->hot : 0;
    if (id == u->tipId) return;
    u->tipId = id;
    if (!u->tip && h) {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        u->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, u->hwnd, NULL, g_app.inst, NULL);
        TOOLINFOW add = { sizeof add, TTF_SUBCLASS, u->hwnd, 1 };
        add.lpszText = L"";
        if (u->tip) {
            SendMessageW(u->tip, TTM_ADDTOOLW, 0, (LPARAM)&add);
            SetWindowTheme(u->tip, u->t.dark ? L"DarkMode_Explorer" : NULL, NULL);
        }
    }
    if (!u->tip) return;
    TOOLINFOW ti = { sizeof ti, TTF_SUBCLASS, u->hwnd, 1 };
    ti.lpszText = L"";                             /* empty rect and text: nothing to show */
    if (h) {
        ti.rect.left = (LONG)(h->r.left * u->scale);
        ti.rect.top = (LONG)(h->r.top * u->scale);
        ti.rect.right = (LONG)(h->r.right * u->scale);
        ti.rect.bottom = (LONG)(h->r.bottom * u->scale);
        ti.lpszText = (LPWSTR)text;
    }
    SendMessageW(u->tip, TTM_POP, 0, 0);
    SendMessageW(u->tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    SendMessageW(u->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

static void on_wheel(Ui *u, int delta)
{
    int p = u->page;
    float maxScroll = SP_MAX(0.0f, u->contentH[p] - u->viewH);
    u->scrollTarget[p] = SP_CLAMP(u->scrollTarget[p] - (float)delta / WHEEL_DELTA * 96.0f, 0.0f, maxScroll);
    ui_animate();
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Ui *u = g_ui;
    if (!u || (u->hwnd && u->hwnd != h)) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_DPICHANGED: {
        u->scale = HIWORD(wp) / 96.0f;
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (u->rt) ID2D1RenderTarget_SetDpi(RT, 96.0f * u->scale, 96.0f * u->scale);
        ui_invalidate();
        return 0;
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp), hh = HIWORD(lp);
        u->w = w / u->scale;
        u->h = hh / u->scale;
        if (u->rt) {
            D2D1_SIZE_U sz = { w, hh };
            ID2D1HwndRenderTarget_Resize(u->rt, &sz);
        }
        ui_invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = (LONG)(940 * u->scale);
        mm->ptMinTrackSize.y = (LONG)(640 * u->scale);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint(u);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        u->mx = GET_X_LPARAM(lp) / u->scale;
        u->my = GET_Y_LPARAM(lp) / u->scale;
        if (!u->tracking) {
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            u->tracking = 1;
        }
        int hot = hit_at(u, u->mx, u->my);
        if (hot != u->hot) {                            /* keep the drag target while the button is down */
            u->hot = HIT_KIND(u->pressed) == HK_SLIDER || HIT_KIND(u->pressed) == HK_DISC ||
                     HIT_KIND(u->pressed) == HK_PSLIDER ? u->pressed : hot;
            ui_invalidate();
            tip_update(u);
        }
        if (u->pressed) {                               /* continuous controls: slider + colour wheel */
            int k = HIT_KIND(u->pressed);
            if (k == HK_SLIDER || k == HK_DISC || k == HK_PSLIDER) pages_drag(u->pressed, u->mx, u->my, 0);
        }
        u->hot = is_text_kind(HIT_KIND(u->pressed)) && u->textFocus == u->pressed ? u->hot : hot;
        if (u->pressed == u->textFocus && te_active()) te_drag(u->mx);   /* drag selection in a text field */
        return 0;
    }
    case WM_MOUSELEAVE:
        u->tracking = 0;
        if (u->hot) {
            u->hot = 0;
            ui_invalidate();
            tip_update(u);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const Hit *hit = u->hot ? hit_find(u, u->hot) : NULL;
            LPCWSTR c = hit && hit->cursor == 1 ? IDC_HAND : hit && hit->cursor == 2 ? IDC_IBEAM : IDC_ARROW;
            SetCursor(LoadCursorW(NULL, c));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        u->pressed = u->hot;
        u->pressedRect = rectf(0, 0, 0, 0);
        if (u->hot) {
            const Hit *hit = hit_find(u, u->hot);
            if (hit) {
                u->pressedRect = hit->r;
                if (hit->focusable) u->focus = u->hot;
            }
        }
        if (u->textFocus && u->hot != u->textFocus)
            pages_text_end(1);   /* commit when the click lands elsewhere */
        {
            int k = HIT_KIND(u->pressed);   /* continuous controls jump to the click right away */
            if (k == HK_SLIDER || k == HK_DISC || k == HK_PSLIDER) pages_drag(u->pressed, u->mx, u->my, 0);
        }
        set_search_focus(u, HIT_KIND(u->hot) == HK_SEARCH);
        if (is_text_kind(HIT_KIND(u->hot))) text_press(u, u->hot, u->mx, u->my);   /* caret to the click */
        ui_invalidate();
        return 0;
    case WM_LBUTTONUP: {
        int id = u->pressed;
        u->pressed = 0;
        ReleaseCapture();
        if (id) {
            int k = HIT_KIND(id);
            if (k == HK_SLIDER || k == HK_DISC || k == HK_PSLIDER) pages_drag(id, u->mx, u->my, 1);
            if (id == u->hot && k != HK_SEARCH && !is_text_kind(k)) pages_activate(id, 0);
        }
        ui_invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (u->hot) {
            int k = HIT_KIND(u->hot);
            if (k == HK_CARD) pages_activate(u->hot, 1);
            else if (is_text_kind(k)) text_press(u, u->hot, u->mx, u->my);   /* 2nd click: word selection */
        }
        return 0;
    case WM_RBUTTONUP:
        if (u->hot) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &p);
            pages_context(u->hot, p.x, p.y);
        }
        return 0;
    case WM_MOUSEWHEEL:
        on_wheel(u, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp != VK_F10) break;
        if (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0) wp = VK_APPS;
        on_key(u, wp);
        return 0;
    case WM_CHAR:
        on_char(u, (wchar_t)wp);
        return 0;
    case WM_TIMER:
        if (wp == TM_ANIM) anim_step(u);
        else if (wp == TM_THUMBS) thumbs_tick(u);
        else if (wp == TM_STORE) pages_store_timer();
        else if (wp == TM_CARET) {
            u->caretOn = !u->caretOn;
            ui_invalidate();
        } else if (wp == TM_STATS) {
            if (!IsIconic(h)) {
                supervisor_sample_stats();
                ui_invalidate();
            }
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        wchar_t **paths = (wchar_t **)calloc(n ? n : 1, sizeof(wchar_t *));
        int got = 0;
        for (UINT i = 0; paths && i < n; i++) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            wchar_t *p = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
            if (p && DragQueryFileW(drop, i, p, len + 1)) paths[got++] = p;
            else free(p);
        }
        DragFinish(drop);
        if (got) library_import((const wchar_t *const *)paths, got, TRUE);
        for (int i = 0; i < got; i++) free(paths[i]);
        free(paths);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0) {
            theme_build(u);
            ui_invalidate();
        }
        return 0;
    case WM_ACTIVATE:
        ui_invalidate();
        break;
    case WM_SP_PREVIEW: {        /* a recorded preview arrived: reload that wallpaper's thumbnails */
        wchar_t *id = (wchar_t *)lp;
        for (int i = 0; id && i < u->thumbCount; i++) {
            if (wcscmp(u->thumbs[i].id, id) == 0) {
                thumb_release(&u->thumbs[i]);
                u->thumbs[i].state = 0;
            }
        }
        free(id);
        ui_invalidate();
        return 0;
    }
    case WM_SP_THUMB:
        on_thumb(u, (ThumbResult *)lp);
        return 0;
    case WM_CLOSE:
        LOGI(L"ui: window closed (Screen+ keeps running in the notification area)");
        ui_destroy();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================================================================ public */

BOOL ui_show_screensaver(void)
{
    if (!ui_show() || !g_ui) return FALSE;
    g_ui->page = PAGE_SETTINGS;
    g_ui->pageAnim = 0.0f;
    g_ui->scroll[PAGE_SETTINGS] = g_ui->scrollTarget[PAGE_SETTINGS] = 0.0f;
    ui_animate();
    return TRUE;
}

BOOL ui_show(void)
{
    if (g_ui) {
        if (IsIconic(g_ui->hwnd)) ShowWindow(g_ui->hwnd, SW_RESTORE);
        else ShowWindow(g_ui->hwnd, SW_SHOW);
        SetForegroundWindow(g_ui->hwnd);
        return TRUE;
    }
    Ui *u = (Ui *)calloc(1, sizeof *u);
    if (!u) return FALSE;
    g_ui = u;
    u->showRequestedAt = time_ms();
    u->target = -1;
    u->textFocus = 0;    /* no Customise field is being typed into */
    u->alpha = 1.0f;
    u->pageAnim = 1.0f;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &SP_IID_ID2D1Factory, NULL, (void **)&u->d2d);
    if (SUCCEEDED(hr)) hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &SP_IID_IDWriteFactory, (IUnknown **)&u->dw);
    if (FAILED(hr) || !create_text_formats(u)) {
        LOGE(L"ui: Direct2D/DirectWrite initialization failed 0x%08lX", (unsigned long)hr);
        ui_destroy();
        return FALSE;
    }

    static int registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = ui_proc;
        wc.hInstance = g_app.inst;
        wc.hIcon = LoadIconW(g_app.inst, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), 0);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = k_class;
        registered = RegisterClassExW(&wc) != 0;
    }

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    u->scale = dpi / 96.0f;
    int ww = (int)(1180 * u->scale), wh = (int)(760 * u->scale);
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    ww = SP_MIN(ww, aw * 94 / 100);
    wh = SP_MIN(wh, ah * 94 / 100);
    int x = mi.rcWork.left + (aw - ww) / 2, y = mi.rcWork.top + (ah - wh) / 2;

    u->hwnd = CreateWindowExW(0, k_class, L"Screen+", WS_OVERLAPPEDWINDOW, x, y, ww, wh, NULL, NULL, g_app.inst, NULL);
    if (!u->hwnd) {
        LOGE(L"ui: CreateWindow failed (err %lu)", GetLastError());
        ui_destroy();
        return FALSE;
    }
    u->scale = GetDpiForWindow(u->hwnd) / 96.0f;
    RECT rc;
    GetClientRect(u->hwnd, &rc);
    u->w = rc.right / u->scale;
    u->h = rc.bottom / u->scale;
    setup_backdrop(u);
    theme_build(u);
    for (int i = 0; i < TOG_COUNT; i++) u->tog[i] = u->togTarget[i] = -1.0f;   /* snap on first frame */
    DragAcceptFiles(u->hwnd, TRUE);
    thumbs_start(u->hwnd);
    pages_rebuild_view();
    SetTimer(u->hwnd, TM_STATS, 1500, NULL);
    supervisor_sample_stats();
    ShowWindow(u->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(u->hwnd);
    UpdateWindow(u->hwnd);
    return TRUE;
}

void ui_destroy(void)
{
    Ui *u = g_ui;
    if (!u) return;
    g_ui = NULL;
    thumbs_stop();
    if (u->hwnd) {
        KillTimer(u->hwnd, TM_ANIM);
        KillTimer(u->hwnd, TM_CARET);
        KillTimer(u->hwnd, TM_STATS);
        KillTimer(u->hwnd, TM_THUMBS);
        DestroyWindow(u->hwnd);
    }
    for (int i = 0; i < u->thumbCount; i++) thumb_release(&u->thumbs[i]);
    free(u->thumbs);
    free(u->view);
    disc_release();
    SP_RELEASE(u->brush);
    SP_RELEASE(u->rt);
    release_text_formats(u);
    SP_RELEASE(u->dw);
    SP_RELEASE(u->d2d);
    free(u);
}

void ui_on_status(void)
{
    ui_invalidate();
}

void ui_refresh_theme(void)
{
    if (!g_ui) return;
    theme_build(g_ui);
    if (g_ui->tip) SetWindowTheme(g_ui->tip, g_ui->t.dark ? L"DarkMode_Explorer" : NULL, NULL);
    ui_invalidate();
}

void ui_on_config_changed(void)
{
    if (!g_ui) return;
    pages_rebuild_view();
    theme_build(g_ui);   /* the theme setting may have changed (CLI) */
    ui_invalidate();
}

void ui_on_customise_changed(void)
{
    pages_customise_reload();   /* the page keeps its cache while closed too */
    if (g_ui) ui_invalidate();
}

BOOL ui_translate(MSG *m)
{
    (void)m;
    return FALSE;
}
