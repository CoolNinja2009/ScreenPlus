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
#define WM_SP_UPDATE        (WM_APP + 66)   /* wParam 0: progress, 1: check finished (lParam: the job) */
#define T_SHELL_UPDATE      2               /* shell window timer: the next update check (app.c's own start at 1) */
#define T_SHELL_STARTUP     3               /* shell window timer: the Start with Windows check, once after start */
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
void app_set_display_mode(int mode);                 /* DisplayMode */
void app_set_screensaver(const wchar_t *wallpaperId); /* "": the desktop's default */
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
BOOL startup_set_enabled(BOOL on);
void startup_sync(void);                               /* T_SHELL_STARTUP */

/* library.c */
int  library_import(const wchar_t *const *paths, int count, BOOL applyFirst);
void library_import_dialog(HWND owner, BOOL folder);
void library_import_lively(void);
int  library_auto_import_lively(void); /* silent startup discovery; returns newly-added entries */
int  library_add_samples(void);        /* the bundled samples\ wallpapers, silently; returns newly-added entries */
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

/* store.c + store_sources.c: the Store page's catalogue, live wallpapers and still pictures from several sites in
 * one feed (docs/store/sources.md). The UI thread owns this state; the page draws the items `view` lists and asks
 * for what is on screen with store_want. */
#define STORE_VARIANTS 6
enum { STORE_NONE = 0, STORE_LOADING, STORE_READY, STORE_FAILED };
enum { STOREDL_IDLE = 0, STOREDL_WAIT, STOREDL_RUNNING, STOREDL_DONE, STOREDL_FAILED, STOREDL_SITE };   /* SITE: on its page */
enum { SKIND_LIVE = 1, SKIND_STILL = 2 };
enum { SF_CATEGORY = 0, SF_RES, SF_SORT, SF_KIND, SF_SOURCE, SF_COUNT };   /* filters; values index store_filter_names */
typedef struct StoreVariant {
    char  key[16];                     /* the site's name for this file: "4k", "3840x2160", "mp4" */
    int   w, h;
    float mb;                          /* size as the site states it (MiB), 0 unknown */
} StoreVariant;
typedef struct StoreItem {
    int          source, kind;         /* store_source_name(source); SKIND_* */
    uint32_t     key;                  /* the card's identity: its site and ref */
    char         ref[128];             /* the site's key for its page (an ASCII path or id) */
    char         media[192];           /* the site's key for its preview */
    char         extra[32];            /* another key some sites need (MotionBGS's media id, Wallhaven's file type) */
    wchar_t      title[SP_NAME_LEN];
    wchar_t      badge[8];             /* the site's quality label: "4K", "HD" */
    int          w, h;                 /* resolution when known, else 0 */
    float        mb;
    int          preview, detail, dl;  /* STORE_*: preview, page read; STOREDL_IDLE, _WAIT (page first) or _FAILED */
    int          previewBig;           /* the preview is the site's larger crop */
    int          hashed, dupOf;        /* hash: of the preview; dupOf: 1 + index of the better copy it folds into */
    uint64_t     hash;
    StoreVariant var[STORE_VARIANTS];  /* the files it comes in */
    int          varCount;
} StoreItem;
typedef struct StoreState {
    StoreItem *items;
    int        count;
    int       *view;                   /* indexes of the items shown, in order: copies and filtered ones left out */
    int        viewCount;
    wchar_t    query[96];              /* "" the latest */
    int        filter[SF_COUNT];
    int        loading;                /* pages are being read (viewCount 0: the first ones) */
    int        more;                   /* a site has another page */
    int        failed;                 /* nothing came and every site failed: error says why */
    wchar_t    error[200];
} StoreState;
const StoreState *store_get(void);
const wchar_t *store_source_name(int source);
int  store_filter_names(int which, const wchar_t **names, int cap);   /* SF_*: the choices filter[which] indexes */
void store_set_filter(int which, int value);           /* lists again; which < 0: every filter back to its first choice */
void store_open(void);                                 /* the page is shown: the first time, lists the latest */
BOOL store_search(const wchar_t *query);               /* "" the latest; TRUE: a new listing started */
void store_more(void);                                 /* the next pages, unless some are loading or none are left */
void store_retry(void);                                /* after failed page reads */
void store_want(int index, int pixelW, int detail);    /* a card is near the screen: its preview (+ its page) */
BOOL store_preview_entry(int index, WallpaperEntry *e);    /* TRUE: preview cached; e stands in for the thumbnail loader */
const StoreVariant *store_best(const StoreItem *it);   /* the largest file it comes in, NULL before its page is read */
int  store_dl_state(int index, int *percent);          /* STOREDL_*; percent: -1 unknown */
void store_download(int index);                        /* the best file into the library (again: cancel); _SITE: its page */
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

/* update.c: the "Auto update" setting. Checks GitHub's latest release, downloads a newer installer (size and
 * SHA-256 as GitHub lists them) and runs it silently; setup updates in place and starts Screen+ again. */
typedef struct UpdateState {
    int     busy;
    int     percent;            /* download, -1: unknown */
    int     supported;          /* an installed copy (portable and development builds never update) */
    wchar_t text[200];          /* status line for Settings ("" before the first check) */
} UpdateState;
void update_init(void);                                /* shell start: clears old downloads, arms the first check */
void update_schedule(BOOL soon);                       /* the setting changed: re-arm (soon: check in a few seconds) or stop */
BOOL update_check(BOOL install);                       /* now; install: a newer release is downloaded and installed */
void update_get(UpdateState *out);
void update_on_timer(void);                            /* T_SHELL_UPDATE */
void update_on_event(WPARAM wp, LPARAM lp);            /* WM_SP_UPDATE */
void update_on_ui_closed(void);                        /* a ready update waits while the window is open */
void update_shutdown(void);
enum { DEPTH_AUTO = 0, DEPTH_CPU = 1, DEPTH_BENCH = 2, DEPTH_CRASHED = 3 };   /* which processor: see depth_infer.c */
int  depth_infer_main(const wchar_t *image, const wchar_t *dir, int mode, int wantPng);     /* ScreenPlus.exe --depth */
