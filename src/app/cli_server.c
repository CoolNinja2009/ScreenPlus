/* Terminal interface, app side. ScreenPlus.com (src/cli/cli_main.c) sends one IPC_CLI_REQUEST
 * per command: UTF-8 JSON {"argv":["apply","Rain","--display","1"]}, with its reply window in
 * wParam. The command runs here, on the shell's UI thread, through the same functions the
 * management window uses, and the answer goes back as IPC_CLI_REPLY before the request's
 * SendMessage returns: {"ok":bool,"text":"<human-readable output>","data":<command JSON>}.
 * Paths in the request are already absolute (the CLI resolves them against its own folder). */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../renderer/html/wallpaper_api.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define MAX_ARGS 64

typedef struct Req {
    wchar_t *argv[MAX_ARGS];   /* positional arguments, argv[0] = command */
    int      argc;
    wchar_t *flags[16];        /* other --switches, e.g. --apply */
    int      flagCount;
    int      display;          /* --display N (1-based), 0 = not given */
} Req;

typedef struct Reply {
    int    ok;
    int    exitAfter;          /* "stop": quit once the reply is out */
    StrBuf text;               /* human-readable output, UTF-8 */
    StrBuf data;               /* one JSON value, empty = null */
} Reply;

/* ------------------------------------------------------------------ output helpers */

