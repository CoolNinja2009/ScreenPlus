#pragma once
#include "bare.h"
#include <stdbool.h>

typedef enum SpBarePage {
    SP_BARE_LIBRARY, SP_BARE_DISPLAYS, SP_BARE_CUSTOMISE,
    SP_BARE_STORE, SP_BARE_SETTINGS
} SpBarePage;

typedef struct SpBareWallpaper {
    const char *id;
    const char *title;
    const char *kind;
    const char *thumbnail_path;
    BareImage thumbnail;
    bool selected;
    bool favourite;
    bool missing;
} SpBareWallpaper;

typedef struct SpBareDisplay {
    const char *name;
    const char *wallpaper_title;
    int index, width, height, hz;
    bool primary, connected, own_wallpaper;
} SpBareDisplay;

typedef struct SpBareStoreItem {
    const char *title, *source, *kind;
    int number;
} SpBareStoreItem;

typedef struct SpBareModel {
    const SpBareWallpaper *wallpapers;
    int wallpaper_count;
    int active_wallpaper;
    const SpBareDisplay *displays;
    int display_count;
    int display_mode;
    const SpBareStoreItem *store_items;
    int store_count;
    bool store_loading;
    const char *store_error;
    bool look_available;
    int look[5]; /* brightness, contrast, saturation, hue, gamma */
    const char *status;
    const char *display_name;
    bool paused;
    bool muted;
    bool start_with_system;
    bool start_minimised;
    bool still_at_start;
    bool auto_update;
    bool sample_updates;
    int appearance;
    int animation_level;
    bool transparent_window;
} SpBareModel;

typedef enum SpBareActionKind {
    SP_BARE_NAVIGATE, SP_BARE_SELECT_WALLPAPER, SP_BARE_IMPORT,
    SP_BARE_PAUSE, SP_BARE_MUTE, SP_BARE_RESTART, SP_BARE_SETTING,
    SP_BARE_REFRESH, SP_BARE_LOAD_MORE, SP_BARE_DISPLAY_MODE,
    SP_BARE_STORE_SEARCH, SP_BARE_STORE_MORE, SP_BARE_STORE_GET,
    SP_BARE_LOOK_SET, SP_BARE_LOOK_RESET
} SpBareActionKind;

/* path belongs to Bare's file dialog and is valid only during dispatch. */
typedef struct SpBareAction { SpBareActionKind kind; int index; int value; const char *path; } SpBareAction;
typedef void (*SpBareDispatch)(void *user, SpBareAction action);

typedef struct SpBareView {
    SpBarePage page;
    int filter;
    int visible_limit;
    int target_display; /* 0 = all; otherwise the CLI display number */
    char search[128];
    char store_search[128];
} SpBareView;

bool sp_bare_wallpaper_matches(const SpBareWallpaper *wallpaper, const char *search, int filter);
void sp_bare_draw(SpBareView *view, const SpBareModel *model, SpBareDispatch dispatch, void *user);
