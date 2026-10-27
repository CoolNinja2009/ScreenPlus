#include "config.h"
#include "../common/util.h"
#include "../common/log.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CONFIG_MAX_FILE (8 * 1024 * 1024)

static const wchar_t *k_imageExt[] = { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif",
                                       L".tif", L".tiff", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp" };
static const wchar_t *k_videoExt[] = { L".mp4", L".m4v", L".mov", L".wmv", L".webm", L".mkv", L".avi", L".mpg",
                                       L".mpeg", L".ts", L".m2ts", L".3gp", L".asf" };
static const wchar_t *k_htmlExt[]  = { L".html", L".htm" };

WallpaperType wallpaper_type_from_path(const wchar_t *path)
{
    const wchar_t *ext = path_ext(path);
    for (size_t i = 0; i < SP_COUNTOF(k_imageExt); i++) if (str_ieq(ext, k_imageExt[i])) return WP_IMAGE;
    for (size_t i = 0; i < SP_COUNTOF(k_videoExt); i++) if (str_ieq(ext, k_videoExt[i])) return WP_VIDEO;
    for (size_t i = 0; i < SP_COUNTOF(k_htmlExt); i++)  if (str_ieq(ext, k_htmlExt[i]))  return WP_HTML;
    return WP_NONE;
}

int wallpaper_is_url(const wchar_t *path)
{
    return str_istarts(path, L"https://") || str_istarts(path, L"http://");
}

int wallpaper_source_exists(const WallpaperEntry *e)
{
    return wallpaper_is_url(e->path) || file_exists(e->path);
}

void config_defaults(Config *c)
{
    WallpaperEntry *items = c->items;
    int cap = c->cap;
    memset(c, 0, sizeof *c);
    c->items = items;
    c->cap = cap;
    c->schema = CONFIG_SCHEMA;
    c->s.startWithWindows = 0;
    c->s.launchToTray = 0;
    c->s.pauseOnFullscreen = 1;
    c->s.pauseWhenCovered = 1;
    c->s.suspendOnLock = 1;
    c->s.pauseOnBattery = 1;
    c->s.audio = 0;
    c->s.autoRecover = 1;
    c->s.paused = 0;
    c->s.htmlMouse = 1;
    c->s.htmlAudio = 1;
    c->s.livelyImport = 1;
    c->s.urlImport = 1;
    c->s.depthAll = 0;
    c->s.theme = THEME_SYSTEM;
    c->s.displayMode = DM_INDEPENDENT;
    c->s.scrAudio = 0;
    c->s.scrDisplays = SCRD_ALL;
    c->s.scrExitOnMouse = 1;
}

void config_free(Config *c)
{
    free(c->items);
    c->items = NULL;
    c->count = c->cap = 0;
}

BOOL config_copy(Config *dst, const Config *src)
{
    WallpaperEntry *items = NULL;
    if (src->count) {
        items = (WallpaperEntry *)malloc(sizeof(WallpaperEntry) * (size_t)src->count);
        if (!items) return FALSE;
        memcpy(items, src->items, sizeof(WallpaperEntry) * (size_t)src->count);
    }
    free(dst->items);
    *dst = *src;
    dst->items = items;
    dst->cap = src->count;
    return TRUE;
}

static int valid_id(const wchar_t *id)
{
    if (wcslen(id) != 16) return 0;
    for (int i = 0; i < 16; i++)
        if (!iswxdigit(id[i])) return 0;
    return 1;
}

static int ensure_cap(Config *c, int need)
{
    if (need <= c->cap) return 1;
    int nc = c->cap ? c->cap * 2 : 32;
    while (nc < need) nc *= 2;
    WallpaperEntry *p = (WallpaperEntry *)realloc(c->items, sizeof(WallpaperEntry) * (size_t)nc);
    if (!p) return 0;
    c->items = p;
    c->cap = nc;
    return 1;
}

int config_index_of(const Config *c, const wchar_t *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < c->count; i++)
        if (wcscmp(c->items[i].id, id) == 0) return i;
    return -1;
}

