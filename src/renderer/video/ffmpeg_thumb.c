/* FFmpeg as a shared media component for the library: poster frames when the Windows thumbnail
 * provider cannot handle a container/codec (e.g. WebM/VP9 without system codecs), and short
 * animations (a preview GIF's whole loop, or a few seconds of a video). Videos decode on the GPU
 * (D3D11VA, a device of our own; only the frames kept are copied back) with software fallback,
 * and are scaled with libswscale to an opaque BGRA cover crop. */
#include "../renderer.h"
#include "../../ui/thumbs.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

#define ANIM_FPS     12       /* videos: frames kept per second */
#define ANIM_SECONDS 4.0      /* videos longer than 6 s: length of the excerpt (starting near 10%) */
#define ANIM_BUDGET  4000     /* ms of decoding per animation at most */

typedef struct Src {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    AVBufferRef     *hw;      /* D3D11VA device, when the codec has a hardware decoder */
    int              vi;
} Src;

typedef struct Scaler {
    struct SwsContext *sws;
    uint8_t           *tmp;
    int                sw, sh, srcW, srcH, srcFmt;
} Scaler;

static void src_close(Src *s)
{
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    av_buffer_unref(&s->hw);
}

static int has_d3d11(const AVCodec *codec)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return 0;
        if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return 1;
    }
}

static enum AVPixelFormat pick_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

static BOOL open_decoder(Src *s, const AVCodec *codec, int hw)
{
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec || avcodec_parameters_to_context(s->dec, s->fmt->streams[s->vi]->codecpar) < 0) {
        avcodec_free_context(&s->dec);
        return FALSE;
    }
    s->dec->pkt_timebase = s->fmt->streams[s->vi]->time_base;
    if (hw) {
        s->dec->hw_device_ctx = av_buffer_ref(s->hw);
        s->dec->get_format = pick_format;
        s->dec->thread_count = 1;
    } else {
        s->dec->thread_count = 2;
    }
    if (avcodec_open2(s->dec, codec, NULL) >= 0) return TRUE;
    avcodec_free_context(&s->dec);
    return FALSE;
}

/* Opens the best video stream. decoder: 0 demuxer only (counting packets), 1 software, 2 GPU if possible. */
static BOOL src_open(Src *s, const char *u, int decoder)
{
    memset(s, 0, sizeof *s);
    if (avformat_open_input(&s->fmt, u, NULL, NULL) < 0) return FALSE;
    s->fmt->max_analyze_duration = AV_TIME_BASE;
    avformat_find_stream_info(s->fmt, NULL);
    const AVCodec *codec = NULL;
    s->vi = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (s->vi < 0 || !codec) {
        src_close(s);
        return FALSE;
    }
    for (unsigned i = 0; i < s->fmt->nb_streams; i++) s->fmt->streams[i]->discard = (int)i == s->vi ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    if (!decoder) return TRUE;
    int hw = decoder == 2 && has_d3d11(codec) && av_hwdevice_ctx_create(&s->hw, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) >= 0;
    if ((hw && open_decoder(s, codec, 1)) || open_decoder(s, codec, 0)) return TRUE;
    src_close(s);
    return FALSE;
}

/* Scales fr to cover w x h and writes the centred crop, fully opaque, to out. */
static BOOL cover(Scaler *c, const AVFrame *fr, int w, int h, uint8_t *out)
{
    if (fr->width <= 0 || fr->height <= 0) return FALSE;
    if (!c->tmp || c->srcW != fr->width || c->srcH != fr->height || c->srcFmt != fr->format) {
        double scale = SP_MAX((double)w / fr->width, (double)h / fr->height);
        c->sw = SP_MAX(w, (int)(fr->width * scale + 0.5));
        c->sh = SP_MAX(h, (int)(fr->height * scale + 0.5));
        c->sws = sws_getCachedContext(c->sws, fr->width, fr->height, (enum AVPixelFormat)fr->format, c->sw, c->sh,
                                      AV_PIX_FMT_BGRA, SWS_AREA, NULL, NULL, NULL);
        free(c->tmp);
        c->tmp = c->sws ? (uint8_t *)malloc((size_t)c->sw * c->sh * 4) : NULL;
        c->srcW = fr->width;
        c->srcH = fr->height;
        c->srcFmt = fr->format;
        if (!c->tmp) return FALSE;
    }
    uint8_t *dst[4] = { c->tmp, NULL, NULL, NULL };
    int ls[4] = { c->sw * 4, 0, 0, 0 };
    sws_scale(c->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, ls);
    int ox = (c->sw - w) / 2, oy = (c->sh - h) / 2;
    for (int y = 0; y < h; y++) {
        uint8_t *row = out + (size_t)y * w * 4;
        memcpy(row, c->tmp + ((size_t)(y + oy) * c->sw + ox) * 4, (size_t)w * 4);
        for (int x = 0; x < w; x++) row[x * 4 + 3] = 255;
    }
    return TRUE;
}

