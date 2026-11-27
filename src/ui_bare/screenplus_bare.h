/* The Screen+ window in Bare: a view over the service's JSON (the same data
 * `ScreenPlus.com <command> --json` prints). Every action is a service command;
 * the data comes back refreshed. live.c feeds it from the running service,
 * preview.c from sample data. */
#pragma once
#include "bare.h"
#include "../common/json.h"
#include <stdbool.h>

typedef enum SpPage { SP_LIBRARY, SP_DISPLAYS, SP_CUSTOMISE, SP_STORE, SP_SETTINGS, SP_PAGE_COUNT } SpPage;

typedef struct SpModel {
    /* The "data" of each reply; NULL until the first answer. */
    const JVal *status;     /* status: state, paused, audio, memoryMB, displays[] */
    const JVal *settings;   /* settings: key -> value */
    const JVal *list;       /* list: [{number, onScreen, wallpaper{...}}] */
    const JVal *look;       /* look: brightness ... (NULL: nothing to adjust) */
    const JVal *layout;     /* layout: layout, border, color, page */
    const JVal *upscale;    /* upscale: upscale, smooth (NULL: not for this wallpaper) */
    const JVal *props;      /* props: properties[] (NULL: a plain file) */
    const JVal *record;     /* record: the take, look/props/volume recordings, presets */
    const JVal *cool;       /* cool: Make wallpaper cooler's state and models */
    double record_time;     /* bare_time() when record arrived (playheads move on from its positions) */
    const JVal *store;      /* store: {loading, failed, error, items[]} */
    bool connected;         /* the service answered the last refresh */
    bool busy;              /* commands are still running */
    float cpu;              /* the renderer's CPU use, % of the machine (< 0: unknown) */
    BareImage backdrop;     /* the desktop behind the window (0: draw a plain background) */
    bool see_through;       /* the system's glass is behind the window (macOS): draw no background */
    const char *message;    /* the last command's answer, shown as a toast (NULL: none) */
    double message_time;    /* bare_time() when it arrived */
    bool message_failed;
    const char *previews;   /* where Screen+ keeps pages' recorded previews, if the service doesn't say */
    /* Callbacks into the host. */
    void *user;
    /* Pictures come from bare_picture; a host with pictures of its own (the sample data) answers instead. */
    BareImage (*picture)(void *user, const char *path, int width, int height);
    void (*run)(void *user, const char *const *argv);       /* a service command (NULL-terminated, copied) */
    void (*open_path)(void *user, const char *path);        /* shows a file in the file manager */
} SpModel;

typedef struct SpView {
    SpPage page;
    double page_time;       /* when the page changed: pages slide in */
    int filter;             /* 0 all, 1 images, 2 videos, 3 web */
    int group;              /* 0 everything, 1 favourites, 2.. a collection */
    int target_display;     /* 0 = all displays; else the display's number */
    char search[128];
    char store_search[128];
    char link[1024];        /* the link popover's text */
    int popover;            /* 0 none, 1 link, 2 sort, 3 card menu, 4 language */
    int menu_card;          /* the card the menu is for (list index) */
    int renaming;           /* 1 + the card whose name is being edited (0: none) */
    int rename_frames;      /* frames since that began (focus lands a frame later) */
    char rename[128];
    BareVec2 popover_at;
    float drag_look[8];     /* Customise sliders while dragged: 5 look, tint strength, tint brightness, volume */
    int prop_drag;          /* 1 + the property slider being dragged */
    float prop_drag_value;
    int prop_editing;       /* 1 + the text property being edited */
    char prop_text[256];
    bool graphs[3];         /* a section's recording graphs are open (look, props, volume) */
    int presets_for;        /* 1 + section * 64 + track: its preset strip is open */
    int preset_times;       /* how many times a preset repeats over the loop */
    BareId wheel_open;      /* the colour control whose wheel is open */
    BareId wheel_drag;      /* the wheel being dragged */
    float wheel_h, wheel_s, wheel_v;
    double wheel_sent;
    int draw_track;         /* 1 + section * 64 + track being drawn over */
    float draw_x, draw_y;   /* where the last sent stroke ended */
    float pen_x, pen_y;     /* where the pen was last frame */
    double draw_sent;
    int draw_key;           /* the track the drawn heights are for (0: none) */
    int draw_n;
    float draw_vals[240];   /* the track as drawn, shown until the service's copy has caught up */
    double draw_release;    /* when the pen came up */
    BareId value_edit;      /* the value being typed (0: none) */
    char value_text[32];
    int rec_slider;         /* 1 + section * 2 + (0 speed, 1 smoothing) while dragged */
    float rec_value;
    int layout_drag;        /* 1 + the drag: 0 move, else edge bits (1 left, 2 right, 4 top, 8 bottom) */
    float layout_rect[4];   /* while dragged: x y w h, display fractions */
    float layout_start[4];  /* the rect when the drag began */
    float layout_view[4];   /* the editor's view (display fractions), held still while dragging */
    BareVec2 layout_grab;   /* where the drag began (window) */
    double layout_sent;
    double layout_hold;     /* show layout_rect until then, or until the service reports it */
    bool value_focused;     /* the typed value's field has had the focus */
    char layout_for[64];    /* the wallpaper the history is for */
    float layout_hist[40][4];   /* undo / redo: each settled layout (mode, and the rect when custom) */
    char layout_hist_mode[40][12];
    int layout_hist_n, layout_hist_at;
    int dragging_look;      /* slider being dragged + 1 */
    double look_sent;
} SpView;

void        sp_draw(SpView *view, const SpModel *model);
/* Helpers the hosts share. */
const char *sp_setting(const SpModel *m, const char *key, const char *def);
bool        sp_wallpaper_matches(const JVal *item, const char *search, int filter, int group, const char *collection);