WallpaperEntry *config_find(Config *c, const wchar_t *id)
{
    int i = config_index_of(c, id);
    return i >= 0 ? &c->items[i] : NULL;
}

WallpaperEntry *config_find_path(Config *c, const wchar_t *path)
{
    for (int i = 0; i < c->count; i++)
        if (str_ieq(c->items[i].path, path)) return &c->items[i];
    return NULL;
}

WallpaperEntry *config_add(Config *c, WallpaperType type, const wchar_t *path, const wchar_t *name)
{
    if (!path || !path[0] || type == WP_NONE || c->count >= CONFIG_MAX_ITEMS) return NULL;
    if (!ensure_cap(c, c->count + 1)) return NULL;
    WallpaperEntry *e = &c->items[c->count];
    memset(e, 0, sizeof *e);
    do { random_id(e->id); } while (config_index_of(c, e->id) >= 0);
    e->type = type;
    str_copy(e->path, SP_COUNTOF(e->path), path);
    if (name && name[0]) str_copy(e->name, SP_COUNTOF(e->name), name);
    else path_stem(e->name, SP_COUNTOF(e->name), path);
    str_sanitize_line(e->name);
    e->added = time_unix_now();
    c->count++;
    return e;
}

BOOL config_remove(Config *c, const wchar_t *id)
{
    int i = config_index_of(c, id);
    if (i < 0) return FALSE;
    wchar_t removed[SP_ID_LEN];
    str_copy(removed, SP_ID_LEN, id);
    memmove(&c->items[i], &c->items[i + 1], sizeof(WallpaperEntry) * (size_t)(c->count - i - 1));
    c->count--;
    if (wcscmp(c->defaultWallpaper, removed) == 0) c->defaultWallpaper[0] = 0;
    if (wcscmp(c->screensaver, removed) == 0) c->screensaver[0] = 0;
    for (int a = 0; a < c->assignCount;) {
        if (wcscmp(c->assign[a].wallpaperId, removed) == 0) {
            c->assign[a] = c->assign[--c->assignCount];
        } else a++;
    }
    return TRUE;
}

static int find_assign(const Config *c, const wchar_t *monitorId)
{
    for (int a = 0; a < c->assignCount; a++)
        if (str_ieq(c->assign[a].monitorId, monitorId)) return a;
    return -1;
}

const wchar_t *config_assignment(const Config *c, const wchar_t *monitorId)
{
    int a = monitorId && c->s.displayMode == DM_INDEPENDENT ? find_assign(c, monitorId) : -1;
    if (a >= 0 && config_index_of(c, c->assign[a].wallpaperId) >= 0) return c->assign[a].wallpaperId;
    if (config_index_of(c, c->defaultWallpaper) >= 0) return c->defaultWallpaper;
    return L"";
}

BOOL config_has_override(const Config *c, const wchar_t *monitorId)
{
    return monitorId && c->s.displayMode == DM_INDEPENDENT && find_assign(c, monitorId) >= 0;
}

void config_assign(Config *c, const wchar_t *monitorId, const wchar_t *wallpaperId)
{
    if (!monitorId) {
        str_copy(c->defaultWallpaper, SP_ID_LEN, wallpaperId);
        c->assignCount = 0;   /* "all displays" replaces every override */
        return;
    }
    int a = find_assign(c, monitorId);
    if (a < 0) {
        if (c->assignCount >= CONFIG_MAX_ASSIGN) {    /* drop the oldest override */
            memmove(&c->assign[0], &c->assign[1], sizeof(c->assign[0]) * (CONFIG_MAX_ASSIGN - 1));
            c->assignCount--;
        }
        a = c->assignCount++;
        str_copy(c->assign[a].monitorId, SP_MONID_LEN, monitorId);
    }
    str_copy(c->assign[a].wallpaperId, SP_ID_LEN, wallpaperId);
    if (!c->defaultWallpaper[0]) str_copy(c->defaultWallpaper, SP_ID_LEN, wallpaperId);
}

void config_clear_override(Config *c, const wchar_t *monitorId)
{
    int a = find_assign(c, monitorId);
    if (a >= 0) c->assign[a] = c->assign[--c->assignCount];
}

