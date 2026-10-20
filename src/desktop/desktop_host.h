/* Hosting wallpaper windows behind the desktop icons.
 *
 * Two shell layouts are supported and detected at runtime (never assumed):
 *   HOSTMODE_PROGMAN  Windows 11 24H2+ : Progman owns SHELLDLL_DefView (icons) and a WorkerW
 *                     child that draws the system wallpaper. Our window becomes a layered child
 *                     of Progman, z-ordered directly below DefView (above the system wallpaper).
 *   HOSTMODE_WORKERW  Windows 10 / 11 before 24H2 : message 0x052C makes Progman spawn a top-level
 *                     WorkerW behind the DefView host; our windows become children of it.
 *   HOSTMODE_FALLBACK No shell desktop (Explorer not running): top-level windows pinned to the
 *                     bottom of the z-order until the shell returns (TaskbarCreated).
 * Explorer is never restarted or terminated by Screen+. */
#pragma once
#include "../common/common.h"

typedef struct DesktopHost {
    int   mode;          /* HOSTMODE_* from ipc.h */
    HWND  progman;
    HWND  defView;
    HWND  workerw;       /* system wallpaper WorkerW (24H2) or our parent (legacy) */
    HWND  parent;        /* window our wallpaper windows are parented to (NULL in fallback) */
    DWORD shellPid;
} DesktopHost;

BOOL desktop_host_discover(DesktopHost *h);
BOOL desktop_host_valid(const DesktopHost *h);
/* Re-parents a hidden popup window into the desktop host and positions it on screenRect. */
BOOL desktop_host_attach(const DesktopHost *h, HWND w, const RECT *screenRect, BOOL layered);
void desktop_host_position(const DesktopHost *h, HWND w, const RECT *screenRect);
void desktop_host_refresh(const DesktopHost *h);
const wchar_t *desktop_host_mode_name(int mode);
