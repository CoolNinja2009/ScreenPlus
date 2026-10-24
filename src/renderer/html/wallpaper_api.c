#include "wallpaper_api.h"
#include "../../common/util.h"
#include "../../common/json.h"
#include "../../common/paths.h"
#include "../../common/log.h"
#include <shlwapi.h>
#include <math.h>
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

/* The package's property list: Lively (saved customisations first, then the package file)
 * or Wallpaper Engine (project.json general.properties). *owner receives what to free. */
static const JVal *package_properties(const WallpaperEntry *wp, wchar_t *folder, size_t fcap, JVal **owner)
{
    wchar_t p[SP_MAX_PATH];
    *owner = NULL;
    path_dirname(folder, fcap, wp->path);
    if (wp->format == WPF_LIVELY) {
        JVal *props = lively_saved_properties(folder);
        if (!props) {
            path_join(p, SP_COUNTOF(p), folder, L"LivelyProperties.json");
            props = json_parse_file(p);
        }
        *owner = props;
        return props && props->t == J_OBJ ? props : NULL;
    }
    if (wp->format == WPF_WE) {
        path_join(p, SP_COUNTOF(p), folder, L"project.json");
        *owner = json_parse_file(p);
        const JVal *props = json_geti(json_geti(*owner, "general"), "properties");
        return props && props->t == J_OBJ ? props : NULL;
    }
    return NULL;
}

/* A stored override. Files written by the first v1.3 build wrapped values as {"v": value};
 * property values are always scalars, so such an object is unwrapped. */
static const JVal *override_value(const JVal *over, const char *key)
{
    const JVal *v = json_get(over, key);
    if (v && v->t == J_OBJ && v->n == 1 && v->keys[0] && !strcmp(v->keys[0], "v")) return &v->kids[0];
    return v;
}

/* {"name": value} with the value each property hands the page right now (Lively: the
 * livelyPropertyListener argument; Wallpaper Engine: the raw property value). */
static JVal *property_values(const WallpaperEntry *wp, const JVal *over)
{
    wchar_t folder[SP_MAX_PATH];
    JVal *owner = NULL;
    const JVal *props = package_properties(wp, folder, SP_COUNTOF(folder), &owner);
    StrBuf b = { 0 };
    sb_puts(&b, "{");
    int count = 0;
    for (int i = 0; props && i < props->n; i++) {
        const JVal *prop = &props->kids[i];
        const char *key = props->keys[i];
        if (!key || !key[0]) continue;
        const JVal *ov = override_value(over, key);
        if (wp->format == WPF_LIVELY) {
            const char *type = json_str(json_get(prop, "type"), "");
            if (!type[0] || !_stricmp(type, "button") || !_stricmp(type, "label")) continue;
            if (count++) sb_puts(&b, ",");
            json_write_string(&b, key);
            sb_puts(&b, ":");
            write_value_from(&b, prop, ov, type);
        } else {
            const JVal *v = ov ? ov : json_get(prop, "value");
            if (!v) continue;
            if (count++) sb_puts(&b, ",");
            json_write_string(&b, key);
            sb_puts(&b, ":");
            json_write(&b, v);
        }
    }
    sb_puts(&b, "}");
    JVal *vals = b.failed ? NULL : json_parse(b.p, b.len);
    sb_free(&b);
    json_free(owner);
    return vals;
}

static int jval_same(const JVal *a, const JVal *b)
{
    if (!a || !b) return a == b;
    StrBuf x = { 0 }, y = { 0 };
    json_write(&x, a);
    json_write(&y, b);
    int same = !x.failed && !y.failed && x.len == y.len && (!x.len || !memcmp(x.p, y.p, x.len));
    sb_free(&x);
    sb_free(&y);
    return same;
}