/* \\?\DISPLAY#DEL40F3#5&2b1c&0&UID4352#{...} -> DEL40F3 (the monitor model). */
static int monitor_model(const wchar_t *id, wchar_t *out, size_t cap)
{
    const wchar_t *p = id ? wcsstr(id, L"DISPLAY#") : NULL;
    const wchar_t *e = p ? wcschr(p + 8, L'#') : NULL;
    if (!e || e == p + 8 || (size_t)(e - p - 8) >= cap) return 0;
    wmemcpy(out, p + 8, (size_t)(e - p - 8));
    out[e - p - 8] = 0;
    return 1;
}

int config_adopt_monitors(Config *c, const wchar_t *const *ids, int n)
{
    int changed = 0;
    for (int m = 0; m < n; m++) {
        wchar_t model[64], other[64];
        if (find_assign(c, ids[m]) >= 0 || !monitor_model(ids[m], model, SP_COUNTOF(model))) continue;
        int found = -1, count = 0;
        for (int a = 0; a < c->assignCount; a++) {
            if (!monitor_model(c->assign[a].monitorId, other, SP_COUNTOF(other)) || !str_ieq(model, other)) continue;
            int connected = 0;
            for (int k = 0; k < n && !connected; k++) connected = str_ieq(ids[k], c->assign[a].monitorId);
            if (!connected) { found = a; count++; }
        }
        if (count == 1) {
            str_copy(c->assign[found].monitorId, SP_MONID_LEN, ids[m]);
            changed++;
        }
    }
    return changed;
}

const wchar_t *config_screensaver(const Config *c)
{
    if (config_index_of(c, c->screensaver) >= 0) return c->screensaver;
    return config_index_of(c, c->defaultWallpaper) >= 0 ? c->defaultWallpaper : L"";
}

/* ---------------------------------------------------------------- parsing */

static int parse_bool(const wchar_t *v, int def)
{
    if (str_ieq(v, L"1") || str_ieq(v, L"true") || str_ieq(v, L"yes") || str_ieq(v, L"on")) return 1;
    if (str_ieq(v, L"0") || str_ieq(v, L"false") || str_ieq(v, L"no") || str_ieq(v, L"off")) return 0;
    return def;
}

static int64_t parse_i64(const wchar_t *v)
{
    int64_t n = _wcstoi64(v, NULL, 10);
    return n < 0 ? 0 : n;
}

typedef enum { SEC_NONE, SEC_APP, SEC_SETTINGS, SEC_ASSIGN, SEC_WALLPAPER, SEC_UNKNOWN } Section;

static void finish_entry(Config *c, WallpaperEntry *e, int *open)
{
    if (!*open) return;
    *open = 0;
    if (!e->path[0]) return;
    if (e->type < WP_IMAGE || e->type > WP_HTML) e->type = wallpaper_is_url(e->path) ? WP_HTML : wallpaper_type_from_path(e->path);
    if (e->type == WP_NONE) return;
    if (!valid_id(e->id) || config_index_of(c, e->id) >= 0) random_id(e->id);
    if (config_find_path(c, e->path)) return;              /* duplicate path */
    if (!e->name[0]) path_stem(e->name, SP_COUNTOF(e->name), e->path);
    str_sanitize_line(e->name);
    if (c->count >= CONFIG_MAX_ITEMS || !ensure_cap(c, c->count + 1)) return;
    c->items[c->count++] = *e;
}

