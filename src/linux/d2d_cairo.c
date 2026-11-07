/* See d2d_cairo.h. */
#include "d2d_cairo.h"
#include "ui_icons.h"
#include "../common/util.h"
#include <cairo.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <math.h>

#define ICON_FAMILY "FluentSystemIcons-Regular"

struct ID2D1Bitmap {
    const SpObjectVtbl *lpVtbl;
    cairo_surface_t    *s;
    int                 w, h, refs;
};

struct ID2D1Brush {
    const SpObjectVtbl *lpVtbl;
    D2D1_COLOR_F        c;
    ID2D1Bitmap        *bmp;       /* a bitmap brush; holds a reference */
    D2D1_MATRIX_3X2_F   m;
    float               opacity;
};

struct ID2D1RenderTarget {
    const SpObjectVtbl *lpVtbl;
    cairo_surface_t    *s;
    cairo_t            *cr;
    float               dpi;
    int                 w, h;
};

typedef struct Format {
    IDWriteTextFormat base;
    PangoFontDescription *desc;
    int  wrap, center, align, trim, icon;
} Format;

typedef struct Layout {
    IDWriteTextLayout base;
    float w, wTrail, h;
} Layout;

/* Text needs a Cairo context even with no target yet: a 1x1 one, for measuring. */
static cairo_t *measure_cr(void)
{
    static cairo_t *cr;
    if (!cr) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cr = cairo_create(s);
        cairo_surface_destroy(s);
    }
    return cr;
}

/* ------------------------------------------------------------------ factories */

static void release_free(void *self) { free(self); }
static const SpObjectVtbl k_plain = { release_free };

HRESULT D2D1CreateFactory(int type, const void *iid, void *opts, void **out)
{
    (void)type; (void)iid; (void)opts;
    ID2D1Factory *f = (ID2D1Factory *)calloc(1, sizeof *f);
    if (!f) return E_FAIL;
    f->lpVtbl = &k_plain;
    *out = f;
    return S_OK;
}

static void format_release(void *self)
{
    Format *f = (Format *)self;
    pango_font_description_free(f->desc);
    free(f);
}
static HRESULT f_wrap(IDWriteTextFormat *f, int v)   { ((Format *)f)->wrap = v == DWRITE_WORD_WRAPPING_WRAP; return S_OK; }
static HRESULT f_para(IDWriteTextFormat *f, int v)   { ((Format *)f)->center = v == DWRITE_PARAGRAPH_ALIGNMENT_CENTER; return S_OK; }
static HRESULT f_align(IDWriteTextFormat *f, int v)  { ((Format *)f)->align = v; return S_OK; }
static HRESULT f_trim(IDWriteTextFormat *f, const DWRITE_TRIMMING *t, IDWriteInlineObject *s) { (void)t; (void)s; ((Format *)f)->trim = 1; return S_OK; }
static const struct IDWriteTextFormatVtbl k_format = { format_release, f_wrap, f_para, f_align, f_trim };

static HRESULT l_metrics(IDWriteTextLayout *l, DWRITE_TEXT_METRICS *m)
{
    Layout *x = (Layout *)l;
    memset(m, 0, sizeof *m);
    m->width = x->w;
    m->widthIncludingTrailingWhitespace = x->wTrail;
    m->height = x->h;
    return S_OK;
}
static const struct IDWriteTextLayoutVtbl k_layout = { release_free, l_metrics };

/* Icon formats write Segoe glyph codes: each becomes the Fluent System Icons glyph that looks like it. */
static char *text_utf8(const wchar_t *s, UINT32 n, const Format *f)
{
    wchar_t *copy = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!copy) return NULL;
    for (UINT32 i = 0; i < n; i++) copy[i] = f->icon ? (wchar_t)ui_icon_map((uint32_t)s[i]) : s[i];
    copy[n] = 0;
    char *u = utf8_from_wide(copy, NULL);
    free(copy);
    return u;
}