char *wpapi_properties_js(const WallpaperEntry *wp, JVal **sent)
{
    if (!wp || (wp->format != WPF_LIVELY && wp->format != WPF_WE)) return NULL;
    wchar_t op[SP_MAX_PATH];
    wpapi_overrides_path(wp->id, op, SP_COUNTOF(op));
    JVal *over = file_exists(op) ? json_parse_file(op) : NULL;
    JVal *vals = property_values(wp, over);
    json_free(over);
    if (!vals || vals->t != J_OBJ) {
        json_free(vals);
        return NULL;
    }
    const JVal *prev = sent ? *sent : NULL;
    int lively = wp->format == WPF_LIVELY, count = 0;
    StrBuf b = { 0 };
    sb_puts(&b, lively ? "(function(){var f=window.livelyPropertyListener;if(typeof f!=='function')return;"
                       : "(function(){var o={");
    for (int i = 0; i < vals->n; i++) {
        const char *key = vals->keys[i];
        if (!key || (prev && jval_same(json_get(prev, key), &vals->kids[i]))) continue;   /* page has it */
        if (lively) {
            sb_puts(&b, "try{f(");
            json_write_string(&b, key);
            sb_puts(&b, ",");
            json_write(&b, &vals->kids[i]);
            sb_puts(&b, ");}catch(e){}");
        } else {
            if (count) sb_puts(&b, ",");
            json_write_string(&b, key);
            sb_puts(&b, ":{\"value\":");
            json_write(&b, &vals->kids[i]);
            sb_puts(&b, "}");
        }
        count++;
    }
    if (lively) {
        sb_puts(&b, "})();");
    } else {
        sb_puts(&b, "};var l=window.wallpaperPropertyListener;if(!l)return;");
        if (!prev) sb_puts(&b, "try{if(l.applyGeneralProperties)l.applyGeneralProperties({fps:60});}catch(e){}");
        sb_puts(&b, "try{if(l.applyUserProperties)l.applyUserProperties(o);}catch(e){}})();");
    }
    int send = !b.failed && (count || (!prev && !lively));
    if (sent) {
        json_free(*sent);
        *sent = vals;
    } else {
        json_free(vals);
    }
    if (!send) {
        sb_free(&b);
        return NULL;
    }
    return b.p;
}

/* ================================================================ customise panel */

static const JVal k_jnull = { J_NULL };

void wpapi_jval_copy(JVal **dst, const JVal *src)
{
    if (!src) src = &k_jnull;
    StrBuf b = { 0 };
    json_write(&b, src);
    if (!b.failed) *dst = json_parse(b.p ? b.p : "", b.len);
    else *dst = NULL;
    sb_free(&b);
}

static CustomType prop_type(const JVal *prop, WallpaperFormat fmt)
{
    const char *t = json_str(json_get(prop, "type"), "");
    if (!_stricmp(t, "slider")) return CP_SLIDER;
    if (!_stricmp(t, "checkbox") || !_stricmp(t, "bool")) return CP_CHECKBOX;
    if (!_stricmp(t, "dropdown") || !_stricmp(t, "scalarDropdown")) return CP_DROPDOWN;   /* value = item index */
    if (!_stricmp(t, "combo")) return CP_COMBO;                                           /* WE: options[].value */
    if (!_stricmp(t, "folderDropdown")) return CP_FILE;                                   /* value = file name */
    if (!_stricmp(t, "color") || !_stricmp(t, "colorpicker")) return CP_COLOR;
    if (!_stricmp(t, "textbox") || !_stricmp(t, "textinput")) return CP_TEXT;
    if (!_stricmp(t, "label") || (fmt == WPF_WE && !_stricmp(t, "text"))) return CP_LABEL;
    return (CustomType)0;                            /* button, file, directory: not editable here */
}

/* folderDropdown choices: files in <wallpaper>\<folder> matching the "*.jpg|*.png" filter,
 * as a JSON array of file names (what Lively stores as the value). */
static JVal *folder_items(const wchar_t *wpFolder, const JVal *prop)
{
    wchar_t sub[SP_MAX_PATH], dir[SP_MAX_PATH], spec[256], pattern[SP_MAX_PATH];
    json_str_w(json_get(prop, "folder"), sub, SP_COUNTOF(sub));
    for (wchar_t *c = sub; *c; c++) if (*c == L'/') *c = L'\\';
    if (sub[0]) path_join(dir, SP_COUNTOF(dir), wpFolder, sub);
    else str_copy(dir, SP_COUNTOF(dir), wpFolder);
    json_str_w(json_get(prop, "filter"), spec, SP_COUNTOF(spec));
    if (!spec[0]) str_copy(spec, SP_COUNTOF(spec), L"*.*");
    for (wchar_t *c = spec; *c; c++) if (*c == L'|') *c = L';';       /* PathMatchSpec list syntax */
    path_join(pattern, SP_COUNTOF(pattern), dir, L"*");
    StrBuf b = { 0 };
    sb_puts(&b, "[");
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!PathMatchSpecW(fd.cFileName, spec)) continue;
            char *name = utf8_from_wide(fd.cFileName, NULL);
            if (!name) continue;
            if (n++) sb_puts(&b, ",");
            json_write_string(&b, name);
            free(name);
        } while (n < 256 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    sb_puts(&b, "]");
    JVal *items = b.failed ? NULL : json_parse(b.p, b.len);
    sb_free(&b);
    return items;
}