static void say(Reply *r, const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, SP_COUNTOF(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    sb_put_wide(&r->text, buf);
}

static void fail(Reply *r, const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, SP_COUNTOF(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    r->ok = 0;
    r->text.len = 0;           /* an error replaces any partial output */
    if (r->text.p) r->text.p[0] = 0;
    sb_put_wide(&r->text, buf);
    sb_puts(&r->text, "\n");
}

static void jstr(StrBuf *b, const wchar_t *w)
{
    char *u = utf8_from_wide(w ? w : L"", NULL);
    json_write_string(b, u ? u : "");
    free(u);
}

static const wchar_t *type_key(WallpaperType t)
{
    return t == WP_IMAGE ? L"image" : t == WP_VIDEO ? L"video" : t == WP_HTML ? L"html" : L"none";
}

static const wchar_t *format_key(int f)
{
    return f == WPF_LIVELY ? L"lively" : f == WPF_WE ? L"wallpaper-engine" : L"plain";
}

static void j_wallpaper(StrBuf *b, const WallpaperEntry *e)
{
    if (!e) {
        sb_puts(b, "null");
        return;
    }
    sb_puts(b, "{\"id\":");
    jstr(b, e->id);
    sb_puts(b, ",\"name\":");
    jstr(b, e->name);
    sb_puts(b, ",\"type\":");
    jstr(b, type_key(e->type));
    sb_puts(b, ",\"format\":");
    jstr(b, format_key(e->format));
    sb_puts(b, ",\"path\":");
    jstr(b, e->path);
    sb_printf(b, ",\"missing\":%s}", e->missing ? "true" : "false");
}

static int has_flag(const Req *q, const wchar_t *flag)
{
    for (int i = 0; i < q->flagCount; i++)
        if (str_ieq(q->flags[i], flag)) return 1;
    return 0;
}

/* argv[from..] joined with spaces: names may be typed without quotes */
static void join_args(const Req *q, int from, wchar_t *out, size_t cap)
{
    out[0] = 0;
    for (int i = from; i < q->argc; i++) {
        if (i > from) str_cat(out, cap, L" ");
        str_cat(out, cap, q->argv[i]);
    }
}

/* ------------------------------------------------------------------ lookups */

static int resolve_display(Reply *r, const Req *q, const MonitorInfo **out)
{
    *out = NULL;
    if (!q->display) return 1;
    for (int i = 0; i < g_app.monCount; i++)
        if (g_app.mons[i].index == q->display) {
            *out = &g_app.mons[i];
            return 1;
        }
    fail(r, L"There is no display %d (this PC has %d). See: screenplus displays", q->display, g_app.monCount);
    return 0;
}

static int wallpaper_on_screen(const wchar_t *id)
{
    for (int i = 0; i < g_app.monCount; i++)
        if (str_ieq(app_wallpaper_for(g_app.mons[i].id), id)) return 1;
    return !g_app.monCount && str_ieq(app_wallpaper_for(NULL), id);
}

/* id, exact name, file path, number from `list`, or a unique part of the name */
static WallpaperEntry *find_wallpaper(Reply *r, const wchar_t *q)
{
    Config *c = &g_app.cfg;
    if (!q || !q[0]) {
        fail(r, L"Name a wallpaper (see: screenplus list).");
        return NULL;
    }
    WallpaperEntry *e = config_find(c, q);
    if (e) return e;
    for (int i = 0; i < c->count; i++)
        if (str_ieq(c->items[i].name, q)) return &c->items[i];
    if ((e = config_find_path(c, q))) return e;
    wchar_t *end = NULL;
    long n = wcstol(q, &end, 10);
    if (end && !*end && n >= 1 && n <= c->count) return &c->items[n - 1];
    int hits = 0;
    WallpaperEntry *hit = NULL;
    for (int i = 0; i < c->count; i++)
        if (str_icontains(c->items[i].name, q)) {
            hits++;
            hit = &c->items[i];
        }
    if (hits == 1) return hit;
    if (!hits) {
        fail(r, L"No wallpaper matches \"%ls\". See: screenplus list", q);
        return NULL;
    }
    fail(r, L"\"%ls\" matches %d wallpapers, be more specific:", q, hits);
    for (int i = 0; i < c->count; i++)
        if (str_icontains(c->items[i].name, q)) say(r, L"  %3d  %ls\n", i + 1, c->items[i].name);
    return NULL;
}

/* the wallpaper a look/props command edits: the one on --display N, else the default */
static WallpaperEntry *target_wallpaper(Reply *r, const Req *q, const MonitorInfo **mon)
{
    if (!resolve_display(r, q, mon)) return NULL;
    WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(*mon ? (*mon)->id : NULL));
    if (!e) fail(r, L"No wallpaper is applied%ls. Apply one first: screenplus apply <wallpaper>",
                 *mon ? L" on that display" : L"");
    return e;
}

static int parse_on_off(const wchar_t *v, int current, int *out)
{
    if (!v) return 0;
    if (str_ieq(v, L"on") || str_ieq(v, L"true") || str_ieq(v, L"1") || str_ieq(v, L"yes")) *out = 1;
    else if (str_ieq(v, L"off") || str_ieq(v, L"false") || str_ieq(v, L"0") || str_ieq(v, L"no")) *out = 0;
    else if (str_ieq(v, L"toggle")) *out = !current;
    else return 0;
    return 1;
}

static void customise_changed(void)
{
    app_apply_customise();
    ui_on_customise_changed();
}

/* ------------------------------------------------------------------ status, list, displays */

static const wchar_t *host_mode_name(int m)
{
    switch (m) {
    case HOSTMODE_WORKERW:  return L"WorkerW";
    case HOSTMODE_PROGMAN:  return L"Progman (24H2+)";
    case HOSTMODE_FALLBACK: return L"fallback window";
    default:                return L"none";
    }
}

static void cmd_status(Reply *r)
{
    int sev = 0;
    const wchar_t *state = app_engine_state_text(&sev);
    const IpcEngineStatus *st = &g_app.status;
    int have = g_app.haveStatus;
    supervisor_sample_stats();
    say(r, L"Screen+ %ls - %ls\n", SP_VERSION_W, state);
    if (g_app.engineProcess)
        say(r, L"Renderer   pid %lu, %ls host, WebView2 %ls, %d of %d display(s) rendering%ls\n", g_app.enginePid,
            have ? host_mode_name(st->hostMode) : L"(starting)", have && st->webviewVersion[0] ? st->webviewVersion : L"n/a",
            have ? st->activeRenderers : 0, g_app.monCount, g_app.safeMode ? L", SAFE MODE" : L"");
    else
        say(r, L"Renderer   not running%ls\n", g_app.engineGaveUp ? L" (gave up after repeated failures: screenplus restart)" : L"");
    wchar_t cond[256] = L"";
    if (g_app.cfg.s.paused) str_cat(cond, SP_COUNTOF(cond), L"paused by you, ");
    if (have && (st->flags & ENGINE_FLAG_FULLSCREEN)) str_cat(cond, SP_COUNTOF(cond), L"fullscreen app, ");
    if (have && (st->flags & ENGINE_FLAG_BATTERY)) str_cat(cond, SP_COUNTOF(cond), L"on battery, ");
    if (have && (st->flags & ENGINE_FLAG_LOCKED)) str_cat(cond, SP_COUNTOF(cond), L"session locked, ");
    if (have && (st->flags & ENGINE_FLAG_DISPLAY_OFF)) str_cat(cond, SP_COUNTOF(cond), L"display off, ");
    if (have && (st->flags & ENGINE_FLAG_SLEEPING)) str_cat(cond, SP_COUNTOF(cond), L"sleeping, ");
    size_t cl = wcslen(cond);
    if (cl > 2) {
        cond[cl - 2] = 0;
        say(r, L"Conditions %ls\n", cond);
    }
    say(r, L"Memory     app %.0f MB, renderer %.0f MB in %d process(es)\n", g_app.shellMem / 1048576.0,
        g_app.engineMem / 1048576.0, g_app.engineProcs);
    if (have && st->lastError[0]) say(r, L"Last error %ls\n", st->lastError);
    if (g_app.notice[0]) say(r, L"Notice     %ls\n", g_app.notice);

    StrBuf *b = &r->data;
    sb_puts(b, "{\"version\":");
    jstr(b, SP_VERSION_W);
    sb_puts(b, ",\"state\":");
    jstr(b, state);
    sb_printf(b, ",\"severity\":%d,\"paused\":%s,\"audio\":%s,\"notice\":", sev, g_app.cfg.s.paused ? "true" : "false",
              g_app.cfg.s.audio ? "true" : "false");
    jstr(b, g_app.notice);
    sb_printf(b, ",\"renderer\":{\"running\":%s,\"pid\":%lu,\"safeMode\":%s,\"gaveUp\":%s,\"restarts\":%d,\"host\":",
              g_app.engineProcess ? "true" : "false", g_app.enginePid, g_app.safeMode ? "true" : "false",
              g_app.engineGaveUp ? "true" : "false", g_app.engineRestarts);
    jstr(b, have ? host_mode_name(st->hostMode) : L"");
    sb_puts(b, ",\"webview2\":");
    jstr(b, have ? st->webviewVersion : L"");
    sb_printf(b, ",\"rendering\":%d,\"flags\":%u,\"lastError\":", have ? st->activeRenderers : 0, have ? st->flags : 0u);
    jstr(b, have ? st->lastError : L"");
    sb_printf(b, "},\"memoryMB\":{\"app\":%.1f,\"renderer\":%.1f,\"rendererProcesses\":%d},\"displays\":[",
              g_app.shellMem / 1048576.0, g_app.engineMem / 1048576.0, g_app.engineProcs);
    for (int i = 0; i < g_app.monCount; i++) {
        const MonitorInfo *m = &g_app.mons[i];
        const IpcMonitorStatus *ms = app_monitor_status(m->id);
        WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(m->id));
        const wchar_t *rs = ms ? render_state_name((RenderState)ms->state) : L"Unknown";
        say(r, L"Display %d  %ls, %ldx%ld @ %u Hz%ls\n", m->index, m->name, m->rect.right - m->rect.left,
            m->rect.bottom - m->rect.top, m->refreshHz, m->primary ? L", primary" : L"");
        say(r, L"           %ls%ls%ls%ls - %ls%ls%ls\n", e ? e->name : L"(no wallpaper)", e ? L" [" : L"",
            e ? type_key(e->type) : L"", e ? L"]" : L"", rs, ms && ms->covered ? L", covered" : L"",
            ms && ms->failed ? L", FAILED (safe state)" : L"");
        /* the field carries the renderer's description while it runs, the reason when it does not */
        int isErr = ms && ms->error[0] && (ms->failed || ms->state == RS_STOPPED || ms->state == RS_RECOVERING);
        if (ms && ms->error[0]) say(r, isErr ? L"           error: %ls\n" : L"           renderer: %ls\n", ms->error);
        if (i) sb_puts(b, ",");
        sb_printf(b, "{\"index\":%d,\"name\":", m->index);
        jstr(b, m->name);
        sb_puts(b, ",\"id\":");
        jstr(b, m->id);
        sb_printf(b, ",\"width\":%ld,\"height\":%ld,\"hz\":%u,\"primary\":%s,\"state\":", m->rect.right - m->rect.left,
                  m->rect.bottom - m->rect.top, m->refreshHz, m->primary ? "true" : "false");
        jstr(b, rs);
        sb_printf(b, ",\"covered\":%s,\"failed\":%s,\"failures\":%d,\"error\":", ms && ms->covered ? "true" : "false",
                  ms && ms->failed ? "true" : "false", ms ? ms->failures : 0);
        jstr(b, isErr ? ms->error : L"");
        sb_puts(b, ",\"renderer\":");
        jstr(b, ms && !isErr ? ms->error : L"");
        sb_puts(b, ",\"wallpaper\":");
        j_wallpaper(b, e);
        sb_puts(b, "}");
    }
    sb_puts(b, "]}");
}

