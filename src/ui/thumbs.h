/* Asynchronous thumbnail loader (one background thread, newest request first).
 * Images/videos: Windows thumbnail cache (IShellItemImageFactory); videos fall back to FFmpeg
 * (first decodable frame near 10%); HTML: preview image in the wallpaper folder. Results are
 * cover-cropped to exactly the requested pixel size to keep GPU memory small.
 * Wallpapers that move get a second, animated result once every still is done: the package's
 * preview GIF (Lively / Wallpaper Engine), an animated GIF wallpaper, or a short video excerpt. */
#pragma once
#include "../settings/config.h"

#define WM_SP_THUMB (WM_APP + 80)

typedef struct ThumbResult {
    wchar_t   id[SP_ID_LEN];
    int       large;
    int       w, h;
    uint8_t  *bgra;     /* NULL: no thumbnail available; otherwise frames * w * h * 4 bytes */
    int       frames;   /* 1: still image */
    uint16_t *delays;   /* frames > 1: display time of each frame in ms */
} ThumbResult;

void thumbs_start(HWND notify);
void thumbs_request(const WallpaperEntry *e, int w, int h, int large);
void thumbs_stop(void);
void thumbs_free(ThumbResult *r);

/* FFmpeg frame grab (renderer/video/ffmpeg_thumb.c): BGRA w x h cover crop, caller frees with free(). */
BOOL ffmpeg_thumbnail(const wchar_t *path, int w, int h, uint8_t **bgra);
/* FFmpeg animation: a whole GIF loop (evenly thinned to maxFrames) or a few seconds of a video.
 * *bgra holds *count frames of w x h; caller frees *bgra and *delays with free(). */
BOOL ffmpeg_animation(const wchar_t *path, int w, int h, int maxFrames, uint8_t **bgra, uint16_t **delays, int *count);