/* Rewrites the overrides file with `key` set to the raw JSON in `valueJson` (NULL: remove
 * the key). Other entries keep their order; the file is created when absent and deleted
 * when nothing is left. Returns 1 on success. */
static int save_override_file(const wchar_t *wallpaperId, const char *key, const char *valueJson)
{
    wchar_t p[SP_MAX_PATH];
    wpapi_overrides_path(wallpaperId, p, SP_COUNTOF(p));
    JVal *over = file_exists(p) ? json_parse_file(p) : NULL;
    StrBuf b = { 0 };
    sb_puts(&b, "{");
    int count = 0, replaced = 0;
    if (over && over->t == J_OBJ) {
        for (int i = 0; i < over->n; i++) {
            if (!over->keys[i]) continue;
            int match = !_stricmp(over->keys[i], key);
            if (match && !valueJson) continue;
            if (count++) sb_puts(&b, ",");
            json_write_string(&b, over->keys[i]);
            sb_puts(&b, ":");
            if (match) {
                sb_puts(&b, valueJson);
                replaced = 1;
            } else {
                json_write(&b, &over->kids[i]);
            }
        }
    }
    if (!replaced && valueJson) {
        if (count++) sb_puts(&b, ",");
        json_write_string(&b, key);
        sb_puts(&b, ":");
        sb_puts(&b, valueJson);
    }
    sb_puts(&b, "}");
    json_free(over);
    if (b.failed) {
        sb_free(&b);
        return 0;
    }
    if (!count) {
        sb_free(&b);
        DeleteFileW(p);
        return 1;
    }
    wchar_t dir[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), p);
    dir_create_recursive(dir);
    FILE *f = _wfopen(p, L"wb");
    if (!f) {
        sb_free(&b);
        return 0;
    }
    size_t n = b.len;
    int ok = fwrite(b.p, 1, n, f) == n;
    ok = !fclose(f) && ok;
    sb_free(&b);
    return ok;
}

int wpapi_save_override(const WallpaperEntry *wp, const char *name, const JVal *value)
{
    if (!wp || !wp->id[0] || !wp->format || !value) return 0;
    StrBuf b = { 0 };
    json_write(&b, value);
    int ok = !b.failed && b.p && save_override_file(wp->id, name, b.p);
    sb_free(&b);
    return ok;
}

void wpapi_clear_override(const wchar_t *wallpaperId)
{
    wchar_t p[SP_MAX_PATH];
    wpapi_overrides_path(wallpaperId, p, SP_COUNTOF(p));
    DeleteFileW(p);
}

void wpapi_clear_properties(const wchar_t *wallpaperId)
{
    wchar_t p[SP_MAX_PATH];
    wpapi_overrides_path(wallpaperId, p, SP_COUNTOF(p));
    JVal *over = file_exists(p) ? json_parse_file(p) : NULL;
    const JVal *a = json_geti(over, "_screenplus_adjust");
    StrBuf b = { 0 };
    if (a) json_write(&b, a);
    DeleteFileW(p);
    if (a && !b.failed && b.p) save_override_file(wallpaperId, "_screenplus_adjust", b.p);   /* keep the look */
    sb_free(&b);
    json_free(over);
}

BOOL wpapi_load_adjust(const wchar_t *wallpaperId, WallpaperAdjust *out)
{
    WallpaperAdjust d = adjust_defaults();
    *out = d;
    if (!wallpaperId || !wallpaperId[0]) return FALSE;
    wchar_t p[SP_MAX_PATH];
    wpapi_overrides_path(wallpaperId, p, SP_COUNTOF(p));
    JVal *over = file_exists(p) ? json_parse_file(p) : NULL;
    const JVal *a = json_geti(over, "_screenplus_adjust");
    if (!a) {
        json_free(over);
        return FALSE;
    }
    out->brightness = SP_CLAMP((int)json_num(json_get(a, "brightness"), 0), -100, 100);
    out->contrast = SP_CLAMP((int)json_num(json_get(a, "contrast"), 0), -100, 100);
    out->saturation = SP_CLAMP((int)json_num(json_get(a, "saturation"), 0), -100, 100);
    out->hue = SP_CLAMP((int)json_num(json_get(a, "hue"), 0), -180, 180);
    out->gamma = SP_CLAMP((int)json_num(json_get(a, "gamma"), 0), -100, 100);
    out->tint = (uint32_t)json_num(json_get(a, "tint"), (double)d.tint) & 0xFFFFFF;
    out->tintLift = SP_CLAMP((int)json_num(json_get(a, "tintLift"), 0), 0, 100);
    out->tintBrightness = SP_CLAMP((int)json_num(json_get(a, "tintBrightness"), (double)d.tintBrightness), 0, 100);
    out->used = adjust_active(out);
    json_free(over);
    return TRUE;
}

