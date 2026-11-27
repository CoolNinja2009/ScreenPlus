#pragma once
#include "common.h"

/* Application directories (created on demand by paths_init):
 *   %LOCALAPPDATA%\ScreenPlus\            root
 *       config.ini (+ config.bak)          settings and library
 *       logs\  logs\crash\                 bounded logs, minidumps
 *       cache\                             thumbnails / previews
 *       downloads\                         wallpapers imported from web links (deleted with their entry)
 *       tools\                             yt-dlp + QuickJS-NG when URL import is enabled (webfetch.h)
 *       WebView2\                          WebView2 user data folder
 */
void paths_init(void);
const wchar_t *paths_root(void);
const wchar_t *paths_config(void);
const wchar_t *paths_config_backup(void);
const wchar_t *paths_logs(void);
const wchar_t *paths_crash(void);
const wchar_t *paths_cache(void);
const wchar_t *paths_webview(void);
const wchar_t *paths_downloads(void);   /* created when something is downloaded */
const wchar_t *paths_exe(void);
const wchar_t *paths_exe_dir(void);
/* Microsoft Store (MSIX) copy: the data root is the package's own LocalCache\Local\ScreenPlus (a real path, so
 * Explorer can read the stills; plain %LOCALAPPDATA% writes would go to a private copy anyway), and programs
 * outside the package start the app through its execution alias, not the WindowsApps path. */
BOOL paths_packaged(void);
const wchar_t *paths_launch_exe(void);  /* the alias when packaged, else paths_exe() */
