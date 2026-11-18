#pragma once
#include "../settings/config.h"

/* Both formats are ZIP archives. An SPF contains one wallpaper; an SPP contains SPF members. */
BOOL spf_export(const WallpaperEntry *entry, const wchar_t *path);
BOOL spp_export(const wchar_t *collection, const wchar_t *path);
int  sp_format_import(const wchar_t *path, const wchar_t *collection);
BOOL sp_format_save_dialog(HWND owner, const wchar_t *suggested, const wchar_t *extension, wchar_t *path, size_t cap);