static int list_match(const WallpaperEntry *e, const wchar_t *f, int onScreen)
{
    if (str_ieq(f, L"image") || str_ieq(f, L"images")) return e->type == WP_IMAGE;
    if (str_ieq(f, L"video") || str_ieq(f, L"videos")) return e->type == WP_VIDEO;
    if (str_ieq(f, L"html") || str_ieq(f, L"web")) return e->type == WP_HTML;
    if (str_ieq(f, L"lively")) return e->format == WPF_LIVELY;
    if (str_ieq(f, L"we")) return e->format == WPF_WE;
    if (str_ieq(f, L"missing")) return e->missing;
    if (str_ieq(f, L"active")) return onScreen;
    return str_icontains(e->name, f);
}

static void cmd_list(Reply *r, const Req *q)
{
    Config *c = &g_app.cfg;
    wchar_t filter[256];
    join_args(q, 1, filter, SP_COUNTOF(filter));
    int shown = 0, anyOn = 0;
    sb_puts(&r->data, "[");
    for (int i = 0; i < c->count; i++) {
        WallpaperEntry *e = &c->items[i];
        int on = wallpaper_on_screen(e->id);
        if (filter[0] && !list_match(e, filter, on)) continue;
        if (!shown) say(r, L"     #  TYPE   NAME                                      ID\n");
        say(r, L"  %lc %3d  %-5ls  %-40.40ls  %ls%ls\n", on ? L'*' : L' ', i + 1, type_key(e->type), e->name, e->id,
            e->missing ? L"  (file missing)" : L"");
        anyOn |= on;
        if (shown) sb_puts(&r->data, ",");
        sb_printf(&r->data, "{\"number\":%d,\"onScreen\":%s,\"wallpaper\":", i + 1, on ? "true" : "false");
        j_wallpaper(&r->data, e);
        sb_puts(&r->data, "}");
        shown++;
    }
    sb_puts(&r->data, "]");
    if (!shown) say(r, c->count ? L"No wallpaper matches \"%ls\".\n" : L"The library is empty. Add files: screenplus import <path>\n", filter);
    else say(r, L"\n%d of %d wallpaper(s)%ls\n", shown, c->count, anyOn ? L"   * = on screen" : L"");
}

static void cmd_displays(Reply *r)
{
    sb_puts(&r->data, "[");
    for (int i = 0; i < g_app.monCount; i++) {
        const MonitorInfo *m = &g_app.mons[i];
        const wchar_t *own = NULL;
        for (int a = 0; a < g_app.cfg.assignCount; a++)
            if (str_ieq(g_app.cfg.assign[a].monitorId, m->id)) own = g_app.cfg.assign[a].wallpaperId;
        WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(m->id));
        say(r, L"Display %d  %ls, %ldx%ld @ %u Hz, %u dpi%ls\n", m->index, m->name, m->rect.right - m->rect.left,
            m->rect.bottom - m->rect.top, m->refreshHz, m->dpi, m->primary ? L", primary" : L"");
        say(r, L"           wallpaper: %ls%ls\n", e ? e->name : L"(none)", own ? L" (own choice)" : L" (all displays)");
        say(r, L"           id: %ls\n", m->id);
        if (i) sb_puts(&r->data, ",");
        sb_printf(&r->data, "{\"index\":%d,\"name\":", m->index);
        jstr(&r->data, m->name);
        sb_puts(&r->data, ",\"id\":");
        jstr(&r->data, m->id);
        sb_printf(&r->data, ",\"width\":%ld,\"height\":%ld,\"hz\":%u,\"dpi\":%u,\"primary\":%s,\"ownWallpaper\":%s,\"wallpaper\":",
                  m->rect.right - m->rect.left, m->rect.bottom - m->rect.top, m->refreshHz, m->dpi,
                  m->primary ? "true" : "false", own ? "true" : "false");
        j_wallpaper(&r->data, e);
        sb_puts(&r->data, "}");
    }
    sb_puts(&r->data, "]");
    if (!g_app.monCount) say(r, L"No displays found.\n");
}

/* ------------------------------------------------------------------ library */

static void cmd_apply(Reply *r, const Req *q)
{
    if (q->argc < 2) {
        fail(r, L"Usage: screenplus apply <name | number | id | file> [--display N]");
        return;
    }
    const MonitorInfo *mon;
    if (!resolve_display(r, q, &mon)) return;
    wchar_t what[SP_MAX_PATH];
    join_args(q, 1, what, SP_COUNTOF(what));
    Config *c = &g_app.cfg;
    WallpaperEntry *e = NULL;
    if (file_exists(what) || dir_exists(what)) {           /* a file or folder: add it first if needed */
        wchar_t entry[SP_MAX_PATH];
        e = config_find_path(c, what);
        if (!e && dir_exists(what) && library_html_entry(what, entry, SP_COUNTOF(entry))) e = config_find_path(c, entry);
        if (!e) {
            int before = c->count;
            const wchar_t *paths[1] = { what };
            library_import(paths, 1, FALSE);
            if (c->count > before) e = &c->items[c->count - 1];
        }
        if (!e) {
            fail(r, L"\"%ls\" is not a supported wallpaper (image, video or web page).", what);
            return;
        }
    } else if (!(e = find_wallpaper(r, what))) {
        return;
    }
    wchar_t id[SP_ID_LEN];
    str_copy(id, SP_COUNTOF(id), e->id);
    app_apply_wallpaper(id, mon ? mon->id : NULL);
    e = config_find(c, id);
    if (mon) say(r, L"Applied \"%ls\" to display %d.\n", e ? e->name : id, mon->index);
    else say(r, L"Applied \"%ls\" to all displays.\n", e ? e->name : id);
    if (e && e->missing) say(r, L"Warning: its file was not found at %ls\n", e->path);
    sb_puts(&r->data, "{\"wallpaper\":");
    j_wallpaper(&r->data, e);
    sb_printf(&r->data, ",\"display\":%d}", mon ? mon->index : 0);
}