void wpapi_save_adjust(const wchar_t *wallpaperId, const WallpaperAdjust *a)
{
    if (!wallpaperId || !wallpaperId[0]) return;
    WallpaperAdjust d = adjust_defaults();
    if (!adjust_active(a) && a->tint == d.tint && a->tintBrightness == d.tintBrightness) {
        save_override_file(wallpaperId, "_screenplus_adjust", NULL);   /* back to neutral: drop the key */
        return;
    }
    char json[256];
    snprintf(json, sizeof json,
             "{\"used\":%d,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hue\":%d,"
             "\"gamma\":%d,\"tint\":%u,\"tintLift\":%d,\"tintBrightness\":%d}",
             adjust_active(a), a->brightness, a->contrast, a->saturation, a->hue, a->gamma,
             (unsigned)(a->tint & 0xFFFFFF), a->tintLift, a->tintBrightness);
    save_override_file(wallpaperId, "_screenplus_adjust", json);
}

CustomProp *wpapi_props_list(const WallpaperEntry *wp, int *count)
{
    *count = 0;
    if (!wp || !wp->format) return NULL;
    wchar_t folder[SP_MAX_PATH];
    JVal *owner = NULL;
    const JVal *props = package_properties(wp, folder, SP_COUNTOF(folder), &owner);
    if (!props || !props->n) {
        json_free(owner);
        return NULL;
    }
    wchar_t op[SP_MAX_PATH];
    wpapi_overrides_path(wp->id, op, SP_COUNTOF(op));
    JVal *over = file_exists(op) ? json_parse_file(op) : NULL;

    CustomProp *list = (CustomProp *)calloc((size_t)props->n, sizeof(CustomProp));
    if (!list) {
        json_free(owner);
        json_free(over);
        return NULL;
    }
    int n = 0;
    for (int i = 0; i < props->n && i < 128; i++) {
        const JVal *prop = &props->kids[i];
        const char *key = props->keys[i];
        if (!key || !key[0] || !_stricmp(key, "_screenplus_adjust")) continue;
        CustomType type = prop_type(prop, wp->format);
        if (!type) continue;
        CustomProp *cp = &list[n];
        size_t keyLen = SP_MIN(strlen(key), sizeof cp->name - 1);   /* util.h str_copyn is wide-only */
        memcpy(cp->name, key, keyLen);
        cp->name[keyLen] = 0;
        cp->type = type;
        json_str_w(json_get(prop, "text"), cp->label, SP_COUNTOF(cp->label));
        if (type == CP_LABEL && !cp->label[0]) json_str_w(json_get(prop, "value"), cp->label, SP_COUNTOF(cp->label));
        if (!cp->label[0]) str_printf(cp->label, SP_COUNTOF(cp->label), L"%.60hs", key);
        /* resolved value: Screen+ override > stored value (Lively-saved or package default) */
        const JVal *overV = override_value(over, key);
        wpapi_jval_copy(&cp->value, overV ? overV : json_get(prop, "value"));
        cp->overridden = overV != NULL;
        switch (type) {
        case CP_DROPDOWN: wpapi_jval_copy(&cp->items, json_get(prop, "items")); break;
        case CP_COMBO:    wpapi_jval_copy(&cp->items, json_get(prop, "options")); break;
        case CP_FILE:     cp->items = folder_items(folder, prop); break;
        case CP_SLIDER: {
            cp->min = json_num(json_get(prop, "min"), 0.0);
            cp->max = json_num(json_get(prop, "max"), 100.0);
            double defStep = json_bool(json_get(prop, "fraction"), 0) ? pow(10.0, -json_num(json_get(prop, "precision"), 2.0))
                                                                       : 1.0;
            cp->step = SP_MAX(json_num(json_get(prop, "step"), defStep), 1e-6);
            if (cp->max <= cp->min) cp->max = cp->min + 1.0;
            break;
        }
        default: break;
        }
        n++;
    }
    json_free(over);
    json_free(owner);
    *count = n;
    if (!n) free(list);
    return n ? list : NULL;
}

/* ---- typed values and choices, shared by the Customise page and the CLI */

JVal *wpapi_num_value(double v, double step)
{
    char t[48];
    int dec = step > 0.0 && step < 1.0 ? (int)ceil(-log10(step) - 1e-9) : 0;
    snprintf(t, sizeof t, "%.*f", SP_CLAMP(dec, 0, 8), v);
    return json_parse(t, strlen(t));
}

