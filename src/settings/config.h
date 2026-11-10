/* Versioned, validated configuration: settings, wallpaper library and monitor assignments.
 * Stored as UTF-8 INI at %LOCALAPPDATA%\ScreenPlus\config.ini, written atomically
 * (temp file + ReplaceFile with a .bak copy). Only the shell process writes it; the
 * engine process reads it on start and when told to reload. */
#pragma once
#include "../common/common.h"

#define CONFIG_SCHEMA     1
#define CONFIG_MAX_ITEMS  4000
#define CONFIG_MAX_ASSIGN 64
#define SP_RULES_LEN      512

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
       WPFLAG_NOCOOL  = 32, /* image the user took back to plain: the "interactive images" setting skips it */
       WPFLAG_DEPTHVIDEO = 64,   /* with WPFLAG_DEPTH: `thumb` is a video (going back makes it WP_VIDEO again) */
       WPFLAG_FAV     = 128,     /* a favourite (star) */
       WPFLAG_PPTX    = 256 };   /* a presentation (pptx.c): `thumb` the .pptx, `path` PowerPoint's video of it */
#define SP_TAGS_LEN 256            /* a wallpaper's collections: "Name;Other" */
#define SP_SLIDE_SEC_DEFAULT 5

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
    wchar_t       tags[SP_TAGS_LEN];   /* collections it is in, ';'-separated */
    int           volCut;              /* 100 - its own volume (0..100): 0 = full; see entry_volume */
    int           slideSec;            /* WPFLAG_PPTX: seconds per slide (0 = SP_SLIDE_SEC_DEFAULT) */
    int           uiMark;              /* runtime only: 1 selected in the Library, 2 waiting for a new collection's name */
    int           missing;             /* runtime only: source not found at last check */
} WallpaperEntry;

static inline int entry_volume(const WallpaperEntry *e) { return 100 - SP_CLAMP(e->volCut, 0, 100); }
static inline int entry_slide_sec(const WallpaperEntry *e) { return e->slideSec > 0 ? e->slideSec : SP_SLIDE_SEC_DEFAULT; }

typedef struct MonitorAssignment {
    wchar_t monitorId[SP_MONID_LEN];
    wchar_t wallpaperId[SP_ID_LEN];
    wchar_t details[96];                 /* "XV272U X1, 2560x1440 @ 200 Hz": which display, for people reading config.ini */
} MonitorAssignment;

typedef struct Settings {
    int       startWithWindows;
    int       launchToTray;
    int       stillWallpaper;   /* a still of the wallpaper is the Windows wallpaper (engine/stills.c) */
    int       pauseOnFullscreen;
    int       pauseWhenCovered;
    int       suspendOnLock;
    int       pauseOnBattery;
    int       audio;          /* video/HTML audio (muted by default) */
    int       autoRecover;
    int       paused;         /* user pause, persisted */
    int       htmlMouse;      /* forward desktop mouse input to HTML wallpapers */
    int       htmlAudio;      /* system-audio spectrum for audio-reactive wallpapers */
    int       webFps;         /* frame-rate cap for HTML wallpapers: 30, 60 or 0 (the display's rate) */
    int       livelyImport;   /* add newly installed Lively Wallpaper packages at launch */
    int       urlImport;      /* link button: import from web links (setup's optional yt-dlp component) */
    int       depthAll;       /* make every image wallpaper interactive (depth.c) when it is applied */
    wchar_t   depthModel[32]; /* the depth model's key (depth.c's table; unknown: the default) */
    ThemeMode theme;
    DisplayMode displayMode;
    int       scrAudio;       /* screensaver sound (muted by default) */
    ScrDisplays scrDisplays;
    int       scrExitOnMouse; /* a mouse move ends the screensaver (keys and clicks always do) */
    int       autoUpdate;     /* check GitHub for a newer release and install it (update.c) */
    int       autoUpdateSamples; /* update the downloaded sample wallpapers that changed (samples_update.c) */
    wchar_t   pauseApps[SP_RULES_LEN]; /* app rules: "a.exe;b.exe", pause while one is in front */
    wchar_t   muteApps[SP_RULES_LEN];  /* mute while one uses the microphone ("*": any app) */
    int       volume;         /* wallpaper sound volume 0..100, times each wallpaper's own */
    int       slideshow;      /* change the wallpaper by itself every slideshowMin minutes (rotate.c) */
    int       slideshowMin;
    int       shuffle;        /* slideshow and Next pick at random */
    wchar_t   slideshowFrom[64]; /* "": every wallpaper, "*": favourites, else a collection's name */
    int       schedule;       /* day and night wallpapers (rotate.c) */
    int       dayStart, nightStart;   /* hours 0..23 */
    wchar_t   dayWallpaper[SP_ID_LEN], nightWallpaper[SP_ID_LEN];
    int       librarySort;    /* LIBSORT_* (the Library page's order) */
} Settings;