static void cmd_import(Reply *r, const Req *q)
{
    if (q->argc < 2) {
        fail(r, L"Usage: screenplus import <file or folder> [...] [--apply]");
        return;
    }
    Config *c = &g_app.cfg;
    const wchar_t *paths[MAX_ARGS];
    int n = 0;
    for (int i = 1; i < q->argc; i++) {
        if (!file_exists(q->argv[i]) && !dir_exists(q->argv[i])) {
            fail(r, L"Not found: %ls", q->argv[i]);
            return;
        }
        paths[n++] = q->argv[i];
    }
    int before = c->count;
    int added = library_import(paths, n, has_flag(q, L"--apply"));
    say(r, L"Added %d wallpaper(s) to the library.\n", added);
    sb_puts(&r->data, "{\"added\":[");
    for (int i = before; i < c->count; i++) {
        say(r, L"  %3d  %ls [%ls]\n", i + 1, c->items[i].name, type_key(c->items[i].type));
        if (i > before) sb_puts(&r->data, ",");
        j_wallpaper(&r->data, &c->items[i]);
    }
    sb_puts(&r->data, "]}");
    if (!added) say(r, L"Nothing new: already in the library, or no supported files found.\n");
}

static void cmd_remove(Reply *r, const Req *q)
{
    wchar_t what[SP_MAX_PATH];
    join_args(q, 1, what, SP_COUNTOF(what));
    WallpaperEntry *e = find_wallpaper(r, what);
    if (!e) return;
    wchar_t id[SP_ID_LEN], name[SP_NAME_LEN];
    str_copy(id, SP_COUNTOF(id), e->id);
    str_copy(name, SP_COUNTOF(name), e->name);
    library_remove(id);
    say(r, L"Removed \"%ls\" from the library (the file itself was not touched).\n", name);
    sb_puts(&r->data, "{\"removed\":");
    jstr(&r->data, id);
    sb_puts(&r->data, "}");
}

static void cmd_next(Reply *r)
{
    app_next_wallpaper();
    WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(NULL));
    if (e) say(r, L"Now showing \"%ls\".\n", e->name);
    else say(r, L"The library is empty.\n");
    sb_puts(&r->data, "{\"wallpaper\":");
    j_wallpaper(&r->data, e);
    sb_puts(&r->data, "}");
}

/* ------------------------------------------------------------------ look (colour grading) */

enum { LK_BRIGHT, LK_CONTRAST, LK_SAT, LK_HUE, LK_GAMMA, LK_TINT, LK_LIFT, LK_TINTV, LK_COUNT };
static const struct { const wchar_t *key, *alias; int min, max; } k_look[LK_COUNT] = {
    { L"brightness",      L"bright",   -100, 100 },
    { L"contrast",        NULL,        -100, 100 },
    { L"saturation",      L"sat",      -100, 100 },
    { L"hue",             L"hue-shift", -180, 180 },
    { L"gamma",           NULL,        -100, 100 },
    { L"tint",            L"color",       0, 0 },
    { L"tint-strength",   L"strength",    0, 100 },
    { L"tint-brightness", NULL,           0, 100 },
};

static int *look_field(WallpaperAdjust *a, int k)
{
    switch (k) {
    case LK_BRIGHT:   return &a->brightness;
    case LK_CONTRAST: return &a->contrast;
    case LK_SAT:      return &a->saturation;
    case LK_HUE:      return &a->hue;
    case LK_GAMMA:    return &a->gamma;
    case LK_LIFT:     return &a->tintLift;
    case LK_TINTV:    return &a->tintBrightness;
    default:          return NULL;
    }
}

static int look_key(const wchar_t *k)
{
    wchar_t norm[48];
    str_copy(norm, SP_COUNTOF(norm), k);
    for (wchar_t *c = norm; *c; c++) if (*c == L'_') *c = L'-';
    for (int i = 0; i < LK_COUNT; i++)
        if (str_ieq(norm, k_look[i].key) || (k_look[i].alias && str_ieq(norm, k_look[i].alias))) return i;
    if (str_ieq(norm, L"colour") || str_ieq(norm, L"tint-color") || str_ieq(norm, L"tint-colour")) return LK_TINT;
    if (str_ieq(norm, L"lift")) return LK_LIFT;
    return -1;
}

static void look_print(Reply *r, const WallpaperEntry *e, const WallpaperAdjust *a)
{
    say(r, L"Look of \"%ls\"%ls\n", e->name, e->type == WP_HTML ? L" (web wallpaper: CSS, gamma not available)" : L"");
    for (int k = 0; k < LK_COUNT; k++) {
        if (k == LK_TINT) {
            say(r, L"  %-16ls #%06X\n", k_look[k].key, a->tint & 0xFFFFFF);
            continue;
        }
        int v = *look_field((WallpaperAdjust *)a, k);
        if (k == LK_LIFT || k == LK_TINTV) say(r, L"  %-16ls %d%%\n", k_look[k].key, v);
        else say(r, L"  %-16ls %+d%ls\n", k_look[k].key, v, k == LK_HUE ? L" deg" : L"");
    }
    say(r, L"%ls\n", adjust_active(a) ? L"Grading is on." : L"Neutral: no grading applied.");
    sb_printf(&r->data, "{\"wallpaper\":");
    j_wallpaper(&r->data, e);
    sb_printf(&r->data, ",\"active\":%s,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hue\":%d,\"gamma\":%d,"
                        "\"tint\":\"#%06X\",\"tintStrength\":%d,\"tintBrightness\":%d}",
              adjust_active(a) ? "true" : "false", a->brightness, a->contrast, a->saturation, a->hue, a->gamma,
              a->tint & 0xFFFFFF, a->tintLift, a->tintBrightness);
}