JVal *wpapi_bool_value(int on) { return json_parse(on ? "true" : "false", on ? 4 : 5); }

JVal *wpapi_str_value(const wchar_t *s)
{
    StrBuf b = { 0 };
    char *u = utf8_from_wide(s ? s : L"", NULL);
    json_write_string(&b, u ? u : "");
    free(u);
    JVal *j = b.failed ? NULL : json_parse(b.p, b.len);
    sb_free(&b);
    return j;
}

static void jval_text(const JVal *v, char *out, size_t cap)
{
    if (!v) out[0] = 0;
    else if (v->t == J_STR) snprintf(out, cap, "%s", v->str ? v->str : "");
    else if (v->t == J_BOOL) snprintf(out, cap, "%s", v->b ? "true" : "false");
    else snprintf(out, cap, "%.10g", json_num(v, 0.0));
}

int wpapi_choice_count(const CustomProp *cp)
{
    return cp && cp->items && cp->items->t == J_ARR ? cp->items->n : 0;
}

int wpapi_choice_current(const CustomProp *cp)
{
    int n = wpapi_choice_count(cp);
    if (cp->type == CP_DROPDOWN) {
        int i = (int)json_num(cp->value, -1.0);
        return i >= 0 && i < n ? i : -1;
    }
    char cur[256], it[256];
    jval_text(cp->value, cur, sizeof cur);
    for (int i = 0; i < n; i++) {
        const JVal *item = &cp->items->kids[i];
        jval_text(cp->type == CP_COMBO ? json_get(item, "value") : item, it, sizeof it);
        if (cp->type == CP_FILE ? !_stricmp(it, cur) : !strcmp(it, cur)) return i;
    }
    return -1;
}

void wpapi_choice_label(const CustomProp *cp, int i, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (i < 0 || i >= wpapi_choice_count(cp)) return;
    const JVal *item = &cp->items->kids[i];
    if (cp->type == CP_COMBO) {
        json_str_w(json_get(item, "label"), out, cap);
        if (out[0]) return;
        item = json_get(item, "value");
    }
    if (item && item->t == J_STR) json_str_w(item, out, cap);
    else if (item) str_printf(out, cap, L"%.6g", json_num(item, 0.0));
}

JVal *wpapi_choice_value(const CustomProp *cp, int i)
{
    if (i < 0 || i >= wpapi_choice_count(cp)) return NULL;
    if (cp->type == CP_DROPDOWN) return wpapi_num_value(i, 1.0);
    JVal *v = NULL;
    const JVal *item = &cp->items->kids[i];
    wpapi_jval_copy(&v, cp->type == CP_COMBO ? json_get(item, "value") : item);
    return v;
}

static uint32_t rgb_from_floats(float r, float g, float b)
{
    return ((uint32_t)lroundf(SP_CLAMP(r, 0.0f, 1.0f) * 255.0f) << 16) |
           ((uint32_t)lroundf(SP_CLAMP(g, 0.0f, 1.0f) * 255.0f) << 8) | (uint32_t)lroundf(SP_CLAMP(b, 0.0f, 1.0f) * 255.0f);
}

uint32_t wpapi_color_get(const CustomProp *cp, int *style)
{
    const char *s = json_str(cp->value, "");
    float r, g, b;
    *style = 0;
    if (s[0] == '#') return (uint32_t)strtoul(s + 1, NULL, 16) & 0xFFFFFF;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        *style = 1;
        return (uint32_t)strtoul(s + 2, NULL, 16) & 0xFFFFFF;
    }
    if (sscanf(s, "%f %f %f", &r, &g, &b) == 3) {
        *style = 2;
        return rgb_from_floats(r, g, b);
    }
    return (uint32_t)strtoul(s, NULL, 16) & 0xFFFFFF;
}

JVal *wpapi_color_value(uint32_t rgb, int style)
{
    wchar_t text[48];
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (style == 2) str_printf(text, SP_COUNTOF(text), L"%.4g %.4g %.4g", r / 255.0, g / 255.0, b / 255.0);
    else str_printf(text, SP_COUNTOF(text), style == 1 ? L"0x%02X%02X%02X" : L"#%02X%02X%02X", r, g, b);
    return wpapi_str_value(text);
}

void wpapi_props_free(CustomProp *list, int count)
{
    if (!list) return;
    for (int i = 0; i < count; i++) {
        json_free(list[i].value);
        json_free(list[i].items);
    }
    free(list);
}
