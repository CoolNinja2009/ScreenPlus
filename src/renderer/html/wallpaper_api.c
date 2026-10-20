#include "wallpaper_api.h"
#include "../../common/util.h"
#include "../../common/json.h"
#include "../../common/paths.h"
#include "../../common/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kept as one ES5 function so it also runs in old wallpaper code bases. Every call into page
 * code is wrapped in try/catch: a broken wallpaper must not break the host's next call. */
static const wchar_t k_shim[] =
    L"(function(){"
    L"if(window.__sp)return;"
    L"var audio=[];"
    L"function call(n,a){var f=window[n];if(typeof f==='function'){try{f.apply(window,a);}catch(e){}}}"
    L"window.__sp={"
    L"wants:function(){var m=0;"
    L"if(audio.length||typeof window.livelyAudioListener==='function')m|=1;"
    L"if(typeof window.livelySystemInformation==='function')m|=2;"
    L"if(typeof window.livelyCurrentTrack==='function')m|=4;return m;},"
    /* Host spectrum: 64 bands, low to high. Wallpaper Engine expects 128 values (64 left + 64 right);
     * Lively expects 128 bands from low to high. Both are built from the mono spectrum here. */
    L"audio:function(a){var n=a.length,we=a.concat(a),lv=new Array(n*2);"
    L"for(var i=0;i<n*2;i++)lv[i]=a[i>>1];"
    L"for(var j=0;j<audio.length;j++){try{audio[j](we);}catch(e){}}call('livelyAudioListener',[lv]);},"
    L"sysinfo:function(s){call('livelySystemInformation',[JSON.stringify(s)]);},"
    L"track:function(s){call('livelyCurrentTrack',[s]);},"
    L"playback:function(p){call('livelyWallpaperPlaybackChanged',[JSON.stringify({IsPaused:p})]);"
    L"var l=window.wallpaperPropertyListener;if(l&&typeof l.setPaused==='function'){try{l.setPaused(p);}catch(e){}}}"
    L"};"
    L"window.wallpaperRegisterAudioListener=function(cb){if(typeof cb==='function')audio.push(cb);};"
    L"var noop=function(){};"
    L"['wallpaperRegisterMediaStatusListener','wallpaperRegisterMediaPropertiesListener',"
    L"'wallpaperRegisterMediaThumbnailListener','wallpaperRegisterMediaPlaybackListener',"
    L"'wallpaperRegisterMediaTimelineListener','wallpaperRequestRandomFileForProperty']"
    L".forEach(function(n){if(!window[n])window[n]=noop;});"
    L"})();";

const wchar_t *wpapi_shim_js(void) { return k_shim; }

void wpapi_overrides_path(const wchar_t *wallpaperId, wchar_t *out, size_t cap)
{
    wchar_t dir[SP_MAX_PATH], name[SP_ID_LEN + 8];
    path_join(dir, SP_COUNTOF(dir), paths_root(), L"props");
    str_printf(name, SP_COUNTOF(name), L"%ls.json", wallpaperId);
    path_join(out, cap, dir, name);
}

/* Lively keeps user customizations in <Library>\SaveData\wpdata\<folder>[\<screen>]\LivelyProperties.json */
static JVal *lively_saved_properties(const wchar_t *folder)
{
    wchar_t parent[SP_MAX_PATH], library[SP_MAX_PATH], base[SP_MAX_PATH], p[SP_MAX_PATH];
    path_dirname(parent, SP_COUNTOF(parent), folder);           /* ...\Library\wallpapers */
    if (!str_ieq(path_filename(parent), L"wallpapers")) return NULL;
    path_dirname(library, SP_COUNTOF(library), parent);         /* ...\Library */
    path_join(base, SP_COUNTOF(base), library, L"SaveData\\wpdata");
    path_join(base, SP_COUNTOF(base), base, path_filename(folder));
    path_join(p, SP_COUNTOF(p), base, L"LivelyProperties.json");
    JVal *j = json_parse_file(p);
    if (j) return j;
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), base, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t sub[SP_MAX_PATH];
        path_join(sub, SP_COUNTOF(sub), base, fd.cFileName);
        path_join(p, SP_COUNTOF(p), sub, L"LivelyProperties.json");
        j = json_parse_file(p);
    } while (!j && FindNextFileW(h, &fd));
    FindClose(h);
    return j;
}

