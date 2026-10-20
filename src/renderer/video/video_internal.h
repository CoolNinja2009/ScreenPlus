/* FFmpeg + Direct3D 11 video pipeline (engine process).
 *
 *   decode thread : libavformat demux -> libavcodec decode (D3D11VA or software)
 *                   -> bounded frame queue (VQ_CAP frames, blocking = backpressure)
 *                   -> audio packets to a bounded packet queue (only when audio is enabled)
 *   present thread: timestamp-driven: sleeps on a high-resolution waitable timer until the
 *                   next frame is due, draws it (NV12/P010 sampled directly from the decoder
 *                   texture array when possible) into every output swap chain, Present(0).
 *   audio thread  : libavcodec audio decode -> libswresample -> WASAPI shared/event mode,
 *                   slaved to the presentation clock. Created only when audio is enabled.
 * One VideoSource is shared by all monitors showing the same file (one decoder). */
#pragma once
#include "../renderer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define VQ_CAP 4      /* decoded video frames queued ahead of presentation */
#define AQ_CAP 256    /* audio packets queued (bounded; oldest dropped when full) */

enum { HW_NONE = 0, HW_ZEROCOPY = 1, HW_COPY = 2 };

typedef struct VFrame {
    AVFrame *f;
    double   pts;   /* seconds on the continuous (loop-adjusted) timeline */
    double   dur;
} VFrame;

typedef struct FrameQueue {
    VFrame             items[VQ_CAP];
    int                head, count;
    SRWLOCK            lock;
    CONDITION_VARIABLE notFull;
    HANDLE             pushed;     /* auto-reset, signalled on push */
    volatile LONG      abort;
} FrameQueue;

typedef struct PacketQueue {
    AVPacket *items[AQ_CAP];
    double    marker[AQ_CAP];      /* loop markers: next loop start time (or -1) */
    int       head, count;
    SRWLOCK   lock;
    int       dropped;
} PacketQueue;

typedef struct VideoSource VideoSource;

typedef struct VideoOutput {
    Renderer                base;
    VideoSource            *src;
    HWND                    hwnd;
    HWND                    notify;
    IDXGISwapChain1        *swap;
    ID3D11RenderTargetView *rtv;
    int                     w, h;
    volatile LONG           newW, newH, resizePending;
    volatile LONG           state;          /* RenderState */
    int                     audio;
    int                     readyPosted;
    int                     flipModel;
    int                     layered;
} VideoOutput;

struct VideoSource {
    volatile LONG     refs;
    wchar_t           path[SP_MAX_PATH];
    char             *pathUtf8;

    HANDLE            decodeThread, presentThread, audioThread;
    HANDLE            wake;                 /* presenter control (auto-reset) */
    HANDLE            audioWake;            /* audio control (auto-reset) */
    HANDLE            decodeWake;           /* decode thread control (auto-reset) */
    volatile LONG     quit;
    volatile LONG     audioQuit;
    volatile LONG     failed;
    volatile LONG     playing;              /* desired: at least one ACTIVE output */
    volatile LONG     redraw;
    volatile LONG     audioWanted;

    CRITICAL_SECTION  outLock;
    VideoOutput      *outputs[SP_MAX_MONITORS];
    int               outputCount;

    /* decode thread */
    AVFormatContext  *fmt;
    AVCodecContext   *vdec;
    AVCodecContext   *adec;
    AVFrame          *tmp;
    int               vidx, aidx;
    int               hwMode;
    int               hwFailed;
    int               hwErrors;
    double            loopBase, loopStartRaw, lastPts, lastDur, lastRaw;
    int               haveStart, rebase, framesThisLoop, emptyLoops, loops, still;
    int64_t           framesDecoded;
    double            decodeMsTotal;

    FrameQueue        fq;
    PacketQueue       aq;

    /* presentation */
    AVFrame          *current;
    double            currentPts;
    volatile double   clockBase;            /* time_ms() at media time 0 */
    volatile LONG     clockValid;
    ID3D11Texture2D  *upTex[3];
    ID3D11ShaderResourceView *upSrv[3];
    int               upKind, upW, upH;     /* software upload layout */
    ID3D11Texture2D  *copyTex;
    ID3D11ShaderResourceView *copySrv[2];
    int               copyW, copyH, copyFmt;
    ID3D11Texture2D  *hwTex;                /* decoder texture array with cached SRVs */
    ID3D11ShaderResourceView *hwSrv[2];
    struct SwsContext *sws;
    AVFrame          *swsFrame;
    int64_t           presented, dropped;

    /* information */
    wchar_t           codec[32];
    int               width, height;
    double            fps;
    wchar_t           err[SP_ERR_LEN];
};

/* video_renderer.c */
double video_media_now(VideoSource *s);
void   video_source_fail(VideoSource *s, HRESULT hr, const wchar_t *fmt, ...);

/* video_audio.c */
void   audio_start(VideoSource *s);    /* decode thread: opens decoder + output thread */
void   audio_stop(VideoSource *s);
void   pq_init(PacketQueue *q);
void   pq_put(PacketQueue *q, AVPacket *pkt, double marker);   /* takes ownership of pkt (may be NULL for markers) */
int    pq_get(PacketQueue *q, AVPacket **pkt, double *marker);
void   pq_clear(PacketQueue *q);