static int parse_text(Config *c, wchar_t *text)
{
    Section sec = SEC_NONE;
    int sawHeader = 0, open = 0;
    WallpaperEntry cur;
    memset(&cur, 0, sizeof cur);
    wchar_t *ctx = NULL;
    for (wchar_t *line = wcstok_s(text, L"\r\n", &ctx); line; line = wcstok_s(NULL, L"\r\n", &ctx)) {
        str_trim(line);
        if (!line[0] || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            wchar_t *end = wcschr(line, L']');
            if (!end) continue;
            *end = 0;
            const wchar_t *name = line + 1;
            if (sec == SEC_WALLPAPER) finish_entry(c, &cur, &open);
            if (str_ieq(name, L"screenplus")) { sec = SEC_APP; sawHeader = 1; }
            else if (str_ieq(name, L"settings")) sec = SEC_SETTINGS;
            else if (str_ieq(name, L"assignments")) sec = SEC_ASSIGN;
            else if (str_ieq(name, L"wallpaper")) {
                sec = SEC_WALLPAPER;
                memset(&cur, 0, sizeof cur);
                open = 1;
            } else sec = SEC_UNKNOWN;
            continue;
        }
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t *key = line, *val = eq + 1;
        str_trim(key);
        str_trim(val);
        switch (sec) {
        case SEC_APP:
            if (str_ieq(key, L"schema")) c->schema = (int)parse_i64(val);
            break;
        case SEC_SETTINGS: {
            Settings *s = &c->s;
            if (str_ieq(key, L"start_with_windows")) s->startWithWindows = parse_bool(val, s->startWithWindows);
            else if (str_ieq(key, L"launch_to_tray")) s->launchToTray = parse_bool(val, s->launchToTray);
            else if (str_ieq(key, L"pause_on_fullscreen")) s->pauseOnFullscreen = parse_bool(val, s->pauseOnFullscreen);
            else if (str_ieq(key, L"pause_when_covered")) s->pauseWhenCovered = parse_bool(val, s->pauseWhenCovered);
            else if (str_ieq(key, L"suspend_on_lock")) s->suspendOnLock = parse_bool(val, s->suspendOnLock);
            else if (str_ieq(key, L"pause_on_battery")) s->pauseOnBattery = parse_bool(val, s->pauseOnBattery);
            else if (str_ieq(key, L"audio")) s->audio = parse_bool(val, s->audio);
            else if (str_ieq(key, L"auto_recover")) s->autoRecover = parse_bool(val, s->autoRecover);
            else if (str_ieq(key, L"paused")) s->paused = parse_bool(val, s->paused);
            else if (str_ieq(key, L"html_mouse")) s->htmlMouse = parse_bool(val, s->htmlMouse);
            else if (str_ieq(key, L"html_audio")) s->htmlAudio = parse_bool(val, s->htmlAudio);
            else if (str_ieq(key, L"lively_auto_import")) s->livelyImport = parse_bool(val, s->livelyImport);
            else if (str_ieq(key, L"url_import")) s->urlImport = parse_bool(val, s->urlImport);
            else if (str_ieq(key, L"interactive_images")) s->depthAll = parse_bool(val, s->depthAll);
            else if (str_ieq(key, L"display_mode"))
                s->displayMode = str_ieq(val, L"span") ? DM_SPAN : str_ieq(val, L"duplicate") ? DM_DUPLICATE : DM_INDEPENDENT;
            else if (str_ieq(key, L"screensaver_audio")) s->scrAudio = parse_bool(val, s->scrAudio);
            else if (str_ieq(key, L"screensaver_displays"))
                s->scrDisplays = str_ieq(val, L"span") ? SCRD_SPAN : str_ieq(val, L"primary") ? SCRD_PRIMARY : SCRD_ALL;
            else if (str_ieq(key, L"screensaver_exit_on_mouse")) s->scrExitOnMouse = parse_bool(val, s->scrExitOnMouse);
            else if (str_ieq(key, L"theme"))
                s->theme = str_ieq(val, L"light") ? THEME_LIGHT : str_ieq(val, L"dark") ? THEME_DARK : THEME_SYSTEM;
            break;
        }
        case SEC_ASSIGN:
            if (!valid_id(val)) break;
            if (str_ieq(key, L"default")) str_copy(c->defaultWallpaper, SP_ID_LEN, val);
            else if (str_ieq(key, L"screensaver")) str_copy(c->screensaver, SP_ID_LEN, val);
            else if (c->assignCount < CONFIG_MAX_ASSIGN && wcslen(key) < SP_MONID_LEN) {
                str_copy(c->assign[c->assignCount].monitorId, SP_MONID_LEN, key);
                str_copy(c->assign[c->assignCount].wallpaperId, SP_ID_LEN, val);
                c->assignCount++;
            }
            break;
        case SEC_WALLPAPER:
            if (str_ieq(key, L"id")) str_copy(cur.id, SP_ID_LEN, val);
            else if (str_ieq(key, L"type"))
                cur.type = str_ieq(val, L"image") ? WP_IMAGE : str_ieq(val, L"video") ? WP_VIDEO
                         : str_ieq(val, L"html") ? WP_HTML : WP_NONE;
            else if (str_ieq(key, L"name")) str_copy(cur.name, SP_COUNTOF(cur.name), val);
            else if (str_ieq(key, L"path")) str_copy(cur.path, SP_COUNTOF(cur.path), val);
            else if (str_ieq(key, L"added")) cur.added = parse_i64(val);
            else if (str_ieq(key, L"last_used")) cur.lastUsed = parse_i64(val);
            else if (str_ieq(key, L"format"))
                cur.format = str_ieq(val, L"lively") ? WPF_LIVELY : str_ieq(val, L"we") ? WPF_WE : WPF_PLAIN;
            else if (str_ieq(key, L"flags")) cur.flags = (int)(parse_i64(val) & 0xff);
            else if (str_ieq(key, L"thumbnail")) str_copy(cur.thumb, SP_COUNTOF(cur.thumb), val);
            break;
        default:
            break;
        }
    }
    if (sec == SEC_WALLPAPER) finish_entry(c, &cur, &open);

    /* drop dangling references */
    if (config_index_of(c, c->defaultWallpaper) < 0) c->defaultWallpaper[0] = 0;
    if (config_index_of(c, c->screensaver) < 0) c->screensaver[0] = 0;
    for (int a = 0; a < c->assignCount;) {
        if (config_index_of(c, c->assign[a].wallpaperId) < 0) c->assign[a] = c->assign[--c->assignCount];
        else a++;
    }
    return sawHeader;
}

