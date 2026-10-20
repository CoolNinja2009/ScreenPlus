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
#include "../../settings/config.h"

/* Script injected into every document before page scripts run. */
const wchar_t *wpapi_shim_js(void);

/* Script applying the wallpaper's current property values (NULL when it has none). free(). */
char *wpapi_properties_js(const WallpaperEntry *wp);

/* Overrides file for a wallpaper: <data>\props\<id>.json ({"name": value, ...}). */
void wpapi_overrides_path(const wchar_t *wallpaperId, wchar_t *out, size_t cap);
