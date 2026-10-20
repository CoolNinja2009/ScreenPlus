/* Minimal C declarations for the DirectWrite interfaces Screen+ uses.
 * The Windows SDK's dwrite.h is C++-only; these vtables mirror its declaration order exactly
 * (verified against dwrite.h of SDK 10.0.26100). Unused slots are typed as void*. */
#pragma once
#include <d2d1.h>

typedef enum DWRITE_FACTORY_TYPE { DWRITE_FACTORY_TYPE_SHARED = 0, DWRITE_FACTORY_TYPE_ISOLATED = 1 } DWRITE_FACTORY_TYPE;
typedef enum DWRITE_FONT_WEIGHT {
    DWRITE_FONT_WEIGHT_LIGHT = 300, DWRITE_FONT_WEIGHT_NORMAL = 400, DWRITE_FONT_WEIGHT_MEDIUM = 500,
    DWRITE_FONT_WEIGHT_SEMI_BOLD = 600, DWRITE_FONT_WEIGHT_BOLD = 700
} DWRITE_FONT_WEIGHT;
typedef enum DWRITE_FONT_STYLE { DWRITE_FONT_STYLE_NORMAL = 0 } DWRITE_FONT_STYLE;
typedef enum DWRITE_FONT_STRETCH { DWRITE_FONT_STRETCH_NORMAL = 5 } DWRITE_FONT_STRETCH;
typedef enum DWRITE_TEXT_ALIGNMENT {
    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_JUSTIFIED
} DWRITE_TEXT_ALIGNMENT;
typedef enum DWRITE_PARAGRAPH_ALIGNMENT {
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_FAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER
} DWRITE_PARAGRAPH_ALIGNMENT;
typedef enum DWRITE_WORD_WRAPPING { DWRITE_WORD_WRAPPING_WRAP = 0, DWRITE_WORD_WRAPPING_NO_WRAP = 1 } DWRITE_WORD_WRAPPING;
typedef enum DWRITE_TRIMMING_GRANULARITY {
    DWRITE_TRIMMING_GRANULARITY_NONE, DWRITE_TRIMMING_GRANULARITY_CHARACTER, DWRITE_TRIMMING_GRANULARITY_WORD
} DWRITE_TRIMMING_GRANULARITY;

typedef struct DWRITE_TRIMMING {
    DWRITE_TRIMMING_GRANULARITY granularity;
    UINT32 delimiter;
    UINT32 delimiterCount;
} DWRITE_TRIMMING;

typedef struct DWRITE_TEXT_METRICS {
    FLOAT left, top, width, widthIncludingTrailingWhitespace, height, layoutWidth, layoutHeight;
    UINT32 maxBidiReorderingDepth, lineCount;
} DWRITE_TEXT_METRICS;

typedef struct IDWriteFactory IDWriteFactory;
typedef struct IDWriteInlineObject IDWriteInlineObject;

typedef struct IDWriteInlineObjectVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteInlineObject *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDWriteInlineObject *);
    ULONG (STDMETHODCALLTYPE *Release)(IDWriteInlineObject *);
} IDWriteInlineObjectVtbl;
struct IDWriteInlineObject { const IDWriteInlineObjectVtbl *lpVtbl; };

/* IDWriteTextFormat methods (25), shared by IDWriteTextLayout which derives from it. */
#define SP_DWRITE_TEXTFORMAT_METHODS(T)                                                              \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);                               \
    ULONG (STDMETHODCALLTYPE *AddRef)(T *);                                                          \
    ULONG (STDMETHODCALLTYPE *Release)(T *);                                                         \
    HRESULT (STDMETHODCALLTYPE *SetTextAlignment)(T *, DWRITE_TEXT_ALIGNMENT);                       \
    HRESULT (STDMETHODCALLTYPE *SetParagraphAlignment)(T *, DWRITE_PARAGRAPH_ALIGNMENT);             \
    HRESULT (STDMETHODCALLTYPE *SetWordWrapping)(T *, DWRITE_WORD_WRAPPING);                         \
    void *SetReadingDirection, *SetFlowDirection, *SetIncrementalTabStop;                            \
    HRESULT (STDMETHODCALLTYPE *SetTrimming)(T *, const DWRITE_TRIMMING *, IDWriteInlineObject *);   \
    void *SetLineSpacing, *GetTextAlignment, *GetParagraphAlignment, *GetWordWrapping,               \
         *GetReadingDirection, *GetFlowDirection, *GetIncrementalTabStop, *GetTrimming,              \
         *GetLineSpacing, *GetFontCollection, *GetFontFamilyNameLength, *GetFontFamilyName,          \
         *GetFontWeight, *GetFontStyle, *GetFontStretch, *GetFontSize, *GetLocaleNameLength,         \
         *GetLocaleName;

