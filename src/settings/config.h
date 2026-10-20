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

/* Origin of a wallpaper package: drives which JavaScript APIs the HTML engine provides. */
typedef enum WallpaperFormat { WPF_PLAIN = 0, WPF_LIVELY = 1, WPF_WE = 2 } WallpaperFormat;
enum { WPFLAG_AUDIO   = 1,  /* audio-reactive (Lively "webaudio") */
       WPFLAG_URL     = 2,  /* online page: path is an http(s) URL */
       WPFLAG_SYSINFO = 4 };/* wants livelySystemInformation (Lively "--system-information") */

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
    ThemeMode theme;
} Settings;

typedef struct Config {
    int               schema;
    Settings          s;
    wchar_t           defaultWallpaper[SP_ID_LEN];
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

/* Effective wallpaper id for a monitor: per-monitor override, else the default ("" if none). */
const wchar_t  *config_assignment(const Config *c, const wchar_t *monitorId);
BOOL            config_has_override(const Config *c, const wchar_t *monitorId);
void            config_assign(Config *c, const wchar_t *monitorIdOrNull, const wchar_t *wallpaperId);
void            config_clear_override(Config *c, const wchar_t *monitorId);

WallpaperType   wallpaper_type_from_path(const wchar_t *path);
int             wallpaper_is_url(const wchar_t *path);
int             wallpaper_source_exists(const WallpaperEntry *e);
