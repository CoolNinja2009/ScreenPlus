/* Pure view filtering checks: no window or GPU is created. */
#include "screenplus_bare.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const char *text = "{\"wallpaper\":{\"id\":\"singularity-1\",\"name\":\"Singularity\",\"type\":\"html\","
                       "\"favourite\":true,\"collections\":\"test 3;Night\"}}";
    JVal *html = json_parse(text, strlen(text));
    assert(html);
    assert(sp_wallpaper_matches(html, "SING", 0, 0, NULL));
    assert(sp_wallpaper_matches(html, "-1", 3, 0, NULL));
    assert(!sp_wallpaper_matches(html, "pond", 0, 0, NULL));
    assert(!sp_wallpaper_matches(html, "", 1, 0, NULL));
    assert(sp_wallpaper_matches(html, "", 0, 1, NULL));
    assert(sp_wallpaper_matches(html, "", 0, 2, "Night"));
    assert(!sp_wallpaper_matches(html, "", 0, 2, "Nigh"));
    assert(!sp_wallpaper_matches(NULL, "", 0, 0, NULL));
    json_free(html);
    return 0;
}
