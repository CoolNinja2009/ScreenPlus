#include "wallpaper_api.h"
#include "../../common/util.h"
#include "../../common/json.h"
#include "../../common/paths.h"
#include "../../common/log.h"
#include <shlwapi.h>
#include <wincodec.h>
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

static JVal *with_live(const wchar_t *id, JVal *over);
static int   live_adjust(const wchar_t *id, WallpaperAdjust *out);

char *wpapi_properties_js(const WallpaperEntry *wp, JVal **sent)
{
    if (!wp || (wp->format != WPF_LIVELY && wp->format != WPF_WE)) return NULL;
    wchar_t op[SP_MAX_PATH];
    wpapi_overrides_path(wp->id, op, SP_COUNTOF(op));
    JVal *over = with_live(wp->id, file_exists(op) ? json_parse_file(op) : NULL);
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

/* A picture WIC can read, saved as a PNG (formats a web page can't show: HEIC, TIFF, JPEG XR...). */
static BOOL convert_to_png(const wchar_t *src, const wchar_t *dst)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *cv = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *pb = NULL;
    UINT w = 0, h = 0;
    GUID pf = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromFilename(f, src, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &cv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                           WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, dst, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fe, &pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fe, pb);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fe, w, h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)cv, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fe);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    SP_RELEASE(pb);
    SP_RELEASE(fe);
    SP_RELEASE(enc);
    SP_RELEASE(st);
    SP_RELEASE(cv);
    SP_RELEASE(frame);
    SP_RELEASE(dec);
    SP_RELEASE(f);
    if (FAILED(hr)) DeleteFileW(dst);
    return SUCCEEDED(hr);
}

