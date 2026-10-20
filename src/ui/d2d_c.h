/* C declarations for the Direct2D 1.0 interfaces used by the UI.
 * Current Windows SDKs ship d2d1.h with only opaque C typedefs, so the vtables are declared
 * here in the exact order of d2d1.h (SDK 10.0.26100). Unused slots are void*. Methods that
 * return structures by value are never called from C (their ABI differs). */
#pragma once
#include <d2d1.h>

#define SP_D2D_UNKNOWN(T)                                                    \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);       \
    ULONG (STDMETHODCALLTYPE *AddRef)(T *);                                  \
    ULONG (STDMETHODCALLTYPE *Release)(T *);
#define SP_D2D_RESOURCE(T) SP_D2D_UNKNOWN(T) void *GetFactory;

typedef struct ID2D1BitmapVtbl {
    SP_D2D_RESOURCE(ID2D1Bitmap)
    void *GetSize, *GetPixelSize, *GetPixelFormat, *GetDpi, *CopyFromBitmap, *CopyFromRenderTarget, *CopyFromMemory;
} ID2D1BitmapVtbl;
struct ID2D1Bitmap { const ID2D1BitmapVtbl *lpVtbl; };

#define SP_D2D_BRUSH(T)                                                      \
    SP_D2D_RESOURCE(T)                                                       \
    void (STDMETHODCALLTYPE *SetOpacity)(T *, FLOAT);                        \
    void (STDMETHODCALLTYPE *SetTransform)(T *, const D2D1_MATRIX_3X2_F *);  \
    void *GetOpacity, *GetTransform;

typedef struct ID2D1BrushVtbl { SP_D2D_BRUSH(ID2D1Brush) } ID2D1BrushVtbl;
struct ID2D1Brush { const ID2D1BrushVtbl *lpVtbl; };

typedef struct ID2D1SolidColorBrushVtbl {
    SP_D2D_BRUSH(ID2D1SolidColorBrush)
    void (STDMETHODCALLTYPE *SetColor)(ID2D1SolidColorBrush *, const D2D1_COLOR_F *);
    void *GetColor;
} ID2D1SolidColorBrushVtbl;
struct ID2D1SolidColorBrush { const ID2D1SolidColorBrushVtbl *lpVtbl; };

typedef struct ID2D1BitmapBrushVtbl {
    SP_D2D_BRUSH(ID2D1BitmapBrush)
    void *SetExtendModeX, *SetExtendModeY, *SetInterpolationMode;
    void (STDMETHODCALLTYPE *SetBitmap)(ID2D1BitmapBrush *, ID2D1Bitmap *);
    void *GetExtendModeX, *GetExtendModeY, *GetInterpolationMode, *GetBitmap;
} ID2D1BitmapBrushVtbl;
struct ID2D1BitmapBrush { const ID2D1BitmapBrushVtbl *lpVtbl; };

