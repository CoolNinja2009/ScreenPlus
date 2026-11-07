/* The part of Direct2D and DirectWrite the management window uses, on Cairo and Pango, with the same names,
 * so ui_core.c, ui_pages.c and textedit.c build for Linux almost unchanged. Only what they call is here.
 * win_ui.h does the same for window messages. */
#pragma once
#include "../common/common.h"
#include "win_ui.h"

typedef struct D2D1_RECT_F { float left, top, right, bottom; } D2D1_RECT_F;
typedef struct D2D1_COLOR_F { float r, g, b, a; } D2D1_COLOR_F;
typedef struct D2D1_POINT_2F { float x, y; } D2D1_POINT_2F;
typedef struct D2D1_SIZE_U { uint32_t width, height; } D2D1_SIZE_U;
typedef struct D2D1_ROUNDED_RECT { D2D1_RECT_F rect; float radiusX, radiusY; } D2D1_ROUNDED_RECT;
typedef struct D2D1_ELLIPSE { D2D1_POINT_2F point; float radiusX, radiusY; } D2D1_ELLIPSE;
typedef struct D2D1_MATRIX_3X2_F { float m11, m12, m21, m22, dx, dy; } D2D1_MATRIX_3X2_F;
typedef struct D2D1_PIXEL_FORMAT { int format, alphaMode; } D2D1_PIXEL_FORMAT;
typedef struct D2D1_BITMAP_PROPERTIES { D2D1_PIXEL_FORMAT pixelFormat; float dpiX, dpiY; } D2D1_BITMAP_PROPERTIES;
typedef struct D2D1_BITMAP_BRUSH_PROPERTIES { int extendModeX, extendModeY, interpolationMode; } D2D1_BITMAP_BRUSH_PROPERTIES;
typedef struct D2D1_RENDER_TARGET_PROPERTIES { int type; D2D1_PIXEL_FORMAT pixelFormat; float dpiX, dpiY; } D2D1_RENDER_TARGET_PROPERTIES;
typedef struct D2D1_HWND_RENDER_TARGET_PROPERTIES { HWND hwnd; D2D1_SIZE_U pixelSize; int presentOptions; } D2D1_HWND_RENDER_TARGET_PROPERTIES;
typedef uint32_t UINT32;

enum { DXGI_FORMAT_B8G8R8A8_UNORM = 87, D2D1_ALPHA_MODE_PREMULTIPLIED = 1, D2D1_ALPHA_MODE_IGNORE = 3,
       D2D1_RENDER_TARGET_TYPE_DEFAULT = 0, D2D1_PRESENT_OPTIONS_NONE = 0, D2D1_EXTEND_MODE_CLAMP = 0,
       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR = 1, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE = 0, D2D1_DRAW_TEXT_OPTIONS_CLIP = 2,
       D2D1_TEXT_ANTIALIAS_MODE_DEFAULT = 0, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE = 2, DWRITE_MEASURING_MODE_NATURAL = 0,
       D2D1_FACTORY_TYPE_SINGLE_THREADED = 0, DWRITE_FACTORY_TYPE_SHARED = 0, DWRITE_FONT_STYLE_NORMAL = 0,
       DWRITE_FONT_STRETCH_NORMAL = 5, DWRITE_WORD_WRAPPING_WRAP = 0, DWRITE_WORD_WRAPPING_NO_WRAP = 1,
       DWRITE_PARAGRAPH_ALIGNMENT_NEAR = 0, DWRITE_PARAGRAPH_ALIGNMENT_CENTER = 2, DWRITE_TRIMMING_GRANULARITY_CHARACTER = 1 };
typedef enum DWRITE_FONT_WEIGHT { DWRITE_FONT_WEIGHT_NORMAL = 400, DWRITE_FONT_WEIGHT_SEMI_BOLD = 600 } DWRITE_FONT_WEIGHT;
typedef enum DWRITE_TEXT_ALIGNMENT { DWRITE_TEXT_ALIGNMENT_LEADING = 0, DWRITE_TEXT_ALIGNMENT_TRAILING = 1,
                                     DWRITE_TEXT_ALIGNMENT_CENTER = 2 } DWRITE_TEXT_ALIGNMENT;