static void cmd_look(Reply *r, const Req *q)
{
    const MonitorInfo *mon;
    WallpaperEntry *e = target_wallpaper(r, q, &mon);
    if (!e) return;
    WallpaperAdjust a;
    wpapi_load_adjust(e->id, &a);
    const wchar_t *sub = q->argc > 1 ? q->argv[1] : L"";
    if (!sub[0] || str_ieq(sub, L"show") || str_ieq(sub, L"get")) {
        look_print(r, e, &a);
        return;
    }
    if (str_ieq(sub, L"reset")) {
        a = adjust_defaults();
        wpapi_save_adjust(e->id, &a);
        customise_changed();
        say(r, L"Look reset to neutral.\n\n");
        look_print(r, e, &a);
        return;
    }
    if (!str_ieq(sub, L"set")) {
        fail(r, L"Usage: screenplus look [set <key> <value> ... | reset] [--display N]");
        return;
    }
    if (q->argc < 3) {
        fail(r, L"Usage: screenplus look set <key> <value> [<key> <value> ...]   (or key=value)\n"
                L"Keys: brightness contrast saturation gamma (-100..100), hue (-180..180),\n"
                L"      tint (#RRGGBB), tint-strength tint-brightness (0..100)");
        return;
    }
    int gammaOnWeb = 0;
    for (int i = 2; i < q->argc; i++) {
        wchar_t key[48], val[48];
        const wchar_t *eq = wcschr(q->argv[i], L'=');
        if (eq) {
            str_copyn(key, SP_COUNTOF(key), q->argv[i], (size_t)(eq - q->argv[i]));
            str_copy(val, SP_COUNTOF(val), eq + 1);
        } else {
            if (i + 1 >= q->argc) {
                fail(r, L"Missing value for \"%ls\".", q->argv[i]);
                return;
            }
            str_copy(key, SP_COUNTOF(key), q->argv[i]);
            str_copy(val, SP_COUNTOF(val), q->argv[++i]);
        }
        int k = look_key(key);
        if (k < 0) {
            fail(r, L"Unknown look key \"%ls\". Keys: brightness contrast saturation hue gamma tint tint-strength tint-brightness", key);
            return;
        }
        if (k == LK_TINT) {
            uint32_t rgb;
            if (!wpapi_parse_rgb(val, &rgb)) {
                fail(r, L"tint wants a colour like #3366FF, not \"%ls\".", val);
                return;
            }
            a.tint = rgb;
            continue;
        }
        wchar_t *end = NULL;
        long v = wcstol(val, &end, 10);
        if (!end || end == val || *end) {
            fail(r, L"%ls wants a whole number, not \"%ls\".", k_look[k].key, val);
            return;
        }
        *look_field(&a, k) = SP_CLAMP((int)v, k_look[k].min, k_look[k].max);
        if (k == LK_GAMMA && v && e->type == WP_HTML) gammaOnWeb = 1;
    }
    wpapi_save_adjust(e->id, &a);
    customise_changed();
    wpapi_load_adjust(e->id, &a);
    look_print(r, e, &a);
    if (gammaOnWeb) say(r, L"Note: gamma has no effect on web wallpapers.\n");
}

/* ------------------------------------------------------------------ wallpaper properties */

static const wchar_t *prop_type_name(CustomType t)
{
    switch (t) {
    case CP_CHECKBOX: return L"toggle";
    case CP_SLIDER:   return L"slider";
    case CP_DROPDOWN: return L"choice";
    case CP_COMBO:    return L"choice";
    case CP_FILE:     return L"file";
    case CP_COLOR:    return L"colour";
    case CP_TEXT:     return L"text";
    default:          return L"label";
    }
}

static void prop_value_text(const CustomProp *cp, wchar_t *out, size_t cap)
{
    int style;
    switch (cp->type) {
    case CP_CHECKBOX: str_copy(out, cap, json_bool(cp->value, 0) ? L"on" : L"off"); break;
    case CP_SLIDER:   str_printf(out, cap, L"%.6g", json_num(cp->value, cp->min)); break;
    case CP_DROPDOWN:
    case CP_COMBO:
    case CP_FILE: {
        int i = wpapi_choice_current(cp);
        if (i >= 0) wpapi_choice_label(cp, i, out, cap);
        else if (cp->value && cp->value->t == J_STR) json_str_w(cp->value, out, cap);
        else str_printf(out, cap, L"%.6g", json_num(cp->value, 0.0));
        break;
    }
    case CP_COLOR:    str_printf(out, cap, L"#%06X", wpapi_color_get(cp, &style)); break;
    case CP_TEXT:     json_str_w(cp->value, out, cap); break;
    default:          out[0] = 0; break;
    }
}

static void prop_range_text(const CustomProp *cp, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (cp->type == CP_SLIDER) {
        str_printf(out, cap, L"%.6g..%.6g", cp->min, cp->max);
        if (cp->step != 1.0) {
            wchar_t s[32];
            str_printf(s, SP_COUNTOF(s), L" step %.6g", cp->step);
            str_cat(out, cap, s);
        }
    } else if (cp->type == CP_FILE) {
        str_printf(out, cap, L"%d file(s) to choose from", wpapi_choice_count(cp));
    } else if (cp->type == CP_DROPDOWN || cp->type == CP_COMBO) {
        int n = wpapi_choice_count(cp);
        for (int i = 0; i < n; i++) {
            wchar_t l[96];
            wpapi_choice_label(cp, i, l, SP_COUNTOF(l));
            if (i) str_cat(out, cap, L" | ");
            str_cat(out, cap, l);
            if (wcslen(out) > 70) {
                str_printf(out + 67, cap - 67, L"... (%d choices)", n);
                break;
            }
        }
    } else if (cp->type == CP_CHECKBOX) {
        str_copy(out, cap, L"on | off");
    }
}

static CustomProp *find_prop(Reply *r, CustomProp *list, int n, const wchar_t *name)
{
    char key[96];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, key, sizeof key, NULL, NULL);
    for (int i = 0; i < n; i++)
        if (!_stricmp(list[i].name, key)) return &list[i];
    int same = 0;                                       /* labels can repeat ("Scaling" twice in Rain) */
    CustomProp *byLabel = NULL;
    for (int i = 0; i < n; i++)
        if (str_ieq(list[i].label, name)) {
            same++;
            byLabel = &list[i];
        }
    if (same == 1) return byLabel;
    if (same > 1) {
        fail(r, L"%d properties are labelled \"%ls\"; use the name instead:", same, name);
        for (int i = 0; i < n; i++)
            if (str_ieq(list[i].label, name)) {
                wchar_t wkey[96];
                MultiByteToWideChar(CP_UTF8, 0, list[i].name, -1, wkey, SP_COUNTOF(wkey));
                say(r, L"  %-22ls %ls\n", wkey, prop_type_name(list[i].type));
            }
        return NULL;
    }
    int hits = 0;
    CustomProp *hit = NULL;
    for (int i = 0; i < n; i++) {
        wchar_t wkey[96];
        MultiByteToWideChar(CP_UTF8, 0, list[i].name, -1, wkey, SP_COUNTOF(wkey));
        if (list[i].type != CP_LABEL && (str_icontains(wkey, name) || str_icontains(list[i].label, name))) {
            hits++;
            hit = &list[i];
        }
    }
    if (hits == 1) return hit;
    fail(r, hits ? L"\"%ls\" matches several properties, use the exact name (screenplus props)."
                 : L"No property \"%ls\". See: screenplus props", name);
    return NULL;
}