static wchar_t *read_text_file(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    wchar_t *text = NULL;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < CONFIG_MAX_FILE) {
        char *buf = (char *)malloc((size_t)size.QuadPart + 1);
        DWORD rd = 0;
        if (buf && ReadFile(f, buf, (DWORD)size.QuadPart, &rd, NULL)) {
            buf[rd] = 0;
            const char *p = buf;
            if (rd >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
                p += 3;
                rd -= 3;
            }
            text = wide_from_utf8(p, (int)rd);
        }
        free(buf);
    }
    CloseHandle(f);
    return text;
}

static BOOL load_from(Config *c, const wchar_t *path)
{
    wchar_t *text = read_text_file(path);
    if (!text) return FALSE;
    config_defaults(c);
    c->count = 0;
    c->schema = 0;
    int ok = parse_text(c, text);
    free(text);
    if (!ok) return FALSE;
    if (c->schema > CONFIG_SCHEMA) LOGW(L"config schema %d is newer than supported %d; unknown keys ignored", c->schema, CONFIG_SCHEMA);
    c->schema = CONFIG_SCHEMA;
    return TRUE;
}

BOOL config_load(Config *c, const wchar_t *path)
{
    if (load_from(c, path)) return TRUE;
    if (file_exists(path)) LOGW(L"config %ls unreadable or corrupt; trying backup", path);
    wchar_t bak[SP_MAX_PATH];
    path_dirname(bak, SP_COUNTOF(bak), path);
    path_join(bak, SP_COUNTOF(bak), bak, L"config.bak");
    if (load_from(c, bak)) {
        LOGW(L"configuration restored from backup");
        return TRUE;
    }
    config_defaults(c);
    c->count = 0;
    return FALSE;
}

static const char *type_key(WallpaperType t)
{
    return t == WP_IMAGE ? "image" : t == WP_VIDEO ? "video" : t == WP_HTML ? "html" : "none";
}

