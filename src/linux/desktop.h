/* What the Linux service needs from the desktop it runs on. Each desktop puts the wallpaper in its own
 * way: Plasma through our wallpaper plugin (desktop_kde.c), Hyprland through screenplus-engine on a
 * layer-shell background surface (desktop_hypr.c, engine_gtk.c). GNOME is not supported yet. */
#pragma once
#include "../common/common.h"
#include "../settings/config.h"
#include "../monitor/monitor.h"

typedef enum DesktopKind { DESK_UNKNOWN = 0, DESK_KDE, DESK_HYPRLAND } DesktopKind;

DesktopKind desktop_detect(void);
const char *desktop_name(DesktopKind k);

/* run.c */
#define SP_SOCKET_NAME "screenplus.sock"
int  sp_socket_path(char *out, size_t cap);
/* Runs argv[0] from PATH, waits, returns its exit code (-1: could not run). out: its stdout, may be NULL. */
int  sp_run(const char *const *argv, char *out, size_t cap);
struct WallpaperEntry;
void kde_page_args(const struct WallpaperEntry *e, char **url, char **shim, char **script);   /* free() each */
int  sp_keep_port(const wchar_t *path, char *token, size_t cap);
void sp_save_port(const wchar_t *path, int port, const char *token);
void sp_fps_js(int fps, char *out, size_t cap);
int  sp_run_read(const char *const *argv, char *out, size_t cap, size_t *got);   /* *got: bytes of output kept */
int  sp_on_path(const char *name);
int  sp_on_battery(void);                  /* mains exists and is unplugged */
char *sp_file_uri(const wchar_t *path);    /* file:///... percent-encoded; http(s) unchanged; free() */

/* desktop_kde.c */
int  kde_monitors(MonitorInfo *out, int max);
int  kde_install_plugin(void);
int  kde_apply(const Config *c, const MonitorInfo *mons, int monCount, int paused);   /* 0: Plasma refused */
void kde_restore(void);

/* desktop_hypr.c */
char *hypr_query(const char *request);     /* the reply to a socket request ("j/monitors"); free(); NULL: none */
int  hypr_monitors(MonitorInfo *out, int max);
int  hypr_fullscreen_on(const char *monitorName);   /* the workspace showing there has a fullscreen window */
