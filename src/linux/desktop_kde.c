/* KDE Plasma: the wallpaper runs inside plasmashell, in our wallpaper plugin (linux/kde/). The service
 * installs the plugin and drives it through Plasma's desktop scripting over D-Bus: each desktop's
 * plugin config is written, and the plugin's QML reacts to the change. */
#include "desktop.h"
#include "service.h"
#include <sys/stat.h>
#include <time.h>
#include "../common/json.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../renderer/html/wallpaper_api.h"

#define PLUGIN_ID "org.screenplus.wallpaper"

/* dbus-send prints the reply as:  method return ...\n   string "the text"\n */
static int plasma_script(const char *script, char *out, size_t cap)
{
    StrBuf arg = { 0 };
    sb_puts(&arg, "string:");
    sb_puts(&arg, script);
    if (arg.failed) { sb_free(&arg); return 0; }
    const char *argv[] = { "dbus-send", "--session", "--print-reply", "--reply-timeout=5000", "--dest=org.kde.plasmashell",
                           "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript", arg.p, NULL };
    static char reply[65536];
    int rc = sp_run(argv, reply, sizeof reply);
    sb_free(&arg);
    if (out && cap) {
        out[0] = 0;
        char *a = strchr(reply, '"'), *b = strrchr(reply, '"');
        if (a && b > a) {
            size_t n = (size_t)(b - a - 1);
            if (n >= cap) n = cap - 1;
            memcpy(out, a + 1, n);
            while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) n--;   /* print() ends the line */
            out[n] = 0;
        }
    }
    return rc == 0;
}

/* Plasma's scripting has no refresh rate; kscreen-doctor (part of Plasma) does. It is only asked when the layout
 * changes, since this runs on every poll. */
typedef struct { LONG x, y; UINT hz; } OutputHz;

static int read_output_hz(OutputHz *out, int max)
{
    static char text[262144];
    const char *argv[] = { "kscreen-doctor", "-j", NULL };
    if (sp_run(argv, text, sizeof text)) return 0;
    JVal *j = json_parse(text, strlen(text));
    const JVal *outs = j ? json_get(j, "outputs") : NULL;
    int n = 0;
    for (int i = 0; outs && outs->t == J_ARR && i < outs->n && n < max; i++) {
        const JVal *o = &outs->kids[i], *modes = json_get(o, "modes"), *pos = json_get(o, "pos");
        const char *now = json_str(json_get(o, "currentModeId"), "");
        if (!json_bool(json_get(o, "enabled"), 1) || !json_bool(json_get(o, "connected"), 1)) continue;
        for (int k = 0; modes && modes->t == J_ARR && k < modes->n; k++) {
            const JVal *m = &modes->kids[k];
            if (strcmp(json_str(json_get(m, "id"), ""), now)) continue;
            out[n].x = (LONG)json_num(json_get(pos, "x"), 0);
            out[n].y = (LONG)json_num(json_get(pos, "y"), 0);
            out[n++].hz = (UINT)(json_num(json_get(m, "refreshRate"), 0) + 0.5);
            break;
        }
    }
    json_free(j);
    return n;
}

