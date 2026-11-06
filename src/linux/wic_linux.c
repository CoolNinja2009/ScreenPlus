/* See wic_linux.h. Pictures come in and go out through libavformat, libavcodec and libswscale. */
#include "wic_linux.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

static void vt_release(Wic *w) { wic_release(w); }
static const WicVtbl k_vt = { vt_release };

void wic_addref(Wic *w) { w->refs++; }

void wic_release(Wic *w)
{
    if (--w->refs > 0) return;
    if (w->stream) wic_release(w->stream);
    free(w->px);
    free(w->path);
    free(w);
}

HRESULT wic_new(Wic **out)
{
    Wic *w = (Wic *)calloc(1, sizeof *w);
    if (!w) return E_OUTOFMEMORY;
    w->lpVtbl = &k_vt;
    w->refs = 1;
    w->orient = 1;
    *out = w;
    return 0;
}

/* ---------------------------------------------------------------- EXIF */

static unsigned rd16(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }
static unsigned rd32(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24 : (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void read_ifd(Wic *w, const uint8_t *t, size_t n, size_t off, int le, int depth)
{
    if (off + 2 > n || depth > 2) return;
    unsigned count = rd16(t + off, le);
    for (unsigned i = 0; i < count && off + 2 + (i + 1) * 12 <= n; i++) {
        const uint8_t *e = t + off + 2 + i * 12;
        unsigned tag = rd16(e, le);
        if (tag == 0x0112) w->orient = (int)rd16(e + 8, le);
        else if (tag == 41989) w->exif35 = rd16(e + 8, le);
        else if (tag == 37386) {
            size_t at = rd32(e + 8, le);
            if (at + 8 <= n) { w->exifNum = rd32(t + at, le); w->exifDen = rd32(t + at + 4, le); }
        } else if (tag == 0x8769) read_ifd(w, t, n, rd32(e + 8, le), le, depth + 1);
    }
}

static void read_exif(Wic *w, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t b[4];
    if (!f) return;
    if (fread(b, 1, 2, f) == 2 && b[0] == 0xFF && b[1] == 0xD8) {
        while (fread(b, 1, 4, f) == 4 && b[0] == 0xFF && b[1] != 0xDA) {
            size_t len = (size_t)(b[2] << 8 | b[3]);
            if (len < 2) break;
            len -= 2;
            if (b[1] != 0xE1 || len < 14) { fseek(f, (long)len, SEEK_CUR); continue; }
            uint8_t *d = (uint8_t *)malloc(len);
            if (d && fread(d, 1, len, f) == len && !memcmp(d, "Exif\0\0", 6)) {
                const uint8_t *t = d + 6;
                int le = t[0] == 'I';
                read_ifd(w, t, len - 6, rd32(t + 4, le), le, 0);
                free(d);
                break;
            }
            free(d);
        }
    }
    fclose(f);
}

UINT image_exif_orientation(Wic *frame) { return frame->orient >= 1 && frame->orient <= 8 ? (UINT)frame->orient : 1; }
int  image_orientation_transform(UINT o) { return (int)o; }

HRESULT wic_meta(Wic *q, const wchar_t *name, PROPVARIANT *v)
{
    if (wcsstr(name, L"41989") && q->exif35 > 0) {
        v->vt = VT_UI2;
        v->uiVal = (uint16_t)q->exif35;
        return 0;
    }
    if (wcsstr(name, L"37386") && q->exifDen > 0) {
        v->vt = VT_UI8;
        v->uhVal.QuadPart = (uint64_t)q->exifNum | (uint64_t)q->exifDen << 32;
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- reading */

HRESULT wic_decoder(const wchar_t *path, Wic **out)
{
    char *p = sp_path_utf8(path);
    AVFormatContext *fc = NULL;
    AVCodecContext *cc = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    struct SwsContext *sws = NULL;
    Wic *w = NULL;
    HRESULT hr = -1;
    if (!p || !pkt || !fr || avformat_open_input(&fc, p, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) goto done;
    int si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) goto done;
    const AVCodec *c = avcodec_find_decoder(fc->streams[si]->codecpar->codec_id);
    if (!c || !(cc = avcodec_alloc_context3(c)) || avcodec_parameters_to_context(cc, fc->streams[si]->codecpar) < 0 || avcodec_open2(cc, c, NULL) < 0) goto done;
    int got = 0;
    while (!got && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0) got = avcodec_receive_frame(cc, fr) >= 0;
        av_packet_unref(pkt);
    }
    if (!got) {
        avcodec_send_packet(cc, NULL);
        got = avcodec_receive_frame(cc, fr) >= 0;
    }
    if (!got || wic_new(&w)) goto done;
    w->w = fr->width;
    w->h = fr->height;
    w->fmt = WIC_BGRA;
    w->bpp = 4;
    w->px = (uint8_t *)malloc((size_t)w->w * w->h * 4);
    sws = sws_getContext(fr->width, fr->height, (enum AVPixelFormat)fr->format, fr->width, fr->height, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
    if (!w->px || !sws) goto done;
    uint8_t *dst[4] = { w->px };
    int line[4] = { w->w * 4 };
    sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, line);
    read_exif(w, p);
    *out = w;
    w = NULL;
    hr = 0;
done:
    if (w) wic_release(w);
    sws_freeContext(sws);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
    free(p);
    return hr;
}

HRESULT wic_frame(Wic *dec, Wic **out) { wic_addref(dec); *out = dec; return 0; }

static HRESULT take(Wic *to, const Wic *from)
{
    size_t n = (size_t)from->w * from->h * from->bpp;
    uint8_t *px = (uint8_t *)malloc(n ? n : 1);
    if (!px) return E_OUTOFMEMORY;
    memcpy(px, from->px, n);
    free(to->px);
    to->px = px;
    to->w = from->w; to->h = from->h; to->bpp = from->bpp; to->fmt = from->fmt;
    return 0;
}

HRESULT wic_copy(Wic *src, Wic **out)
{
    HRESULT hr = wic_new(out);
    return hr ? hr : take(*out, src);
}

HRESULT wic_scale(Wic *sc, Wic *src, UINT w, UINT h)
{
    struct SwsContext *s = sws_getContext(src->w, src->h, AV_PIX_FMT_BGRA, (int)w, (int)h, AV_PIX_FMT_BGRA, SWS_BICUBIC | SWS_ACCURATE_RND, NULL, NULL, NULL);
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!s || !px) { sws_freeContext(s); free(px); return E_OUTOFMEMORY; }
    const uint8_t *in[4] = { src->px };
    int inLine[4] = { src->w * 4 }, outLine[4] = { (int)w * 4 };
    uint8_t *dst[4] = { px };
    sws_scale(s, in, inLine, 0, src->h, dst, outLine);
    sws_freeContext(s);
    free(sc->px);
    sc->px = px;
    sc->w = (int)w; sc->h = (int)h; sc->bpp = 4; sc->fmt = WIC_BGRA;
    return 0;
}

HRESULT wic_convert(Wic *cv, Wic *src, const GUID *fmt)
{
    int bpp = fmt->id == WIC_GRAY ? 1 : fmt->id == WIC_RGB ? 3 : 4;
    size_t n = (size_t)src->w * src->h;
    uint8_t *px = (uint8_t *)malloc(n * bpp);
    if (!px) return E_OUTOFMEMORY;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = src->px + i * 4;
        if (bpp == 4) memcpy(px + i * 4, s, 4);
        else if (bpp == 3) { px[i * 3] = s[2]; px[i * 3 + 1] = s[1]; px[i * 3 + 2] = s[0]; }
        else px[i] = (uint8_t)((s[2] * 77 + s[1] * 150 + s[0] * 29) >> 8);
    }
    free(cv->px);
    cv->px = px;
    cv->w = src->w; cv->h = src->h; cv->bpp = bpp; cv->fmt = fmt->id;
    return 0;
}

/* EXIF orientations 2-8. */
HRESULT wic_rotate(Wic *rot, Wic *src, int o)
{
    int W = o >= 5 ? src->h : src->w, H = o >= 5 ? src->w : src->h, sw = src->w, sh = src->h;
    uint32_t *px = (uint32_t *)malloc((size_t)W * H * 4);
    const uint32_t *in = (const uint32_t *)src->px;
    if (!px) return E_OUTOFMEMORY;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int sx = x, sy = y;
            switch (o) {
            case 2: sx = sw - 1 - x; break;
            case 3: sx = sw - 1 - x; sy = sh - 1 - y; break;
            case 4: sy = sh - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = sh - 1 - x; break;
            case 7: sx = sw - 1 - y; sy = sh - 1 - x; break;
            case 8: sx = sw - 1 - y; sy = x; break;
            }
            px[(size_t)y * W + x] = in[(size_t)sy * sw + sx];
        }
    free(rot->px);
    rot->px = (uint8_t *)px;
    rot->w = W; rot->h = H; rot->bpp = 4; rot->fmt = WIC_BGRA;
    return 0;
}