typedef struct DWRITE_TRIMMING { int granularity; UINT32 delimiter, delimiterCount; } DWRITE_TRIMMING;
typedef struct DWRITE_TEXT_METRICS { float left, top, width, widthIncludingTrailingWhitespace, height; } DWRITE_TEXT_METRICS;

#define D2DERR_RECREATE_TARGET ((HRESULT)0x8899000C)
#define S_OK 0
#define E_FAIL ((HRESULT)0x80004005)
static const int SP_IID_ID2D1Factory = 0, SP_IID_IDWriteFactory = 0;

/* Every object starts with a vtable holding Release, so common.h's SP_RELEASE works on them. */
typedef struct SpObject SpObject;
typedef struct SpObjectVtbl { void (*Release)(void *self); } SpObjectVtbl;
#undef SP_RELEASE
#define SP_RELEASE(p) do { if (p) { (*(const SpObjectVtbl **)(void *)(p))->Release(p); (p) = NULL; } } while (0)

typedef struct ID2D1Factory      { const SpObjectVtbl *lpVtbl; } ID2D1Factory;
typedef struct ID2D1Bitmap       ID2D1Bitmap;
typedef struct ID2D1Brush        ID2D1Brush, ID2D1SolidColorBrush, ID2D1BitmapBrush;
typedef struct ID2D1RenderTarget ID2D1RenderTarget, ID2D1HwndRenderTarget;

typedef struct IDWriteTextFormat IDWriteTextFormat;
typedef struct IDWriteTextLayout IDWriteTextLayout;
typedef struct IDWriteFactory    IDWriteFactory;
typedef struct IDWriteInlineObject { const SpObjectVtbl *lpVtbl; } IDWriteInlineObject;

struct IDWriteTextFormatVtbl {
    void    (*Release)(void *self);
    HRESULT (*SetWordWrapping)(IDWriteTextFormat *f, int v);
    HRESULT (*SetParagraphAlignment)(IDWriteTextFormat *f, int v);
    HRESULT (*SetTextAlignment)(IDWriteTextFormat *f, int v);
    HRESULT (*SetTrimming)(IDWriteTextFormat *f, const DWRITE_TRIMMING *t, IDWriteInlineObject *sign);
};
struct IDWriteTextFormat { const struct IDWriteTextFormatVtbl *lpVtbl; };

struct IDWriteTextLayoutVtbl {
    void    (*Release)(void *self);
    HRESULT (*GetMetrics)(IDWriteTextLayout *l, DWRITE_TEXT_METRICS *m);
};
struct IDWriteTextLayout { const struct IDWriteTextLayoutVtbl *lpVtbl; };

struct IDWriteFactoryVtbl {
    void    (*Release)(void *self);
    HRESULT (*CreateTextFormat)(IDWriteFactory *f, const wchar_t *family, void *coll, DWRITE_FONT_WEIGHT w, int style, int stretch,
                                float size, const wchar_t *locale, IDWriteTextFormat **out);
    HRESULT (*CreateEllipsisTrimmingSign)(IDWriteFactory *f, IDWriteTextFormat *fmt, IDWriteInlineObject **out);
    HRESULT (*CreateTextLayout)(IDWriteFactory *f, const wchar_t *s, UINT32 n, IDWriteTextFormat *fmt, float maxW, float maxH,
                                IDWriteTextLayout **out);
};
struct IDWriteFactory { const struct IDWriteFactoryVtbl *lpVtbl; };

HRESULT D2D1CreateFactory(int type, const void *iid, void *opts, void **out);
HRESULT DWriteCreateFactory(int type, const void *iid, void **out);
#define IUnknown void

HRESULT ID2D1Factory_CreateHwndRenderTarget(ID2D1Factory *f, const D2D1_RENDER_TARGET_PROPERTIES *rp,
                                            const D2D1_HWND_RENDER_TARGET_PROPERTIES *hp, ID2D1HwndRenderTarget **out);
