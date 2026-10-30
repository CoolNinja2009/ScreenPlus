/* "Show a still while Screen+ starts": a still of each display's wallpaper becomes the Windows
 * desktop wallpaper, so Windows itself shows the right picture the moment the desktop appears at
 * sign-in, whatever holds Screen+ back (other startup programs, software injected into it), until
 * the engine's first frame covers it. Engine only. */
#pragma once
#include "../common/common.h"
#include "../settings/config.h"
#include "../monitor/monitor.h"

/* The still this display/wallpaper/look/size gets (cache\stills): it exists once it was made and set. */
BOOL stills_path(const MonitorInfo *mon, const WallpaperEntry *wp, wchar_t *out, size_t cap);
/* Makes it on a background thread and sets it: jpeg is a page html_grab took, NULL makes it from the file. */
void stills_make(const MonitorInfo *mon, const WallpaperEntry *wp, IStream *jpeg);
/* The setting went off: the user's own Windows wallpaper comes back. */
void stills_restore(void);