HRESULT wic_pixels(Wic *s, UINT stride, UINT size, BYTE *out)
{
    size_t line = (size_t)s->w * s->bpp;
    if (stride < line || size < stride * (size_t)(s->h - 1) + line) return -1;
    for (int y = 0; y < s->h; y++) memcpy(out + (size_t)y * stride, s->px + y * line, line);
    return 0;
}

/* ---------------------------------------------------------------- writing */

HRESULT wic_stream_file(Wic *st, const wchar_t *path) { st->path = wcsdup_sp(path); return st->path ? 0 : E_OUTOFMEMORY; }
HRESULT wic_stream_mem(Wic *st, uint8_t *buf, size_t cap) { st->mem = buf; st->cap = cap; return 0; }
HRESULT wic_encoder(Wic *enc, const GUID *container) { enc->container = container->id; return 0; }

HRESULT wic_write(Wic *fe, Wic *src)
{
    fe->w = src->w; fe->h = src->h;
    return take(fe, src);
}

HRESULT wic_write_px(Wic *fe, UINT lines, UINT stride, const BYTE *px)
{
    int bpp = fe->fmt == WIC_GRAY ? 1 : fe->fmt == WIC_RGB ? 3 : 4;
    size_t line = (size_t)fe->w * bpp;
    uint8_t *p = (uint8_t *)malloc(line * lines);
    if (!p) return E_OUTOFMEMORY;
    for (UINT y = 0; y < lines; y++) memcpy(p + y * line, px + (size_t)y * stride, line);
    free(fe->px);
    fe->px = p;
    fe->bpp = bpp;
    return 0;
}