enum { LIBSORT_CUSTOM = 0, LIBSORT_NAME, LIBSORT_ADDED, LIBSORT_TYPE, LIBSORT_SIZE, LIBSORT_COUNT };

typedef struct Config {
    int               schema;
    Settings          s;
    wchar_t           defaultWallpaper[SP_ID_LEN];
    wchar_t           screensaver[SP_ID_LEN];   /* screensaver wallpaper, "": the desktop's default */
    MonitorAssignment assign[CONFIG_MAX_ASSIGN];
    int               assignCount;
    int               perDisplay;               /* displays have their own saved wallpaper (2.3+) */
    WallpaperEntry   *items;
    int               count;
    int               cap;
} Config;

void            config_defaults(Config *c);
BOOL            config_load(Config *c, const wchar_t *path);    /* FALSE: missing/corrupt, defaults applied */
BOOL            config_save(const Config *c, const wchar_t *path);
void            config_free(Config *c);
BOOL            config_copy(Config *dst, const Config *src);

/* App rule lists: lowercase exe names separated by ';'. */
void            config_rule_name(wchar_t *out, size_t cap, const wchar_t *app);  /* path or name -> "name.exe" */
BOOL            config_rule_has(const wchar_t *list, const wchar_t *app);
BOOL            config_rule_add(wchar_t *list, size_t cap, const wchar_t *app);  /* FALSE: already there or full */
BOOL            config_rule_remove(wchar_t *list, const wchar_t *app);
int             config_rule_at(const wchar_t *list, int index, wchar_t *out, size_t cap);   /* 0: no such entry */

/* Collections (WallpaperEntry.tags): names separated by ';', compared ignoring case. */
BOOL            config_tag_has(const WallpaperEntry *e, const wchar_t *name);
BOOL            config_tag_set(WallpaperEntry *e, const wchar_t *name, BOOL on);   /* FALSE: no change */
int             config_collections(const Config *c, wchar_t (*out)[64], int max);  /* every name, in first-seen order */
/* Is e one the slideshow may show? from: "" every wallpaper, "*" favourites, else a collection. */
BOOL            config_in_source(const WallpaperEntry *e, const wchar_t *from);

WallpaperEntry *config_find(Config *c, const wchar_t *id);
int             config_index_of(const Config *c, const wchar_t *id);
WallpaperEntry *config_find_path(Config *c, const wchar_t *path);
WallpaperEntry *config_add(Config *c, WallpaperType type, const wchar_t *path, const wchar_t *name);
BOOL            config_remove(Config *c, const wchar_t *id);
BOOL            config_move(Config *c, int from, int to);   /* library order: the entry at from goes to index to */

/* Effective wallpaper id for a monitor: the one saved for that display (independent mode), or the default
 * (duplicate/span); when none is saved or it was removed, the first wallpaper by name ("" for an empty library). */
const wchar_t  *config_assignment(const Config *c, const wchar_t *monitorId);
const wchar_t  *config_first_by_name(const Config *c);
BOOL            config_has_override(const Config *c, const wchar_t *monitorId);   /* its own, not the default */
BOOL            config_saved_for(const Config *c, const wchar_t *monitorId);      /* anything saved under that id */
/* NULL monitor = all displays: the default and every saved display's wallpaper. */
void            config_assign(Config *c, const wchar_t *monitorIdOrNull, const wchar_t *wallpaperId);
void            config_clear_override(Config *c, const wchar_t *monitorId);
/* Every display keeps its own wallpaper under its EDID id (monitor.h), with its details (name, size, rate) in
 * [displays]. On each refresh of the connected displays: an override saved under a display's old port id, or for the
 * same model on a port now empty, moves to its id; a display with nothing saved gets the first wallpaper by name
 * (the first time, after an update from 2.2, the default it was showing). Returns how many changed. */
struct MonitorInfo;
int             config_adopt_monitors(Config *c, const struct MonitorInfo *mons, int n);
/* Screensaver wallpaper id: the chosen one, else the desktop's default ("" if none). */
const wchar_t  *config_screensaver(const Config *c);

WallpaperType   wallpaper_type_from_path(const wchar_t *path);
int             wallpaper_is_url(const wchar_t *path);
int             wallpaper_source_exists(const WallpaperEntry *e);
