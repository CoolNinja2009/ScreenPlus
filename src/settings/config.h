/* Versioned, validated configuration: settings, wallpaper library and monitor assignments.
 * Stored as UTF-8 INI at %LOCALAPPDATA%\ScreenPlus\config.ini, written atomically
 * (temp file + ReplaceFile with a .bak copy). Only the shell process writes it; the
 * engine process reads it on start and when told to reload. */
#pragma once
#include "../common/common.h"

#define CONFIG_SCHEMA     1
#define CONFIG_MAX_ITEMS  4000
#define CONFIG_MAX_ASSIGN 64

typedef enum ThemeMode { THEME_SYSTEM = 0, THEME_LIGHT = 1, THEME_DARK = 2 } ThemeMode;
/* How the displays share wallpapers. Independent: per-display overrides over the default.
 * Duplicate: the default on every display (one decoder per video, shared). Span: the default
 * stretched over the bounding box of all displays, one renderer. Overrides are kept either way. */
typedef enum DisplayMode { DM_INDEPENDENT = 0, DM_DUPLICATE = 1, DM_SPAN = 2 } DisplayMode;
/* Screensaver displays: the wallpaper on every display, spanned across them, or on the primary
 * display only (the others black). */
typedef enum ScrDisplays { SCRD_ALL = 0, SCRD_SPAN = 1, SCRD_PRIMARY = 2 } ScrDisplays;

/* Colour grading applied to any wallpaper (video/image shaders, web CSS): the Customise
 * panel edits it, <data>\props\<id>.json keeps it under "_screenplus_adjust". */
typedef struct WallpaperAdjust {
    int      used;          /* 0: defaults, no grading at all */
    int      brightness;    /* -100..100 */
    int      contrast;      /* -100..100 */
    int      saturation;    /* -100..100 */
    int      hue;           /* -180..180 degrees */
    int      gamma;         /* -100..100 (0 = 1.0) */
    uint32_t tint;          /* 0xRRGGBB */
    int      tintLift;      /* 0..100: tint colour strength ('lift the blacks') */
    int      tintBrightness;/* 0..100: tint colour brightness (100 = full) */
} WallpaperAdjust;

static inline WallpaperAdjust adjust_defaults(void)
{
    WallpaperAdjust a;
    ZeroMemory(&a, sizeof a);
    a.tint = 0xFF7575;      /* styling default; does nothing until tintLift > 0 */
    a.tintBrightness = 100;
    return a;
}

/* Grading is on as soon as any control leaves neutral (the tint colour alone changes nothing).
 * `used` is always derived from the values, never trusted from storage. */
static inline int adjust_active(const WallpaperAdjust *a)
{
    return a->brightness || a->contrast || a->saturation || a->hue || a->gamma || a->tintLift > 0;
}

/* Origin of a wallpaper package: drives which JavaScript APIs the HTML engine provides. */
typedef enum WallpaperFormat { WPF_PLAIN = 0, WPF_LIVELY = 1, WPF_WE = 2 } WallpaperFormat;
enum { WPFLAG_AUDIO   = 1,  /* audio-reactive (Lively "webaudio") */
       WPFLAG_URL     = 2,  /* path is an http(s) URL: an online page, or a streamed video */
       WPFLAG_SYSINFO = 4,  /* wants livelySystemInformation (Lively "--system-information") */
       WPFLAG_YTDLP   = 8,  /* stream wallpaper (video, path is a page): yt-dlp finds the media address */
       WPFLAG_DEPTH   = 16, /* image made interactive (depth.c): path is the generated page, `thumb` the picture */
       WPFLAG_NOCOOL  = 32 };/* image the user took back to plain: the "interactive images" setting skips it */

typedef struct WallpaperEntry {
    wchar_t       id[SP_ID_LEN];
    WallpaperType type;
    wchar_t       name[SP_NAME_LEN];
    wchar_t       path[SP_MAX_PATH];   /* file (image/video) or HTML entry document */
    int64_t       added;               /* unix seconds */
    int64_t       lastUsed;            /* unix seconds, 0 = never */
    int           format;              /* WallpaperFormat */
    int           flags;               /* WPFLAG_* */
    wchar_t       thumb[SP_MAX_PATH];  /* optional preview image (package thumbnail) */
    int           missing;             /* runtime only: source not found at last check */
} WallpaperEntry;

typedef struct MonitorAssignment {
    wchar_t monitorId[SP_MONID_LEN];
    wchar_t wallpaperId[SP_ID_LEN];
} MonitorAssignment;

typedef struct Settings {
    int       startWithWindows;
    int       launchToTray;
    int       pauseOnFullscreen;
    int       pauseWhenCovered;
    int       suspendOnLock;
    int       pauseOnBattery;
    int       audio;          /* video/HTML audio (muted by default) */
    int       autoRecover;
    int       paused;         /* user pause, persisted */
    int       htmlMouse;      /* forward desktop mouse input to HTML wallpapers */
    int       htmlAudio;      /* system-audio spectrum for audio-reactive wallpapers */
    int       livelyImport;   /* add newly installed Lively Wallpaper packages at launch */
    int       urlImport;      /* link button: import from web links (setup's optional yt-dlp component) */
    int       depthAll;       /* make every image wallpaper interactive (depth.c) when it is applied */
    ThemeMode theme;
    DisplayMode displayMode;
    int       scrAudio;       /* screensaver sound (muted by default) */
    ScrDisplays scrDisplays;
    int       scrExitOnMouse; /* a mouse move ends the screensaver (keys and clicks always do) */
} Settings;

typedef struct Config {
    int               schema;
    Settings          s;
    wchar_t           defaultWallpaper[SP_ID_LEN];
    wchar_t           screensaver[SP_ID_LEN];   /* screensaver wallpaper, "": the desktop's default */
    MonitorAssignment assign[CONFIG_MAX_ASSIGN];
    int               assignCount;
    WallpaperEntry   *items;
    int               count;
    int               cap;
} Config;

void            config_defaults(Config *c);
BOOL            config_load(Config *c, const wchar_t *path);    /* FALSE: missing/corrupt, defaults applied */
BOOL            config_save(const Config *c, const wchar_t *path);
void            config_free(Config *c);
BOOL            config_copy(Config *dst, const Config *src);

WallpaperEntry *config_find(Config *c, const wchar_t *id);
int             config_index_of(const Config *c, const wchar_t *id);
WallpaperEntry *config_find_path(Config *c, const wchar_t *path);
WallpaperEntry *config_add(Config *c, WallpaperType type, const wchar_t *path, const wchar_t *name);
BOOL            config_remove(Config *c, const wchar_t *id);

/* Effective wallpaper id for a monitor: per-monitor override (independent mode only), else the
 * default ("" if none). */
const wchar_t  *config_assignment(const Config *c, const wchar_t *monitorId);
BOOL            config_has_override(const Config *c, const wchar_t *monitorId);
void            config_assign(Config *c, const wchar_t *monitorIdOrNull, const wchar_t *wallpaperId);
void            config_clear_override(Config *c, const wchar_t *monitorId);
/* A display moved to another port or dock gets a new id: re-key the one override stored for the same
 * model whose display isn't connected. ids: the connected displays. Returns how many changed. */
int             config_adopt_monitors(Config *c, const wchar_t *const *ids, int n);
/* Screensaver wallpaper id: the chosen one, else the desktop's default ("" if none). */
const wchar_t  *config_screensaver(const Config *c);

WallpaperType   wallpaper_type_from_path(const wchar_t *path);
int             wallpaper_is_url(const wchar_t *path);
int             wallpaper_source_exists(const WallpaperEntry *e);
