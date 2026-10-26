/* yt-dlp (installed by webfetch.h) run as a separate low-priority process, by the shell (importing a
 * link, url_import.c) and by the engine (looking up a stream wallpaper's media address, video_stream.c). */
#pragma once
#include "common.h"

typedef void (*YtLine)(void *ctx, char *line);   /* one output line (UTF-8, no line break) */

/* Runs yt-dlp with the common options plus `args`, passing each output line to cb. Returns its exit
 * code, or -1 when it could not start (err says why) or was stopped: `cancel` signalled or *quit set
 * (either may be NULL). Stopping ends its worker processes and lets its launcher clean up. */
int  ytdlp_run(const wchar_t *args, const wchar_t *workDir, HANDLE cancel, volatile LONG *quit,
               YtLine cb, void *ctx, wchar_t *err, size_t errCap);

/* -f/-S options: one file with video (never merged: there is no ffmpeg.exe), no larger than maxRes
 * (the display's smaller side), AV1 and HDR only as a last resort. stream: prefer a single http(s)
 * file that can be read in byte ranges (YouTube's DASH formats are). */
void ytdlp_format(wchar_t *out, size_t cap, int maxRes, int stream);

/* "[youtube] abc: Private video. Use --cookies ..." -> "Private video." */
void ytdlp_tidy_error(wchar_t *s);

typedef struct YtStream {
    wchar_t url[4096];          /* media address (temporary) */
    wchar_t headers[2048];      /* request headers yt-dlp would send ("Name: value\r\n"...) */
    wchar_t format[32];         /* format id: a fresh address must point at the same file */
    wchar_t protocol[32];
    wchar_t title[SP_NAME_LEN];
    wchar_t thumb[1024];        /* preview image URL */
    wchar_t error[300];
    int64_t size;               /* bytes, -1 unknown */
    int64_t expires;            /* unix seconds, 0 unknown */
    double  duration;           /* seconds, 0 unknown */
    int     streamable;         /* a single http(s) file */
    int     live;               /* a live broadcast: no end, no file */
    int     unsupported;        /* yt-dlp has no extractor for the page */
} YtStream;

/* Looks a page up without downloading anything. formatId: that format again (NULL: choose one). */
BOOL ytdlp_resolve(const wchar_t *page, int maxRes, const wchar_t *formatId, HANDLE cancel, volatile LONG *quit,
                   YtStream *out);

/* cache\streams\<hash><ext>; ".url" keeps the last address until shortly before it expires */
void ytdlp_cache_path(const wchar_t *page, const wchar_t *ext, wchar_t *out, size_t cap);
BOOL ytdlp_cache_load(const wchar_t *page, YtStream *out);
void ytdlp_cache_save(const wchar_t *page, const YtStream *s);
