/* Videos the interactive depth page can play on this PC. WebView2 decodes HEVC only on the GPU: a PC whose GPU
 * has no HEVC decoder for the video's size (older or low-end ones) leaves the page's <video>, and so the whole
 * wallpaper, black. Such a video is re-encoded once for the page: FFmpeg decodes it (software, like the depth
 * child), Windows' own Media Foundation encoders write H.264 + AAC (this FFmpeg build has no encoders), at most
 * 1920x1080 so a weak PC also plays it smoothly. The timing stays the source's, so the depth frames still line up.
 * Runs on the depth job's worker thread. */
#include "app.h"
#include "../renderer/renderer.h"
#include "../common/util.h"
#include "../common/log.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUT_W    1920
#define OUT_H    1080
#define AUDIO_HZ 48000

/* The page's decoder is the GPU's, through D3D11, as Chromium's is: a profile and a configuration for this size. */
static BOOL hevc_on_gpu(int w, int h, int ten)
{
    ID3D11Device *dev = NULL;
    ID3D11VideoDevice *vd = NULL;
    BOOL ok = FALSE;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                                 D3D11_SDK_VERSION, &dev, NULL, NULL)))
        return FALSE;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&vd))) {
        D3D11_VIDEO_DECODER_DESC d;
        BOOL fmt = FALSE;
        UINT n = 0;
        d.Guid = ten ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        d.SampleWidth = (UINT)w;
        d.SampleHeight = (UINT)h;
        d.OutputFormat = ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        ok = SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(vd, &d.Guid, d.OutputFormat, &fmt)) && fmt &&
             SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfigCount(vd, &d, &n)) && n > 0;
        ID3D11VideoDevice_Release(vd);
    }
    ID3D11Device_Release(dev);
    return ok;
}

BOOL webvideo_needed(const wchar_t *path)
{
    AVFormatContext *fmt = NULL;
    BOOL need = FALSE;
    char *u = utf8_from_wide(path, NULL);
    if (u && video_ffmpeg_available(NULL, 0) && avformat_open_input(&fmt, u, NULL, NULL) >= 0 &&
        avformat_find_stream_info(fmt, NULL) >= 0) {
        int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        const AVCodecParameters *p = vi >= 0 ? fmt->streams[vi]->codecpar : NULL;
        if (p && p->codec_id == AV_CODEC_ID_HEVC) {
            int ten = p->profile == AV_PROFILE_HEVC_MAIN_10 || p->format == AV_PIX_FMT_YUV420P10LE;
            need = !hevc_on_gpu(p->width, p->height, ten);
            LOGI(L"depth: HEVC %dx%d%ls %ls on this GPU", p->width, p->height, ten ? L" 10-bit" : L"",
                 need ? L"has no decoder: re-encoding it for the page" : L"plays");
        }
    }
    avformat_close_input(&fmt);
    free(u);
    return need;
}

static IMFMediaType *media_type(const GUID *major, const GUID *sub)
{
    IMFMediaType *t = NULL;
    if (FAILED(MFCreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, major);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    return t;
}

static void video_attrs(IMFMediaType *t, int w, int h, AVRational fps)
{
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, ((UINT64)fps.num << 32) | (UINT32)fps.den);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
}

static void audio_attrs(IMFMediaType *t)
{
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_HZ);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
}

