#include "audio_visualizer.h"
#include "../common/log.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <math.h>
#include <stdlib.h>

/* The Windows headers only declare these COM GUIDs in C builds.  Defining the
 * four values locally avoids a C++-only uuid library dependency. */
const IID IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const CLSID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4c32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

#define AV_BINS 64
#define AV_FFT  1024

static struct {
    HANDLE thread, quit;
    CRITICAL_SECTION lock;
    LONG initialized, running, haveData;
    ULONGLONG retryAfter;
    float bands[AV_BINS];
} A;

static float sample_at(const BYTE *p, const WAVEFORMATEX *f, int frame)
{
    int ch = f->nChannels ? f->nChannels : 1;
    int bps = f->wBitsPerSample / 8;
    const BYTE *q = p + (size_t)frame * f->nBlockAlign;
    float sum = 0;
    for (int c = 0; c < ch; c++, q += bps) {
        if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && bps == 4) sum += *(const float *)q;
        else if (bps == 2) sum += *(const short *)q / 32768.0f;
        else if (bps == 4) sum += *(const int *)q / 2147483648.0f;
    }
    return sum / ch;
}

static void make_spectrum(const float samples[AV_FFT])
{
    float result[AV_BINS];
    for (int b = 0; b < AV_BINS; b++) {
        /* Log-like bins are friendlier to wallpaper visualisers than linear FFT bins. */
        int lo = 1 + (int)(pow((double)AV_FFT / 2, (double)b / AV_BINS));
        int hi = 1 + (int)(pow((double)AV_FFT / 2, (double)(b + 1) / AV_BINS));
        if (hi <= lo) hi = lo + 1;
        double energy = 0;
        for (int k = lo; k < hi; k++) {
            double re = 0, im = 0;
            for (int n = 0; n < AV_FFT; n++) {
                double v = samples[n] * (0.5 - 0.5 * cos(6.283185307179586 * n / (AV_FFT - 1)));
                double a = 6.283185307179586 * k * n / AV_FFT;
                re += v * cos(a); im -= v * sin(a);
            }
            energy += sqrt(re * re + im * im) / AV_FFT;
        }
        /* Keep Lively's familiar 0..1 scale while retaining quiet detail. */
        result[b] = (float)fmin(1.0, sqrt(energy / (hi - lo)) * 8.0);
    }
    EnterCriticalSection(&A.lock);
    memcpy(A.bands, result, sizeof result);
    LeaveCriticalSection(&A.lock);
    InterlockedExchange(&A.haveData, 1);
}

static DWORD WINAPI capture_thread(void *unused)
{
    (void)unused;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL;
    IAudioClient *client = NULL; IAudioCaptureClient *cap = NULL;
    WAVEFORMATEX *fmt = NULL;
    float ring[AV_FFT] = { 0 }; int at = 0, started = 0;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IMMDeviceEnumerator, (void **)&en)) ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) ||
        FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client)) ||
        FAILED(IAudioClient_GetMixFormat(client, &fmt)) ||
        FAILED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       200000, 0, fmt, NULL)) ||
        FAILED(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&cap)) ||
        FAILED(IAudioClient_Start(client))) {
        LOGW(L"audio visualizer: WASAPI loopback is unavailable");
        goto done;
    }
    started = 1;
    while (WaitForSingleObject(A.quit, 15) == WAIT_TIMEOUT) {
        UINT32 packets = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &packets))) break;
        while (packets) {
            BYTE *data = NULL; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, NULL, NULL))) goto done;
            for (UINT32 i = 0; i < frames; i++) {
                ring[at++] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : sample_at(data, fmt, (int)i);
                if (at == AV_FFT) { make_spectrum(ring); at = 0; }
            }
            IAudioCaptureClient_ReleaseBuffer(cap, frames);
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &packets))) goto done;
        }
    }
done:
    if (client) IAudioClient_Stop(client);
    SP_RELEASE(cap); SP_RELEASE(client); SP_RELEASE(dev); SP_RELEASE(en);
    CoTaskMemFree(fmt);
    if (SUCCEEDED(hr)) CoUninitialize();
    if (!started) A.retryAfter = GetTickCount64() + 10000;
    InterlockedExchange(&A.running, 0);
    return 0;
}

BOOL audio_visualizer_start(void)
{
    if (GetTickCount64() < A.retryAfter) return FALSE;
    if (A.thread) {
        if (WaitForSingleObject(A.thread, 0) == WAIT_TIMEOUT) return TRUE;
        CloseHandle(A.thread); CloseHandle(A.quit); A.thread = A.quit = NULL;
    }
    InterlockedExchange(&A.running, 1);
    if (!InterlockedCompareExchange(&A.initialized, 1, 0)) InitializeCriticalSection(&A.lock);
    A.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!A.quit) { InterlockedExchange(&A.running, 0); return FALSE; }
    A.thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!A.thread) { CloseHandle(A.quit); A.quit = NULL; InterlockedExchange(&A.running, 0); return FALSE; }
    LOGD(L"audio visualizer: capturing system audio");
    return TRUE;
}

void audio_visualizer_stop(void)
{
    if (!A.thread) return;
    LOGD(L"audio visualizer: stopped");
    InterlockedExchange(&A.running, 0);
    SetEvent(A.quit); WaitForSingleObject(A.thread, 1500);
    CloseHandle(A.thread); CloseHandle(A.quit); A.thread = A.quit = NULL;
    InterlockedExchange(&A.haveData, 0);
}

BOOL audio_visualizer_spectrum(float out[AV_BINS])
{
    if (!A.running || !InterlockedCompareExchange(&A.haveData, 0, 0)) return FALSE;
    EnterCriticalSection(&A.lock); memcpy(out, A.bands, sizeof A.bands); LeaveCriticalSection(&A.lock);
    return TRUE;
}
