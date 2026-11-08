/* What the window's shared code calls that Linux doesn't have: the screensaver answers "not available", and
 * ui_pages.c hides its section. depth_choose_model is here because the file dialog is the window's. */
#include "../ui/ui_internal.h"
#include "../common/scrnsave.h"
#include "desktop.h"

DWORD windows_build_number(void) { return 22000; }

/* Each display's number, as a notification: Plasma and Hyprland both show them. */
void identify_displays(const MonitorInfo *mons, int n)
{
    for (int i = 0; i < n; i++) {
        char text[160];
        snprintf(text, sizeof text, "Display %d: %ldx%ld at %ld,%ld", i + 1, (long)(mons[i].rect.right - mons[i].rect.left),
                 (long)(mons[i].rect.bottom - mons[i].rect.top), (long)mons[i].rect.left, (long)mons[i].rect.top);
        const char *argv[] = { "notify-send", "-a", "Screen+", "-t", "5000", text, NULL };
        sp_run(argv, NULL, 0);
    }
}

int sp_pick_colour(uint32_t *rgb)
{
    char now[16], out[64] = "";
    snprintf(now, sizeof now, "#%06X", *rgb & 0xFFFFFF);
    const char *kde[] = { "kdialog", "--getcolor", "--default", now, NULL };
    const char *gtk[] = { "zenity", "--color-selection", "--color", now, NULL };
    if (sp_run(kde, out, sizeof out) && sp_run(gtk, out, sizeof out)) return 0;
    unsigned r, g, b;
    if (sscanf(out, "#%2x%2x%2x", &r, &g, &b) == 3 || sscanf(out, "rgb(%u,%u,%u)", &r, &g, &b) == 3) {
        *rgb = r << 16 | g << 8 | b;
        return 1;
    }
    return 0;
}

BOOL depth_choose_model(int i, HWND owner)
{
    wchar_t **paths = NULL;
    (void)owner;
    int n = sp_pick_paths(0, &paths);
    BOOL ok = n > 0 && depth_model_has_weights(paths[0]) && depth_set_model_file(i, paths[0]);
    for (int k = 0; k < n; k++) free(paths[k]);
    free(paths);
    return ok;
}

void scr_query(ScrState *st) { memset(st, 0, sizeof *st); }
BOOL scr_enable(BOOL on, wchar_t *err, size_t cap) { (void)on; if (cap) err[0] = 0; return FALSE; }
BOOL scr_set_timeout(int seconds) { (void)seconds; return FALSE; }
BOOL scr_set_secure(BOOL on) { (void)on; return FALSE; }
BOOL scr_launch(const wchar_t *args) { (void)args; return FALSE; }
