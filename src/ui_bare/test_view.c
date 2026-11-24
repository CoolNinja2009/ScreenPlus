/* Pure view filtering checks: no window or GPU is created. */
#include "screenplus_bare.h"
#include <assert.h>

int main(void) {
    const SpBareWallpaper html = { .id = "singularity-1", .title = "Singularity", .kind = "HTML" };
    const SpBareWallpaper image = { .id = "pond-2", .title = "Koi Pond", .kind = "Image" };
    assert(sp_bare_wallpaper_matches(&html, "SING", 0));
    assert(sp_bare_wallpaper_matches(&html, "-1", 3));
    assert(!sp_bare_wallpaper_matches(&html, "pond", 0));
    assert(!sp_bare_wallpaper_matches(&html, "", 1));
    assert(sp_bare_wallpaper_matches(&image, "kOi", 1));
    assert(!sp_bare_wallpaper_matches(&image, "", -1));
    assert(!sp_bare_wallpaper_matches(&image, "", 4));
    assert(!sp_bare_wallpaper_matches(NULL, "", 0));
    return 0;
}