typedef struct IDWriteTextFormatVtbl {
    SP_DWRITE_TEXTFORMAT_METHODS(IDWriteTextFormat)
} IDWriteTextFormatVtbl;
struct IDWriteTextFormat { const IDWriteTextFormatVtbl *lpVtbl; };

typedef struct IDWriteTextLayoutVtbl {
    SP_DWRITE_TEXTFORMAT_METHODS(IDWriteTextLayout)
    HRESULT (STDMETHODCALLTYPE *SetMaxWidth)(IDWriteTextLayout *, FLOAT);
    HRESULT (STDMETHODCALLTYPE *SetMaxHeight)(IDWriteTextLayout *, FLOAT);
    void *SetFontCollection, *SetFontFamilyName, *SetFontWeight, *SetFontStyle, *SetFontStretch, *SetFontSize,
         *SetUnderline, *SetStrikethrough, *SetDrawingEffect, *SetInlineObject, *SetTypography, *SetLocaleName,
         *GetMaxWidth, *GetMaxHeight, *GetFontCollection2, *GetFontFamilyNameLength2, *GetFontFamilyName2,
         *GetFontWeight2, *GetFontStyle2, *GetFontStretch2, *GetFontSize2, *GetUnderline, *GetStrikethrough,
         *GetDrawingEffect, *GetInlineObject, *GetTypography, *GetLocaleNameLength2, *GetLocaleName2, *Draw,
         *GetLineMetrics;
    HRESULT (STDMETHODCALLTYPE *GetMetrics)(IDWriteTextLayout *, DWRITE_TEXT_METRICS *);
} IDWriteTextLayoutVtbl;
struct IDWriteTextLayout { const IDWriteTextLayoutVtbl *lpVtbl; };

typedef struct IDWriteFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteFactory *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDWriteFactory *);
    ULONG (STDMETHODCALLTYPE *Release)(IDWriteFactory *);
    void *GetSystemFontCollection, *CreateCustomFontCollection, *RegisterFontCollectionLoader,
         *UnregisterFontCollectionLoader, *CreateFontFileReference, *CreateCustomFontFileReference, *CreateFontFace,
         *CreateRenderingParams, *CreateMonitorRenderingParams, *CreateCustomRenderingParams, *RegisterFontFileLoader,
         *UnregisterFontFileLoader;
    HRESULT (STDMETHODCALLTYPE *CreateTextFormat)(IDWriteFactory *, const WCHAR *, void *, DWRITE_FONT_WEIGHT,
                                                  DWRITE_FONT_STYLE, DWRITE_FONT_STRETCH, FLOAT, const WCHAR *,
                                                  IDWriteTextFormat **);
    void *CreateTypography, *GetGdiInterop;
    HRESULT (STDMETHODCALLTYPE *CreateTextLayout)(IDWriteFactory *, const WCHAR *, UINT32, IDWriteTextFormat *, FLOAT,
                                                  FLOAT, IDWriteTextLayout **);
    void *CreateGdiCompatibleTextLayout;
    HRESULT (STDMETHODCALLTYPE *CreateEllipsisTrimmingSign)(IDWriteFactory *, IDWriteTextFormat *, IDWriteInlineObject **);
    void *CreateTextAnalyzer, *CreateNumberSubstitution, *CreateGlyphRunAnalysis;
} IDWriteFactoryVtbl;
struct IDWriteFactory { const IDWriteFactoryVtbl *lpVtbl; };

/* {b859ee5a-d838-4b5b-a2e8-1adc7d93db48} */
static const IID SP_IID_IDWriteFactory = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

EXTERN_C HRESULT WINAPI DWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType, REFIID iid, IUnknown **factory);