static JVal *parse_prop_value(Reply *r, const CustomProp *cp, const wchar_t *v)
{
    switch (cp->type) {
    case CP_CHECKBOX: {
        int on;
        if (!parse_on_off(v, json_bool(cp->value, 0), &on)) {
            fail(r, L"%ls is a toggle: use on, off or toggle.", cp->label);
            return NULL;
        }
        return wpapi_bool_value(on);
    }
    case CP_SLIDER: {
        wchar_t *end = NULL;
        double d = wcstod(v, &end);
        if (!end || end == v || *end) {
            fail(r, L"%ls wants a number between %.6g and %.6g.", cp->label, cp->min, cp->max);
            return NULL;
        }
        double step = cp->step > 0 ? cp->step : 1.0;
        d = SP_CLAMP(cp->min + floor((d - cp->min) / step + 0.5) * step, cp->min, cp->max);
        return wpapi_num_value(d, step);
    }
    case CP_DROPDOWN:
    case CP_COMBO:
    case CP_FILE: {
        int n = wpapi_choice_count(cp), hits = 0, pick = -1;
        wchar_t l[160];
        for (int i = 0; i < n && pick < 0; i++) {
            wpapi_choice_label(cp, i, l, SP_COUNTOF(l));
            if (str_ieq(l, v)) pick = i;
        }
        if (pick < 0 && cp->type == CP_DROPDOWN) {      /* Lively dropdowns also take the item number */
            wchar_t *end = NULL;
            long i = wcstol(v, &end, 10);
            if (end && end != v && !*end && i >= 0 && i < n) pick = (int)i;
        }
        for (int i = 0; i < n && pick < 0; i++) {
            wpapi_choice_label(cp, i, l, SP_COUNTOF(l));
            if (str_icontains(l, v)) {
                hits++;
                if (hits == 1) pick = i;
            }
        }
        if (pick < 0 || hits > 1) {
            wchar_t all[512];
            prop_range_text(cp, all, SP_COUNTOF(all));
            if (hits > 1) fail(r, L"\"%ls\" matches several choices of %ls. Choices: %ls", v, cp->label, all);
            else fail(r, L"%ls has no choice \"%ls\". Choices: %ls", cp->label, v, all);
            return NULL;
        }
        return wpapi_choice_value(cp, pick);
    }
    case CP_COLOR: {
        uint32_t rgb;
        int style;
        if (!wpapi_parse_rgb(v, &rgb)) {
            fail(r, L"%ls wants a colour like #3366FF.", cp->label);
            return NULL;
        }
        wpapi_color_get(cp, &style);
        return wpapi_color_value(rgb, style);
    }
    case CP_TEXT:
        return wpapi_str_value(v);
    default:
        fail(r, L"%ls is a label, not a setting.", cp->label);
        return NULL;
    }
}

static void props_print(Reply *r, const WallpaperEntry *e, CustomProp *list, int n)
{
    if (!n) say(r, L"\"%ls\" has no settings of its own.\n", e->name);
    else say(r, L"Properties of \"%ls\" (%ls)\n", e->name, format_key(e->format));
    sb_puts(&r->data, "{\"wallpaper\":");
    j_wallpaper(&r->data, e);
    sb_puts(&r->data, ",\"properties\":[");
    int out = 0;
    for (int i = 0; i < n; i++) {
        CustomProp *cp = &list[i];
        wchar_t key[96], val[256], range[512];
        MultiByteToWideChar(CP_UTF8, 0, cp->name, -1, key, SP_COUNTOF(key));
        if (cp->type == CP_LABEL) {
            say(r, L"  -- %ls\n", cp->label);
            continue;
        }
        prop_value_text(cp, val, SP_COUNTOF(val));
        prop_range_text(cp, range, SP_COUNTOF(range));
        wchar_t shown[32];                              /* long file names: keep the table aligned */
        str_copy(shown, SP_COUNTOF(shown), val);
        if (wcslen(val) > 24) str_copy(shown + 21, SP_COUNTOF(shown) - 21, L"...");
        say(r, L"  %-22ls %-20.20ls %-7ls %-24ls %ls%ls\n", key, cp->label, prop_type_name(cp->type), shown, range,
            cp->overridden ? L"  (changed)" : L"");
        if (out++) sb_puts(&r->data, ",");
        sb_puts(&r->data, "{\"name\":");
        json_write_string(&r->data, cp->name);
        sb_puts(&r->data, ",\"label\":");
        jstr(&r->data, cp->label);
        sb_puts(&r->data, ",\"type\":");
        jstr(&r->data, prop_type_name(cp->type));
        sb_puts(&r->data, ",\"value\":");
        json_write(&r->data, cp->value);
        sb_puts(&r->data, ",\"display\":");
        jstr(&r->data, val);
        if (cp->type == CP_SLIDER) sb_printf(&r->data, ",\"min\":%.10g,\"max\":%.10g,\"step\":%.10g", cp->min, cp->max, cp->step);
        if (wpapi_choice_count(cp)) {
            sb_puts(&r->data, ",\"choices\":[");
            for (int c = 0; c < wpapi_choice_count(cp); c++) {
                wchar_t l[160];
                wpapi_choice_label(cp, c, l, SP_COUNTOF(l));
                if (c) sb_puts(&r->data, ",");
                jstr(&r->data, l);
            }
            sb_puts(&r->data, "]");
        }
        sb_printf(&r->data, ",\"changed\":%s}", cp->overridden ? "true" : "false");
    }
    sb_puts(&r->data, "]}");
}

