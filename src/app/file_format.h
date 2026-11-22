#pragma once
#include "../settings/config.h"

/* Both formats are ZIP archives. An SPF contains one wallpaper; an SPP contains SPF members. */
BOOL spf_export(const WallpaperEntry *entry, const wchar_t *path);
BOOL spp_export(const wchar_t *collection, const wchar_t *path);
BOOL spp_export_start(const wchar_t *collection, const wchar_t *path); /* GUI: package on a worker */
/* GUI: package on a worker; paths gives each item's file, or NULL names them after the wallpapers in directory */
BOOL spf_export_start(const WallpaperEntry *items, int count, const wchar_t *const *paths, const wchar_t *directory);
void sp_format_file_name(const wchar_t *name, const wchar_t *extension, wchar_t *out, size_t cap); /* "Name.spf", safe */
void sp_format_unique_path(const wchar_t *dir, const wchar_t *file, wchar_t *out, size_t cap);    /* adds " (2)" */
void sp_export_on_event(WPARAM wp, LPARAM lp);               /* WM_SP_FORMAT, shell thread */
int  sp_format_import(const wchar_t *path, const wchar_t *collection);
BOOL sp_format_save_dialog(HWND owner, const wchar_t *suggested, const wchar_t *extension, wchar_t *path, size_t cap);

/* Preview: what an archive holds, unpacked but not in the library yet. */
typedef struct FormatItem {
    WallpaperEntry e;            /* name, type, flags; path/thumb point into stage (or the link) */
    wchar_t  stage[SP_MAX_PATH]; /* where it is unpacked; "" once imported */
    int      ok, link, depthModel;   /* depthModel: -1 not depth, -2 unknown model */
    int64_t  bytes;              /* the media (an HTML wallpaper's whole folder) */
    int      upscale, lookCount, propCount, recTracks[3];
    float    recSec[3];          /* recordings: look, properties, volume */
    wchar_t  looks[160], props[160], recKeys[3][160];
} FormatItem;

typedef struct FormatPreview {
    wchar_t     file[SP_MAX_PATH], stage[SP_MAX_PATH], name[SP_NAME_LEN];
    int         playlist, minutes, count, ok;   /* ok: how many items can be added */
    int64_t     bytes;
    FormatItem *items;
} FormatPreview;

typedef struct FormatPreviewJob {
    wchar_t        file[SP_MAX_PATH];
    int64_t        bytes;
    FormatPreview *preview;      /* valid once ready */
    volatile LONG  ready, closed, refs;
} FormatPreviewJob;

FormatPreview    *sp_preview_load(const wchar_t *file);        /* blocking (CLI); free with sp_preview_free */
void              sp_preview_free(FormatPreview *p);
int               sp_preview_import(FormatPreview *p);          /* adds the items; returns how many */
int               sp_preview_rows(const FormatItem *it, wchar_t rows[][2][200], int max);   /* label, value */
FormatPreviewJob *sp_preview_start(const wchar_t *file);        /* GUI: loads on a worker, WM_SP_FORMAT 2 when ready */
void              sp_preview_close(FormatPreviewJob *job, BOOL import);