/* cover() for decoded frames: GPU frames are copied to system memory first. */
static BOOL cover_frame(Scaler *c, const AVFrame *fr, AVFrame *tmp, int w, int h, uint8_t *out)
{
    if (fr->format != AV_PIX_FMT_D3D11) return cover(c, fr, w, h, out);
    BOOL ok = av_hwframe_transfer_data(tmp, fr, 0) >= 0 && cover(c, tmp, w, h, out);
    av_frame_unref(tmp);
    return ok;
}

static void scaler_free(Scaler *c)
{
    sws_freeContext(c->sws);
    free(c->tmp);
}

static BOOL poster(const char *u, int w, int h, int decoder, int seek, uint8_t **out)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc(), *tmp = av_frame_alloc();
    Scaler sc = { 0 };
    Src s;
    BOOL ok = FALSE;
    if (pkt && fr && tmp && src_open(&s, u, decoder)) {
        /* GIF frames build on each other: those start from the beginning */
        if (seek && s.fmt->duration > 0 && s.dec->codec_id != AV_CODEC_ID_GIF)
            av_seek_frame(s.fmt, -1, s.fmt->duration / 10, AVSEEK_FLAG_BACKWARD);
        int got = 0;
        for (int n = 0; n < 600 && !got; n++) {
            if (av_read_frame(s.fmt, pkt) < 0) {
                avcodec_send_packet(s.dec, NULL);       /* drain on EOF */
                got = avcodec_receive_frame(s.dec, fr) >= 0;
                break;
            }
            if (pkt->stream_index == s.vi && avcodec_send_packet(s.dec, pkt) >= 0) got = avcodec_receive_frame(s.dec, fr) >= 0;
            av_packet_unref(pkt);
        }
        uint8_t *buf = got ? (uint8_t *)malloc((size_t)w * h * 4) : NULL;
        if (buf && cover_frame(&sc, fr, tmp, w, h, buf)) {
            *out = buf;
            ok = TRUE;
        } else {
            free(buf);
        }
        src_close(&s);
    }
    scaler_free(&sc);
    av_frame_free(&tmp);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return ok;
}

BOOL ffmpeg_thumbnail(const wchar_t *path, int w, int h, uint8_t **out)
{
    *out = NULL;
    if (w <= 0 || h <= 0 || !video_ffmpeg_available(NULL, 0)) return FALSE;
    char *u = utf8_from_wide(path, NULL);
    BOOL ok = u && (poster(u, w, h, 2, 1, out) || poster(u, w, h, 1, 1, out));
    free(u);
    return ok;
}

/* not a tenth in like the library's poster: the still must match the first frame playback shows */
BOOL ffmpeg_first_frame(const wchar_t *path, int w, int h, uint8_t **out)
{
    *out = NULL;
    if (w <= 0 || h <= 0 || !video_ffmpeg_available(NULL, 0)) return FALSE;
    char *u = utf8_from_wide(path, NULL);
    BOOL ok = u && (poster(u, w, h, 2, 0, out) || poster(u, w, h, 1, 0, out));
    free(u);
    return ok;
}

static int count_frames(const char *u)
{
    AVPacket *pkt = av_packet_alloc();
    Src s;
    int n = 0;
    if (pkt && src_open(&s, u, 0)) {
        while (n < 10000 && av_read_frame(s.fmt, pkt) >= 0) {
            if (pkt->stream_index == s.vi) n++;
            av_packet_unref(pkt);
        }
        src_close(&s);
    }
    av_packet_free(&pkt);
    return n;
}

typedef struct Anim {
    uint8_t *px;            /* maxFrames * w * h * 4 */
    double  *at;            /* start time of each kept frame, seconds */
    int      n, gif, gpu;
    double   step, tail;    /* video frame spacing; end of the last decoded GIF frame */
} Anim;