int wpapi_add_folder_file(const WallpaperEntry *wp, const char *name, const wchar_t *src, wchar_t *outPath, size_t cap,
                          wchar_t *err, size_t ecap)
{
    wchar_t folder[SP_MAX_PATH], sub[SP_MAX_PATH], dir[SP_MAX_PATH], spec[256], base[SP_MAX_PATH], dst[SP_MAX_PATH];
    JVal *owner = NULL;
    const JVal *props = package_properties(wp, folder, SP_COUNTOF(folder), &owner);
    const JVal *prop = props ? json_get(props, name) : NULL;
    int ok = 0;
    err[0] = 0;
    if (!prop) {
        str_copy(err, ecap, L"The wallpaper no longer has this setting.");
        goto done;
    }
    json_str_w(json_get(prop, "folder"), sub, SP_COUNTOF(sub));
    for (wchar_t *c = sub; *c; c++) if (*c == L'/') *c = L'\\';
    if (sub[0]) path_join(dir, SP_COUNTOF(dir), folder, sub);
    else str_copy(dir, SP_COUNTOF(dir), folder);
    json_str_w(json_get(prop, "filter"), spec, SP_COUNTOF(spec));
    if (!spec[0]) str_copy(spec, SP_COUNTOF(spec), L"*.*");
    for (wchar_t *c = spec; *c; c++) if (*c == L'|') *c = L';';
    const wchar_t *fname = path_filename(src);
    /* as it is when the wallpaper takes the type; else as a PNG when it takes those */
    int convert = !PathMatchSpecW(fname, spec);
    if (convert && !PathMatchSpecW(L"x.png", spec)) {
        str_printf(err, ecap, L"This wallpaper only takes %ls files.", spec);
        goto done;
    }
    if (!dir_create_recursive(dir)) {
        str_copy(err, ecap, L"The wallpaper's folder can't be written to.");
        goto done;
    }
    str_copy(base, SP_COUNTOF(base), fname);
    if (convert) {
        wchar_t *dot = wcsrchr(base, L'.');
        if (dot) *dot = 0;
        str_cat(base, SP_COUNTOF(base), L".png");
    }
    path_join(dst, SP_COUNTOF(dst), dir, base);
    for (int i = 2; file_exists(dst) && i < 100; i++) {          /* never replace a file already there */
        wchar_t stem[SP_MAX_PATH], alt[SP_MAX_PATH];
        str_copy(stem, SP_COUNTOF(stem), base);
        wchar_t *dot = wcsrchr(stem, L'.');
        const wchar_t *ext = dot ? path_ext(base) : L"";
        if (dot) *dot = 0;
        str_printf(alt, SP_COUNTOF(alt), L"%ls (%d)%ls", stem, i, ext);
        path_join(dst, SP_COUNTOF(dst), dir, alt);
    }
    ok = convert ? convert_to_png(src, dst) : CopyFileW(src, dst, TRUE);
    if (!ok) str_copy(err, ecap, convert ? L"That picture could not be read." : L"The file could not be copied into the wallpaper's folder.");
    else {
        str_copy(outPath, cap, dst);
        LOGI(L"wallpaper: %ls added to %ls%ls", fname, dir, convert ? L" (as a PNG)" : L"");
    }
done:
    json_free(owner);
    return ok;
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
    if (live_adjust(wallpaperId, out)) return TRUE;     /* a recording is playing */
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

int wpapi_parse_rgb(const wchar_t *s, uint32_t *rgb)
{
    if (!s) return 0;
    if (s[0] == L'#') s++;
    else if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    if (wcslen(s) != 6 || wcsspn(s, L"0123456789abcdefABCDEF") != 6) return 0;
    *rgb = (uint32_t)wcstoul(s, NULL, 16) & 0xFFFFFF;
    return 1;
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

/* ================================================================ recordings */

static const char *const k_recSection[REC_SECTIONS] = { "look", "props" };

double wprec_now(void) { return (double)(filetime_now() / 10000ULL) - 11644473600000.0; }

void wprec_free(Rec *r)
{
    for (int i = 0; r && i < r->count; i++) {
        free(r->t[i].v);
        free(r->t[i].play);
        json_free(r->t[i].opts);
    }
    if (r) {
        free(r->t);
        memset(r, 0, sizeof *r);
    }
}

static float colour_mix(uint32_t a, uint32_t b, double f)
{
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) {
        double x = ((a >> s) & 255) + ((double)((b >> s) & 255) - ((a >> s) & 255)) * f;
        o |= (uint32_t)SP_CLAMP((int)lround(x), 0, 255) << s;
    }
    return (float)o;
}

/* play[] = v[] smoothed with a circular box filter (it loops, so the seam is smoothed too). Colours smooth per
 * channel; on/off and choices are never smoothed. */
void wprec_prepare(Rec *r)
{
    int rad = r->smooth * REC_RATE / 100;            /* up to a second either side */
    for (int i = 0; i < r->count; i++) {
        RecTrack *t = &r->t[i];
        free(t->play);
        t->play = (float *)malloc(sizeof(float) * (size_t)SP_MAX(r->n, 1));
        if (!t->play) continue;
        int smooth = rad > 0 && (t->kind == RT_NUM || t->kind == RT_COLOR) && r->n > 2;
        for (int k = 0; k < r->n; k++) {
            if (!smooth) { t->play[k] = t->v[k]; continue; }
            double s[3] = { 0 }, cnt = 0;
            for (int d = -rad; d <= rad; d++) {
                float x = t->v[((k + d) % r->n + r->n) % r->n];
                if (t->kind == RT_COLOR) for (int c = 0; c < 3; c++) s[c] += ((uint32_t)x >> (c * 8)) & 255;
                else s[0] += x;
                cnt++;
            }
            if (t->kind == RT_COLOR) {
                uint32_t o = 0;
                for (int c = 0; c < 3; c++) o |= (uint32_t)lround(s[c] / cnt) << (c * 8);
                t->play[k] = (float)o;
            } else {
                t->play[k] = (float)(s[0] / cnt);
            }
        }
    }
}

double wprec_pos(const Rec *r, double unixMs)
{
    if (r->n < 1) return 0.0;
    double p = fmod((unixMs - r->epoch) / 1000.0 * r->speed * REC_RATE, (double)r->n);
    return p < 0 ? p + r->n : p;
}

double wprec_num(const RecTrack *t, int n, double pos)
{
    const float *v = t->play ? t->play : t->v;
    if (n < 1 || !v) return 0.0;
    int a = SP_CLAMP((int)pos, 0, n - 1), b = (a + 1) % n;
    double f = pos - floor(pos);
    if (t->kind == RT_NUM) return v[a] + (v[b] - v[a]) * f;
    if (t->kind == RT_COLOR) return colour_mix((uint32_t)v[a], (uint32_t)v[b], f);
    return v[a];
}

JVal *wprec_value(const RecTrack *t, int n, double pos)
{
    double x = wprec_num(t, n, pos);
    switch (t->kind) {
    case RT_BOOL:   return wpapi_bool_value(x >= 0.5);
    case RT_COLOR:  return wpapi_color_value((uint32_t)x & 0xFFFFFF, t->style);
    case RT_CHOICE: {
        JVal *o = NULL;
        int i = (int)x;
        if (t->opts && i >= 0 && i < t->opts->n) wpapi_jval_copy(&o, &t->opts->kids[i]);
        return o;
    }
    default:        return wpapi_num_value(x, t->step > 0 ? t->step : 0.01);
    }
}

void wprec_look(const Rec *r, double pos, WallpaperAdjust *a)
{
    for (int i = 0; i < r->count; i++) {
        const RecTrack *t = &r->t[i];
        double x = wprec_num(t, r->n, pos);
        int v = (int)lround(x);
        if (!strcmp(t->key, "brightness")) a->brightness = SP_CLAMP(v, -100, 100);
        else if (!strcmp(t->key, "contrast")) a->contrast = SP_CLAMP(v, -100, 100);
        else if (!strcmp(t->key, "saturation")) a->saturation = SP_CLAMP(v, -100, 100);
        else if (!strcmp(t->key, "hue")) a->hue = SP_CLAMP(v, -180, 180);
        else if (!strcmp(t->key, "gamma")) a->gamma = SP_CLAMP(v, -100, 100);
        else if (!strcmp(t->key, "tint")) a->tint = (uint32_t)x & 0xFFFFFF;
        else if (!strcmp(t->key, "tintLift")) a->tintLift = SP_CLAMP(v, 0, 100);
        else if (!strcmp(t->key, "tintBrightness")) a->tintBrightness = SP_CLAMP(v, 0, 100);
    }
    a->used = adjust_active(a);
}

static void rec_write(StrBuf *b, const Rec *r, int played)   /* played: the smoothed samples (Plasma plays them as they are) */
{
    sb_printf(b, "{\"n\":%d,\"speed\":%.3f,\"smooth\":%d,\"epoch\":%.0f,\"tracks\":[", r->n, r->speed, r->smooth, r->epoch);
    for (int i = 0; i < r->count; i++) {
        const RecTrack *t = &r->t[i];
        if (i) sb_puts(b, ",");
        sb_puts(b, "{\"key\":");
        json_write_string(b, t->key);
        sb_printf(b, ",\"kind\":%d,\"style\":%d,\"step\":%.6g,\"lo\":%.6g,\"hi\":%.6g", t->kind, t->style, t->step, t->lo, t->hi);
        if (t->opts) {
            sb_puts(b, ",\"opts\":");
            json_write(b, t->opts);
        }
        sb_puts(b, ",\"v\":[");
        for (int k = 0; k < r->n; k++) sb_printf(b, k ? ",%.7g" : "%.7g", (played && t->play ? t->play : t->v)[k]);
        sb_puts(b, "]}");
    }
    sb_puts(b, "]}");
}

char *wprec_json(const Rec *r)
{
    StrBuf b = { 0 };
    rec_write(&b, r, 1);
    if (b.failed) { sb_free(&b); return NULL; }
    return b.p;
}

static BOOL rec_from(const JVal *j, Rec *out)
{
    memset(out, 0, sizeof *out);
    const JVal *tr = json_get(j, "tracks");
    int n = (int)json_num(json_get(j, "n"), 0);
    if (n < 2 || n > REC_MAX_SEC * REC_RATE || !tr || tr->t != J_ARR || !tr->n) return FALSE;
    out->t = (RecTrack *)calloc((size_t)tr->n, sizeof *out->t);
    if (!out->t) return FALSE;
    out->n = n;
    out->speed = SP_CLAMP(json_num(json_get(j, "speed"), 1.0), 0.25, 4.0);
    out->smooth = SP_CLAMP((int)json_num(json_get(j, "smooth"), 0), 0, 100);
    out->epoch = json_num(json_get(j, "epoch"), 0);
    for (int i = 0; i < tr->n; i++) {
        const JVal *x = &tr->kids[i], *v = json_get(x, "v");
        if (!v || v->t != J_ARR || v->n != n) continue;
        RecTrack *t = &out->t[out->count];
        strncpy_s(t->key, sizeof t->key, json_str(json_get(x, "key"), ""), _TRUNCATE);
        t->kind = SP_CLAMP((int)json_num(json_get(x, "kind"), 0), RT_NUM, RT_COLOR);
        t->style = (int)json_num(json_get(x, "style"), 0);
        t->step = json_num(json_get(x, "step"), 1);
        t->lo = json_num(json_get(x, "lo"), 0);
        t->hi = json_num(json_get(x, "hi"), 100);
        if (json_get(x, "opts")) wpapi_jval_copy(&t->opts, json_get(x, "opts"));
        if (!t->key[0] || !(t->v = (float *)malloc(sizeof(float) * (size_t)n))) {
            json_free(t->opts);
            t->opts = NULL;
            continue;
        }
        for (int k = 0; k < n; k++) t->v[k] = (float)json_num(&v->kids[k], 0);
        out->count++;
    }
    if (!out->count) { wprec_free(out); return FALSE; }
    wprec_prepare(out);
    return TRUE;
}

BOOL wprec_load(const wchar_t *wallpaperId, int section, Rec *out)
{
    memset(out, 0, sizeof *out);
    if (!wallpaperId || !wallpaperId[0] || section < 0 || section >= REC_SECTIONS) return FALSE;
    wchar_t p[SP_MAX_PATH];
    wpapi_overrides_path(wallpaperId, p, SP_COUNTOF(p));
    JVal *over = file_exists(p) ? json_parse_file(p) : NULL;
    BOOL ok = rec_from(json_get(json_get(over, "_screenplus_rec"), k_recSection[section]), out);
    json_free(over);
    return ok;
}

BOOL wprec_save(const wchar_t *wallpaperId, int section, const Rec *r)
{
    if (!wallpaperId || !wallpaperId[0] || section < 0 || section >= REC_SECTIONS) return FALSE;
    Rec other;
    StrBuf b = { 0 };
    int count = 0;
    sb_puts(&b, "{");
    for (int s = 0; s < REC_SECTIONS; s++) {
        int mine = s == section, loaded = !mine && wprec_load(wallpaperId, s, &other);
        const Rec *x = mine ? r : loaded ? &other : NULL;
        if (x && x->n > 1 && x->count) {
            if (count++) sb_puts(&b, ",");
            sb_printf(&b, "\"%s\":", k_recSection[s]);
            rec_write(&b, x, 0);
        }
        if (loaded) wprec_free(&other);
    }
    sb_puts(&b, "}");
    int ok = !b.failed && save_override_file(wallpaperId, "_screenplus_rec", count ? b.p : NULL);
    sb_free(&b);
    return ok;
}

/* ---- the recorder: every change with its time; turned into REC_RATE samples when it stops */

typedef struct TakeEvent { double t; int track; JVal *v; } TakeEvent;
typedef struct TakeTrack { char key[80]; int kind, style; double step, lo, hi; JVal *start; } TakeTrack;
struct RecTake {
    double     t0;
    TakeTrack *tr;
    int        trCount, trCap;
    TakeEvent *ev;
    int        evCount, evCap;
};

RecTake *wprec_take_start(void)
{
    RecTake *k = (RecTake *)calloc(1, sizeof *k);
    if (k) k->t0 = time_ms();
    return k;
}

double wprec_take_seconds(const RecTake *k) { return k ? (time_ms() - k->t0) / 1000.0 : 0.0; }

void wprec_take_note(RecTake *k, const char *key, int kind, int style, double step, double lo, double hi,
                     const JVal *before, const JVal *now)
{
    if (!k || !now || wprec_take_seconds(k) > REC_MAX_SEC) return;
    int i = 0;
    while (i < k->trCount && strcmp(k->tr[i].key, key)) i++;
    if (i == k->trCount) {
        if (k->trCount == k->trCap) {
            int cap = k->trCap ? k->trCap * 2 : 8;
            TakeTrack *n = (TakeTrack *)realloc(k->tr, sizeof *n * (size_t)cap);
            if (!n) return;
            k->tr = n;
            k->trCap = cap;
        }
        TakeTrack *t = &k->tr[k->trCount++];
        memset(t, 0, sizeof *t);
        strncpy_s(t->key, sizeof t->key, key, _TRUNCATE);
        t->kind = kind;
        t->style = style;
        t->step = step;
        t->lo = lo;
        t->hi = hi;
        wpapi_jval_copy(&t->start, before ? before : now);
    }
    if (k->evCount == k->evCap) {
        int cap = k->evCap ? k->evCap * 2 : 256;
        TakeEvent *n = (TakeEvent *)realloc(k->ev, sizeof *n * (size_t)cap);
        if (!n) return;
        k->ev = n;
        k->evCap = cap;
    }
    TakeEvent *e = &k->ev[k->evCount++];
    e->t = wprec_take_seconds(k);
    e->track = i;
    wpapi_jval_copy(&e->v, now);
}

static int same_json(const JVal *a, const JVal *b)
{
    StrBuf x = { 0 }, y = { 0 };
    json_write(&x, a);
    json_write(&y, b);
    int same = !x.failed && !y.failed && x.len == y.len && (!x.len || !memcmp(x.p, y.p, x.len));
    sb_free(&x);
    sb_free(&y);
    return same;
}

/* A value as a sample: on/off as 0/1, a choice as its index in opts (added when new), a colour as 0xRRGGBB. */
static float sample_of(RecTrack *t, const JVal *v)
{
    if (t->kind == RT_BOOL) return json_bool(v, json_num(v, 0) != 0) ? 1.0f : 0.0f;
    if (t->kind == RT_COLOR) {
        if (v && v->t == J_NUM) return (float)((uint32_t)v->num & 0xFFFFFF);
        CustomProp cp;
        memset(&cp, 0, sizeof cp);
        cp.value = (JVal *)v;
        cp.type = CP_COLOR;
        int style = 0;
        return (float)(wpapi_color_get(&cp, &style) & 0xFFFFFF);
    }
    if (t->kind == RT_CHOICE) {
        for (int i = 0; t->opts && i < t->opts->n; i++) if (same_json(&t->opts->kids[i], v)) return (float)i;
        StrBuf b = { 0 };
        sb_puts(&b, "[");
        for (int i = 0; t->opts && i < t->opts->n; i++) { json_write(&b, &t->opts->kids[i]); sb_puts(&b, ","); }
        json_write(&b, v);
        sb_puts(&b, "]");
        JVal *n = b.failed ? NULL : json_parse(b.p, b.len);
        sb_free(&b);
        if (!n) return 0.0f;
        json_free(t->opts);
        t->opts = n;
        return (float)(n->n - 1);
    }
    return (float)json_num(v, 0);
}

BOOL wprec_take_finish(RecTake *k, Rec *out)
{
    memset(out, 0, sizeof *out);
    if (!k) return FALSE;
    double secs = SP_MIN(wprec_take_seconds(k), (double)REC_MAX_SEC);
    int n = SP_MAX((int)ceil(secs * REC_RATE), 2);
    BOOL ok = k->evCount > 0 && (out->t = (RecTrack *)calloc((size_t)SP_MAX(k->trCount, 1), sizeof *out->t)) != NULL;
    out->n = n;
    out->speed = 1.0;
    out->epoch = wprec_now();
    for (int i = 0; ok && i < k->trCount; i++) {
        TakeTrack *tt = &k->tr[i];
        RecTrack *t = &out->t[out->count];
        strncpy_s(t->key, sizeof t->key, tt->key, _TRUNCATE);
        t->kind = tt->kind;
        t->style = tt->style;
        t->step = tt->step;
        t->lo = tt->lo;
        t->hi = tt->hi;
        if (!(t->v = (float *)malloc(sizeof(float) * (size_t)n))) continue;
        float cur = sample_of(t, tt->start);
        int e = 0, changed = 0;
        for (int s = 0; s < n; s++) {                 /* hold each value until the next change */
            double at = (double)s / REC_RATE;
            for (; e < k->evCount && k->ev[e].t <= at; e++)
                if (k->ev[e].track == i) cur = sample_of(t, k->ev[e].v);
            t->v[s] = cur;
            changed |= s && cur != t->v[0];
        }
        if (!changed) {                               /* touched but never moved: nothing to play */
            free(t->v);
            json_free(t->opts);
            memset(t, 0, sizeof *t);
            continue;
        }
        if (t->kind == RT_NUM || t->kind == RT_COLOR) {   /* the loop's seam: ease the last half second into the start */
            int m = SP_MIN(REC_RATE / 2, n / 4);
            float end = t->v[n - 1 - m];
            for (int s = n - m; s < n; s++) {
                double f = (double)(s - (n - 1 - m)) / (m + 1), w = f * f * (3 - 2 * f);
                t->v[s] = t->kind == RT_COLOR ? colour_mix((uint32_t)end, (uint32_t)t->v[0], w) : (float)(end + (t->v[0] - end) * w);
            }
        }
        out->count++;
    }
    for (int i = 0; i < k->trCount; i++) json_free(k->tr[i].start);
    for (int i = 0; i < k->evCount; i++) json_free(k->ev[i].v);
    free(k->tr);
    free(k->ev);
    free(k);
    if (!ok || !out->count) {
        wprec_free(out);
        return FALSE;
    }
    wprec_prepare(out);
    return TRUE;
}

/* ---- live values (engine processes) */

typedef struct LiveVals { wchar_t id[SP_ID_LEN]; int hasAdj; WallpaperAdjust adj; JVal *props; } LiveVals;
static LiveVals g_live[SP_MAX_MONITORS];

static LiveVals *live_find(const wchar_t *id)
{
    for (int i = 0; id && id[0] && i < SP_MAX_MONITORS; i++) if (g_live[i].id[0] && str_ieq(g_live[i].id, id)) return &g_live[i];
    return NULL;
}

void wpapi_live_set(const wchar_t *wallpaperId, const WallpaperAdjust *adj, const JVal *props)
{
    LiveVals *l = live_find(wallpaperId);
    if (!adj && !props) {
        if (l) {
            json_free(l->props);
            memset(l, 0, sizeof *l);
        }
        return;
    }
    for (int i = 0; !l && i < SP_MAX_MONITORS; i++) if (!g_live[i].id[0]) l = &g_live[i];
    if (!l) return;
    str_copy(l->id, SP_COUNTOF(l->id), wallpaperId);
    l->hasAdj = adj != NULL;
    if (adj) l->adj = *adj;
    json_free(l->props);
    l->props = NULL;
    if (props) wpapi_jval_copy(&l->props, props);
}

/* The overrides with playback's property values on top. */
static JVal *with_live(const wchar_t *id, JVal *over)
{
    LiveVals *l = live_find(id);
    if (!l || !l->props || l->props->t != J_OBJ) return over;
    StrBuf b = { 0 };
    int count = 0;
    sb_puts(&b, "{");
    for (int i = 0; over && over->t == J_OBJ && i < over->n; i++) {
        if (!over->keys[i] || json_get(l->props, over->keys[i])) continue;
        if (count++) sb_puts(&b, ",");
        json_write_string(&b, over->keys[i]);
        sb_puts(&b, ":");
        json_write(&b, &over->kids[i]);
    }
    for (int i = 0; i < l->props->n; i++) {
        if (!l->props->keys[i]) continue;
        if (count++) sb_puts(&b, ",");
        json_write_string(&b, l->props->keys[i]);
        sb_puts(&b, ":");
        json_write(&b, &l->props->kids[i]);
    }
    sb_puts(&b, "}");
    JVal *m = b.failed ? NULL : json_parse(b.p, b.len);
    sb_free(&b);
    if (!m) return over;
    json_free(over);
    return m;
}

static int live_adjust(const wchar_t *id, WallpaperAdjust *out)
{
    LiveVals *l = live_find(id);
    if (!l || !l->hasAdj) return 0;
    *out = l->adj;
    out->used = adjust_active(out);
    return 1;
}