static void cmd_props(Reply *r, const Req *q)
{
    const MonitorInfo *mon;
    WallpaperEntry *e = target_wallpaper(r, q, &mon);
    if (!e) return;
    if (!e->format) {
        fail(r, L"\"%ls\" is a plain %ls file: it has no settings of its own. Its look can still be changed:"
                L" screenplus look", e->name, type_key(e->type));
        return;
    }
    const wchar_t *sub = q->argc > 1 ? q->argv[1] : L"";
    if (str_ieq(sub, L"reset")) {
        wpapi_clear_properties(e->id);
        customise_changed();
        say(r, L"Properties restored to the wallpaper's defaults.\n\n");
    } else if (str_ieq(sub, L"set")) {
        if (q->argc < 4) {
            fail(r, L"Usage: screenplus props set <name> <value>   (see: screenplus props)");
            return;
        }
        int n = 0;
        CustomProp *list = wpapi_props_list(e, &n);
        CustomProp *cp = find_prop(r, list, n, q->argv[2]);
        wchar_t value[SP_MAX_PATH];
        join_args(q, 3, value, SP_COUNTOF(value));
        JVal *v = cp ? parse_prop_value(r, cp, value) : NULL;
        if (v) {
            if (wpapi_save_override(e, cp->name, v)) {
                customise_changed();
                json_free(cp->value);
                cp->value = v;
                v = NULL;
                wchar_t key[96], shown[256];
                MultiByteToWideChar(CP_UTF8, 0, cp->name, -1, key, SP_COUNTOF(key));
                prop_value_text(cp, shown, SP_COUNTOF(shown));
                say(r, L"%ls (%ls) = %ls\n", key, cp->label, shown);
                sb_puts(&r->data, "{\"name\":");
                json_write_string(&r->data, cp->name);
                sb_puts(&r->data, ",\"value\":");
                json_write(&r->data, cp->value);
                sb_puts(&r->data, "}");
            } else {
                fail(r, L"Could not save the value (is %%LOCALAPPDATA%%\\ScreenPlus writable?).");
            }
            json_free(v);
        }
        wpapi_props_free(list, n);
        return;
    } else if (sub[0] && !str_ieq(sub, L"show") && !str_ieq(sub, L"get")) {
        fail(r, L"Usage: screenplus props [set <name> <value> | reset] [--display N]");
        return;
    }
    int n = 0;
    CustomProp *list = wpapi_props_list(e, &n);
    props_print(r, e, list, n);
    wpapi_props_free(list, n);
}

/* ------------------------------------------------------------------ settings */

enum { SK_BOOL, SK_THEME };
static const struct { const wchar_t *key; size_t off; int kind; const wchar_t *desc; } k_settings[] = {
    { L"start_with_windows",  offsetof(Settings, startWithWindows),  SK_BOOL,  L"Start Screen+ when you sign in" },
    { L"launch_to_tray",      offsetof(Settings, launchToTray),      SK_BOOL,  L"Start in the notification area" },
    { L"pause_on_fullscreen", offsetof(Settings, pauseOnFullscreen), SK_BOOL,  L"Pause the display a fullscreen app or game is on" },
    { L"pause_when_covered",  offsetof(Settings, pauseWhenCovered),  SK_BOOL,  L"Stop rendering while windows cover the desktop" },
    { L"suspend_on_lock",     offsetof(Settings, suspendOnLock),     SK_BOOL,  L"Suspend while the PC is locked" },
    { L"pause_on_battery",    offsetof(Settings, pauseOnBattery),    SK_BOOL,  L"Pause on battery or energy saver" },
    { L"audio",               offsetof(Settings, audio),             SK_BOOL,  L"Play wallpaper sound" },
    { L"auto_recover",        offsetof(Settings, autoRecover),       SK_BOOL,  L"Restart failed renderers automatically" },
    { L"paused",              offsetof(Settings, paused),            SK_BOOL,  L"Wallpapers paused" },
    { L"html_mouse",          offsetof(Settings, htmlMouse),         SK_BOOL,  L"Mouse input for web wallpapers" },
    { L"html_audio",          offsetof(Settings, htmlAudio),         SK_BOOL,  L"System audio spectrum for web wallpapers" },
    { L"lively_auto_import",  offsetof(Settings, livelyImport),      SK_BOOL,  L"Add new Lively Wallpaper packages at launch" },
    { L"theme",               offsetof(Settings, theme),             SK_THEME, L"Window theme: system, light or dark" },
};

static const wchar_t *theme_name(int t) { return t == THEME_LIGHT ? L"light" : t == THEME_DARK ? L"dark" : L"system"; }

static void settings_print(Reply *r)
{
    Settings *s = &g_app.cfg.s;
    sb_puts(&r->data, "{");
    for (size_t i = 0; i < SP_COUNTOF(k_settings); i++) {
        int v = *(int *)((char *)s + k_settings[i].off);
        const wchar_t *shown = k_settings[i].kind == SK_THEME ? theme_name(v) : v ? L"on" : L"off";
        say(r, L"  %-20ls %-7ls %ls\n", k_settings[i].key, shown, k_settings[i].desc);
        if (i) sb_puts(&r->data, ",");
        jstr(&r->data, k_settings[i].key);
        sb_puts(&r->data, ":");
        if (k_settings[i].kind == SK_THEME) jstr(&r->data, shown);
        else sb_puts(&r->data, v ? "true" : "false");
    }
    sb_puts(&r->data, "}");
}

static void cmd_settings(Reply *r, const Req *q)
{
    const wchar_t *sub = q->argc > 1 ? q->argv[1] : L"";
    if (!sub[0] || str_ieq(sub, L"show") || str_ieq(sub, L"get")) {
        settings_print(r);
        return;
    }
    if (!str_ieq(sub, L"set") || q->argc < 4) {
        fail(r, L"Usage: screenplus settings [set <key> <value>]   (see: screenplus settings)");
        return;
    }
    wchar_t key[48];
    str_copy(key, SP_COUNTOF(key), q->argv[2]);
    for (wchar_t *c = key; *c; c++) if (*c == L'-') *c = L'_';
    Settings *s = &g_app.cfg.s;
    for (size_t i = 0; i < SP_COUNTOF(k_settings); i++) {
        if (!str_ieq(key, k_settings[i].key)) continue;
        int *field = (int *)((char *)s + k_settings[i].off);
        const wchar_t *v = q->argv[3];
        if (k_settings[i].kind == SK_THEME) {
            if (str_ieq(v, L"light")) *field = THEME_LIGHT;
            else if (str_ieq(v, L"dark")) *field = THEME_DARK;
            else if (str_ieq(v, L"system")) *field = THEME_SYSTEM;
            else {
                fail(r, L"theme is system, light or dark.");
                return;
            }
            app_save_config();
        } else {
            int on;
            if (!parse_on_off(v, *field, &on)) {
                fail(r, L"%ls is on or off (or toggle).", k_settings[i].key);
                return;
            }
            if (field == &s->startWithWindows) {
                if (!startup_set_enabled(on)) {
                    fail(r, L"Could not update the Windows startup entry.");
                    return;
                }
                *field = on;
                app_save_config();
            } else if (field == &s->audio) {
                app_set_audio(on);
            } else if (field == &s->paused) {
                app_set_paused(on);
            } else {
                *field = on;
                app_save_config();
            }
        }
        ui_on_config_changed();
        say(r, L"%ls = %ls\n", k_settings[i].key,
            k_settings[i].kind == SK_THEME ? theme_name(*field) : *field ? L"on" : L"off");
        sb_puts(&r->data, "{");
        jstr(&r->data, k_settings[i].key);
        sb_puts(&r->data, ":");
        if (k_settings[i].kind == SK_THEME) jstr(&r->data, theme_name(*field));
        else sb_puts(&r->data, *field ? "true" : "false");
        sb_puts(&r->data, "}");
        return;
    }
    fail(r, L"Unknown setting \"%ls\". See: screenplus settings", q->argv[2]);
}