static PangoLayout *make_layout(cairo_t *cr, const char *text, const Format *f, float width)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, f->desc);
    pango_layout_set_text(l, text, -1);
    if (width > 0.0f) pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    if (f->wrap) pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    else if (f->trim && width > 0.0f) pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, f->align == DWRITE_TEXT_ALIGNMENT_CENTER ? PANGO_ALIGN_CENTER :
                                  f->align == DWRITE_TEXT_ALIGNMENT_TRAILING ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    return l;
}

static HRESULT w_format(IDWriteFactory *fa, const wchar_t *family, void *coll, DWRITE_FONT_WEIGHT w, int style, int stretch,
                        float size, const wchar_t *locale, IDWriteTextFormat **out)
{
    (void)fa; (void)coll; (void)style; (void)stretch; (void)locale;
    Format *f = (Format *)calloc(1, sizeof *f);
    if (!f) return E_FAIL;
    f->base.lpVtbl = &k_format;
    f->icon = family && wcsstr(family, L"Icons") != NULL;
    f->icon |= family && wcsstr(family, L"MDL2") != NULL;
    f->desc = pango_font_description_new();
    pango_font_description_set_family(f->desc, f->icon ? ICON_FAMILY : "Inter,Noto Sans,sans-serif");
    pango_font_description_set_weight(f->desc, w == DWRITE_FONT_WEIGHT_SEMI_BOLD ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(f->desc, size * PANGO_SCALE);
    *out = &f->base;
    return S_OK;
}

static HRESULT w_sign(IDWriteFactory *fa, IDWriteTextFormat *f, IDWriteInlineObject **out) { (void)fa; (void)f; *out = NULL; return S_OK; }

static HRESULT w_layout(IDWriteFactory *fa, const wchar_t *s, UINT32 n, IDWriteTextFormat *fmt, float maxW, float maxH, IDWriteTextLayout **out)
{
    (void)fa; (void)maxH;
    Format *f = (Format *)fmt;
    Layout *x = (Layout *)calloc(1, sizeof *x);
    char *u = text_utf8(s, n, f);
    if (!x || !u) {
        free(x); free(u);
        return E_FAIL;
    }
    x->base.lpVtbl = &k_layout;
    Format plain = *f;
    plain.trim = 0;                     /* measuring: never ellipsized */
    PangoLayout *l = make_layout(measure_cr(), u, &plain, f->wrap ? maxW : 0.0f);
    int w, h;
    pango_layout_get_size(l, &w, &h);
    x->w = x->wTrail = (float)w / PANGO_SCALE;
    x->h = (float)h / PANGO_SCALE;
    g_object_unref(l);
    free(u);
    *out = &x->base;
    return S_OK;
}

static const struct IDWriteFactoryVtbl k_factory = { release_free, w_format, w_sign, w_layout };

HRESULT DWriteCreateFactory(int type, const void *iid, void **out)
{
    (void)type; (void)iid;
    IDWriteFactory *f = (IDWriteFactory *)calloc(1, sizeof *f);
    if (!f) return E_FAIL;
    f->lpVtbl = &k_factory;
    *out = f;
    return S_OK;
}

void d2d_set_icon_font(const char *path)
{
    FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)path);
    pango_cairo_font_map_set_default(NULL);     /* the next layout sees the new font */
}

/* ------------------------------------------------------------------ render target */

static void target_release(void *self)
{
    ID2D1RenderTarget *t = (ID2D1RenderTarget *)self;
    if (t->cr) cairo_destroy(t->cr);
    if (t->s) cairo_surface_destroy(t->s);
    free(t);
}
static const SpObjectVtbl k_target = { target_release };

static void target_alloc(ID2D1RenderTarget *t, int w, int h)
{
    if (t->cr) cairo_destroy(t->cr);
    if (t->s) cairo_surface_destroy(t->s);
    t->w = w;
    t->h = h;
    t->s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    t->cr = cairo_create(t->s);
}

