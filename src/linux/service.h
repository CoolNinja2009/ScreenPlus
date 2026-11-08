#pragma once
#include "../common/common.h"

extern int g_startupLaunch;                /* --startup: the session started us */
int  service_main(void);                  /* `screenplus --daemon`; returns the exit code */
int  cli_main(int argc, char **argv);     /* everything else */
int  sp_autostart_set(int on);            /* ~/.config/autostart/screenplus.desktop */
int  sp_autostart_get(void);
struct Config;
struct WallpaperEntry;
void webhost_start(void);                 /* web wallpapers' folders as local web hosts */
void webhost_sync(const struct Config *c);
char *webhost_url(const struct WallpaperEntry *e);   /* NULL: not a local page, or no host; free() */
char *webhost_recording(const wchar_t *wallpaperId, const char *json);   /* a recording's address for Plasma; free() */
void feed_start(void);                    /* audio, system and track data for pages */
void feed_set_paused(int paused);
void feed_set_audio(int on);              /* the html_audio setting */
void url_import_tick(void);               /* service loop: finishes a running download */