/* ------------------------------------------------------------------ dispatch */

static void run(Reply *r, const Req *q)
{
    const wchar_t *c = q->argc ? q->argv[0] : L"";
    if (str_ieq(c, L"ping")) {
        say(r, L"Screen+ %ls is running (pid %lu).\n", SP_VERSION_W, GetCurrentProcessId());
        sb_printf(&r->data, "{\"pid\":%lu}", GetCurrentProcessId());
    } else if (str_ieq(c, L"status")) {
        cmd_status(r);
    } else if (str_ieq(c, L"list") || str_ieq(c, L"ls")) {
        cmd_list(r, q);
    } else if (str_ieq(c, L"displays") || str_ieq(c, L"monitors")) {
        cmd_displays(r);
    } else if (str_ieq(c, L"apply") || str_ieq(c, L"set")) {
        cmd_apply(r, q);
    } else if (str_ieq(c, L"import") || str_ieq(c, L"add")) {
        cmd_import(r, q);
    } else if (str_ieq(c, L"remove") || str_ieq(c, L"rm")) {
        cmd_remove(r, q);
    } else if (str_ieq(c, L"next")) {
        cmd_next(r);
    } else if (str_ieq(c, L"pause") || str_ieq(c, L"resume") || str_ieq(c, L"toggle")) {
        int on = str_ieq(c, L"pause") ? 1 : str_ieq(c, L"resume") ? 0 : !g_app.cfg.s.paused;
        app_set_paused(on);
        say(r, on ? L"Wallpapers paused.\n" : L"Wallpapers playing.\n");
        sb_printf(&r->data, "{\"paused\":%s}", on ? "true" : "false");
    } else if (str_ieq(c, L"audio") || str_ieq(c, L"sound")) {
        int on = g_app.cfg.s.audio;
        if (q->argc > 1 && !parse_on_off(q->argv[1], on, &on)) {
            fail(r, L"Usage: screenplus audio [on | off | toggle]");
            return;
        }
        if (q->argc > 1) app_set_audio(on);
        say(r, L"Wallpaper sound is %ls.\n", on ? L"on" : L"off");
        sb_printf(&r->data, "{\"audio\":%s}", on ? "true" : "false");
    } else if (str_ieq(c, L"restart")) {
        app_restart_renderer();
        say(r, L"Renderer restarted.\n");
    } else if (str_ieq(c, L"show")) {
        app_show_ui();
        say(r, L"Window opened.\n");
    } else if (str_ieq(c, L"hide")) {
        ui_destroy();
        say(r, L"Window closed (Screen+ keeps running in the notification area).\n");
    } else if (str_ieq(c, L"stop") || str_ieq(c, L"quit-app")) {
        say(r, L"Screen+ is closing.\n");
        r->exitAfter = 1;
    } else if (str_ieq(c, L"look") || str_ieq(c, L"grade")) {
        cmd_look(r, q);
    } else if (str_ieq(c, L"props") || str_ieq(c, L"prop") || str_ieq(c, L"properties")) {
        cmd_props(r, q);
    } else if (str_ieq(c, L"settings") || str_ieq(c, L"setting") || str_ieq(c, L"config")) {
        cmd_settings(r, q);
    } else {
        fail(r, L"Unknown command \"%ls\". See: screenplus help", c);
    }
}

void cli_server_handle(HWND replyTo, const void *request, DWORD size)
{
    wchar_t cls[32] = L"";
    if (!replyTo || !GetClassNameW(replyTo, cls, SP_COUNTOF(cls)) || !str_ieq(cls, SP_CLI_CLASS)) return;
    Req q;
    ZeroMemory(&q, sizeof q);
    Reply r = { 1 };
    JVal *j = json_parse((const char *)request, size);
    const JVal *argv = json_get(j, "argv");
    for (int i = 0; argv && argv->t == J_ARR && i < argv->n; i++) {
        const char *a = json_str(&argv->kids[i], NULL);
        wchar_t *w = a ? wide_from_utf8(a, -1) : NULL;
        if (!w) continue;
        int isDisplay = str_ieq(w, L"--display") || str_ieq(w, L"-d");
        if (isDisplay && i + 1 < argv->n) {
            const char *n = json_str(&argv->kids[++i], "0");
            q.display = atoi(n);
            free(w);
        } else if (str_istarts(w, L"--display=")) {
            q.display = _wtoi(w + 10);
            free(w);
        } else if (w[0] == L'-' && w[1] == L'-' && q.flagCount < (int)SP_COUNTOF(q.flags)) {
            q.flags[q.flagCount++] = w;
        } else if (q.argc < MAX_ARGS) {
            q.argv[q.argc++] = w;
        } else {
            free(w);
        }
    }
    json_free(j);

    wchar_t line[512];
    line[0] = 0;
    for (int i = 0; i < q.argc; i++) {
        if (i) str_cat(line, SP_COUNTOF(line), L" ");
        str_cat(line, SP_COUNTOF(line), q.argv[i]);
    }
    int quiet = q.argc && (str_ieq(q.argv[0], L"status") || str_ieq(q.argv[0], L"ping"));   /* `status --watch` polls */
    if (quiet) LOGD(L"cli: %ls", line);
    else LOGI(L"cli: %ls", line);

    run(&r, &q);

    StrBuf out = { 0 };
    sb_printf(&out, "{\"ok\":%s,\"text\":", r.ok ? "true" : "false");
    json_write_string(&out, r.text.p ? r.text.p : "");
    sb_puts(&out, ",\"data\":");
    sb_puts(&out, r.ok && r.data.len && !r.data.failed ? r.data.p : "null");
    sb_puts(&out, "}");
    if (!out.failed) ipc_send(g_app.shellWnd, replyTo, IPC_CLI_REPLY, out.p, (DWORD)out.len, 5000);
    sb_free(&out);
    sb_free(&r.text);
    sb_free(&r.data);
    for (int i = 0; i < q.argc; i++) free(q.argv[i]);
    for (int i = 0; i < q.flagCount; i++) free(q.flags[i]);
    if (r.exitAfter) PostMessageW(g_app.shellWnd, WM_SP_SHELLCMD, CMD_EXIT, 0);
}