HRESULT ID2D1Factory_CreateHwndRenderTarget(ID2D1Factory *f, const D2D1_RENDER_TARGET_PROPERTIES *rp,
                                            const D2D1_HWND_RENDER_TARGET_PROPERTIES *hp, ID2D1HwndRenderTarget **out)
{
    (void)f;
    ID2D1RenderTarget *t = (ID2D1RenderTarget *)calloc(1, sizeof *t);
    if (!t) return E_FAIL;
    t->lpVtbl = &k_target;
    t->dpi = rp->dpiX;
    target_alloc(t, (int)hp->pixelSize.width, (int)hp->pixelSize.height);
    *out = t;
    return S_OK;
}

void ID2D1HwndRenderTarget_Resize(ID2D1HwndRenderTarget *t, const D2D1_SIZE_U *size) { target_alloc(t, (int)size->width, (int)size->height); }
void ID2D1RenderTarget_SetDpi(ID2D1RenderTarget *t, float x, float y) { (void)y; t->dpi = x; }
void ID2D1RenderTarget_SetTextAntialiasMode(ID2D1RenderTarget *t, int mode) { (void)t; (void)mode; }

void ID2D1RenderTarget_BeginDraw(ID2D1RenderTarget *t)
{
    cairo_identity_matrix(t->cr);
    cairo_reset_clip(t->cr);
    cairo_scale(t->cr, t->dpi / 96.0, t->dpi / 96.0);
}

HRESULT ID2D1RenderTarget_EndDraw(ID2D1RenderTarget *t, void *a, void *b)
{
    (void)a; (void)b;
    cairo_surface_flush(t->s);
    return S_OK;
}

void ID2D1RenderTarget_SetTransform(ID2D1RenderTarget *t, const D2D1_MATRIX_3X2_F *m) { (void)t; (void)m; }   /* only ever identity */

const void *d2d_frame(ID2D1RenderTarget *t, int *w, int *h, int *stride)
{
    *w = t->w;
    *h = t->h;
    *stride = cairo_image_surface_get_stride(t->s);
    return cairo_image_surface_get_data(t->s);
}

void ID2D1RenderTarget_Clear(ID2D1RenderTarget *t, const D2D1_COLOR_F *c)
{
    cairo_save(t->cr);
    cairo_set_operator(t->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(t->cr, c->r, c->g, c->b, c->a);
    cairo_paint(t->cr);
    cairo_restore(t->cr);
}

/* ------------------------------------------------------------------ brushes, bitmaps */

static void brush_release(void *self)
{
    ID2D1Brush *b = (ID2D1Brush *)self;
    if (b->bmp) ID2D1Bitmap_Release(b->bmp);
    free(b);
}
static const SpObjectVtbl k_brush = { brush_release };

HRESULT ID2D1RenderTarget_CreateSolidColorBrush(ID2D1RenderTarget *t, const D2D1_COLOR_F *c, void *props, ID2D1SolidColorBrush **out)
{
    (void)t; (void)props;
    ID2D1Brush *b = (ID2D1Brush *)calloc(1, sizeof *b);
    if (!b) return E_FAIL;
    b->lpVtbl = &k_brush;
    b->c = *c;
    b->opacity = 1.0f;
    *out = b;
    return S_OK;
}

void ID2D1SolidColorBrush_SetColor(ID2D1SolidColorBrush *b, const D2D1_COLOR_F *c) { b->c = *c; }

void ID2D1Bitmap_Release(ID2D1Bitmap *b)
{
    if (!b || --b->refs > 0) return;
    cairo_surface_destroy(b->s);
    free(b);
}
static void bitmap_release(void *self) { ID2D1Bitmap_Release((ID2D1Bitmap *)self); }
static const SpObjectVtbl k_bitmap = { bitmap_release };

HRESULT ID2D1RenderTarget_CreateBitmap(ID2D1RenderTarget *t, D2D1_SIZE_U size, const void *bits, UINT32 pitch,
                                       const D2D1_BITMAP_PROPERTIES *p, ID2D1Bitmap **out)
{
    (void)t; (void)p;
    ID2D1Bitmap *b = (ID2D1Bitmap *)calloc(1, sizeof *b);
    if (!b) return E_FAIL;
    b->lpVtbl = &k_bitmap;
    b->refs = 1;
    b->w = (int)size.width;
    b->h = (int)size.height;
    b->s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, b->w, b->h);
    unsigned char *d = cairo_image_surface_get_data(b->s);
    int stride = cairo_image_surface_get_stride(b->s);
    for (int y = 0; y < b->h; y++) memcpy(d + (size_t)y * stride, (const unsigned char *)bits + (size_t)y * pitch, (size_t)b->w * 4);
    cairo_surface_mark_dirty(b->s);
    *out = b;
    return S_OK;
}

