/* Compatibility layer for web wallpapers written for Lively Wallpaper and Wallpaper Engine.
 *
 * Host -> page only (ExecuteScript); pages have no channel back to Screen+.
 *   Lively : livelyPropertyListener(name, value), livelyAudioListener(float[128]),
 *            livelySystemInformation(json), livelyCurrentTrack(json),
 *            livelyWallpaperPlaybackChanged(json)
 *   WE     : wallpaperPropertyListener.applyUserProperties / applyGeneralProperties / setPaused,
 *            wallpaperRegisterAudioListener(cb) (128 bins: 64 left + 64 right)
 * Property values come from the package (LivelyProperties.json / project.json), Lively's own
 * saved customizations (SaveData\wpdata) and Screen+ overrides (<data>\props\<id>.json). */
#pragma once
#include "../../common/common.h"
#include "../../common/json.h"
#include "../../settings/config.h"

/* Script injected into every document before page scripts run. */
const wchar_t *wpapi_shim_js(void);

/* Script handing the page its property values (Lively livelyPropertyListener, WE
 * applyUserProperties); free(). *sent tracks what the page already has: only changed values
 * are sent and *sent is updated (*sent == NULL: a fresh page, everything is sent). NULL when
 * there is nothing to send. */
char *wpapi_properties_js(const WallpaperEntry *wp, JVal **sent);

/* Overrides file for a wallpaper: <data>\props\<id>.json ({"name": value, ...}). */
void wpapi_overrides_path(const wchar_t *wallpaperId, wchar_t *out, size_t cap);

/* ---- Customise panel: author properties + per-wallpaper colour grading. ----
 *
 * The wallpaper's own settings (LivelyProperties.json / WE project.json, including
 * Lively's saved customisations) resolve into editable controls; changed values are
 * stored in the overrides file and applied on the next page load / re-apply pass.
 * Colour grading is stored in the same file under the reserved key
 * "_screenplus_adjust" and consumed by the engine when creating renderers and by
 * the renderers when the panel re-applies. Values never leak to page code: that key
 * matches no known author property name. */

typedef enum CustomType {
    CP_CHECKBOX = 1, CP_SLIDER,
    CP_DROPDOWN,     /* Lively dropdown: value = index into items (strings) */
    CP_COMBO,        /* Wallpaper Engine combo: items = [{"label","value"}], value = an option value */
    CP_FILE,         /* Lively folderDropdown: items = file names found in the folder, value = file name */
    CP_COLOR,        /* "#RRGGBB" (Lively) or "r g b" floats 0..1 (Wallpaper Engine) */
    CP_TEXT, CP_LABEL
} CustomType;

typedef struct CustomProp {
    char       name[80];     /* property key as passed to the wallpaper API */
    wchar_t    label[64];    /* display name; the key when the package gives none */
    CustomType type;
    JVal      *value;        /* resolved current value (override > saved > package default) */
    JVal      *items;        /* CP_DROPDOWN / CP_COMBO / CP_FILE choices (see CustomType) */
    double     min, max, step;
    int        overridden;   /* differs from the package default */
} CustomProp;

CustomProp *wpapi_props_list(const WallpaperEntry *wp, int *count);   /* malloc array, free with wpapi_props_free */
void        wpapi_props_free(CustomProp *list, int count);

/* Deletes every stored override (properties and grading). */
void wpapi_clear_override(const wchar_t *wallpaperId);
/* Drops the property overrides only, so the wallpaper runs on its package/saved values again. */
void wpapi_clear_properties(const wchar_t *wallpaperId);

/* Colour grading. out gets defaults when nothing is stored; out->used is derived (adjust_active). */
BOOL wpapi_load_adjust(const wchar_t *wallpaperId, WallpaperAdjust *out);
void wpapi_save_adjust(const wchar_t *wallpaperId, const WallpaperAdjust *a);

/* Stores one override value; native type (checkbox bool, slider/dropdown number,
 * colour/text string). Returns 1 when the file was written. */
int wpapi_save_override(const WallpaperEntry *wp, const char *name, const JVal *value);

/* Helper for changing a stored JVal clone: replaces *dst with a deep copy of src. */
void wpapi_jval_copy(JVal **dst, const JVal *src);