#define SP_D2D_RENDERTARGET(T)                                                                                       \
    SP_D2D_RESOURCE(T)                                                                                               \
    HRESULT (STDMETHODCALLTYPE *CreateBitmap)(T *, D2D1_SIZE_U, const void *, UINT32, const D2D1_BITMAP_PROPERTIES *, \
                                              ID2D1Bitmap **);                                                       \
    void *CreateBitmapFromWicBitmap, *CreateSharedBitmap;                                                            \
    HRESULT (STDMETHODCALLTYPE *CreateBitmapBrush)(T *, ID2D1Bitmap *, const D2D1_BITMAP_BRUSH_PROPERTIES *,          \
                                                   const D2D1_BRUSH_PROPERTIES *, ID2D1BitmapBrush **);              \
    HRESULT (STDMETHODCALLTYPE *CreateSolidColorBrush)(T *, const D2D1_COLOR_F *, const D2D1_BRUSH_PROPERTIES *,      \
                                                       ID2D1SolidColorBrush **);                                     \
    void *CreateGradientStopCollection, *CreateLinearGradientBrush, *CreateRadialGradientBrush,                      \
         *CreateCompatibleRenderTarget, *CreateLayer, *CreateMesh;                                                   \
    void (STDMETHODCALLTYPE *DrawLine)(T *, D2D1_POINT_2F, D2D1_POINT_2F, ID2D1Brush *, FLOAT, ID2D1StrokeStyle *);  \
    void *DrawRectangle;                                                                                             \
    void (STDMETHODCALLTYPE *FillRectangle)(T *, const D2D1_RECT_F *, ID2D1Brush *);                                 \
    void (STDMETHODCALLTYPE *DrawRoundedRectangle)(T *, const D2D1_ROUNDED_RECT *, ID2D1Brush *, FLOAT,              \
                                                   ID2D1StrokeStyle *);                                              \
    void (STDMETHODCALLTYPE *FillRoundedRectangle)(T *, const D2D1_ROUNDED_RECT *, ID2D1Brush *);                    \
    void *DrawEllipse;                                                                                               \
    void (STDMETHODCALLTYPE *FillEllipse)(T *, const D2D1_ELLIPSE *, ID2D1Brush *);                                  \
    void *DrawGeometry, *FillGeometry, *FillMesh, *FillOpacityMask, *DrawBitmap;                                     \
    void (STDMETHODCALLTYPE *DrawText)(T *, const WCHAR *, UINT32, IDWriteTextFormat *, const D2D1_RECT_F *,         \
                                       ID2D1Brush *, D2D1_DRAW_TEXT_OPTIONS, DWRITE_MEASURING_MODE);                 \
    void *DrawTextLayout, *DrawGlyphRun;                                                                             \
    void (STDMETHODCALLTYPE *SetTransform)(T *, const D2D1_MATRIX_3X2_F *);                                          \
    void *GetTransform, *SetAntialiasMode, *GetAntialiasMode;                                                        \
    void (STDMETHODCALLTYPE *SetTextAntialiasMode)(T *, D2D1_TEXT_ANTIALIAS_MODE);                                   \
    void *GetTextAntialiasMode, *SetTextRenderingParams, *GetTextRenderingParams, *SetTags, *GetTags, *PushLayer,    \
         *PopLayer, *Flush, *SaveDrawingState, *RestoreDrawingState;                                                 \
    void (STDMETHODCALLTYPE *PushAxisAlignedClip)(T *, const D2D1_RECT_F *, D2D1_ANTIALIAS_MODE);                    \
    void (STDMETHODCALLTYPE *PopAxisAlignedClip)(T *);                                                               \
    void (STDMETHODCALLTYPE *Clear)(T *, const D2D1_COLOR_F *);                                                      \
    void (STDMETHODCALLTYPE *BeginDraw)(T *);                                                                        \
    HRESULT (STDMETHODCALLTYPE *EndDraw)(T *, D2D1_TAG *, D2D1_TAG *);                                               \
    void *GetPixelFormat;                                                                                            \
    void (STDMETHODCALLTYPE *SetDpi)(T *, FLOAT, FLOAT);                                                             \
    void *GetDpi, *GetSize, *GetPixelSize, *GetMaximumBitmapSize, *IsSupported;

typedef struct ID2D1RenderTargetVtbl { SP_D2D_RENDERTARGET(ID2D1RenderTarget) } ID2D1RenderTargetVtbl;
struct ID2D1RenderTarget { const ID2D1RenderTargetVtbl *lpVtbl; };

typedef struct ID2D1HwndRenderTargetVtbl {
    SP_D2D_RENDERTARGET(ID2D1HwndRenderTarget)
    void *CheckWindowState;
    HRESULT (STDMETHODCALLTYPE *Resize)(ID2D1HwndRenderTarget *, const D2D1_SIZE_U *);
    void *GetHwnd;
} ID2D1HwndRenderTargetVtbl;
struct ID2D1HwndRenderTarget { const ID2D1HwndRenderTargetVtbl *lpVtbl; };

