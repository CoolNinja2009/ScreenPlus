#include "now_playing.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/log.h"
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <shcore.h>
#include <windows.media.control.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

/* WinRT interface IDs are only declared for C; define the two this file queries. */
static const IID k_IID_IAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID k_IID_ManagerStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };

typedef __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionManager         Manager;
typedef __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionManagerStatics  ManagerStatics;
typedef __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSession                Session;
typedef __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionMediaProperties MediaProps;
typedef __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamReference                           StreamRef;
typedef __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamWithContentType                     StreamWithType;

#define MAX_THUMB (4u << 20)

static struct {
    HANDLE           thread, quit;
    CRITICAL_SECTION lock;
    int              init;
    char            *json;
    unsigned         serial;
} NP;

static __declspec(thread) HANDLE t_quit;   /* the worker's own stop event (a detached worker keeps it) */

static int quitting(void) { return WaitForSingleObject(t_quit, 0) == WAIT_OBJECT_0; }

/* Polls an IAsyncOperation (any WinRT async object) until it finishes; FALSE on error/timeout/quit. */
static BOOL wait_async(void *op, DWORD timeoutMs)
{
    IAsyncInfo *info = NULL;
    if (!op || FAILED(((IUnknown *)op)->lpVtbl->QueryInterface((IUnknown *)op, &k_IID_IAsyncInfo, (void **)&info))) return FALSE;
    AsyncStatus st = (AsyncStatus)0;   /* Started */
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (SUCCEEDED(info->lpVtbl->get_Status(info, &st)) && (int)st == 0 && GetTickCount64() < end && !quitting()) Sleep(10);
    if ((int)st == 0) info->lpVtbl->Cancel(info);
    info->lpVtbl->Release(info);
    return (int)st == 1;               /* Completed */
}

