/* Screen+ — shared definitions for every module.
 * Conventions:
 *   - Wide (UTF-16) strings everywhere at API boundaries; UTF-8 only on disk.
 *   - Functions returning BOOL report success; HRESULT is used for COM paths.
 *   - Fixed-capacity buffers are always bounds-checked via the helpers in util.h.
 */
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define SP_APP_NAME     L"Screen+"
#define SP_APP_ID       L"ScreenPlus"
#ifdef SP_VERSION_STR                  /* project version from CMake (the tag, for release builds) */
#define SP_WIDE_(s)     L##s
#define SP_WIDE(s)      SP_WIDE_(s)
#define SP_VERSION_W    SP_WIDE(SP_VERSION_STR)
#else
#define SP_VERSION_W    L"1.0.0"
#endif

#define SP_MAX_PATH     1024   /* long-path aware (manifest), bounded for fixed structs */
#define SP_ID_LEN       17     /* 16 hex digits + NUL */
#define SP_MONID_LEN    160    /* monitor device interface path */
#define SP_NAME_LEN     128
#define SP_MAX_MONITORS 16
#define SP_ERR_LEN      192

#define SP_COUNTOF(a)   (sizeof(a) / sizeof((a)[0]))
#define SP_MIN(a, b)    ((a) < (b) ? (a) : (b))
#define SP_MAX(a, b)    ((a) > (b) ? (a) : (b))
#define SP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

/* Release any C-style COM interface pointer and clear it. */
#define SP_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

typedef enum WallpaperType {
    WP_NONE  = 0,
    WP_IMAGE = 1,
    WP_VIDEO = 2,
    WP_HTML  = 3
} WallpaperType;

/* Runtime states of a wallpaper renderer (one per monitor slot). */
typedef enum RenderState {
    RS_STOPPED    = 0,  /* renderer shut down / safe state */
    RS_ACTIVE     = 1,  /* visible and rendering normally */
    RS_OCCLUDED   = 2,  /* covered by windows: no frame production */
    RS_SUSPENDED  = 3,  /* paused by policy (user, lock, battery, fullscreen, sleep) */
    RS_RECOVERING = 4   /* restarting after a failure */
} RenderState;

const wchar_t *wallpaper_type_name(WallpaperType t);
const wchar_t *render_state_name(RenderState s);