BOOL config_save(const Config *c, const wchar_t *path)
{
    StrBuf b = { 0 };
    const Settings *s = &c->s;
    sb_puts(&b, "; Screen+ configuration. Managed by the application; edit while Screen+ is not running.\r\n");
    sb_printf(&b, "[screenplus]\r\nschema=%d\r\n\r\n", CONFIG_SCHEMA);
    sb_printf(&b, "[settings]\r\nstart_with_windows=%d\r\nlaunch_to_tray=%d\r\npause_on_fullscreen=%d\r\n"
                  "pause_when_covered=%d\r\nsuspend_on_lock=%d\r\npause_on_battery=%d\r\naudio=%d\r\n"
                  "auto_recover=%d\r\npaused=%d\r\nhtml_mouse=%d\r\nhtml_audio=%d\r\nlively_auto_import=%d\r\n"
                  "url_import=%d\r\ninteractive_images=%d\r\ntheme=%s\r\n\r\n",
              s->startWithWindows, s->launchToTray, s->pauseOnFullscreen, s->pauseWhenCovered, s->suspendOnLock,
              s->pauseOnBattery, s->audio, s->autoRecover, s->paused, s->htmlMouse, s->htmlAudio, s->livelyImport,
              s->urlImport, s->depthAll,
              s->displayMode == DM_SPAN ? "span" : s->displayMode == DM_DUPLICATE ? "duplicate" : "independent",
              s->scrAudio, s->scrDisplays == SCRD_SPAN ? "span" : s->scrDisplays == SCRD_PRIMARY ? "primary" : "all",
              s->scrExitOnMouse, s->theme == THEME_LIGHT ? "light" : s->theme == THEME_DARK ? "dark" : "system");
    sb_puts(&b, "[assignments]\r\n");
    if (c->defaultWallpaper[0]) {
        sb_puts(&b, "default=");
        sb_put_wide(&b, c->defaultWallpaper);
        sb_puts(&b, "\r\n");
    }
    for (int a = 0; a < c->assignCount; a++) {
        sb_put_wide(&b, c->assign[a].monitorId);
        sb_puts(&b, "=");
        sb_put_wide(&b, c->assign[a].wallpaperId);
        sb_puts(&b, "\r\n");
    }
    for (int i = 0; i < c->count; i++) {
        const WallpaperEntry *e = &c->items[i];
        wchar_t name[SP_NAME_LEN];
        str_copy(name, SP_COUNTOF(name), e->name);
        str_sanitize_line(name);
        sb_puts(&b, "\r\n[wallpaper]\r\nid=");
        sb_put_wide(&b, e->id);
        sb_printf(&b, "\r\ntype=%s\r\nname=", type_key(e->type));
        sb_put_wide(&b, name);
        sb_puts(&b, "\r\npath=");
        sb_put_wide(&b, e->path);
        sb_printf(&b, "\r\nadded=%lld\r\nlast_used=%lld\r\n", (long long)e->added, (long long)e->lastUsed);
        if (e->format) sb_printf(&b, "format=%s\r\n", e->format == WPF_LIVELY ? "lively" : "we");
        if (e->flags) sb_printf(&b, "flags=%d\r\n", e->flags);
        if (e->thumb[0]) {
            sb_puts(&b, "thumbnail=");
            sb_put_wide(&b, e->thumb);
            sb_puts(&b, "\r\n");
        }
    }
    if (b.failed) { sb_free(&b); return FALSE; }

    wchar_t tmp[SP_MAX_PATH], bak[SP_MAX_PATH], dir[SP_MAX_PATH];
    str_printf(tmp, SP_COUNTOF(tmp), L"%ls.tmp", path);
    path_dirname(dir, SP_COUNTOF(dir), path);
    path_join(bak, SP_COUNTOF(bak), dir, L"config.bak");
    dir_create_recursive(dir);

    BOOL ok = FALSE;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(f, b.p, (DWORD)b.len, &wr, NULL) && wr == b.len && FlushFileBuffers(f);
        CloseHandle(f);
    }
    sb_free(&b);
    if (!ok) {
        LOGE(L"config: failed writing %ls (err %lu)", tmp, GetLastError());
        DeleteFileW(tmp);
        return FALSE;
    }
    if (file_exists(path)) {
        ok = ReplaceFileW(path, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, NULL, NULL);
        if (!ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
        ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) {
        LOGE(L"config: failed replacing %ls (err %lu)", path, GetLastError());
        DeleteFileW(tmp);
    }
    return ok;
}
