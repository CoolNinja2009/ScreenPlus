#pragma once
#include "../settings/config.h"

/* Both formats are ZIP archives. An SPF contains one wallpaper; an SPP contains SPF members. */
BOOL spf_export(const WallpaperEntry *entry, const wchar_t *path);
BOOL spp_export(const wchar_t *collection, const wchar_t *path);
BOOL spp_export_start(const wchar_t *collection, const wchar_t *path); /* GUI: package on a worker */
BOOL spf_export_many_start(const WallpaperEntry *items, int count, const wchar_t *directory);
void sp_export_on_event(WPARAM wp, LPARAM lp);               /* WM_SP_FORMAT, shell thread */
int  sp_format_import(const wchar_t *path, const wchar_t *collection);
BOOL sp_format_save_dialog(HWND owner, const wchar_t *suggested, const wchar_t *extension, wchar_t *path, size_t cap);
