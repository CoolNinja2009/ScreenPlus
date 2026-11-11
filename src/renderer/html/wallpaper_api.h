/* Compatibility layer for web wallpapers written for Lively Wallpaper and Wallpaper Engine.
 *
 * Host -> page only (ExecuteScript); pages have no channel back to Screen+.
 *   Lively : livelyPropertyListener(name, value), livelyAudioListener(float[128]),
 *            livelySystemInformation(json), livelyCurrentTrack(json),
 *            livelyWallpaperPlaybackChanged(json)
 *   WE     : wallpaperPropertyListener.applyUserProperties / applyGeneralProperties / setPaused,
 *            wallpaperRegisterAudioListener(cb) (128 bins: 64 left + 64 right)
 * Property values come from the package (LivelyProperties.json / project.json), Lively's own
 * saved customizations (SaveData\wpdata) and Screen+ overrides (<data>\props\<id>.json). */
#pragma once
#include "../../common/common.h"
#include "../../common/json.h"
#include "../../settings/config.h"

/* Script injected into every document before page scripts run. */
const wchar_t *wpapi_shim_js(void);

/* Script handing the page its property values (Lively livelyPropertyListener, WE
 * applyUserProperties); free(). *sent tracks what the page already has: only changed values
 * are sent and *sent is updated (*sent == NULL: a fresh page, everything is sent). NULL when
 * there is nothing to send. */
char *wpapi_properties_js(const WallpaperEntry *wp, JVal **sent);

/* Overrides file for a wallpaper: <data>\props\<id>.json ({"name": value, ...}). */
void wpapi_overrides_path(const wchar_t *wallpaperId, wchar_t *out, size_t cap);

/* ---- Customise panel: author properties + per-wallpaper colour grading. ----
 *
 * The wallpaper's own settings (LivelyProperties.json / WE project.json, including
 * Lively's saved customisations) resolve into editable controls; changed values are
 * stored in the overrides file and applied on the next page load / re-apply pass.
 * Colour grading is stored in the same file under the reserved key
 * "_screenplus_adjust" and consumed by the engine when creating renderers and by
 * the renderers when the panel re-applies. Values never leak to page code: that key
 * matches no known author property name. */

typedef enum CustomType {
    CP_CHECKBOX = 1, CP_SLIDER,
    CP_DROPDOWN,     /* Lively dropdown: value = index into items (strings) */
    CP_COMBO,        /* Wallpaper Engine combo: items = [{"label","value"}], value = an option value */
    CP_FILE,         /* Lively folderDropdown: items = file names found in the folder, value = file name */
    CP_COLOR,        /* "#RRGGBB" (Lively) or "r g b" floats 0..1 (Wallpaper Engine) */
    CP_TEXT, CP_LABEL
} CustomType;

typedef struct CustomProp {
    char       name[80];     /* property key as passed to the wallpaper API */
    wchar_t    label[64];    /* display name; the key when the package gives none */
    CustomType type;
    JVal      *value;        /* resolved current value (override > saved > package default) */
    JVal      *items;        /* CP_DROPDOWN / CP_COMBO / CP_FILE choices (see CustomType) */
    double     min, max, step;
    int        overridden;   /* differs from the package default */
} CustomProp;

CustomProp *wpapi_props_list(const WallpaperEntry *wp, int *count);   /* malloc array, free with wpapi_props_free */
void        wpapi_props_free(CustomProp *list, int count);

/* Deletes every stored override (properties and grading). */
void wpapi_clear_override(const wchar_t *wallpaperId);
/* Drops the property overrides only, so the wallpaper runs on its package/saved values again. */
void wpapi_clear_properties(const wchar_t *wallpaperId);

/* Colour grading. out gets defaults when nothing is stored; out->used is derived (adjust_active). */
BOOL wpapi_load_adjust(const wchar_t *wallpaperId, WallpaperAdjust *out);
void wpapi_save_adjust(const wchar_t *wallpaperId, const WallpaperAdjust *a);
/* Upscaling (Customise, videos and images): UPSCALE_* under "_screenplus_upscale"; not part of the
 * look, so recordings and Reset look leave it alone. Off is today's scaling (bilinear video, cubic images). */
enum { UPSCALE_OFF = 0, UPSCALE_SMOOTH = 1, UPSCALE_SHARP = 2, UPSCALE_REALESRGAN = 3 };
int  wpapi_load_upscale(const wchar_t *wallpaperId);
void wpapi_save_upscale(const wchar_t *wallpaperId, int mode);

/* Stores one override value; native type (checkbox bool, slider/dropdown number,
 * colour/text string). Returns 1 when the file was written. */
int wpapi_save_override(const WallpaperEntry *wp, const char *name, const JVal *value);

/* Lively folderDropdown "Choose a file": copies src into the property's folder in the package (as a PNG when
 * the wallpaper doesn't take its type but takes PNGs; a free name, nothing replaced). outPath: the copy (its file
 * name is the value to store). 0: err says why. */