typedef struct ID2D1FactoryVtbl {
    SP_D2D_UNKNOWN(ID2D1Factory)
    void *ReloadSystemMetrics, *GetDesktopDpi, *CreateRectangleGeometry, *CreateRoundedRectangleGeometry,
         *CreateEllipseGeometry, *CreateGeometryGroup, *CreateTransformedGeometry, *CreatePathGeometry,
         *CreateStrokeStyle, *CreateDrawingStateBlock, *CreateWicBitmapRenderTarget;
    HRESULT (STDMETHODCALLTYPE *CreateHwndRenderTarget)(ID2D1Factory *, const D2D1_RENDER_TARGET_PROPERTIES *,
                                                        const D2D1_HWND_RENDER_TARGET_PROPERTIES *, ID2D1HwndRenderTarget **);
    void *CreateDxgiSurfaceRenderTarget, *CreateDCRenderTarget;
} ID2D1FactoryVtbl;
struct ID2D1Factory { const ID2D1FactoryVtbl *lpVtbl; };

/* {06152247-6f50-465a-9245-118bfd3b6007} */
static const IID SP_IID_ID2D1Factory = { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };

#define ID2D1Factory_CreateHwndRenderTarget(T, a, b, c)       ((T)->lpVtbl->CreateHwndRenderTarget(T, a, b, c))
#define ID2D1RenderTarget_CreateBitmap(T, a, b, c, d, e)        ((T)->lpVtbl->CreateBitmap(T, a, b, c, d, e))
#define ID2D1RenderTarget_CreateBitmapBrush(T, a, b, c, d)      ((T)->lpVtbl->CreateBitmapBrush(T, a, b, c, d))
#define ID2D1RenderTarget_CreateSolidColorBrush(T, a, b, c)     ((T)->lpVtbl->CreateSolidColorBrush(T, a, b, c))
#define ID2D1RenderTarget_DrawLine(T, a, b, c, d, e)            ((T)->lpVtbl->DrawLine(T, a, b, c, d, e))
#define ID2D1RenderTarget_FillRectangle(T, a, b)                ((T)->lpVtbl->FillRectangle(T, a, b))
#define ID2D1RenderTarget_DrawRoundedRectangle(T, a, b, c, d)   ((T)->lpVtbl->DrawRoundedRectangle(T, a, b, c, d))
#define ID2D1RenderTarget_FillRoundedRectangle(T, a, b)         ((T)->lpVtbl->FillRoundedRectangle(T, a, b))
#define ID2D1RenderTarget_FillEllipse(T, a, b)                  ((T)->lpVtbl->FillEllipse(T, a, b))
#define ID2D1RenderTarget_DrawText(T, a, b, c, d, e, f, g)      ((T)->lpVtbl->DrawText(T, a, b, c, d, e, f, g))
#define ID2D1RenderTarget_SetTransform(T, a)                    ((T)->lpVtbl->SetTransform(T, a))
#define ID2D1RenderTarget_SetTextAntialiasMode(T, a)            ((T)->lpVtbl->SetTextAntialiasMode(T, a))
#define ID2D1RenderTarget_PushAxisAlignedClip(T, a, b)          ((T)->lpVtbl->PushAxisAlignedClip(T, a, b))
#define ID2D1RenderTarget_PopAxisAlignedClip(T)                 ((T)->lpVtbl->PopAxisAlignedClip(T))
#define ID2D1RenderTarget_Clear(T, a)                           ((T)->lpVtbl->Clear(T, a))
#define ID2D1RenderTarget_BeginDraw(T)                          ((T)->lpVtbl->BeginDraw(T))
#define ID2D1RenderTarget_EndDraw(T, a, b)                      ((T)->lpVtbl->EndDraw(T, a, b))
#define ID2D1RenderTarget_SetDpi(T, a, b)                       ((T)->lpVtbl->SetDpi(T, a, b))
#define ID2D1HwndRenderTarget_Resize(T, a)                      ((T)->lpVtbl->Resize(T, a))
#define ID2D1SolidColorBrush_SetColor(T, a)                     ((T)->lpVtbl->SetColor(T, a))
#define ID2D1BitmapBrush_SetTransform(T, a)                     ((T)->lpVtbl->SetTransform(T, a))
#define ID2D1BitmapBrush_SetOpacity(T, a)                       ((T)->lpVtbl->SetOpacity(T, a))
#define ID2D1BitmapBrush_SetBitmap(T, a)                        ((T)->lpVtbl->SetBitmap(T, a))
