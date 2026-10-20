/* Shell <-> engine protocol. Both processes are the same executable; messages travel
 * as WM_COPYDATA (synchronous, sent with SendMessageTimeout so a hung peer never blocks
 * the sender for long). Structures are fixed-size PODs and versioned. */
#pragma once
#include "common.h"

#define SP_SHELL_CLASS     L"ScreenPlus.Shell"
#define SP_ENGINE_CLASS    L"ScreenPlus.Engine"
#define SP_WALLPAPER_CLASS L"ScreenPlus.Wallpaper"
#define SP_INSTANCE_MUTEX  L"Local\\ScreenPlus.Instance.v1"
#define SP_IPC_VERSION     1

enum {
    IPC_ENGINE_HELLO  = 0x53500001, /* engine -> shell : IpcHello */
    IPC_ENGINE_STATUS = 0x53500002, /* engine -> shell : IpcEngineStatus */
    IPC_ENGINE_CMD    = 0x53500003, /* shell -> engine : IpcCommand */
    IPC_SHELL_CMD     = 0x53500004  /* 2nd instance -> shell : IpcCommand */
};

typedef enum IpcCommandId {
    CMD_NONE = 0,
    /* engine commands */
    CMD_RELOAD_CONFIG = 1,
    CMD_SET_PAUSED,          /* arg 0/1 */
    CMD_SET_AUDIO,           /* arg 0/1 */
    CMD_RESTART_RENDERERS,   /* arg 0 = failed only, 1 = all */
    CMD_SHUTDOWN,
    CMD_REQUEST_STATUS,
    /* shell commands (second instance / command line) */
    CMD_SHOW_UI = 100,
    CMD_TOGGLE_PAUSE,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_NEXT_WALLPAPER,
    CMD_EXIT
} IpcCommandId;

typedef struct IpcCommand {
    uint32_t cmd;
    uint32_t arg;
} IpcCommand;

typedef struct IpcHello {
    uint32_t version;
    uint32_t pid;
    uint64_t hwnd;
    uint64_t processStart;
} IpcHello;

enum { HOSTMODE_NONE = 0, HOSTMODE_WORKERW = 1, HOSTMODE_PROGMAN = 2, HOSTMODE_FALLBACK = 3 };

enum {
    ENGINE_FLAG_PAUSED      = 0x01,
    ENGINE_FLAG_LOCKED      = 0x02,
    ENGINE_FLAG_BATTERY     = 0x04,
    ENGINE_FLAG_FULLSCREEN  = 0x08,
    ENGINE_FLAG_DISPLAY_OFF = 0x10,
    ENGINE_FLAG_SAFE_MODE   = 0x20,
    ENGINE_FLAG_WEBVIEW2    = 0x40,
    ENGINE_FLAG_SLEEPING    = 0x80
};

typedef struct IpcMonitorStatus {
    wchar_t monitorId[SP_MONID_LEN];
    wchar_t wallpaperId[SP_ID_LEN];
    int32_t type;      /* WallpaperType */
    int32_t state;     /* RenderState */
    int32_t failed;    /* in safe state after repeated failures */
    int32_t failures;
    int32_t covered;
    int32_t shared;    /* decoder shared with another monitor */
    wchar_t error[SP_ERR_LEN];
} IpcMonitorStatus;

typedef struct IpcEngineStatus {
    uint32_t version;
    uint32_t flags;
    int32_t  hostMode;
    int32_t  monitorCount;
    int32_t  activeRenderers;
    int32_t  reserved;
    uint64_t processStart;   /* engine process creation (FILETIME ticks) */
    uint64_t firstFrame;     /* first wallpaper frame visible (FILETIME ticks), 0 if none */
    double   initMs;         /* engine native init duration */
    wchar_t  webviewVersion[48];
    wchar_t  lastError[SP_ERR_LEN];
    IpcMonitorStatus monitors[SP_MAX_MONITORS];
} IpcEngineStatus;

/* Sends a WM_COPYDATA payload with a bounded timeout. Returns FALSE on failure/timeout. */
static __inline BOOL ipc_send(HWND from, HWND to, ULONG_PTR id, const void *data, DWORD size, UINT timeoutMs)
{
    COPYDATASTRUCT cds;
    DWORD_PTR result = 0;
    if (!to || !IsWindow(to)) return FALSE;
    cds.dwData = id;
    cds.cbData = size;
    cds.lpData = (PVOID)data;
    /* SMTO_NORMAL (not BLOCK): while waiting, the sender still services messages sent to it,
     * so two processes sending to each other at the same moment cannot deadlock. */
    return SendMessageTimeoutW(to, WM_COPYDATA, (WPARAM)from, (LPARAM)&cds,
                               SMTO_ABORTIFHUNG | SMTO_NORMAL, timeoutMs, &result) != 0;
}
