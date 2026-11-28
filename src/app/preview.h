/* Recorded previews for HTML wallpapers that ship none, and for app wallpapers (.exe), like Lively's lively_t.jpg / lively_p.gif:
 * the wallpaper runs for a few seconds in an off-screen window of a helper process
 * (ScreenPlus.exe --capture-preview <id>) and is saved as <cache>\previews\<id>.jpg, plus
 * <id>.gif when the page moves. */
#pragma once
#include "../common/common.h"
#include "../settings/config.h"

#define WM_SP_PREVIEW (WM_APP + 81)   /* a preview was recorded: lParam = malloc'ed wallpaper id (receiver frees) */

void preview_paths(const wchar_t *id, wchar_t *gif, wchar_t *jpg, size_t cap);
/* Queues a recording (thread-safe; at most once per wallpaper per session, one at a time).
 * notify receives WM_SP_PREVIEW when it succeeded. */
void preview_request(const wchar_t *id, HWND notify);
/* An app wallpaper's recording (programs ship no moving preview): 2 a GIF, 1 only the still (it doesn't move),
 * 0 none. One older than the program (its update, or a page this sample used to be) is deleted: it is recorded again. */
int  preview_app_recorded(const wchar_t *id, const wchar_t *program);
/* At start: app recordings older than their program go, and all of them when the recorder changed (cached until then). */
void preview_app_upkeep(const WallpaperEntry *items, int count);
/* The helper process. Returns 0 when the preview files were written. */
int  preview_capture_main(const wchar_t *id);
