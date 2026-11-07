/* The part of Windows Imaging Component depth_infer.c uses, on FFmpeg's libraries, so that file stays shared. Every
 * "interface" is one Wic object: pixels in memory (BGRA unless converted) or a small stream or encoder record. */
#pragma once
#include "compat.h"

typedef struct GUID { int id; } GUID;
typedef struct Wic Wic;
typedef struct WicVtbl { void (*Release)(Wic *); } WicVtbl;
struct Wic {
    const WicVtbl *lpVtbl;
    int      refs, w, h, bpp, fmt, container, orient;
    uint8_t *px;
    wchar_t *path;              /* a decoder's file, a file stream's destination */
    uint8_t *mem;               /* a memory stream */
    size_t   cap, written;
    Wic     *stream;
    double   exif35, exifNum, exifDen;
};
typedef Wic IWICImagingFactory, IWICBitmapSource, IWICBitmap, IWICBitmapDecoder, IWICBitmapFrameDecode, IWICBitmapFrameEncode, IWICBitmapEncoder,
            IWICBitmapScaler, IWICFormatConverter, IWICBitmapFlipRotator, IWICStream, IWICMetadataQueryReader, IPropertyBag2, IStream;
typedef union ULARGE_INTEGER { uint64_t QuadPart; } ULARGE_INTEGER;
typedef struct PROPVARIANT { int vt; uint16_t uiVal; ULARGE_INTEGER uhVal; } PROPVARIANT;

enum { VT_EMPTY = 0, VT_UI2 = 18, VT_UI8 = 21 };
enum { WIC_GRAY = 1, WIC_RGB, WIC_BGRA, WIC_PNG, WIC_JPEG };
static const GUID GUID_WICPixelFormat8bppGray = { WIC_GRAY }, GUID_WICPixelFormat24bppRGB = { WIC_RGB }, GUID_WICPixelFormat32bppBGRA = { WIC_BGRA },
                  GUID_ContainerFormatPng = { WIC_PNG }, GUID_ContainerFormatJpeg = { WIC_JPEG };
#define WICDecodeMetadataCacheOnDemand 0
#define WICBitmapCacheOnLoad 0
#define WICBitmapEncoderNoCache 0
#define WICBitmapDitherTypeNone 0
#define WICBitmapPaletteTypeCustom 0
#define WICBitmapInterpolationModeHighQualityCubic 0
#define WICBitmapInterpolationModeFant 0
#define STREAM_SEEK_CUR 1
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
typedef int WICBitmapTransformOptions;

HRESULT wic_decoder(const wchar_t *path, Wic **out);
HRESULT wic_frame(Wic *dec, Wic **out);
HRESULT wic_new(Wic **out);
HRESULT wic_copy(Wic *src, Wic **out);
HRESULT wic_scale(Wic *sc, Wic *src, UINT w, UINT h);
HRESULT wic_convert(Wic *cv, Wic *src, const GUID *fmt);
HRESULT wic_rotate(Wic *rot, Wic *src, int orientation);
HRESULT wic_pixels(Wic *s, UINT stride, UINT size, BYTE *out);
HRESULT wic_stream_file(Wic *st, const wchar_t *path);
HRESULT wic_stream_mem(Wic *st, uint8_t *buf, size_t cap);
HRESULT wic_encoder(Wic *enc, const GUID *container);
HRESULT wic_write(Wic *fe, Wic *src);
HRESULT wic_write_px(Wic *fe, UINT lines, UINT stride, const BYTE *px);
HRESULT wic_commit(Wic *fe);
HRESULT wic_meta(Wic *q, const wchar_t *name, PROPVARIANT *v);
void    wic_addref(Wic *w);
void    wic_release(Wic *w);

#define IWICImagingFactory_CreateDecoderFromFilename(f, p, a, b, c, out) wic_decoder((p), (out))
#define IWICBitmapDecoder_GetFrame(d, i, out)               wic_frame((d), (out))
#define IWICBitmapFrameDecode_GetSize(fr, pw, ph)           (*(pw) = (UINT)(fr)->w, *(ph) = (UINT)(fr)->h, 0)
#define IWICBitmapFrameDecode_GetMetadataQueryReader(fr, q) (wic_addref(fr), *(q) = (fr), 0)
#define IWICMetadataQueryReader_GetMetadataByName(q, n, v)  wic_meta((q), (n), (v))
#define IWICBitmapSource_AddRef(s)                          wic_addref(s)
#define IWICBitmapSource_Release(s)                         wic_release(s)
#define IWICImagingFactory_CreateBitmapFromSource(f, s, o, out) wic_copy((s), (out))
#define IWICImagingFactory_CreateBitmapFlipRotator(f, out)  wic_new(out)
#define IWICBitmapFlipRotator_Initialize(r, s, o)           wic_rotate((r), (s), (o))
#define IWICImagingFactory_CreateBitmapScaler(f, out)       wic_new(out)
#define IWICBitmapScaler_Initialize(sc, s, w, h, mode)      wic_scale((sc), (s), (w), (h))
#define IWICImagingFactory_CreateFormatConverter(f, out)    wic_new(out)
#define IWICFormatConverter_Initialize(cv, s, g, a, b, c, d) wic_convert((cv), (s), (g))
#define IWICFormatConverter_CopyPixels(cv, r, st, n, out)   wic_pixels((cv), (st), (n), (out))
#define IWICImagingFactory_CreateStream(f, out)             wic_new(out)
#define IWICStream_InitializeFromFilename(st, p, a)         wic_stream_file((st), (p))
#define IWICStream_InitializeFromMemory(st, b, n)           wic_stream_mem((st), (b), (n))
#define IWICStream_Seek(st, z, o, pos)                      ((pos)->QuadPart = (st)->written, 0)
#define IWICImagingFactory_CreateEncoder(f, g, v, out)      (wic_new(out) ? 1 : wic_encoder(*(out), (g)))
#define IWICBitmapEncoder_Initialize(e, st, o)              (wic_addref(st), (e)->stream = (st), 0)
#define IWICBitmapEncoder_CreateNewFrame(e, fe, pb)         (wic_addref(e), *(fe) = (e), *(pb) = NULL, 0)
#define IWICBitmapFrameEncode_Initialize(fe, pb)            0
#define IWICBitmapFrameEncode_SetSize(fe, W, H)             ((fe)->w = (int)(W), (fe)->h = (int)(H), 0)
#define IWICBitmapFrameEncode_SetPixelFormat(fe, g)         ((fe)->fmt = (g)->id, 0)
#define IWICBitmapFrameEncode_WriteSource(fe, s, r)         wic_write((fe), (s))
#define IWICBitmapFrameEncode_WritePixels(fe, l, st, n, p)  wic_write_px((fe), (l), (st), (p))
#define IWICBitmapFrameEncode_Commit(fe)                    wic_commit(fe)
#define IWICBitmapEncoder_Commit(e)                         0
#define PropVariantInit(v)                                  memset((v), 0, sizeof *(v))
#define PropVariantClear(v)                                 memset((v), 0, sizeof *(v))
#define CoInitializeEx(a, b)                                0
#define CoUninitialize()                                    ((void)0)
#define COINIT_MULTITHREADED                                0
#define CoCreateInstance(a, b, c, d, out)                   wic_new((Wic **)(out))
#define CLSCTX_INPROC_SERVER                                0
