#include "screenplus_bare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Preview {
    SpBareView view;
    SpBareModel model;
    SpBareWallpaper walls[3];
    char status[256];
} Preview;

static void action(void *user, SpBareAction a) {
    Preview *p = user;
    switch (a.kind) {
    case SP_BARE_SELECT_WALLPAPER:
        p->model.active_wallpaper = a.index;
        for (int i = 0; i < p->model.wallpaper_count; i++) p->walls[i].selected = i == a.index;
        break;
    case SP_BARE_PAUSE: p->model.paused = a.value != 0; break;
    case SP_BARE_MUTE: p->model.muted = a.value != 0; break;
    case SP_BARE_SETTING:
        switch (a.index) {
        case 0: p->model.start_with_system = a.value != 0; break;
        case 1: p->model.start_minimised = a.value != 0; break;
        case 2: p->model.still_at_start = a.value != 0; break;
        case 3: p->model.auto_update = a.value != 0; break;
        case 4: p->model.sample_updates = a.value != 0; break;
        default: break;
        }
        if (a.index == 5) p->model.appearance = a.value;
        if (a.index == 6) p->model.animation_level = a.value;
        break;
    case SP_BARE_IMPORT:
        snprintf(p->status, sizeof p->status, "Selected: %.230s", a.path ? a.path : "");
        p->model.status = p->status;
        break;
    default: break;
    }
}

int main(int argc, char **argv) {
    const char *samples = argc > 1 ? argv[1] : "samples";
    Preview p = { 0 };
    p.walls[0] = (SpBareWallpaper){ .id = "KoiPond", .title = "Koi Pond", .kind = "HTML" };
    p.walls[1] = (SpBareWallpaper){ .id = "Aurora", .title = "Aurora", .kind = "HTML" };
    p.walls[2] = (SpBareWallpaper){ .id = "Singularity", .title = "Singularity", .kind = "HTML",
                                    .selected = true, .favourite = true };
    p.model.wallpapers = p.walls;
    p.model.wallpaper_count = 3;
    p.model.active_wallpaper = 2;
    p.model.status = "Idle (desktop covered)";
    p.model.display_name = "All displays";
    if (!bare_window(1500, 960, "Screen+ Bare UI preview")) return 1;
    bare_background(BARE_RGB(9, 22, 24));
    for (int i = 0; i < 3; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s/preview.jpg", samples, p.walls[i].id);
        p.walls[i].thumbnail = bare_image_load(path);
    }
    while (bare_frame()) sp_bare_draw(&p.view, &p.model, action, &p);
    bare_quit();
    return 0;
}