/* A writer with H.264 (and AAC when `audio`); FALSE leaves nothing open. */
static BOOL open_writer(const wchar_t *dst, int w, int h, AVRational fps, BOOL audio, IMFSinkWriter **sw, DWORD *vs, DWORD *as)
{
    IMFAttributes *a = NULL;
    IMFMediaType *vo = media_type(&MFMediaType_Video, &MFVideoFormat_H264), *vin = media_type(&MFMediaType_Video, &MFVideoFormat_NV12);
    IMFMediaType *ao = audio ? media_type(&MFMediaType_Audio, &MFAudioFormat_AAC) : NULL;
    IMFMediaType *ain = audio ? media_type(&MFMediaType_Audio, &MFAudioFormat_PCM) : NULL;
    HRESULT hr = vo && vin && (!audio || (ao && ain)) ? MFCreateAttributes(&a, 1) : E_OUTOFMEMORY;
    *sw = NULL;
    if (SUCCEEDED(hr)) {
        IMFAttributes_SetUINT32(a, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        video_attrs(vo, w, h, fps);
        IMFMediaType_SetUINT32(vo, &MF_MT_AVG_BITRATE, (UINT32)((double)w * h * av_q2d(fps) * 0.12));
        video_attrs(vin, w, h, fps);
        hr = MFCreateSinkWriterFromURL(dst, NULL, a, sw);
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(*sw, vo, vs);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(*sw, *vs, vin, NULL);
    if (SUCCEEDED(hr) && audio) {
        audio_attrs(ao);
        IMFMediaType_SetUINT32(ao, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   /* 192 kbit/s */
        audio_attrs(ain);
        IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_HZ * 4);
        hr = IMFSinkWriter_AddStream(*sw, ao, as);
        if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(*sw, *as, ain, NULL);
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(*sw);
    if (FAILED(hr)) {
        LOGW(L"depth: Media Foundation writer%ls failed (0x%08lx)", audio ? L" with sound" : L"", hr);
        if (*sw) IMFSinkWriter_Release(*sw);
        *sw = NULL;
        DeleteFileW(dst);
    }
    if (a) IMFAttributes_Release(a);
    if (vo) IMFMediaType_Release(vo);
    if (vin) IMFMediaType_Release(vin);
    if (ao) IMFMediaType_Release(ao);
    if (ain) IMFMediaType_Release(ain);
    return SUCCEEDED(hr);
}

static HRESULT write_bytes(IMFSinkWriter *sw, DWORD stream, const uint8_t *const *planes, const int *sizes, int count,
                           LONGLONG t, LONGLONG dur)
{
    IMFMediaBuffer *b = NULL;
    IMFSample *s = NULL;
    BYTE *p = NULL;
    DWORD total = 0;
    for (int i = 0; i < count; i++) total += (DWORD)sizes[i];
    HRESULT hr = MFCreateMemoryBuffer(total, &b);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(b, &p, NULL, NULL);
    if (SUCCEEDED(hr)) {
        for (int i = 0; i < count; i++) {
            memcpy(p, planes[i], (size_t)sizes[i]);
            p += sizes[i];
        }
        IMFMediaBuffer_Unlock(b);
        hr = IMFMediaBuffer_SetCurrentLength(b, total);
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(s, b);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(s, t);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(s, dur);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(sw, stream, s);
    if (s) IMFSample_Release(s);
    if (b) IMFMediaBuffer_Release(b);
    return hr;
}

static AVCodecContext *open_decoder(AVFormatContext *fmt, int si)
{
    const AVCodec *c = avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id);
    AVCodecContext *d = c ? avcodec_alloc_context3(c) : NULL;
    if (d && avcodec_parameters_to_context(d, fmt->streams[si]->codecpar) >= 0) {
        d->thread_count = 0;
        if (avcodec_open2(d, c, NULL) >= 0) return d;
    }
    avcodec_free_context(&d);
    return NULL;
}

BOOL webvideo_make(const wchar_t *src, const wchar_t *dst, HANDLE cancel, void (*progress)(int percent))
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *vd = NULL, *ad = NULL;
    struct SwsContext *sws = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    IMFSinkWriter *sw = NULL;
    DWORD vs = 0, as = 0;
    uint8_t *nv12 = NULL, *pcm = NULL;
    int pcmCap = 0, ok = 0, lastPct = -1;
    BOOL com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)), mf = FALSE;
    char *u = utf8_from_wide(src, NULL);
    double t0 = (double)GetTickCount64();

    if (!u || !pkt || !fr || avformat_open_input(&fmt, u, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) goto done;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    if (vi < 0 || !(vd = open_decoder(fmt, vi)) || vd->width <= 0 || vd->height <= 0) goto done;
    if (ai >= 0 && !(ad = open_decoder(fmt, ai))) ai = -1;
    AVStream *vst = fmt->streams[vi];
    AVRational fps = av_guess_frame_rate(fmt, vst, NULL);
    if (fps.num <= 0 || fps.den <= 0 || av_q2d(fps) > 240) fps = (AVRational){ 30, 1 };
    double k = vd->width > OUT_W || vd->height > OUT_H ? fmin((double)OUT_W / vd->width, (double)OUT_H / vd->height) : 1.0;
    int w = (int)(vd->width * k) & ~1, h = (int)(vd->height * k) & ~1;
    LONGLONG frameDur = (LONGLONG)(10000000.0 / av_q2d(fps));
    double dur = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
    double start = vst->start_time != AV_NOPTS_VALUE ? vst->start_time * av_q2d(vst->time_base) : 0;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) goto done;
    mf = TRUE;
    if (!(ad && open_writer(dst, w, h, fps, TRUE, &sw, &vs, &as))) {
        avcodec_free_context(&ad);
        ai = -1;
        if (!open_writer(dst, w, h, fps, FALSE, &sw, &vs, &as)) goto done;
    }
    if (ad) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_HZ, &ad->ch_layout, ad->sample_fmt, ad->sample_rate, 0, NULL) < 0 ||
            swr_init(swr) < 0)
            goto done;
    }
    if (!(nv12 = (uint8_t *)malloc((size_t)w * h * 3 / 2))) goto done;

    LONGLONG lastV = -1, audioT = -1;
    int eof = 0;
    for (;;) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) goto done;
        int r = eof ? AVERROR_EOF : av_read_frame(fmt, pkt);
        if (r < 0 && !eof) {
            eof = 1;
            avcodec_send_packet(vd, NULL);
            if (ad) avcodec_send_packet(ad, NULL);
        } else if (!eof) {
            if (pkt->stream_index == vi) avcodec_send_packet(vd, pkt);
            else if (ad && pkt->stream_index == ai) avcodec_send_packet(ad, pkt);
            av_packet_unref(pkt);
        }
        int drained = 1;
        while (avcodec_receive_frame(vd, fr) >= 0) {
            drained = 0;
            int64_t pts = fr->best_effort_timestamp;
            double sec = pts != AV_NOPTS_VALUE ? pts * av_q2d(vst->time_base) - start : (lastV + frameDur) / 1e7;
            LONGLONG t = (LONGLONG)(fmax(sec, 0.0) * 1e7);
            if (t <= lastV) t = lastV + 1;
            lastV = t;
            uint8_t *dp[4] = { nv12, nv12 + (size_t)w * h, NULL, NULL };
            int dl[4] = { w, w, 0, 0 };
            sws = sws_getCachedContext(sws, fr->width, fr->height, (enum AVPixelFormat)fr->format, w, h, AV_PIX_FMT_NV12,
                                       SWS_BICUBIC, NULL, NULL, NULL);
            if (!sws) goto done;
            sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dp, dl);
            const uint8_t *planes[1] = { nv12 };
            int sizes[1] = { w * h * 3 / 2 };
            if (FAILED(write_bytes(sw, vs, planes, sizes, 1, t, frameDur))) goto done;
            int pct = dur > 0 ? (int)(sec * 100 / dur) : -1;
            if (progress && pct != lastPct && pct >= 0 && pct <= 100) progress(lastPct = pct);
        }
        while (ad && avcodec_receive_frame(ad, fr) >= 0) {
            drained = 0;
            if (audioT < 0) {                                    /* from the first sound's place against the picture's */
                AVStream *ast = fmt->streams[ai];
                double sec = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * av_q2d(ast->time_base) - start : 0;
                audioT = (LONGLONG)(fmax(sec, 0.0) * 1e7);
            }
            int n = swr_get_out_samples(swr, fr->nb_samples);
            if (n <= 0) continue;
            if (n * 4 > pcmCap) {
                uint8_t *np = (uint8_t *)realloc(pcm, (size_t)n * 4);
                if (!np) goto done;
                pcm = np;
                pcmCap = n * 4;
            }
            n = swr_convert(swr, &pcm, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
            if (n <= 0) continue;
            LONGLONG d = (LONGLONG)n * 10000000 / AUDIO_HZ;
            const uint8_t *planes[1] = { pcm };
            int sizes[1] = { n * 4 };
            if (FAILED(write_bytes(sw, as, planes, sizes, 1, audioT, d))) goto done;
            audioT += d;
        }
        if (eof && drained) break;
    }
    ok = SUCCEEDED(IMFSinkWriter_Finalize(sw));
    if (ok) LOGI(L"depth: re-encoded '%ls' to H.264 %dx%d%ls in %.1f s", path_filename(src), w, h, ai >= 0 ? L" with sound" : L"",
                 ((double)GetTickCount64() - t0) / 1000.0);

done:
    if (sw) IMFSinkWriter_Release(sw);
    if (!ok) {
        LOGW(L"depth: '%ls' could not be re-encoded for the page", path_filename(src));
        DeleteFileW(dst);
    }
    if (mf) MFShutdown();
    if (com) CoUninitialize();
    free(nv12);
    free(pcm);
    swr_free(&swr);
    sws_freeContext(sws);
    avcodec_free_context(&vd);
    avcodec_free_context(&ad);
    avformat_close_input(&fmt);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    free(u);
    return ok;
}
