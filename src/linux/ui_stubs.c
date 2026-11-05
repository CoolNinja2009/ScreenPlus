/* What the window's shared code calls that Linux doesn't have yet: depth wallpapers, the Store and the screensaver.
 * Each answers "not available", and ui_pages.c hides the Store and screensaver sections on Linux. */
#include "../ui/ui_internal.h"
#include "../common/scrnsave.h"

DWORD windows_build_number(void) { return 22000; }
void  update_on_ui_closed(void) {}
void  url_import_cancel(void) {}
void  identify_displays(const MonitorInfo *mons, int n) { (void)mons; (void)n; }

static const DepthModel k_noModel = { L"", L"Not on Linux yet", L"" };
int               depth_model_count(void) { return 0; }
const DepthModel *depth_model(int i) { (void)i; return &k_noModel; }
int               depth_model_find(const wchar_t *key) { (void)key; return -1; }
BOOL              depth_model_ready(int i) { (void)i; return FALSE; }
void              depth_estimate(int model, double seconds, DepthEstimate *out) { (void)model; (void)seconds; memset(out, 0, sizeof *out); }
double            depth_video_seconds(const wchar_t *path) { (void)path; return 0.0; }
int               depth_entry_model(const WallpaperEntry *e) { (void)e; return -1; }
BOOL              depth_ready(void) { return FALSE; }
BOOL              depth_can_cool(const WallpaperEntry *e) { (void)e; return FALSE; }
BOOL              depth_make_file(const wchar_t *image, const wchar_t *out) { (void)image; (void)out; return FALSE; }
BOOL              depth_start(const wchar_t *id) { (void)id; return FALSE; }
void              depth_cancel(void) {}
void              depth_get(DepthState *out) { memset(out, 0, sizeof *out); }
void              depth_revert(const wchar_t *id) { (void)id; }

static StoreState g_store;
const StoreState  *store_get(void) { return &g_store; }
const wchar_t     *store_source_name(int source) { (void)source; return L""; }
int                store_filter_names(int which, const wchar_t **names, int cap) { (void)which; (void)names; (void)cap; return 0; }
void               store_set_filter(int which, int value) { (void)which; (void)value; }
void               store_open(void) {}
BOOL               store_search(const wchar_t *q) { (void)q; return FALSE; }
void               store_more(void) {}
void               store_retry(void) {}
void               store_want(int i, int w, int detail) { (void)i; (void)w; (void)detail; }
BOOL               store_preview_entry(int i, WallpaperEntry *e) { (void)i; (void)e; return FALSE; }
const StoreVariant *store_best(const StoreItem *it) { (void)it; return NULL; }
int                store_dl_state(int i, int *percent) { (void)i; if (percent) *percent = -1; return STOREDL_IDLE; }
void               store_download(int i) { (void)i; }
void               store_page_url(int i, wchar_t *out, size_t cap) { (void)i; if (cap) out[0] = 0; }
void               store_block(int i) { (void)i; }
void               store_unblock_all(void) {}

void scr_query(ScrState *st) { memset(st, 0, sizeof *st); }
BOOL scr_enable(BOOL on, wchar_t *err, size_t cap) { (void)on; if (cap) err[0] = 0; return FALSE; }
BOOL scr_set_timeout(int seconds) { (void)seconds; return FALSE; }
BOOL scr_set_secure(BOOL on) { (void)on; return FALSE; }
BOOL scr_launch(const wchar_t *args) { (void)args; return FALSE; }