static char *base64(const BYTE *p, size_t n)
{
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = (char *)malloc((n + 2) / 3 * 4 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *o++ = k[v >> 18 & 63];
        *o++ = k[v >> 12 & 63];
        *o++ = i + 1 < n ? k[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? k[v & 63] : '=';
    }
    *o = 0;
    return out;
}

/* Album art as base64 (PNG/JPEG bytes as provided by the player), or NULL. */
static char *read_thumbnail(StreamRef *ref)
{
    __FIAsyncOperation_1_Windows__CStorage__CStreams__CIRandomAccessStreamWithContentType *op = NULL;
    StreamWithType *rs = NULL;
    IStream *st = NULL;
    BYTE *buf = NULL;
    size_t len = 0;
    char *out = NULL;
    if (SUCCEEDED(ref->lpVtbl->OpenReadAsync(ref, &op)) && wait_async(op, 3000) &&
        SUCCEEDED(op->lpVtbl->GetResults(op, &rs)) && rs &&
        SUCCEEDED(CreateStreamOverRandomAccessStream((IUnknown *)rs, &IID_IStream, (void **)&st))) {
        buf = (BYTE *)malloc(MAX_THUMB);
        ULONG got = 0;
        while (buf && len < MAX_THUMB && SUCCEEDED(IStream_Read(st, buf + len, (ULONG)(MAX_THUMB - len), &got)) && got)
            len += got;
        if (buf && len && len < MAX_THUMB) out = base64(buf, len);
    }
    free(buf);
    SP_RELEASE(st);
    SP_RELEASE(rs);
    SP_RELEASE(op);
    return out;
}

static void put_hstring(StrBuf *b, const char *key, HSTRING h)
{
    UINT32 n = 0;
    const wchar_t *w = h ? WindowsGetStringRawBuffer(h, &n) : NULL;
    char *u = utf8_from_wide(w ? w : L"", NULL);
    sb_printf(b, "\"%s\":", key);
    json_write_string(b, u ? u : "");
    sb_puts(b, ",");
    free(u);
    if (h) WindowsDeleteString(h);
}

/* Builds the livelyCurrentTrack JSON. The thumbnail is re-read only when the track changes
 * (or it was not available yet); *thumb / *thumbKey cache it between polls. */
static char *current_track(Manager *mgr, char **thumb, char **thumbKey)
{
    Session *s = NULL;
    __FIAsyncOperation_1_Windows__CMedia__CControl__CGlobalSystemMediaTransportControlsSessionMediaProperties *op = NULL;
    MediaProps *p = NULL;
    char *json = NULL;
    if (SUCCEEDED(mgr->lpVtbl->GetCurrentSession(mgr, &s)) && s &&
        SUCCEEDED(s->lpVtbl->TryGetMediaPropertiesAsync(s, &op)) && wait_async(op, 3000) &&
        SUCCEEDED(op->lpVtbl->GetResults(op, &p)) && p) {
        HSTRING title = NULL, artist = NULL, album = NULL, albumArtist = NULL, subtitle = NULL;
        INT32 track = 0, tracks = 0;
        StreamRef *ref = NULL;
        p->lpVtbl->get_Title(p, &title);
        p->lpVtbl->get_Artist(p, &artist);
        p->lpVtbl->get_AlbumTitle(p, &album);
        p->lpVtbl->get_AlbumArtist(p, &albumArtist);
        p->lpVtbl->get_Subtitle(p, &subtitle);
        p->lpVtbl->get_TrackNumber(p, &track);
        p->lpVtbl->get_AlbumTrackCount(p, &tracks);
        p->lpVtbl->get_Thumbnail(p, &ref);

        /* key = title|artist|album, compared as UTF-8 */
        UINT32 n1 = 0, n2 = 0, n3 = 0;
        const wchar_t *t1 = title ? WindowsGetStringRawBuffer(title, &n1) : L"";
        const wchar_t *t2 = artist ? WindowsGetStringRawBuffer(artist, &n2) : L"";
        const wchar_t *t3 = album ? WindowsGetStringRawBuffer(album, &n3) : L"";
        wchar_t wkey[1024];
        str_printf(wkey, SP_COUNTOF(wkey), L"%ls|%ls|%ls", t1, t2, t3);
        char *k = utf8_from_wide(wkey, NULL);
        if (k && (!*thumbKey || strcmp(k, *thumbKey) != 0 || (!*thumb && ref))) {
            free(*thumb);
            *thumb = ref ? read_thumbnail(ref) : NULL;
            free(*thumbKey);
            *thumbKey = k;
            k = NULL;
        }
        free(k);
        SP_RELEASE(ref);

        StrBuf b = { 0 };
        sb_puts(&b, "{");
        put_hstring(&b, "Title", title);
        put_hstring(&b, "Artist", artist);
        put_hstring(&b, "AlbumTitle", album);
        put_hstring(&b, "AlbumArtist", albumArtist);
        put_hstring(&b, "Subtitle", subtitle);
        sb_printf(&b, "\"TrackNumber\":%d,\"AlbumTrackCount\":%d,\"Thumbnail\":", track, tracks);
        if (*thumb) {
            sb_puts(&b, "\"");
            sb_puts(&b, *thumb);
            sb_puts(&b, "\"}");
        } else {
            sb_puts(&b, "null}");
        }
        if (!b.failed) json = b.p;
        else sb_free(&b);
    }
    SP_RELEASE(p);
    SP_RELEASE(op);
    SP_RELEASE(s);
    return json ? json : _strdup("null");
}

static void publish(char *json)
{
    if (!json) return;
    EnterCriticalSection(&NP.lock);
    if (!NP.json || strcmp(NP.json, json) != 0) {
        LOGI(L"now playing: %ls", strcmp(json, "null") ? L"track information updated" : L"nothing playing");
        free(NP.json);
        NP.json = json;
        json = NULL;
        if (++NP.serial == 0) NP.serial = 1;   /* 0 means "never delivered" to callers */
    }
    LeaveCriticalSection(&NP.lock);
    free(json);
}

static unsigned __stdcall worker(void *arg)
{
    t_quit = (HANDLE)arg;
    SetThreadDescription(GetCurrentThread(), L"ScreenPlus now playing");
    HRESULT hrRo = RoInitialize(RO_INIT_MULTITHREADED);
    ManagerStatics *statics = NULL;
    __FIAsyncOperation_1_Windows__CMedia__CControl__CGlobalSystemMediaTransportControlsSessionManager *op = NULL;
    Manager *mgr = NULL;
    HSTRING cls = NULL;
    const wchar_t *name = RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager;
    if (SUCCEEDED(WindowsCreateString(name, (UINT32)wcslen(name), &cls)) &&
        SUCCEEDED(RoGetActivationFactory(cls, &k_IID_ManagerStatics, (void **)&statics)) &&
        SUCCEEDED(statics->lpVtbl->RequestAsync(statics, &op)) && wait_async(op, 5000))
        op->lpVtbl->GetResults(op, &mgr);
    if (cls) WindowsDeleteString(cls);
    SP_RELEASE(op);
    SP_RELEASE(statics);
    if (!mgr) {
        LOGW(L"now playing: media session manager unavailable; wallpapers get no track information");
        publish(_strdup("null"));
    } else {
        char *thumb = NULL, *thumbKey = NULL;
        do publish(current_track(mgr, &thumb, &thumbKey));
        while (WaitForSingleObject(t_quit, 1000) == WAIT_TIMEOUT);
        free(thumb);
        free(thumbKey);
        mgr->lpVtbl->Release(mgr);
    }
    if (SUCCEEDED(hrRo)) RoUninitialize();
    return 0;
}

BOOL now_playing_start(void)
{
    if (!NP.init) {
        InitializeCriticalSection(&NP.lock);
        NP.init = 1;
    }
    if (NP.thread) return TRUE;
    NP.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!NP.quit) return FALSE;
    NP.thread = (HANDLE)_beginthreadex(NULL, 0, worker, NP.quit, 0, NULL);
    if (!NP.thread) {
        CloseHandle(NP.quit);
        NP.quit = NULL;
        return FALSE;
    }
    LOGI(L"now playing: watching the Windows media session");
    return TRUE;
}

void now_playing_stop(void)
{
    if (!NP.thread) return;
    SetEvent(NP.quit);
    if (WaitForSingleObject(NP.thread, 1500) == WAIT_OBJECT_0) CloseHandle(NP.quit);
    else LOGW(L"now playing: worker still busy; detaching it");   /* it exits once its WinRT call returns */
    CloseHandle(NP.thread);
    NP.thread = NP.quit = NULL;
    EnterCriticalSection(&NP.lock);
    free(NP.json);
    NP.json = NULL;
    LeaveCriticalSection(&NP.lock);
}

char *now_playing_get(unsigned *serial)
{
    char *copy = NULL;
    if (!NP.init) return NULL;
    EnterCriticalSection(&NP.lock);
    if (NP.json && NP.serial != *serial) {
        copy = _strdup(NP.json);
        if (copy) *serial = NP.serial;
    }
    LeaveCriticalSection(&NP.lock);
    return copy;
}