HRESULT wic_commit(Wic *fe)
{
    int jpeg = fe->container == WIC_JPEG;
    enum AVPixelFormat in = fe->bpp == 1 ? AV_PIX_FMT_GRAY8 : fe->bpp == 3 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_BGRA;
    enum AVPixelFormat outFmt = jpeg ? AV_PIX_FMT_YUVJ444P : fe->bpp == 1 ? AV_PIX_FMT_GRAY8 : fe->bpp == 3 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_RGBA;
    const AVCodec *c = avcodec_find_encoder(jpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    AVCodecContext *cc = c ? avcodec_alloc_context3(c) : NULL;
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = NULL;
    HRESULT hr = -1;
    if (!cc || !fr || !pkt || !fe->stream) goto done;
    cc->width = fe->w;
    cc->height = fe->h;
    cc->pix_fmt = outFmt;
    cc->time_base = (AVRational){ 1, 25 };
    cc->color_range = jpeg ? AVCOL_RANGE_JPEG : AVCOL_RANGE_UNSPECIFIED;
    if (jpeg) { cc->flags |= AV_CODEC_FLAG_QSCALE; cc->qmin = cc->qmax = 3; }
    if (avcodec_open2(cc, c, NULL) < 0) goto done;
    fr->format = outFmt;
    fr->width = fe->w;
    fr->height = fe->h;
    if (av_frame_get_buffer(fr, 0) < 0) goto done;
    fr->quality = FF_QP2LAMBDA * 3;
    sws = sws_getContext(fe->w, fe->h, in, fe->w, fe->h, outFmt, SWS_POINT, NULL, NULL, NULL);
    if (!sws) goto done;
    const uint8_t *src[4] = { fe->px };
    int line[4] = { fe->w * fe->bpp };
    sws_scale(sws, src, line, 0, fe->h, fr->data, fr->linesize);
    if (avcodec_send_frame(cc, fr) < 0 || avcodec_send_frame(cc, NULL) < 0 || avcodec_receive_packet(cc, pkt) < 0) goto done;
    Wic *st = fe->stream;
    if (st->mem) {
        if ((size_t)pkt->size > st->cap) goto done;
        memcpy(st->mem, pkt->data, (size_t)pkt->size);
        st->written = (size_t)pkt->size;
        hr = 0;
    } else {
        char *p = sp_path_utf8(st->path);
        FILE *f = p ? fopen(p, "wb") : NULL;
        free(p);
        if (f) {
            hr = fwrite(pkt->data, 1, (size_t)pkt->size, f) == (size_t)pkt->size ? 0 : -1;
            if (fclose(f)) hr = -1;
        }
    }
done:
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&cc);
    return hr;
}
