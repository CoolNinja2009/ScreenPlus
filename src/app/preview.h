/* Recorded previews for HTML wallpapers that ship none, like Lively's lively_t.jpg / lively_p.gif:
 * the wallpaper runs for a few seconds in an off-screen window of a helper process
 * (ScreenPlus.exe --capture-preview <id>) and is saved as <cache>\previews\<id>.jpg, plus
 * <id>.gif when the page moves. */
#pragma once
#include "../common/common.h"

#define WM_SP_PREVIEW (WM_APP + 81)   /* a preview was recorded: lParam = malloc'ed wallpaper id (receiver frees) */

void preview_paths(const wchar_t *id, wchar_t *gif, wchar_t *jpg, size_t cap);
/* Queues a recording (thread-safe; at most once per wallpaper per session, one at a time).
 * notify receives WM_SP_PREVIEW when it succeeded. */
void preview_request(const wchar_t *id, HWND notify);
/* The helper process. Returns 0 when the preview files were written. */
int  preview_capture_main(const wchar_t *id);
