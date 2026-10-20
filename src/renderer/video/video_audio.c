/* Optional wallpaper audio: FFmpeg decode + libswresample -> WASAPI (shared mode, event driven).
 * Created only while audio is enabled; slaved to the video presentation clock (it inserts
 * silence or drops samples when drift exceeds 50 ms) and pauses with the video. */
#include "video_internal.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

static const GUID k_clsidEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iidEnumerator   = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iidAudioClient  = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_iidRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID k_subtypeFloat    = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/* ------------------------------------------------------------ packet queue */

void pq_init(PacketQueue *q)
{
    ZeroMemory(q, sizeof *q);
    InitializeSRWLock(&q->lock);
}

void pq_put(PacketQueue *q, AVPacket *pkt, double marker)
{
    AcquireSRWLockExclusive(&q->lock);
    if (q->count == AQ_CAP) {            /* bounded memory: drop the oldest packet */
        av_packet_free(&q->items[q->head]);
        q->head = (q->head + 1) % AQ_CAP;
        q->count--;
        q->dropped++;
    }
    int i = (q->head + q->count) % AQ_CAP;
    q->items[i] = pkt;
    q->marker[i] = marker;
    q->count++;
    ReleaseSRWLockExclusive(&q->lock);
}

int pq_get(PacketQueue *q, AVPacket **pkt, double *marker)
{
    AcquireSRWLockExclusive(&q->lock);
    if (!q->count) {
        ReleaseSRWLockExclusive(&q->lock);
        return 0;
    }
    *pkt = q->items[q->head];
    *marker = q->marker[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % AQ_CAP;
    q->count--;
    ReleaseSRWLockExclusive(&q->lock);
    return 1;
}

void pq_clear(PacketQueue *q)
{
    AVPacket *p;
    double m;
    while (pq_get(q, &p, &m)) av_packet_free(&p);
}

/* ------------------------------------------------------------ WASAPI output */

typedef struct AudioDev {
    IMMDevice          *device;
    IAudioClient       *client;
    IAudioRenderClient *render;
    HANDLE              event;
    UINT32              bufFrames;
    int                 rate, channels, started;
} AudioDev;

typedef struct Pcm {
    float  *buf;
    int     cap;       /* frames */
    int     frames;
    double  pts;       /* timeline time of buf[0] */
} Pcm;

typedef struct AudioState {
    SwrContext     *swr;
    int             inRate, inFmt;
    AVChannelLayout inLayout;
    double          loopBase;
} AudioState;

static void dev_close(AudioDev *d)
{
    if (d->client && d->started) IAudioClient_Stop(d->client);
    SP_RELEASE(d->render);
    SP_RELEASE(d->client);
    SP_RELEASE(d->device);
    if (d->event) CloseHandle(d->event);
    ZeroMemory(d, sizeof *d);
}

static HRESULT dev_open(AudioDev *d)
{
    IMMDeviceEnumerator *en = NULL;
    WAVEFORMATEX *mix = NULL;
    HRESULT hr = CoCreateInstance(&k_clsidEnumerator, NULL, CLSCTX_ALL, &k_iidEnumerator, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &d->device);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(d->device, &k_iidAudioClient, CLSCTX_ALL, NULL, (void **)&d->client);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(d->client, &mix);
    if (SUCCEEDED(hr)) {
        WAVEFORMATEXTENSIBLE wf;
        ZeroMemory(&wf, sizeof wf);
        int ch = SP_CLAMP((int)mix->nChannels, 1, 8);
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD)ch;
        wf.Format.nSamplesPerSec = mix->nSamplesPerSec;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = (WORD)(ch * 4);
        wf.Format.nAvgBytesPerSec = mix->nSamplesPerSec * (DWORD)wf.Format.nBlockAlign;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask
                         : (ch == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER);
        wf.SubFormat = k_subtypeFloat;
        hr = IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     1000000 /* 100 ms */, 0, (WAVEFORMATEX *)&wf, NULL);
        d->rate = (int)mix->nSamplesPerSec;
        d->channels = ch;
    }
    if (SUCCEEDED(hr)) {
        d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = d->event ? IAudioClient_SetEventHandle(d->client, d->event) : E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(d->client, &d->bufFrames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(d->client, &k_iidRenderClient, (void **)&d->render);
    CoTaskMemFree(mix);
    SP_RELEASE(en);
    if (FAILED(hr)) dev_close(d);
    return hr;
}

static void pcm_consume(Pcm *p, int n, int channels, int rate)
{
    n = SP_MIN(n, p->frames);
    if (n <= 0) return;
    memmove(p->buf, p->buf + (size_t)n * channels, (size_t)(p->frames - n) * channels * sizeof(float));
    p->frames -= n;
    p->pts += (double)n / rate;
}

static void append_frame(VideoSource *s, AudioDev *d, Pcm *p, AudioState *as, AVFrame *fr)
{
    if (!as->swr || fr->format != as->inFmt || fr->sample_rate != as->inRate ||
        av_channel_layout_compare(&fr->ch_layout, &as->inLayout) != 0) {
        swr_free(&as->swr);
        AVChannelLayout out;
        av_channel_layout_default(&out, d->channels);
        if (swr_alloc_set_opts2(&as->swr, &out, AV_SAMPLE_FMT_FLT, d->rate, &fr->ch_layout,
                                (enum AVSampleFormat)fr->format, fr->sample_rate, 0, NULL) < 0 ||
            swr_init(as->swr) < 0) {
            swr_free(&as->swr);
            return;
        }
        as->inFmt = fr->format;
        as->inRate = fr->sample_rate;
        av_channel_layout_uninit(&as->inLayout);
        av_channel_layout_copy(&as->inLayout, &fr->ch_layout);
    }
    int outMax = swr_get_out_samples(as->swr, fr->nb_samples);
    if (outMax <= 0) return;
    if (p->frames + outMax > p->cap) {
        int nc = SP_MAX(p->cap * 2, p->frames + outMax + 4096);
        if (nc > d->rate * 4) {          /* bounded: never hold more than ~4 s of PCM */
            pcm_consume(p, p->frames, d->channels, d->rate);
            nc = SP_MAX(p->cap, outMax + 4096);
        }
        float *nb = (float *)realloc(p->buf, (size_t)nc * d->channels * sizeof(float));
        if (!nb) return;
        p->buf = nb;
        p->cap = nc;
    }
    uint8_t *outp = (uint8_t *)(p->buf + (size_t)p->frames * d->channels);
    int n = swr_convert(as->swr, &outp, outMax, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (n <= 0) return;
    if (p->frames == 0) {
        double tb = av_q2d(s->adec->pkt_timebase);
        int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
        if (ts != AV_NOPTS_VALUE) p->pts = as->loopBase + (ts * tb - s->loopStartRaw);
    }
    p->frames += n;
}

static int refill(VideoSource *s, AudioDev *d, Pcm *p, AudioState *as, AVFrame *fr)
{
    if (!s->haveStart) return 0;      /* timeline origin comes from the first video frame */
    for (int guard = 0; guard < 32; guard++) {
        AVPacket *pkt = NULL;
        double marker = -1.0;
        if (!pq_get(&s->aq, &pkt, &marker)) return 0;
        if (!pkt) {                   /* loop boundary */
            as->loopBase = marker;
            avcodec_flush_buffers(s->adec);
            continue;
        }
        int r = avcodec_send_packet(s->adec, pkt);
        av_packet_free(&pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) continue;
        int got = 0;
        while (avcodec_receive_frame(s->adec, fr) >= 0) {
            append_frame(s, d, p, as, fr);
            av_frame_unref(fr);
            got = 1;
        }
        if (got) return 1;
    }
    return 0;
}

static HRESULT feed(VideoSource *s, AudioDev *d, Pcm *p, AudioState *as, AVFrame *fr)
{
    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(d->client, &padding);
    if (FAILED(hr)) return hr;
    UINT32 avail = d->bufFrames > padding ? d->bufFrames - padding : 0;
    for (int guard = 0; avail > 0 && guard < 64; guard++) {
        if (p->frames == 0) {
            if (!refill(s, d, p, as, fr)) break;
            continue;
        }
        double playAt = video_media_now(s) + (double)padding / d->rate;
        double diff = p->pts - playAt;
        BYTE *buf = NULL;
        if (diff > 0.05) {               /* audio early: pad with silence */
            UINT32 n = (UINT32)SP_MIN((double)avail, diff * d->rate);
            if (!n) break;
            if (FAILED(hr = IAudioRenderClient_GetBuffer(d->render, n, &buf))) return hr;
            if (FAILED(hr = IAudioRenderClient_ReleaseBuffer(d->render, n, AUDCLNT_BUFFERFLAGS_SILENT))) return hr;
            padding += n;
            avail -= n;
            continue;
        }
        if (diff < -0.05) {              /* audio late: drop samples */
            int n = (int)SP_MIN((double)p->frames, -diff * d->rate);
            pcm_consume(p, n > 0 ? n : p->frames, d->channels, d->rate);
            continue;
        }
        UINT32 n = SP_MIN(avail, (UINT32)p->frames);
        if (FAILED(hr = IAudioRenderClient_GetBuffer(d->render, n, &buf))) return hr;
        memcpy(buf, p->buf, (size_t)n * d->channels * sizeof(float));
        if (FAILED(hr = IAudioRenderClient_ReleaseBuffer(d->render, n, 0))) return hr;
        pcm_consume(p, (int)n, d->channels, d->rate);
        padding += n;
        avail -= n;
    }
    return S_OK;
}

static unsigned __stdcall audio_main(void *arg)
{
    VideoSource *s = (VideoSource *)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus audio");
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    AudioDev dev;
    Pcm pcm;
    AudioState as;
    ZeroMemory(&dev, sizeof dev);
    ZeroMemory(&pcm, sizeof pcm);
    ZeroMemory(&as, sizeof as);
    as.inFmt = -1;
    AVFrame *fr = av_frame_alloc();
    int failures = 0;
    double retryAt = 0.0;
    while (fr && !s->quit && !s->audioQuit) {
        if (!dev.client) {
            double now = time_ms();
            if (now < retryAt) {
                WaitForSingleObject(s->audioWake, (DWORD)(retryAt - now) + 1);
                continue;
            }
            HRESULT hr = dev_open(&dev);
            if (FAILED(hr)) {
                LOGW(L"audio: output device unavailable (0x%08lX)", (unsigned long)hr);
                if (++failures >= 5) {
                    LOGW(L"audio: giving up after repeated device failures; video continues silently");
                    break;
                }
                retryAt = time_ms() + 1000.0 * failures;
                continue;
            }
            failures = 0;
            swr_free(&as.swr);
            pcm.frames = 0;
            LOGI(L"audio: WASAPI shared output %d Hz, %d ch, buffer %u frames", dev.rate, dev.channels, dev.bufFrames);
        }
        if (!s->playing || !s->clockValid || s->failed) {
            if (dev.started) {
                IAudioClient_Stop(dev.client);
                dev.started = 0;
            }
            WaitForSingleObject(s->audioWake, INFINITE);
            continue;
        }
        HRESULT hr = feed(s, &dev, &pcm, &as, fr);
        if (SUCCEEDED(hr) && !dev.started) {
            hr = IAudioClient_Start(dev.client);
            dev.started = SUCCEEDED(hr);
        }
        if (FAILED(hr)) {
            LOGW(L"audio: device error 0x%08lX; reopening default device", (unsigned long)hr);
            dev_close(&dev);
            retryAt = time_ms() + 500.0;
            continue;
        }
        HANDLE hs[2] = { dev.event, s->audioWake };
        WaitForMultipleObjects(2, hs, FALSE, 100);
    }
    dev_close(&dev);
    swr_free(&as.swr);
    av_channel_layout_uninit(&as.inLayout);
    av_frame_free(&fr);
    free(pcm.buf);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------ lifecycle (decode thread) */

void audio_start(VideoSource *s)
{
    if (s->adec || s->audioThread || s->aidx == -2 || !s->fmt) return;
    int idx = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, s->vidx, NULL, 0);
    if (idx < 0) {
        s->aidx = -2;
        LOGI(L"audio: %ls has no audio stream", path_filename(s->path));
        return;
    }
    AVStream *st = s->fmt->streams[idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    int ok = c && avcodec_parameters_to_context(c, st->codecpar) >= 0;
    if (ok) {
        c->pkt_timebase = st->time_base;
        ok = avcodec_open2(c, codec, NULL) >= 0;
    }
    if (!ok) {
        avcodec_free_context(&c);
        s->aidx = -2;
        LOGW(L"audio: no usable decoder for %hs", avcodec_get_name(st->codecpar->codec_id));
        return;
    }
    s->adec = c;
    s->aidx = idx;
    s->audioQuit = 0;
    st->discard = AVDISCARD_DEFAULT;
    s->audioThread = (HANDLE)_beginthreadex(NULL, 0, audio_main, s, 0, NULL);
    if (!s->audioThread) {
        st->discard = AVDISCARD_ALL;
        avcodec_free_context(&s->adec);
        s->aidx = -2;
        return;
    }
    LOGI(L"audio: enabled (%hs, %d Hz, %d ch)", codec->name, c->sample_rate, c->ch_layout.nb_channels);
}

void audio_stop(VideoSource *s)
{
    if (s->audioThread) {
        InterlockedExchange(&s->audioQuit, 1);
        SetEvent(s->audioWake);
        if (WaitForSingleObject(s->audioThread, 5000) != WAIT_OBJECT_0) {
            LOGE(L"audio: thread did not stop; leaking decoder");
            CloseHandle(s->audioThread);
            s->audioThread = NULL;
            s->adec = NULL;
            return;
        }
        CloseHandle(s->audioThread);
        s->audioThread = NULL;
        LOGI(L"audio: disabled");
    }
    if (s->aidx >= 0 && s->fmt && (unsigned)s->aidx < s->fmt->nb_streams)
        s->fmt->streams[s->aidx]->discard = AVDISCARD_ALL;
    pq_clear(&s->aq);
    avcodec_free_context(&s->adec);
    if (s->aidx >= 0) s->aidx = -1;
}
