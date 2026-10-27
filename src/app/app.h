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
#define WM_SP_STORE         (WM_APP + 65)   /* lParam: a Store worker's result */
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
BOOL url_import_start_titled(const wchar_t *url, int mode, const wchar_t *title);   /* title: the entry's name (NULL: found) */
void html_unescape(wchar_t *s);                        /* the usual named entities and apostrophes, in place */
void url_import_cancel(void);
void url_import_get(UrlImportState *out);
void url_import_on_event(WPARAM wp, LPARAM lp);        /* WM_SP_URLIMPORT */
void url_import_shutdown(void);
void url_import_sweep(void);                           /* startup: delete downloads no longer in the library */
/* An entry was removed: deletes what Screen+ made for it (its download, a stream's cached address and
 * preview). TRUE when a downloaded wallpaper file was deleted. */
BOOL url_import_forget(const wchar_t *path, const wchar_t *thumb);
BOOL url_import_is_download(const wchar_t *path);      /* a file Screen+ downloaded (removed with its entry) */

/* store.c: the Store page's catalogue, MotionBGS live wallpapers (docs/store/motionbgs.md). The UI thread owns
 * this state; the page reads it through store_get and asks for what is on screen with store_want. */
#define STORE_VARIANTS 4
enum { STORE_NONE = 0, STORE_LOADING, STORE_READY, STORE_FAILED };
enum { STOREDL_IDLE = 0, STOREDL_WAIT, STOREDL_RUNNING, STOREDL_DONE, STOREDL_FAILED };
typedef struct StoreVariant {
    wchar_t kind[8];                   /* "4k", "hd", "mhd", "gif": its /dl/<kind>/<id>/ address */
    int     w, h;
    float   mb;                        /* size as the page states it (MiB), 0 unknown */
} StoreVariant;
typedef struct StoreItem {
    int          id;                   /* MotionBGS media id, also the download id */
    wchar_t      slug[96];             /* its page: https://motionbgs.com/<slug> */
    wchar_t      title[SP_NAME_LEN];
    wchar_t      poster[128];          /* the poster's file name under /media/<id>/ */
    wchar_t      badge[8];             /* the site's quality label: "4K", "HD" */
    int          w, h;                 /* source resolution when the poster's name carries it, else 0 */
    int          preview, detail;      /* STORE_*: the preview image; the page's download list */
    int          previewBig;           /* the cached preview is the 546x308 one */
    int          dl;                   /* STOREDL_IDLE, _WAIT (its page is read first) or _FAILED */
    StoreVariant var[STORE_VARIANTS];  /* detail STORE_READY: what the page offers */
    int          varCount;
} StoreItem;
typedef struct StoreState {
    StoreItem *items;
    int        count;
    wchar_t    query[96];              /* what is listed: "" the latest wallpapers */
    int        loading;                /* a page is being read (count 0: the first one) */
    int        more;                   /* another page exists */
    int        failed;                 /* the last page read failed: error says why */
    wchar_t    error[200];
} StoreState;
const StoreState *store_get(void);
void store_open(void);                                 /* the page is shown: the first time, lists the latest */
BOOL store_search(const wchar_t *query);               /* "" the latest; TRUE: a new listing started */
void store_more(void);                                 /* the next page, unless one is loading or none is left */
void store_retry(void);                                /* after a failed page read */
void store_want(int index, int pixelW, int detail);    /* a card is near the screen: its preview (+ its page's downloads) */
BOOL store_preview_entry(int index, WallpaperEntry *e);    /* TRUE: preview cached; e stands in for the thumbnail loader */
const StoreVariant *store_best(const StoreItem *it);   /* the largest video it offers, NULL before its page is read */
int  store_dl_state(int index, int *percent);          /* STOREDL_*; percent: -1 unknown */
void store_download(int index);                        /* the best variant into the library (again: cancel) */
void store_page_url(int index, wchar_t *out, size_t cap);
void store_on_event(WPARAM wp, LPARAM lp);             /* WM_SP_STORE */
void store_on_import(void);                            /* after url_import_on_event: the queued download may start */
void store_shutdown(void);

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