void    ID2D1HwndRenderTarget_Resize(ID2D1HwndRenderTarget *t, const D2D1_SIZE_U *size);
void    ID2D1RenderTarget_SetDpi(ID2D1RenderTarget *t, float x, float y);
void    ID2D1RenderTarget_SetTextAntialiasMode(ID2D1RenderTarget *t, int mode);
HRESULT ID2D1RenderTarget_CreateSolidColorBrush(ID2D1RenderTarget *t, const D2D1_COLOR_F *c, void *props, ID2D1SolidColorBrush **out);
void    ID2D1SolidColorBrush_SetColor(ID2D1SolidColorBrush *b, const D2D1_COLOR_F *c);
void    ID2D1RenderTarget_BeginDraw(ID2D1RenderTarget *t);
HRESULT ID2D1RenderTarget_EndDraw(ID2D1RenderTarget *t, void *a, void *b);
void    ID2D1RenderTarget_SetTransform(ID2D1RenderTarget *t, const D2D1_MATRIX_3X2_F *m);
void    ID2D1RenderTarget_Clear(ID2D1RenderTarget *t, const D2D1_COLOR_F *c);
void    ID2D1RenderTarget_FillRectangle(ID2D1RenderTarget *t, const D2D1_RECT_F *r, ID2D1Brush *b);
void    ID2D1RenderTarget_FillRoundedRectangle(ID2D1RenderTarget *t, const D2D1_ROUNDED_RECT *r, ID2D1Brush *b);
void    ID2D1RenderTarget_DrawRoundedRectangle(ID2D1RenderTarget *t, const D2D1_ROUNDED_RECT *r, ID2D1Brush *b, float w, void *style);
void    ID2D1RenderTarget_DrawLine(ID2D1RenderTarget *t, D2D1_POINT_2F a, D2D1_POINT_2F b, ID2D1Brush *br, float w, void *style);
void    ID2D1RenderTarget_FillEllipse(ID2D1RenderTarget *t, const D2D1_ELLIPSE *e, ID2D1Brush *b);
void    ID2D1RenderTarget_DrawText(ID2D1RenderTarget *t, const wchar_t *s, UINT32 n, IDWriteTextFormat *f, const D2D1_RECT_F *r,
                                   ID2D1Brush *b, int options, int measuring);
void    ID2D1RenderTarget_PushAxisAlignedClip(ID2D1RenderTarget *t, const D2D1_RECT_F *r, int mode);
void    ID2D1RenderTarget_PopAxisAlignedClip(ID2D1RenderTarget *t);
HRESULT ID2D1RenderTarget_CreateBitmap(ID2D1RenderTarget *t, D2D1_SIZE_U size, const void *bits, UINT32 pitch,
                                       const D2D1_BITMAP_PROPERTIES *p, ID2D1Bitmap **out);
void    ID2D1RenderTarget_DrawBitmap(ID2D1RenderTarget *t, ID2D1Bitmap *b, const D2D1_RECT_F *r, float opacity, int mode, void *src);
HRESULT ID2D1RenderTarget_CreateBitmapBrush(ID2D1RenderTarget *t, ID2D1Bitmap *bmp, const D2D1_BITMAP_BRUSH_PROPERTIES *p,
                                            void *bp, ID2D1BitmapBrush **out);
void    ID2D1BitmapBrush_SetBitmap(ID2D1BitmapBrush *b, ID2D1Bitmap *bmp);
void    ID2D1BitmapBrush_SetTransform(ID2D1BitmapBrush *b, const D2D1_MATRIX_3X2_F *m);
void    ID2D1BitmapBrush_SetOpacity(ID2D1BitmapBrush *b, float o);
void    ID2D1Bitmap_Release(ID2D1Bitmap *b);

/* For ui_sdl.c: the finished frame (BGRA, premultiplied) and its size in pixels. */
const void *d2d_frame(ID2D1RenderTarget *t, int *w, int *h, int *stride);
void d2d_set_icon_font(const char *path);
