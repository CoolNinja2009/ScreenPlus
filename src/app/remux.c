/* Joins a video-only file and an audio-only file into one .mkv without re-encoding (url_import.c: the
 * high-quality formats sites like YouTube offer only as separate tracks). Screen+ ships the FFmpeg
 * libraries, not ffmpeg.exe, so yt-dlp can't merge them itself. Matroska takes any codec pair (VP9,
 * H.264, Opus, AAC), and the video renderer plays it like any other file. */
#include "app.h"
#include "../common/util.h"
#include "../common/log.h"
#include <libavformat/avformat.h>
#include <stdlib.h>

typedef struct In {
    AVFormatContext *f;
    int       idx;          /* the stream taken from this file */
    int       out;          /* its index in the output */
    AVPacket *pkt;          /* the next packet, read ahead */
    int       have, eof;
} In;

static int open_in(In *in, const wchar_t *path, enum AVMediaType type)
{
    char *p = utf8_from_wide(path, NULL);
    in->idx = -1;
    if (p && avformat_open_input(&in->f, p, NULL, NULL) >= 0 && avformat_find_stream_info(in->f, NULL) >= 0)
        in->idx = av_find_best_stream(in->f, type, -1, -1, NULL, 0);
    free(p);
    return in->idx >= 0;
}

static void read_ahead(In *in)
{
    while (!in->have && !in->eof) {
        if (av_read_frame(in->f, in->pkt) < 0) in->eof = 1;
        else if (in->pkt->stream_index == in->idx) in->have = 1;
        else av_packet_unref(in->pkt);
    }
}

static int64_t when(const In *in)
{
    return in->pkt->dts != AV_NOPTS_VALUE ? in->pkt->dts : in->pkt->pts;
}

BOOL remux_join(const wchar_t *video, const wchar_t *audio, const wchar_t *out)
{
    In in[2] = { 0 };
    AVFormatContext *o = NULL;
    char *po = utf8_from_wide(out, NULL);
    int ok = 0, err = 0;
    if (!po || !open_in(&in[0], video, AVMEDIA_TYPE_VIDEO) || !open_in(&in[1], audio, AVMEDIA_TYPE_AUDIO) ||
        avformat_alloc_output_context2(&o, NULL, "matroska", po) < 0 || !o)
        goto done;
    for (int i = 0; i < 2; i++) {
        AVStream *src = in[i].f->streams[in[i].idx], *st = avformat_new_stream(o, NULL);
        if (!st || avcodec_parameters_copy(st->codecpar, src->codecpar) < 0 || !(in[i].pkt = av_packet_alloc())) goto done;
        st->codecpar->codec_tag = 0;
        st->time_base = src->time_base;
        in[i].out = st->index;
    }
    if (avio_open(&o->pb, po, AVIO_FLAG_WRITE) < 0) goto done;
    if (avformat_write_header(o, NULL) < 0) {
        avio_closep(&o->pb);
        goto done;
    }
    for (;;) {                                     /* both files in time order */
        read_ahead(&in[0]);
        read_ahead(&in[1]);
        int k = -1;
        if (in[0].have && in[1].have) {
            int64_t a = when(&in[0]), b = when(&in[1]);
            if (a == AV_NOPTS_VALUE) k = 0;
            else if (b == AV_NOPTS_VALUE) k = 1;
            else k = av_compare_ts(a, in[0].f->streams[in[0].idx]->time_base, b, in[1].f->streams[in[1].idx]->time_base) <= 0 ? 0 : 1;
        } else if (in[0].have) k = 0;
        else if (in[1].have) k = 1;
        if (k < 0) break;
        AVPacket *p = in[k].pkt;
        av_packet_rescale_ts(p, in[k].f->streams[in[k].idx]->time_base, o->streams[in[k].out]->time_base);
        p->stream_index = in[k].out;
        p->pos = -1;
        in[k].have = 0;
        if ((err = av_interleaved_write_frame(o, p)) < 0) break;    /* takes the packet */
    }
    ok = av_write_trailer(o) >= 0 && err >= 0 && in[0].eof && in[1].eof;
    avio_closep(&o->pb);
done:
    if (!ok) {
        LOGW(L"remux: could not join '%ls' and '%ls'", video, audio);
        if (po) DeleteFileW(out);
    }
    for (int i = 0; i < 2; i++) {
        av_packet_free(&in[i].pkt);
        avformat_close_input(&in[i].f);
    }
    avformat_free_context(o);
    free(po);
    return ok;
}