int kde_monitors(MonitorInfo *out, int max)
{
    static OutputHz hz[SP_MAX_MONITORS];
    static int hzCount;
    static long hzKey = -1;
    static const char script[] =
        "var r=[];for(var i=0;i<screenCount;i++){var g=screenGeometry(i);"
        "r.push({x:g.x,y:g.y,w:g.width,h:g.height});}print(JSON.stringify(r));";
    static char text[16384];
    if (!plasma_script(script, text, sizeof text)) return 0;
    JVal *j = json_parse(text, strlen(text));
    int n = 0;
    for (int i = 0; j && j->t == J_ARR && i < j->n && n < max; i++) {
        const JVal *m = &j->kids[i];
        MonitorInfo *o = &out[n];
        memset(o, 0, sizeof *o);
        str_printf(o->id, SP_COUNTOF(o->id), L"screen-%d", i);
        str_printf(o->name, SP_COUNTOF(o->name), L"Screen %d", i + 1);
        o->rect.left = (LONG)json_num(json_get(m, "x"), 0);
        o->rect.top = (LONG)json_num(json_get(m, "y"), 0);
        o->rect.right = o->rect.left + (LONG)json_num(json_get(m, "w"), 0);
        o->rect.bottom = o->rect.top + (LONG)json_num(json_get(m, "h"), 0);
        o->work = o->rect;
        o->dpi = 96;
        o->primary = i == 0;
        o->index = ++n;
    }
    json_free(j);
    long key = n;
    for (int i = 0; i < n; i++)
        key = key * 31 + out[i].rect.left * 7 + out[i].rect.top * 5 + out[i].rect.right * 3 + out[i].rect.bottom;
    if (n && key != hzKey) {
        hzKey = key;
        hzCount = read_output_hz(hz, SP_MAX_MONITORS);
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < hzCount; k++)
            if (hz[k].x == out[i].rect.left && hz[k].y == out[i].rect.top) out[i].refreshHz = hz[k].hz;
    return n;
}

static int install_plugin(const char *id, const char *kind)
{
    char *dir = sp_path_utf8(paths_exe_dir());
    const char *home = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    char src[MAX_PATH], base[MAX_PATH], dst[MAX_PATH + 64];
    snprintf(src, sizeof src, "%s/kde/%s", dir ? dir : ".", id);
    free(dir);
    if (home && home[0]) snprintf(base, sizeof base, "%s/plasma/%s", home, kind);
    else snprintf(base, sizeof base, "%s/.local/share/plasma/%s", h ? h : "", kind);
    snprintf(dst, sizeof dst, "%s/%s", base, id);
    const char *same[] = { "diff", "-rq", src, dst, NULL };
    if (!sp_run(same, NULL, 0)) return 1;           /* never pull it from under a running Plasma without need */
    const char *mk[] = { "mkdir", "-p", base, NULL };
    const char *rm[] = { "rm", "-rf", dst, NULL };
    const char *cp[] = { "cp", "-r", src, dst, NULL };
    if (sp_run(mk, NULL, 0) || sp_run(rm, NULL, 0) || sp_run(cp, NULL, 0)) {
        LOGE(L"kde: could not install %hs", id);
        return 0;
    }
    return 1;
}

int kde_install_plugin(void)
{
    return install_plugin(PLUGIN_ID, "wallpapers") && install_plugin("org.screenplus.capture", "plasmoids");
}

/* The Lively / Wallpaper Engine shim as a file WebEngine can load before the page's own scripts. */
static char *shim_url(int fps)
{
    wchar_t path[SP_MAX_PATH], name[40];
    char limit[640];
    str_printf(name, SP_COUNTOF(name), L"wallpaper-api-%d.js", fps);     /* its own name: a changed limit reloads the page */
    path_join(path, SP_COUNTOF(path), paths_cache(), name);
    sp_fps_js(fps, limit, sizeof limit);
    char *js = utf8_from_wide(wpapi_shim_js(), NULL), *p = sp_path_utf8(path);
    FILE *f = p ? fopen(p, "w") : NULL;
    if (f) {
        fputs(limit, f);
        if (js) fputs(js, f);
        wchar_t feedPath[SP_MAX_PATH];
        path_join(feedPath, SP_COUNTOF(feedPath), paths_cache(), L"feed.js");
        char *fp = sp_path_utf8(feedPath), line[1024];
        FILE *feed = fp ? fopen(fp, "r") : NULL;
        free(fp);
        if (feed) {
            while (fgets(line, sizeof line, feed)) fputs(line, f);
            fclose(feed);
        }
        fclose(f);
    }
    free(js);
    free(p);
    return sp_file_uri(path);
}

static char *page_address(const WallpaperEntry *e)
{
    char *hosted = webhost_url(e);
    return hosted ? hosted : wallpaper_is_url(e->path) ? utf8_from_wide(e->path, NULL) : sp_file_uri(e->path);
}

