/* Shell process: tray, management UI, configuration owner and engine supervisor. */
#pragma once
#include "../common/common.h"
#include "../common/ipc.h"
#include "../settings/config.h"
#include "../monitor/monitor.h"
#include "../diagnostics/diag.h"

#define WM_SP_ENGINE_EXITED (WM_APP + 60)
#define WM_SP_TRAY          (WM_APP + 61)
#define WM_SP_STATUS        (WM_APP + 62)

enum { NOTICE_NONE = 0, NOTICE_INFO, NOTICE_WARN, NOTICE_ERROR };

typedef struct AppState {
    HINSTANCE        inst;
    HWND             shellWnd;
    Config           cfg;
    MonitorInfo      mons[SP_MAX_MONITORS];
    int              monCount;
    IpcEngineStatus  status;
    int              haveStatus;
    uint64_t         processStart;

    /* supervisor */
    HWND             engineWnd;
    DWORD            enginePid;
    HANDLE           engineProcess;
    HANDLE           engineWait;
    HANDLE           job;
    uint64_t         engineSpawnTime;
    int              crashes;            /* within the current window */
    double           firstCrashAt;
    int              safeMode;
    int              engineGaveUp;       /* stopped after repeated crashes */
    int              restartPending;
    int              engineRestarts;
    wchar_t          engineError[SP_ERR_LEN];

    /* diagnostics */
    double           nativeInitMs;       /* process start -> tray + engine launched */
    double           uiReadyMs;          /* process start -> first UI frame */
    double           wallpaperMs;        /* process start -> first wallpaper frame */
    CpuSampler       cpuShell, cpuEngine;
    double           shellCpu, engineCpu;
    size_t           shellMem, engineMem;
    int              engineProcs;

    wchar_t          notice[256];
    int              noticeKind;
    int              exiting;
    int              startHidden;
} AppState;

extern AppState g_app;

int  app_main(HINSTANCE inst, int argc, wchar_t **argv);

void app_save_config(void);                                  /* atomic save + engine reload */
void app_apply_customise(void);                              /* Customise panel: renderers re-read properties + grading */
void app_apply_wallpaper(const wchar_t *wallpaperId, const wchar_t *monitorIdOrNull);
const wchar_t *app_wallpaper_for(const wchar_t *monitorIdOrNull);
void app_set_paused(BOOL paused);
void app_set_audio(BOOL on);
void app_next_wallpaper(void);
void app_restart_renderer(void);
void app_exit(void);
void app_show_ui(void);
void app_notice(int kind, const wchar_t *fmt, ...);
void app_refresh_monitors(void);
const IpcMonitorStatus *app_monitor_status(const wchar_t *monitorId);
const wchar_t *app_engine_state_text(int *severity);         /* severity: 0 ok, 1 paused, 2 warn, 3 error */

/* supervisor.c */
BOOL supervisor_start(void);
void supervisor_on_hello(const IpcHello *h);
void supervisor_on_exit(DWORD code, DWORD pid);
void supervisor_on_timer(void);
BOOL supervisor_send(uint32_t cmd, uint32_t arg);
void supervisor_stop(void);
void supervisor_restart(void);
void supervisor_sample_stats(void);

/* startup.c */
BOOL startup_is_enabled(void);
BOOL startup_set_enabled(BOOL on);

/* library.c */
int  library_import(const wchar_t *const *paths, int count, BOOL applyFirst);
void library_import_dialog(HWND owner, BOOL folder);
void library_import_lively(void);
int  library_auto_import_lively(void); /* silent startup discovery; returns newly-added entries */
void library_import_wallpaper_engine(void);
void library_remove(const wchar_t *id);
void library_check_missing(void);
BOOL library_html_entry(const wchar_t *folder, wchar_t *out, size_t cap);
