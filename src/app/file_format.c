#include "file_format.h"
#include "app.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../renderer/html/wallpaper_api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <shobjidl.h>
#define sp_seek _fseeki64
#define sp_tell _ftelli64
#else
#define sp_seek fseeko
#define sp_tell ftello
#endif

/* Archive paths are deliberately fixed and small. The extraction validator below rejects paths that could
 * escape the temporary directory, symlinks and unusually large expanded archives. */
#define FORMAT_MAX_FILES 10000
#define FORMAT_MAX_BYTES (20ULL * 1024 * 1024 * 1024)

static unsigned u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned u32(const unsigned char *p) { return u16(p) | (u16(p + 2) << 16); }

static BOOL archive_safe(const wchar_t *archive)
{
    FILE *f = _wfopen(archive, L"rb");
    if (!f) return FALSE;
    if (sp_seek(f, 0, SEEK_END)) { fclose(f); return FALSE; }
    int64_t size = sp_tell(f);
    if (size < 22 || size > FORMAT_MAX_BYTES) { fclose(f); return FALSE; }
    size_t tail = (size_t)(size < 65557 ? size : 65557);
    unsigned char *buf = (unsigned char *)malloc(tail);
    if (!buf || sp_seek(f, size - (int64_t)tail, SEEK_SET) || fread(buf, 1, tail, f) != tail) {
        free(buf); fclose(f); return FALSE;
    }
    size_t end = tail;
    while (end >= 22) {
        if (u32(buf + end - 22) == 0x06054b50 && end - 22 + 22 + u16(buf + end - 2) == tail) break;
        end--;
    }
    BOOL ok = end >= 22;
    uint64_t total = 0;
    unsigned count = ok ? u16(buf + end - 12) : 0;
    unsigned dirSize = ok ? u32(buf + end - 10) : 0, dirAt = ok ? u32(buf + end - 6) : 0;
    if (!count || count > FORMAT_MAX_FILES || count == 65535 || dirAt == 0xffffffffu ||
        (uint64_t)dirAt + dirSize > (uint64_t)size) ok = FALSE;
    free(buf);
    if (ok && sp_seek(f, dirAt, SEEK_SET)) ok = FALSE;
    for (unsigned i = 0; ok && i < count; i++) {
        unsigned char h[46];
        if (fread(h, 1, sizeof h, f) != sizeof h || u32(h) != 0x02014b50) { ok = FALSE; break; }
        unsigned nameLen = u16(h + 28), extra = u16(h + 30), comment = u16(h + 32);
        unsigned mode = u32(h + 38) >> 16;
        total += u32(h + 24);
        if (!nameLen || nameLen > 1024 || total > FORMAT_MAX_BYTES ||
            u32(h + 24) == 0xffffffffu || u32(h + 20) == 0xffffffffu ||
            u32(h + 42) == 0xffffffffu || u16(h + 8) & 1 ||
            (u32(h + 38) & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((mode & 0170000) != 0 && (mode & 0170000) != 0100000 && (mode & 0170000) != 0040000)) {
            ok = FALSE; break;
        }
        char name[1025];
        if (fread(name, 1, nameLen, f) != nameLen) { ok = FALSE; break; }
        name[nameLen] = 0;
        if (name[0] == '/' || name[0] == '\\' || strchr(name, ':') || strchr(name, '\\') ||
            strstr(name, "//") || strstr(name, "../") || !strcmp(name, "..") || strstr(name, "/..")) ok = FALSE;
        for (unsigned j = 0; ok && j < nameLen; j++) if ((unsigned char)name[j] < 32) ok = FALSE;
        if (fseek(f, (long)(extra + comment), SEEK_CUR)) ok = FALSE;
        int64_t next = sp_tell(f);
        if (ok && (uint64_t)u32(h + 42) + 30 + nameLen > (uint64_t)size) ok = FALSE;
        if (ok && sp_seek(f, u32(h + 42), SEEK_SET)) ok = FALSE;
        if (ok) {
            unsigned char local[30]; char localName[1025];
            if (fread(local, 1, sizeof local, f) != sizeof local || u32(local) != 0x04034b50 ||
                u16(local + 26) != nameLen || fread(localName, 1, nameLen, f) != nameLen ||
                memcmp(localName, name, nameLen)) ok = FALSE;
        }
        if (ok && sp_seek(f, next, SEEK_SET)) ok = FALSE;
    }
    fclose(f);
    return ok;
}

static BOOL run_tar(const wchar_t *mode, const wchar_t *archive, const wchar_t *dir)
{
    wchar_t exe[SP_MAX_PATH], cmd[SP_MAX_PATH * 3];
    GetSystemDirectoryW(exe, SP_COUNTOF(exe));
    path_join(exe, SP_COUNTOF(exe), exe, L"tar.exe");
    /* The Linux Win32 shim maps /usr/bin/tar to bsdtar when available. bsdtar understands ZIP on both OSes. */
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" %ls \"%ls\"%ls", exe,
               str_ieq(mode, L"pack") ? L"-a --format zip -cf" : L"-xf", archive,
               str_ieq(mode, L"pack") ? L" ." : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    DWORD code = 1;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return FALSE;
    if (WaitForSingleObject(pi.hProcess, 600000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

static BOOL pack_atomic(const wchar_t *stage, const wchar_t *dest)
{
    wchar_t parent[SP_MAX_PATH], name[64], temp[SP_MAX_PATH], id[SP_ID_LEN];
    path_dirname(parent, SP_COUNTOF(parent), dest);
    random_id(id);
    str_printf(name, SP_COUNTOF(name), L".screenplus-%ls.tmp", id);
    path_join(temp, SP_COUNTOF(temp), parent, name);
    BOOL ok = run_tar(L"pack", temp, stage) && MoveFileExW(temp, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp);
    return ok;
}

static void remove_tree(const wchar_t *dir)
{
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[SP_MAX_PATH]; path_join(p, SP_COUNTOF(p), dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static BOOL copy_tree(const wchar_t *src, const wchar_t *dst, int depth)
{
    if (depth > 24 || !dir_create_recursive(dst)) return FALSE;
    wchar_t pattern[SP_MAX_PATH]; path_join(pattern, SP_COUNTOF(pattern), src, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return dir_exists(src);   /* an empty asset folder is valid */
    BOOL ok = TRUE;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = FALSE; break; }
        wchar_t a[SP_MAX_PATH], b[SP_MAX_PATH];
        path_join(a, SP_COUNTOF(a), src, fd.cFileName);
        path_join(b, SP_COUNTOF(b), dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = copy_tree(a, b, depth + 1);
        else ok = CopyFileW(a, b, FALSE);
        if (!ok) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static BOOL write_text(const wchar_t *path, const StrBuf *b)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return FALSE;
    BOOL ok = fwrite(b->p, 1, b->len, f) == b->len;
    if (fclose(f)) ok = FALSE;
    return ok;
}

static void json_w(StrBuf *b, const wchar_t *w)
{
    char *s = utf8_from_wide(w, NULL);
    json_write_string(b, s ? s : "");
    free(s);
}

static BOOL new_stage(wchar_t *out, size_t cap)
{
    wchar_t root[SP_MAX_PATH], id[SP_ID_LEN];
    path_join(root, SP_COUNTOF(root), paths_root(), L"format-temp");
    random_id(id);
    path_join(out, cap, root, id);
    return dir_create_recursive(out);
}

static BOOL spf_pack(const WallpaperEntry *e, const wchar_t *dest)
{
    if (!e || wallpaper_is_url(e->path) || !file_exists(e->path)) return FALSE;
    wchar_t stage[SP_MAX_PATH], media[SP_MAX_PATH], target[SP_MAX_PATH], rel[SP_MAX_PATH],
            folder[SP_MAX_PATH], props[SP_MAX_PATH], out[SP_MAX_PATH];
    if (!new_stage(stage, SP_COUNTOF(stage))) return FALSE;
    path_dirname(folder, SP_COUNTOF(folder), e->path);
    BOOL group = e->type == WP_HTML;
    path_join(media, SP_COUNTOF(media), stage, L"media");
    path_join(target, SP_COUNTOF(target), media, group ? L"wallpaper" : path_filename(e->path));
    if (group) {
        str_printf(rel, SP_COUNTOF(rel), L"media/wallpaper/%ls", path_filename(e->path));
        if (!copy_tree(folder, target, 0)) { remove_tree(stage); return FALSE; }
    } else {
        str_printf(rel, SP_COUNTOF(rel), L"media/%ls", path_filename(e->path));
        if (!dir_create_recursive(media) || !CopyFileW(e->path, target, FALSE)) { remove_tree(stage); return FALSE; }
    }
    wchar_t thumbRel[64] = L"";
    if (e->thumb[0] && file_exists(e->thumb)) {
        str_printf(thumbRel, SP_COUNTOF(thumbRel), L"thumbnail%ls", path_ext(e->thumb));
        path_join(target, SP_COUNTOF(target), stage, thumbRel);
        if (!CopyFileW(e->thumb, target, FALSE)) { remove_tree(stage); return FALSE; }
    }
    wpapi_overrides_path(e->id, props, SP_COUNTOF(props));
    if (file_exists(props)) {
        path_join(target, SP_COUNTOF(target), stage, L"props.json");
        if (!CopyFileW(props, target, FALSE)) { remove_tree(stage); return FALSE; }
    }
    StrBuf b = { 0 };
    sb_puts(&b, "{\"screenplus\":\"spf\",\"version\":1,\"name\":"); json_w(&b, e->name);
    sb_printf(&b, ",\"type\":%d,\"format\":%d,\"flags\":%d,\"volCut\":%d,\"slideSec\":%d,\"path\":",
              e->type, e->format, e->flags & ~(WPFLAG_PPTX | WPFLAG_YTDLP), e->volCut, e->slideSec);
    json_w(&b, rel); sb_puts(&b, ",\"thumb\":"); json_w(&b, thumbRel); sb_puts(&b, "}");
    path_join(out, SP_COUNTOF(out), stage, L"manifest.json");
    BOOL ok = !b.failed && write_text(out, &b);
    sb_free(&b);
    if (ok) ok = pack_atomic(stage, dest);
    remove_tree(stage);
    return ok;
}

BOOL spf_export(const WallpaperEntry *entry, const wchar_t *path) { return spf_pack(entry, path); }

BOOL spp_export(const wchar_t *collection, const WallpaperEntry *single, const wchar_t *path)
{
    wchar_t stage[SP_MAX_PATH], target[SP_MAX_PATH], member[64];
    if (!single && (!collection || !collection[0])) return FALSE;
    if (!new_stage(stage, SP_COUNTOF(stage))) return FALSE;
    int count = 0;
    for (int i = 0; i < g_app.cfg.count; i++) {
        const WallpaperEntry *e = &g_app.cfg.items[i];
        if (single ? wcscmp(single->id, e->id) != 0 : !config_tag_has(e, collection)) continue;
        str_printf(member, SP_COUNTOF(member), L"%04d.spf", count);
        path_join(target, SP_COUNTOF(target), stage, member);
        if (!spf_pack(e, target)) { remove_tree(stage); return FALSE; }
        count++;
    }
    if (!count) { remove_tree(stage); return FALSE; }
    StrBuf b = { 0 };
    sb_puts(&b, "{\"screenplus\":\"spp\",\"version\":1,\"name\":");
    json_w(&b, single ? single->name : collection);
    sb_printf(&b, ",\"count\":%d,\"intervalMinutes\":%d}", count, g_app.cfg.s.slideshowMin);
    path_join(target, SP_COUNTOF(target), stage, L"manifest.json");
    BOOL ok = !b.failed && write_text(target, &b);
    sb_free(&b);
    if (ok) ok = pack_atomic(stage, path);
    remove_tree(stage);
    return ok;
}

static BOOL relative_safe(const wchar_t *s)
{
    if (!s[0] || s[0] == L'/' || s[0] == L'\\' || wcschr(s, L':') || wcschr(s, L'\\')) return FALSE;
    if (wcsstr(s, L"../") || wcsstr(s, L"/..") || !wcscmp(s, L"..")) return FALSE;
    return TRUE;
}

static int import_spf(const wchar_t *path, const wchar_t *collection)
{
    if (!archive_safe(path)) return 0;
    wchar_t stage[SP_MAX_PATH], manifest[SP_MAX_PATH], src[SP_MAX_PATH], dest[SP_MAX_PATH],
            rel[SP_MAX_PATH], thumbRel[SP_MAX_PATH], name[SP_NAME_LEN], id[SP_ID_LEN], props[SP_MAX_PATH];
    if (!new_stage(stage, SP_COUNTOF(stage))) return 0;
    if (!run_tar(L"extract", path, stage)) { remove_tree(stage); return 0; }
    path_join(manifest, SP_COUNTOF(manifest), stage, L"manifest.json");
    JVal *j = json_parse_file(manifest);
    int ok = j && !strcmp(json_str(json_get(j, "screenplus"), ""), "spf") && json_num(json_get(j, "version"), 0) == 1;
    if (ok) {
        json_str_w(json_get(j, "path"), rel, SP_COUNTOF(rel));
        json_str_w(json_get(j, "thumb"), thumbRel, SP_COUNTOF(thumbRel));
        json_str_w(json_get(j, "name"), name, SP_COUNTOF(name));
        ok = relative_safe(rel) && (!thumbRel[0] || relative_safe(thumbRel));
    }
    if (ok) {
        path_join(src, SP_COUNTOF(src), stage, rel);
        ok = file_exists(src);
    }
    if (!ok) { json_free(j); remove_tree(stage); return 0; }
    WallpaperType type = (WallpaperType)(int)json_num(json_get(j, "type"), 0);
    int flags = (int)json_num(json_get(j, "flags"), 0) & ~(WPFLAG_URL | WPFLAG_YTDLP | WPFLAG_PPTX);
    if (type < WP_IMAGE || type > WP_HTML || ((flags & WPFLAG_DEPTH) && type != WP_HTML)) {
        json_free(j); remove_tree(stage); return 0;
    }
    do { random_id(id); } while (config_find(&g_app.cfg, id));
    path_join(dest, SP_COUNTOF(dest), paths_root(), (flags & WPFLAG_DEPTH) ? L"depth" : L"packages");
    path_join(dest, SP_COUNTOF(dest), dest, id);
    if (flags & WPFLAG_DEPTH) {
        wchar_t media[SP_MAX_PATH];
        path_join(media, SP_COUNTOF(media), stage, L"media/wallpaper");
        ok = copy_tree(media, dest, 0);
        path_join(src, SP_COUNTOF(src), dest, path_filename(rel));
    } else {
        ok = copy_tree(stage, dest, 0);
        path_join(src, SP_COUNTOF(src), dest, rel);
    }
    if (!ok || !file_exists(src)) { remove_tree(dest); json_free(j); remove_tree(stage); return 0; }
    WallpaperEntry *e = config_add(&g_app.cfg, type, src, name);
    if (!e) { remove_tree(dest); json_free(j); remove_tree(stage); return 0; }
    str_copy(e->id, SP_COUNTOF(e->id), id);
    e->format = (int)json_num(json_get(j, "format"), WPF_PLAIN);
    e->flags = flags;
    e->volCut = SP_CLAMP((int)json_num(json_get(j, "volCut"), 0), 0, 100);
    e->slideSec = SP_CLAMP((int)json_num(json_get(j, "slideSec"), 0), 0, 3600);
    if (thumbRel[0]) {
        if (flags & WPFLAG_DEPTH) {
            wchar_t from[SP_MAX_PATH];
            path_join(from, SP_COUNTOF(from), stage, thumbRel);
            path_join(src, SP_COUNTOF(src), dest, path_filename(thumbRel));
            CopyFileW(from, src, FALSE);
        } else path_join(src, SP_COUNTOF(src), dest, thumbRel);
        if (file_exists(src)) str_copy(e->thumb, SP_COUNTOF(e->thumb), src);
    }
    if (collection && collection[0]) config_tag_set(e, collection, TRUE);
    path_join(src, SP_COUNTOF(src), (flags & WPFLAG_DEPTH) ? stage : dest, L"props.json");
    if (file_exists(src)) {
        wpapi_overrides_path(e->id, props, SP_COUNTOF(props));
        wchar_t dir[SP_MAX_PATH]; path_dirname(dir, SP_COUNTOF(dir), props);
        if (dir_create_recursive(dir)) CopyFileW(src, props, FALSE);
    }
    json_free(j); remove_tree(stage);
    return 1;
}

int sp_format_import(const wchar_t *path, const wchar_t *collection)
{
    if (str_ieq(path_ext(path), L".spf")) return import_spf(path, collection);
    if (!str_ieq(path_ext(path), L".spp") || !archive_safe(path)) return 0;
    wchar_t stage[SP_MAX_PATH], manifest[SP_MAX_PATH], member[64], target[SP_MAX_PATH], name[SP_NAME_LEN];
    if (!new_stage(stage, SP_COUNTOF(stage))) return 0;
    if (!run_tar(L"extract", path, stage)) { remove_tree(stage); return 0; }
    path_join(manifest, SP_COUNTOF(manifest), stage, L"manifest.json");
    JVal *j = json_parse_file(manifest);
    int count = j && !strcmp(json_str(json_get(j, "screenplus"), ""), "spp") &&
                json_num(json_get(j, "version"), 0) == 1 ? (int)json_num(json_get(j, "count"), 0) : 0;
    int minutes = j ? (int)json_num(json_get(j, "intervalMinutes"), 0) : 0;
    json_str_w(j ? json_get(j, "name") : NULL, name, SP_COUNTOF(name));
    json_free(j);
    if (count < 1 || count > FORMAT_MAX_FILES) { remove_tree(stage); return 0; }
    for (int i = 0; i < count; i++) {
        str_printf(member, SP_COUNTOF(member), L"%04d.spf", i);
        path_join(target, SP_COUNTOF(target), stage, member);
        if (!file_exists(target) || !archive_safe(target)) { remove_tree(stage); return 0; }
    }
    int added = 0;
    for (int i = 0; i < count; i++) {
        str_printf(member, SP_COUNTOF(member), L"%04d.spf", i);
        path_join(target, SP_COUNTOF(target), stage, member);
        if (!file_exists(target)) break;
        added += import_spf(target, collection ? collection : (count > 1 ? name : L""));
    }
    if (added && minutes > 0 && minutes <= 1440) g_app.cfg.s.slideshowMin = minutes;
    remove_tree(stage);
    return added;
}

#ifdef _WIN32
BOOL sp_format_save_dialog(HWND owner, const wchar_t *suggested, wchar_t *path, size_t cap)
{
    IFileSaveDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&dlg))) return FALSE;
    static const COMDLG_FILTERSPEC filter[] = { { L"Screen+ playlist", L"*.spp" } };
    IFileSaveDialog_SetFileTypes(dlg, 1, filter);
    IFileSaveDialog_SetDefaultExtension(dlg, L"spp");
    IFileSaveDialog_SetFileName(dlg, suggested);
    IFileSaveDialog_SetTitle(dlg, L"Export Screen+ playlist");
    BOOL ok = FALSE;
    if (SUCCEEDED(IFileSaveDialog_Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileSaveDialog_GetResult(dlg, &item))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
                str_copy(path, cap, p); ok = TRUE; CoTaskMemFree(p);
            }
            IShellItem_Release(item);
        }
    }
    IFileSaveDialog_Release(dlg);
    return ok;
}
#endif