void kde_page_args(const WallpaperEntry *e, char **url, char **shim, char **script)
{
    JVal *sent = NULL;
    *url = page_address(e);
    *shim = shim_url(0);
    *script = wpapi_properties_js(e, &sent);
    json_free(sent);
}

/* What a stream wallpaper plays right now: yt-dlp's address, kept for two hours (they expire). Sites with no
 * combined stream give the picture alone, as a muted wallpaper wants it anyway. */
static char *stream_address(const wchar_t *link)
{
    wchar_t dir[SP_MAX_PATH], name[32], file[SP_MAX_PATH];
    path_join(dir, SP_COUNTOF(dir), paths_cache(), L"streams");
    dir_create_recursive(dir);
    str_printf(name, SP_COUNTOF(name), L"%08x.url", hash_fnv1a_w(link));
    path_join(file, SP_COUNTOF(file), dir, name);
    char *p = sp_path_utf8(file), *u = utf8_from_wide(link, NULL);
    static char out[8192];
    out[0] = 0;
    struct stat st;
    if (p && !stat(p, &st) && time(NULL) - st.st_mtime < 7200) {
        FILE *f = fopen(p, "r");
        if (f) {
            if (!fgets(out, sizeof out, f)) out[0] = 0;
            fclose(f);
        }
    }
    if (!out[0] && u) {
        const char *argv[] = { "yt-dlp", "-g", "--no-playlist", "-f", "b[protocol^=http]/bv*[height<=1080][vcodec^=avc1][protocol^=http]/bv*[height<=1080][protocol^=http]/b", u, NULL };
        if (!sp_run(argv, out, sizeof out)) {
            out[strcspn(out, "\r\n")] = 0;
            FILE *f = p ? fopen(p, "w") : NULL;
            if (f) {
                fputs(out, f);
                fclose(f);
            }
        } else {
            out[0] = 0;
            LOGW(L"kde: yt-dlp could not read %ls", link);
        }
    }
    free(p);
    free(u);
    return out[0] ? strdup(out) : NULL;
}

/* Customise recordings for the plugin to play (Recording.qml): {"we":0|1,"look":rec,"props":rec}; NULL when none. */
static char *recording_json(const WallpaperEntry *e)
{
    StrBuf b = { 0 };
    int any = 0;
    sb_printf(&b, "{\"we\":%d", e->format == WPF_WE);
    for (int s = 0; s < REC_SECTIONS; s++) {
        Rec r;
        if (!wprec_load(e->id, s, &r)) continue;
        if (r.paused > 0) {                         /* paused: the section's own settings hold */
            wprec_free(&r);
            continue;
        }
        char *j = wprec_json(&r);
        if (j) {
            sb_printf(&b, ",\"%s\":", s == REC_LOOK ? "look" : "props");
            sb_puts(&b, j);
            any = 1;
        }
        free(j);
        wprec_free(&r);
    }
    sb_puts(&b, "}");
    if (!any || b.failed) {
        sb_free(&b);
        return NULL;
    }
    return b.p;
}