void ID2D1RenderTarget_DrawBitmap(ID2D1RenderTarget *t, ID2D1Bitmap *b, const D2D1_RECT_F *r, float opacity, int mode, void *src)
{
    (void)mode; (void)src;
    cairo_save(t->cr);
    cairo_translate(t->cr, r->left, r->top);
    cairo_scale(t->cr, (r->right - r->left) / b->w, (r->bottom - r->top) / b->h);
    cairo_set_source_surface(t->cr, b->s, 0, 0);
    cairo_paint_with_alpha(t->cr, opacity);
    cairo_restore(t->cr);
}

HRESULT ID2D1RenderTarget_CreateBitmapBrush(ID2D1RenderTarget *t, ID2D1Bitmap *bmp, const D2D1_BITMAP_BRUSH_PROPERTIES *p, void *bp, ID2D1BitmapBrush **out)
{
    (void)p; (void)bp;
    D2D1_COLOR_F c = { 0, 0, 0, 1 };
    if (FAILED(ID2D1RenderTarget_CreateSolidColorBrush(t, &c, NULL, out))) return E_FAIL;
    (*out)->m.m11 = (*out)->m.m22 = 1.0f;
    ID2D1BitmapBrush_SetBitmap(*out, bmp);
    return S_OK;
}

void ID2D1BitmapBrush_SetBitmap(ID2D1BitmapBrush *b, ID2D1Bitmap *bmp)
{
    if (bmp) bmp->refs++;
    if (b->bmp) ID2D1Bitmap_Release(b->bmp);
    b->bmp = bmp;
}
void ID2D1BitmapBrush_SetTransform(ID2D1BitmapBrush *b, const D2D1_MATRIX_3X2_F *m) { b->m = *m; }
void ID2D1BitmapBrush_SetOpacity(ID2D1BitmapBrush *b, float o) { b->opacity = o; }

/* ------------------------------------------------------------------ drawing */

static void use_brush(cairo_t *cr, const ID2D1Brush *b)
{
    if (!b->bmp) {
        cairo_set_source_rgba(cr, b->c.r, b->c.g, b->c.b, b->c.a);
        return;
    }
    cairo_set_source_surface(cr, b->bmp->s, 0, 0);
    cairo_pattern_t *p = cairo_get_source(cr);
    cairo_matrix_t m;
    cairo_matrix_init(&m, b->m.m11, b->m.m12, b->m.m21, b->m.m22, b->m.dx, b->m.dy);
    cairo_matrix_invert(&m);            /* a pattern's matrix maps the page back to the picture */
    cairo_pattern_set_matrix(p, &m);
    cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
}

