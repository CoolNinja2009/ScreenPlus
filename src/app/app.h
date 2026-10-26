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
#define WM_SP_URLIMPORT     (WM_APP + 63)   /* wParam 0: progress, 1: job finished (lParam: the job) */
#define WM_SP_DEPTH         (WM_APP + 64)   /* wParam 0: progress, 1: job finished (lParam: the job) */
#define WM_SP_SHELLCMD      (WM_APP + 70)   /* wParam: IpcCommandId, run outside the sender's call */

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

/* cli_server.c: one ScreenPlus.com request (IPC_CLI_REQUEST), answered to replyTo */
void cli_server_handle(HWND replyTo, const void *request, DWORD size);

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

/* url_import.c: import from a web link (the Library's link button), one job at a time */
enum { URLMODE_AUTO = 0, URLMODE_DOWNLOAD = 1, URLMODE_PAGE = 2 };   /* AUTO: long videos stream */
typedef struct UrlImportState {
    int     busy;
    int     percent;        /* -1: unknown */
    int     failed;         /* text says why the last attempt failed */
    int     finished;       /* jobs finished so far */
    int     lastOk;         /* the last job added (or found) its wallpaper */
    wchar_t text[200];      /* busy: what is happening */
} UrlImportState;
BOOL url_import_start(const wchar_t *url, int mode);   /* FALSE: invalid link (state.text says why) or busy */
void url_import_cancel(void);
void url_import_get(UrlImportState *out);
void url_import_on_event(WPARAM wp, LPARAM lp);        /* WM_SP_URLIMPORT */
void url_import_shutdown(void);
void url_import_sweep(void);                           /* startup: delete downloads no longer in the library */
/* An entry was removed: deletes what Screen+ made for it (its download, a stream's cached address and
 * preview). TRUE when a downloaded wallpaper file was deleted. */
BOOL url_import_forget(const wchar_t *path, const wchar_t *thumb);
BOOL url_import_is_download(const wchar_t *path);      /* a file Screen+ downloaded (removed with its entry) */

/* depth.c: "Make wallpaper cooler" turns an image wallpaper into an interactive depth web wallpaper (the
 * entry becomes WP_HTML + WPFLAG_DEPTH). One job at a time; the downloads (ONNX Runtime, model) are only
 * ever started by the user's own action. depth_infer.c is the --depth child process that runs the model. */
typedef struct DepthState {
    int     busy;
    int     percent;        /* -1: unknown */
    int     failed;         /* text says why the last job failed */
    wchar_t id[SP_ID_LEN];  /* wallpaper being converted; empty: only fetching the files */
    wchar_t text[200];
} DepthState;
BOOL depth_ready(void);                                /* runtime and model are downloaded (cheap check) */
BOOL depth_can_cool(const WallpaperEntry *e);          /* a still picture that exists (not an animated GIF) */
BOOL depth_start(const wchar_t *wallpaperId);          /* "" fetches the files only; FALSE: busy or not an image */
void depth_cancel(void);
void depth_get(DepthState *out);
void depth_revert(const wchar_t *wallpaperId);         /* back to the plain image; the depth data is kept */
void depth_auto(const wchar_t *wallpaperId);           /* "interactive images" setting: cool this wallpaper if it is a new image */
void depth_auto_all(BOOL fetchMissing);                /* the same for every assigned wallpaper; fetchMissing: the user just switched it on */
void depth_forget(const WallpaperEntry *e);            /* the entry was removed: delete its generated page */
void depth_sweep(void);                                /* startup: delete generated pages no entry uses */
void depth_on_event(WPARAM wp, LPARAM lp);             /* WM_SP_DEPTH */
void depth_shutdown(void);
enum { DEPTH_AUTO = 0, DEPTH_CPU = 1, DEPTH_BENCH = 2, DEPTH_CRASHED = 3 };   /* which processor: see depth_infer.c */
int  depth_infer_main(const wchar_t *image, const wchar_t *dir, int mode, int wantPng);     /* ScreenPlus.exe --depth */