static void put_wallpaper(StrBuf *b, const Config *c, const wchar_t *id, int paused, const RECT *span)
{
    const WallpaperEntry *e = id && id[0] ? config_find((Config *)c, id) : NULL;
    char *uri = !e ? NULL : (e->flags & WPFLAG_YTDLP) ? stream_address(e->path)
              : page_address(e);
    WallpaperAdjust a = adjust_defaults();
    if (e) wpapi_load_adjust(e->id, &a);
    sb_puts(b, "{\"Source\":");
    json_write_string(b, uri ? uri : "");
    sb_printf(b, ",\"Kind\":\"%s\"", !e ? "none" : e->type == WP_HTML ? "html" : e->type == WP_IMAGE ? "image" : "video");
    sb_printf(b, ",\"Muted\":%s,\"Paused\":%s,\"PauseOnFullscreen\":%s", c->s.audio && !g_micBusy ? "false" : "true",
              paused ? "true" : "false", c->s.pauseOnFullscreen ? "true" : "false");
    char *apps = utf8_from_wide(c->s.pauseApps, NULL);
    sb_puts(b, ",\"PauseApps\":");
    json_write_string(b, apps ? apps : "");
    free(apps);
    sb_printf(b, ",\"Mouse\":%s,\"PauseWhenCovered\":%s", c->s.htmlMouse ? "true" : "false", c->s.pauseWhenCovered ? "true" : "false");
    sb_printf(b, ",\"SpanX\":%d,\"SpanY\":%d,\"SpanW\":%d,\"SpanH\":%d", span ? span->left : 0, span ? span->top : 0,
              span ? span->right - span->left : 0, span ? span->bottom - span->top : 0);
    sb_printf(b, ",\"Brightness\":%d,\"Contrast\":%d,\"Saturation\":%d", a.brightness, a.contrast, a.saturation);
    sb_printf(b, ",\"Upscale\":%d", e && e->type != WP_HTML ? wpapi_load_upscale(e->id) : 0);
    char *rec = e ? recording_json(e) : NULL;
    char *recUrl = rec ? webhost_recording(e->id, rec) : NULL;   /* too big for the script: the plugin fetches it */
    sb_puts(b, ",\"Recording\":");
    json_write_string(b, recUrl ? recUrl : rec && strlen(rec) < 16384 ? rec : "");
    if (rec && !recUrl && strlen(rec) >= 16384) LOGW(L"kde: no web host, so the recording can't reach Plasma");
    free(recUrl);
    free(rec);
    if (e && e->type == WP_HTML) {
        char *shim = shim_url(c->s.webFps);
        JVal *sent = NULL;
        char *js = wpapi_properties_js(e, &sent);
        sb_puts(b, ",\"ShimUrl\":");
        json_write_string(b, shim ? shim : "");
        sb_puts(b, ",\"Script\":");
        json_write_string(b, js ? js : "");
        json_free(sent);
        free(js);
        free(shim);
    }
    sb_puts(b, "}");
    free(uri);
}

int kde_apply(const Config *c, const MonitorInfo *mons, int monCount, int paused)
{
    StrBuf s = { 0 };
    sb_puts(&s, "var cfg=[");
    for (int i = 0; i < SP_MAX(monCount, 1); i++) {
        if (i) sb_puts(&s, ",");
        const wchar_t *id = monCount ? config_assignment(c, mons[i].id) : c->defaultWallpaper;
        /* span: every display shows its part of one picture the size of them all */
        RECT part = { 0 }, *span = NULL;
        if (c->s.displayMode == DM_SPAN && monCount > 1) {
            RECT all = mons[0].rect;
            for (int k = 1; k < monCount; k++) {
                all.left = SP_MIN(all.left, mons[k].rect.left);
                all.top = SP_MIN(all.top, mons[k].rect.top);
                all.right = SP_MAX(all.right, mons[k].rect.right);
                all.bottom = SP_MAX(all.bottom, mons[k].rect.bottom);
            }
            part.left = mons[i].rect.left - all.left;
            part.top = mons[i].rect.top - all.top;
            part.right = part.left + (all.right - all.left);
            part.bottom = part.top + (all.bottom - all.top);
            span = &part;
        }
        put_wallpaper(&s, c, id, paused, span);
    }
    sb_puts(&s, "];var ds=desktops();for(var i=0;i<ds.length;i++){var d=ds[i];if(d.screen<0)continue;"
                "var c=cfg[d.screen]||cfg[0];d.wallpaperPlugin=\"" PLUGIN_ID "\";"
                "d.currentConfigGroup=[\"Wallpaper\",\"" PLUGIN_ID "\",\"General\"];"
                "for(var k in c)d.writeConfig(k,c[k]);d.reloadConfig();}");
    int ok = !s.failed && plasma_script(s.p, NULL, 0);
    sb_free(&s);
    if (!ok) LOGE(L"kde: plasmashell refused the wallpaper (are the desktop widgets locked?)");
    return ok;
}

void kde_restore(void)
{
    plasma_script("var ds=desktops();for(var i=0;i<ds.length;i++){ds[i].wallpaperPlugin=\"org.kde.image\";ds[i].reloadConfig();}", NULL, 0);
}