static void write_value_from(StrBuf *b, const JVal *prop, const JVal *over, const char *type)
{
    const JVal *v = over ? over : json_get(prop, "value");
    if (!_stricmp(type, "checkbox")) sb_puts(b, json_bool(v, 0) ? "true" : "false");
    else if (!_stricmp(type, "slider") || !_stricmp(type, "dropdown") || !_stricmp(type, "scalarDropdown")) {
        char t[48];
        double d = json_num(v, 0.0);
        snprintf(t, sizeof t, "%.10g", d);
        sb_puts(b, t);
    } else if (!_stricmp(type, "folderDropdown")) {
        const char *folder = json_str(json_get(prop, "folder"), "");
        const char *file = json_str(v, "");
        StrBuf s = { 0 };
        if (file[0]) {
            sb_puts(&s, folder);
            if (folder[0]) sb_puts(&s, "/");
            sb_puts(&s, file);
        }
        json_write_string(b, s.p ? s.p : "");
        sb_free(&s);
    } else {
        json_write(b, v);   /* color, textbox and anything newer: pass through */
    }
}

static char *lively_js(const WallpaperEntry *wp, const JVal *over)
{
    wchar_t folder[SP_MAX_PATH], p[SP_MAX_PATH];
    path_dirname(folder, SP_COUNTOF(folder), wp->path);
    JVal *props = lively_saved_properties(folder);
    if (!props) {
        path_join(p, SP_COUNTOF(p), folder, L"LivelyProperties.json");
        props = json_parse_file(p);
    }
    if (!props || props->t != J_OBJ) {
        json_free(props);
        return NULL;
    }
    StrBuf b = { 0 };
    sb_puts(&b, "(function(){var f=window.livelyPropertyListener;if(typeof f!=='function')return;");
    int count = 0;
    for (int i = 0; i < props->n; i++) {
        const JVal *prop = &props->kids[i];
        const char *type = json_str(json_get(prop, "type"), "");
        if (!type[0] || !_stricmp(type, "button") || !_stricmp(type, "label")) continue;
        sb_puts(&b, "try{f(");
        json_write_string(&b, props->keys[i]);
        sb_puts(&b, ",");
        write_value_from(&b, prop, json_get(over, props->keys[i]), type);
        sb_puts(&b, ");}catch(e){}");
        count++;
    }
    sb_puts(&b, "})();");
    json_free(props);
    if (b.failed || !count) {
        sb_free(&b);
        return NULL;
    }
    return b.p;
}

static char *we_js(const WallpaperEntry *wp, const JVal *over)
{
    wchar_t folder[SP_MAX_PATH], p[SP_MAX_PATH];
    path_dirname(folder, SP_COUNTOF(folder), wp->path);
    path_join(p, SP_COUNTOF(p), folder, L"project.json");
    JVal *proj = json_parse_file(p);
    const JVal *props = json_geti(json_geti(proj, "general"), "properties");
    StrBuf b = { 0 };
    sb_puts(&b, "(function(){var o={");
    int count = 0;
    for (int i = 0; props && props->t == J_OBJ && i < props->n; i++) {
        const JVal *prop = &props->kids[i];
        const JVal *v = json_get(over, props->keys[i]);
        if (!v) v = json_get(prop, "value");
        if (!v) continue;
        if (count++) sb_puts(&b, ",");
        json_write_string(&b, props->keys[i]);
        sb_puts(&b, ":{\"value\":");
        json_write(&b, v);
        sb_puts(&b, "}");
    }
    sb_puts(&b, "};var l=window.wallpaperPropertyListener;if(!l)return;"
                "try{if(l.applyGeneralProperties)l.applyGeneralProperties({fps:60});}catch(e){}"
                "try{if(l.applyUserProperties)l.applyUserProperties(o);}catch(e){}})();");
    json_free(proj);
    if (b.failed) {
        sb_free(&b);
        return NULL;
    }
    return b.p;
}

char *wpapi_properties_js(const WallpaperEntry *wp)
{
    wchar_t op[SP_MAX_PATH];
    wpapi_overrides_path(wp->id, op, SP_COUNTOF(op));
    JVal *over = file_exists(op) ? json_parse_file(op) : NULL;
    char *js = NULL;
    if (wp->format == WPF_LIVELY) js = lively_js(wp, over);
    else if (wp->format == WPF_WE) js = we_js(wp, over);
    json_free(over);
    return js;
}
