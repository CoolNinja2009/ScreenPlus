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
    const JVal *store;      /* store: {loading, failed, error, items[]} */
    bool connected;         /* the service answered the last refresh */
    bool busy;              /* commands are still running */
    float cpu;              /* the renderer's CPU use, % of the machine (< 0: unknown) */
    BareImage backdrop;     /* the desktop behind the window (0: draw a plain background) */
    const char *message;    /* the last command's answer, shown as a toast (NULL: none) */
    double message_time;    /* bare_time() when it arrived */
    bool message_failed;
    /* Callbacks into the host. */
    void *user;
    BareImage (*thumbnail)(void *user, const char *path);   /* cached; 0 while it loads */
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
    BareVec2 popover_at;
    float drag_look[5];     /* Customise sliders while dragged */
    int dragging_look;      /* slider being dragged + 1 */
    double look_sent;
} SpView;

void        sp_draw(SpView *view, const SpModel *model);
/* Helpers the hosts share. */
const char *sp_setting(const SpModel *m, const char *key, const char *def);
bool        sp_wallpaper_matches(const JVal *item, const char *search, int filter, int group, const char *collection);