/* Decodes the kept frames into a; returns the number kept. */
static int animate(const char *u, int w, int h, int maxFrames, int decoder, Anim *a)
{
    size_t fsz = (size_t)w * h * 4;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc(), *tmp = av_frame_alloc();
    Scaler sc = { 0 };
    Src s;
    a->n = 0;
    a->tail = 0.0;
    a->step = 1.0 / ANIM_FPS;
    if (!pkt || !fr || !tmp || !src_open(&s, u, decoder)) goto done;

    AVStream *st = s.fmt->streams[s.vi];
    double tb = av_q2d(st->time_base), base = st->start_time != AV_NOPTS_VALUE ? st->start_time * tb : 0.0;
    int stride = 1;
    double to = 1e9, next = 0.0;
    a->gif = s.dec->codec_id == AV_CODEC_ID_GIF;
    if (a->gif) {
        int total = count_frames(u);      /* keep the whole loop: every stride-th frame */
        if (total > maxFrames) stride = (total + maxFrames - 1) / maxFrames;
    } else {
        s.dec->skip_loop_filter = AVDISCARD_ALL;   /* software decode: invisible at thumbnail size, faster */
        double dur = s.fmt->duration > 0 ? (double)s.fmt->duration / AV_TIME_BASE : 0.0;
        if (dur > 6.0) {
            next = dur * 0.1;
            to = next + ANIM_SECONDS;
            av_seek_frame(s.fmt, -1, (int64_t)(next * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        }
    }

    ULONGLONG started = GetTickCount64();
    int idx = 0, eof = 0, stop = 0;
    while (!stop && a->n < maxFrames) {
        if (!eof) {
            if (av_read_frame(s.fmt, pkt) < 0) {
                eof = 1;
                avcodec_send_packet(s.dec, NULL);
            } else {
                if (pkt->stream_index == s.vi) avcodec_send_packet(s.dec, pkt);
                av_packet_unref(pkt);
            }
        }
        while (!stop && a->n < maxFrames && avcodec_receive_frame(s.dec, fr) >= 0) {
            double ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * tb - base : idx * 0.1;
            double dur = fr->duration > 0 ? fr->duration * tb : (a->gif ? 0.1 : a->step);
            int keep = 0;
            if (a->gif) keep = idx % stride == 0;
            else if (ts >= to) stop = 1;
            else keep = ts + 0.001 >= next;     /* also skips the pre-roll after a backward seek */
            idx++;
            if (fr->format == AV_PIX_FMT_D3D11) a->gpu = 1;
            if (keep && cover_frame(&sc, fr, tmp, w, h, a->px + fsz * (size_t)a->n)) {
                a->at[a->n++] = ts;
                next = ts + a->step;
            }
            if (ts + dur > a->tail) a->tail = ts + dur;
            av_frame_unref(fr);
            if (GetTickCount64() - started > ANIM_BUDGET) stop = 1;
        }
        if (eof) break;                 /* drained (or the decoder failed) */
    }
    src_close(&s);
done:
    scaler_free(&sc);
    av_frame_free(&tmp);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return a->n;
}

BOOL ffmpeg_animation(const wchar_t *path, int w, int h, int maxFrames, uint8_t **bgra, uint16_t **delays, int *count)
{
    *bgra = NULL;
    *delays = NULL;
    *count = 0;
    if (w <= 0 || h <= 0 || maxFrames < 2 || !video_ffmpeg_available(NULL, 0)) return FALSE;
    char *u = utf8_from_wide(path, NULL);
    Anim a = { 0 };
    a.px = (uint8_t *)malloc((size_t)w * h * 4 * (size_t)maxFrames);
    a.at = (double *)malloc(sizeof(double) * (size_t)maxFrames);
    uint16_t *ms = NULL;
    double t0 = time_ms();
    if (u && a.px && a.at && (animate(u, w, h, maxFrames, 2, &a) >= 2 || animate(u, w, h, maxFrames, 1, &a) >= 2) &&
        (ms = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)a.n)) != NULL) {
        for (int i = 0; i < a.n; i++) {
            double d = i + 1 < a.n ? a.at[i + 1] - a.at[i] : (a.gif ? a.tail - a.at[i] : a.step);
            int v = (int)(d * 1000.0 + 0.5);
            ms[i] = (uint16_t)SP_CLAMP(v, 20, 5000);
        }
        *bgra = a.px;
        a.px = NULL;
        *delays = ms;
        *count = a.n;
        LOGI(L"thumbnails: %d-frame preview of %ls (%ls, %.0f ms)", a.n, path_filename(path),
             a.gpu ? L"GPU decode" : L"software decode", time_ms() - t0);
    }
    free(a.px);
    free(a.at);
    free(u);
    return *count >= 2;
}