int wpapi_add_folder_file(const WallpaperEntry *wp, const char *name, const wchar_t *src, wchar_t *outPath, size_t cap,
                          wchar_t *err, size_t ecap);

/* Helper for changing a stored JVal clone: replaces *dst with a deep copy of src. */
void wpapi_jval_copy(JVal **dst, const JVal *src);

/* Typed values to store (native JSON scalars, exactly what the packages use). */
JVal *wpapi_num_value(double v, double step);   /* rounded to the step's decimals */
JVal *wpapi_bool_value(int on);
JVal *wpapi_str_value(const wchar_t *s);

/* Choice properties (CP_DROPDOWN / CP_COMBO / CP_FILE). */
int   wpapi_choice_count(const CustomProp *cp);
int   wpapi_choice_current(const CustomProp *cp);                          /* -1: the value matches no item */
void  wpapi_choice_label(const CustomProp *cp, int i, wchar_t *out, size_t cap);
JVal *wpapi_choice_value(const CustomProp *cp, int i);                     /* value that selects item i */

/* Colour properties: style 0 "#RRGGBB", 1 "0xRRGGBB", 2 Wallpaper Engine "r g b" floats. */
uint32_t wpapi_color_get(const CustomProp *cp, int *style);
JVal    *wpapi_color_value(uint32_t rgb, int style);
int      wpapi_parse_rgb(const wchar_t *s, uint32_t *rgb);                 /* typed "#RRGGBB", "0xRRGGBB" or "RRGGBB" */

/* ---- Recordings (Customise's record button): what the user did to a section's settings, played back on a
 * loop. Stored in props\<id>.json as "_screenplus_rec": {"look": rec, "props": rec, "volume": rec}, each sampled REC_RATE times a
 * second. Playback position comes from the wall clock (epoch = unix ms of sample 0), so the engine, the window's
 * graphs and Plasma's plugin agree without talking. */
enum { REC_LOOK = 0, REC_PROPS = 1, REC_VOLUME = 2, REC_SECTIONS = 3 };
enum { RT_NUM, RT_BOOL, RT_CHOICE, RT_COLOR };   /* interpolated number; on/off; one of opts; 0xRRGGBB per channel */
#define REC_RATE    30
#define REC_MAX_SEC 600

typedef struct RecTrack {
    char   key[80];       /* look: "brightness", "tint"...; props: the property name */
    int    kind;          /* RT_* */
    int    style;         /* RT_COLOR on a property: wpapi_color_value's style */
    double step, lo, hi;  /* RT_NUM: rounding and the graph's range */
    float *v;             /* n samples as recorded (RT_CHOICE: index into opts) */
    float *play;          /* n samples as played: v smoothed (wprec_prepare) */
    JVal  *opts;          /* RT_CHOICE: J_ARR of the values seen */
} RecTrack;

typedef struct Rec {
    int       n;          /* samples; 0: no recording */
    double    speed;      /* 0.25..4 */
    int       smooth;     /* 0..100 */
    double    epoch;      /* unix ms */
    double    paused;     /* unix ms it was paused at (the playhead stays there); 0: playing */
    int       count;
    RecTrack *t;
} Rec;

double    wprec_now(void);                                   /* unix ms */
void      wprec_free(Rec *r);
BOOL      wprec_load(const wchar_t *wallpaperId, int section, Rec *out);
BOOL      wprec_save(const wchar_t *wallpaperId, int section, const Rec *r);   /* r NULL or empty: delete it */
char     *wprec_json(const Rec *r);                          /* one section, smoothed samples, as JSON (Plasma plays it); free() */
void      wprec_prepare(Rec *r);                             /* rebuild play[] after an edit or a smooth change */
double    wprec_pos(const Rec *r, double unixMs);            /* fractional sample index */
double    wprec_num(const RecTrack *t, int n, double pos);   /* RT_NUM/BOOL/CHOICE: value; RT_COLOR: packed rgb */
JVal     *wprec_value(const RecTrack *t, int n, double pos); /* a property's value at pos; json_free */
void      wprec_look(const Rec *r, double pos, WallpaperAdjust *a);   /* overwrites the recorded fields */

/* The recorder: notes each change with the value it had when recording began. */
typedef struct RecTake RecTake;
RecTake  *wprec_take_start(void);
void      wprec_take_note(RecTake *k, const char *key, int kind, int style, double step, double lo, double hi,
                          const JVal *before, const JVal *now);
double    wprec_take_seconds(const RecTake *k);
BOOL      wprec_take_finish(RecTake *k, Rec *out);           /* frees k; FALSE: nothing changed */

/* Live values (engine processes only): playback's look and properties, used instead of the stored ones by
 * wpapi_load_adjust and wpapi_properties_js. Both NULL: back to the stored values. */
void      wpapi_live_set(const wchar_t *wallpaperId, const WallpaperAdjust *adj, const JVal *props);