static void round_path(cairo_t *cr, const D2D1_RECT_F *r, float rad)
{
    float w = r->right - r->left, h = r->bottom - r->top;
    rad = fminf(rad, fminf(w, h) * 0.5f);
    cairo_new_sub_path(cr);
    cairo_arc(cr, r->right - rad, r->top + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, r->right - rad, r->bottom - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, r->left + rad, r->bottom - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, r->left + rad, r->top + rad, rad, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

static void fill_path(cairo_t *cr, const ID2D1Brush *b)
{
    use_brush(cr, b);
    if (b->bmp && b->opacity < 1.0f) {
        cairo_save(cr);
        cairo_clip(cr);
        cairo_paint_with_alpha(cr, b->opacity);
        cairo_restore(cr);
    } else {
        cairo_fill(cr);
    }
}

void ID2D1RenderTarget_FillRectangle(ID2D1RenderTarget *t, const D2D1_RECT_F *r, ID2D1Brush *b)
{
    cairo_rectangle(t->cr, r->left, r->top, r->right - r->left, r->bottom - r->top);
    fill_path(t->cr, b);
}

void ID2D1RenderTarget_FillRoundedRectangle(ID2D1RenderTarget *t, const D2D1_ROUNDED_RECT *r, ID2D1Brush *b)
{
    round_path(t->cr, &r->rect, r->radiusX);
    fill_path(t->cr, b);
}

void ID2D1RenderTarget_DrawRoundedRectangle(ID2D1RenderTarget *t, const D2D1_ROUNDED_RECT *r, ID2D1Brush *b, float w, void *style)
{
    (void)style;
    round_path(t->cr, &r->rect, r->radiusX);
    use_brush(t->cr, b);
    cairo_set_line_width(t->cr, w);
    cairo_stroke(t->cr);
}

void ID2D1RenderTarget_DrawLine(ID2D1RenderTarget *t, D2D1_POINT_2F a, D2D1_POINT_2F b, ID2D1Brush *br, float w, void *style)
{
    (void)style;
    cairo_move_to(t->cr, a.x, a.y);
    cairo_line_to(t->cr, b.x, b.y);
    use_brush(t->cr, br);
    cairo_set_line_width(t->cr, w);
    cairo_stroke(t->cr);
}

void ID2D1RenderTarget_FillEllipse(ID2D1RenderTarget *t, const D2D1_ELLIPSE *e, ID2D1Brush *b)
{
    cairo_new_sub_path(t->cr);
    cairo_arc(t->cr, e->point.x, e->point.y, e->radiusX, 0, 2 * M_PI);
    fill_path(t->cr, b);
}

void ID2D1RenderTarget_DrawText(ID2D1RenderTarget *t, const wchar_t *s, UINT32 n, IDWriteTextFormat *fmt, const D2D1_RECT_F *r,
                                ID2D1Brush *b, int options, int measuring)
{
    (void)options; (void)measuring;
    Format *f = (Format *)fmt;
    char *u = text_utf8(s, n, f);
    if (!u) return;
    float w = r->right - r->left, h = r->bottom - r->top;
    PangoLayout *l = make_layout(t->cr, u, f, w);
    int lw, lh;
    pango_layout_get_size(l, &lw, &lh);
    float y = r->top + (f->center ? (h - (float)lh / PANGO_SCALE) * 0.5f : 0.0f);
    cairo_save(t->cr);
    cairo_rectangle(t->cr, r->left, r->top, w, h);
    cairo_clip(t->cr);
    use_brush(t->cr, b);
    cairo_move_to(t->cr, r->left, y);
    pango_cairo_show_layout(t->cr, l);
    cairo_restore(t->cr);
    g_object_unref(l);
    free(u);
}

void ID2D1RenderTarget_PushAxisAlignedClip(ID2D1RenderTarget *t, const D2D1_RECT_F *r, int mode)
{
    (void)mode;
    cairo_save(t->cr);
    cairo_rectangle(t->cr, r->left, r->top, r->right - r->left, r->bottom - r->top);
    cairo_clip(t->cr);
}

void ID2D1RenderTarget_PopAxisAlignedClip(ID2D1RenderTarget *t) { cairo_restore(t->cr); }
