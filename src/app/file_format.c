#include "file_format.h"
#include "app.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../renderer/html/wallpaper_api.h"
#include "../ui/ui.h"
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

/* The last wallpaper type a file may hold here: programs (WP_APP) only where they can run. A program in a file is
 * no riskier than the .exe it came from, and the preview says plainly that it is one before it is added. */
#ifdef _WIN32
#define FORMAT_MAX_TYPE WP_APP
#else
#define FORMAT_MAX_TYPE WP_HTML
#endif

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

/* mode: "pack" deflates, "store" keeps already-compressed media as is (a video packs at disk speed), else extract */
static BOOL run_tar(const wchar_t *mode, const wchar_t *archive, const wchar_t *dir)
{
    BOOL pack = str_ieq(mode, L"pack"), store = str_ieq(mode, L"store");
    wchar_t exe[SP_MAX_PATH], cmd[SP_MAX_PATH * 3];
    GetSystemDirectoryW(exe, SP_COUNTOF(exe));
    path_join(exe, SP_COUNTOF(exe), exe, L"tar.exe");
    /* The Linux Win32 shim maps /usr/bin/tar to bsdtar when available. bsdtar understands ZIP on both OSes. */
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" %ls \"%ls\"%ls", exe,
               store ? L"--format zip --options zip:compression=store -cf" : pack ? L"--format zip -cf" : L"-xf",
               archive, pack || store ? L" ." : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    DWORD code = 1;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return FALSE;
    if (WaitForSingleObject(pi.hProcess, 600000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

static BOOL pack_atomic(const wchar_t *stage, const wchar_t *dest, BOOL store)
{
    wchar_t parent[SP_MAX_PATH], name[64], temp[SP_MAX_PATH], id[SP_ID_LEN];
    path_dirname(parent, SP_COUNTOF(parent), dest);
    random_id(id);
    str_printf(name, SP_COUNTOF(name), L".screenplus-%ls.tmp", id);
    path_join(temp, SP_COUNTOF(temp), parent, name);
    BOOL ok = run_tar(store ? L"store" : L"pack", temp, stage) && MoveFileExW(temp, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
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
    if (!e) return FALSE;
    BOOL link = wallpaper_is_url(e->path);
    if (!link && !file_exists(e->path)) return FALSE;
    wchar_t stage[SP_MAX_PATH], media[SP_MAX_PATH], target[SP_MAX_PATH], rel[SP_MAX_PATH],
            folder[SP_MAX_PATH], props[SP_MAX_PATH], out[SP_MAX_PATH];
    if (!new_stage(stage, SP_COUNTOF(stage))) return FALSE;
    if (link) str_copy(rel, SP_COUNTOF(rel), e->path);
    else {
        path_dirname(folder, SP_COUNTOF(folder), e->path);
        wchar_t lp[SP_MAX_PATH];
        path_join(lp, SP_COUNTOF(lp), folder, L"LivelyProperties.json");
        /* a page's whole folder; a program's only when it is a package (properties beside it), never e.g. Downloads */
        BOOL group = e->type == WP_HTML || (e->type == WP_APP && file_exists(lp));
        path_join(media, SP_COUNTOF(media), stage, L"media");
        path_join(target, SP_COUNTOF(target), media, group ? L"wallpaper" : path_filename(e->path));
        if (group) {
            str_printf(rel, SP_COUNTOF(rel), L"media/wallpaper/%ls", path_filename(e->path));
            if (!copy_tree(folder, target, 0)) { remove_tree(stage); return FALSE; }
        } else {
            str_printf(rel, SP_COUNTOF(rel), L"media/%ls", path_filename(e->path));
            if (!dir_create_recursive(media) || !CopyFileW(e->path, target, FALSE)) { remove_tree(stage); return FALSE; }
        }
    }
    wchar_t thumbRel[64] = L"";
    if (!link && e->thumb[0] && file_exists(e->thumb)) {
        str_printf(thumbRel, SP_COUNTOF(thumbRel), L"thumbnail%ls", path_ext(e->thumb));
        path_join(target, SP_COUNTOF(target), stage, thumbRel);
        if (!CopyFileW(e->thumb, target, FALSE)) { remove_tree(stage); return FALSE; }
    }
    wpapi_overrides_path(e->id, props, SP_COUNTOF(props));
    if (!link && file_exists(props)) {
        path_join(target, SP_COUNTOF(target), stage, L"props.json");
        if (!CopyFileW(props, target, FALSE)) { remove_tree(stage); return FALSE; }
    }
    StrBuf b = { 0 };
    sb_puts(&b, "{\"screenplus\":\"spf\",\"version\":1,\"name\":"); json_w(&b, e->name);
    sb_printf(&b, ",\"type\":%d,\"format\":%d,\"flags\":%d,\"volCut\":%d,\"slideSec\":%d,\"path\":",
              e->type, e->format, e->flags & ~(WPFLAG_PPTX | (link ? 0 : WPFLAG_YTDLP)), e->volCut, e->slideSec);
    json_w(&b, rel);
    if (!link) { sb_puts(&b, ",\"thumb\":"); json_w(&b, thumbRel); }
    if (link && file_exists(props)) {
        JVal *custom = json_parse_file(props);
        if (!custom || custom->t != J_OBJ) {
            json_free(custom); sb_free(&b); remove_tree(stage); return FALSE;
        }
        sb_puts(&b, ",\"customisation\":");
        json_write(&b, custom);
        json_free(custom);
    }
    sb_puts(&b, "}");
    path_join(out, SP_COUNTOF(out), stage, L"manifest.json");
    BOOL ok = !b.failed && write_text(out, &b);
    sb_free(&b);
    if (ok) ok = pack_atomic(stage, dest, !link && e->type != WP_HTML && e->type != WP_APP);
    remove_tree(stage);
    return ok;
}

BOOL spf_export(const WallpaperEntry *entry, const wchar_t *path)
{
    BOOL ok = spf_pack(entry, path);
    if (!ok) LOGW(L"Export of '%ls' to %ls failed", entry ? entry->name : L"", path);
    return ok;
}

typedef struct PlaylistExport {
    WallpaperEntry *items;
    int count, minutes, ok;
    wchar_t name[64], path[SP_MAX_PATH];
} PlaylistExport;

static PlaylistExport *playlist_snapshot(const wchar_t *collection, const wchar_t *path)
{
    if (!collection || !collection[0]) return NULL;
    PlaylistExport *j = (PlaylistExport *)calloc(1, sizeof *j);
    if (!j) return NULL;
    j->items = (WallpaperEntry *)calloc((size_t)g_app.cfg.count, sizeof *j->items);
    if (!j->items) { free(j); return NULL; }
    str_copy(j->name, SP_COUNTOF(j->name), collection);
    str_copy(j->path, SP_COUNTOF(j->path), path);
    j->minutes = g_app.cfg.s.slideshowMin;
    for (int i = 0; i < g_app.cfg.count; i++)
        if (config_tag_has(&g_app.cfg.items[i], collection)) j->items[j->count++] = g_app.cfg.items[i];
    if (!j->count) { free(j->items); free(j); return NULL; }
    return j;
}

static BOOL playlist_pack(const PlaylistExport *j)
{
    wchar_t stage[SP_MAX_PATH], target[SP_MAX_PATH], member[64];
    if (!new_stage(stage, SP_COUNTOF(stage))) return FALSE;
    for (int i = 0; i < j->count; i++) {
        str_printf(member, SP_COUNTOF(member), L"%04d.spf", i);
        path_join(target, SP_COUNTOF(target), stage, member);
        if (!spf_pack(&j->items[i], target)) { remove_tree(stage); return FALSE; }
    }
    StrBuf b = { 0 };
    sb_puts(&b, "{\"screenplus\":\"spp\",\"version\":1,\"name\":");
    json_w(&b, j->name);
    sb_printf(&b, ",\"count\":%d,\"intervalMinutes\":%d}", j->count, j->minutes);
    path_join(target, SP_COUNTOF(target), stage, L"manifest.json");
    BOOL ok = !b.failed && write_text(target, &b);
    sb_free(&b);
    if (ok) ok = pack_atomic(stage, j->path, TRUE);   /* members are archives already */
    remove_tree(stage);
    return ok;
}

BOOL spp_export(const wchar_t *collection, const wchar_t *path)
{
    PlaylistExport *j = playlist_snapshot(collection, path);
    if (!j) return FALSE;
    BOOL ok = playlist_pack(j);
    free(j->items); free(j);
    return ok;
}

static DWORD WINAPI playlist_worker(LPVOID arg)
{
    PlaylistExport *j = (PlaylistExport *)arg;
    j->ok = playlist_pack(j);
    if (!PostMessageW(g_app.shellWnd, WM_SP_FORMAT, 0, (LPARAM)j)) {
        free(j->items); free(j);
    }
    return 0;
}

BOOL spp_export_start(const wchar_t *collection, const wchar_t *path)
{
    PlaylistExport *j = playlist_snapshot(collection, path);
    if (!j) return FALSE;
    HANDLE h = CreateThread(NULL, 0, playlist_worker, j, 0, NULL);
    if (!h) { free(j->items); free(j); return FALSE; }
    CloseHandle(h);
    return TRUE;
}

void sp_format_file_name(const wchar_t *name, const wchar_t *extension, wchar_t *out, size_t cap)
{
    wchar_t safe[SP_NAME_LEN];
    str_copy(safe, SP_COUNTOF(safe), name ? name : L"");
    for (wchar_t *p = safe; *p; p++) if (*p < 32 || wcschr(L"<>:\"/\\|?*", *p)) *p = L'-';
    str_trim(safe);
    size_t n = wcslen(safe);
    while (n && (safe[n - 1] == L'.' || safe[n - 1] == L' ')) safe[--n] = 0;   /* Windows drops these */
    str_printf(out, cap, L"%ls.%ls", safe[0] ? safe : L"Wallpaper", extension);
}

/* dir\file, or dir\file (2) and so on when that name is taken on disk or by one of the first `taken` paths */
static void unique_path(const wchar_t *dir, const wchar_t *file, wchar_t (*taken)[SP_MAX_PATH], int ntaken,
                        wchar_t *out, size_t cap)
{
    wchar_t stem[SP_MAX_PATH], name[SP_MAX_PATH];
    str_copy(stem, SP_COUNTOF(stem), file);
    wchar_t *dot = wcsrchr(stem, L'.');
    const wchar_t *ext = dot ? file + (dot - stem) : L"";
    if (dot) *dot = 0;
    path_join(out, cap, dir, file);
    for (int i = 2; i < 1000; i++) {
        int clash = file_exists(out);
        for (int k = 0; !clash && k < ntaken; k++) clash = str_ieq(taken[k], out);
        if (!clash) return;
        str_printf(name, SP_COUNTOF(name), L"%ls (%d)%ls", stem, i, ext);
        path_join(out, cap, dir, name);
    }
}

void sp_format_unique_path(const wchar_t *dir, const wchar_t *file, wchar_t *out, size_t cap)
{ unique_path(dir, file, NULL, 0, out, cap); }

typedef struct ManyExport {
    WallpaperEntry *items;
    wchar_t (*paths)[SP_MAX_PATH];
    int count, done;
} ManyExport;

static DWORD WINAPI many_worker(LPVOID arg)
{
    ManyExport *j = (ManyExport *)arg;
    for (int i = 0; i < j->count; i++) j->done += spf_export(&j->items[i], j->paths[i]) != 0;  /* a failure skips only itself */
    if (!PostMessageW(g_app.shellWnd, WM_SP_FORMAT, 1, (LPARAM)j)) {
        free(j->paths); free(j->items); free(j);
    }
    return 0;
}

BOOL spf_export_start(const WallpaperEntry *items, int count, const wchar_t *const *paths, const wchar_t *directory)
{
    if (!items || count < 1 || (!paths && (!directory || !directory[0]))) return FALSE;
    ManyExport *j = (ManyExport *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    j->items = (WallpaperEntry *)calloc((size_t)count, sizeof *j->items);
    j->paths = (wchar_t (*)[SP_MAX_PATH])calloc((size_t)count, sizeof *j->paths);
    if (!j->items || !j->paths) { free(j->paths); free(j->items); free(j); return FALSE; }
    memcpy(j->items, items, (size_t)count * sizeof *items);
    j->count = count;
    for (int i = 0; i < count; i++) {
        if (paths) { str_copy(j->paths[i], SP_MAX_PATH, paths[i]); continue; }
        wchar_t file[SP_MAX_PATH];
        sp_format_file_name(items[i].name, L"spf", file, SP_COUNTOF(file));
        unique_path(directory, file, j->paths, i, j->paths[i], SP_MAX_PATH);
    }
    HANDLE h = CreateThread(NULL, 0, many_worker, j, 0, NULL);
    if (!h) { free(j->paths); free(j->items); free(j); return FALSE; }
    CloseHandle(h);
    return TRUE;
}

void sp_export_on_event(WPARAM wp, LPARAM lp)
{
    if (wp == 2) { ui_on_status(); return; }   /* a preview finished reading: redraw it */
    if (wp == 1) {
        ManyExport *m = (ManyExport *)lp;
        if (!m) return;
        if (m->count == 1)
            app_notice(m->done ? NOTICE_INFO : NOTICE_WARN, m->done ? L"Exported '%ls'." : L"Could not export '%ls'.",
                       m->items[0].name);
        else if (m->done == m->count) app_notice(NOTICE_INFO, L"Exported %d wallpapers.", m->count);
        else app_notice(NOTICE_WARN, L"Exported %d of %d wallpapers. The log names the others.", m->done, m->count);
        free(m->paths); free(m->items); free(m);
        return;
    }
    PlaylistExport *j = (PlaylistExport *)lp;
    if (!j) return;
    if (j->ok) app_notice(NOTICE_INFO, L"Exported collection '%ls' (%d wallpaper%ls).", j->name, j->count,
                          j->count == 1 ? L"" : L"s");
    else app_notice(NOTICE_WARN, L"Could not export collection '%ls'.", j->name);
    free(j->items); free(j);
}

/* both live under the data folder, so a rename normally does it; copy when that fails */
static BOOL move_tree(const wchar_t *src, const wchar_t *dst)
{
    wchar_t parent[SP_MAX_PATH];
    path_dirname(parent, SP_COUNTOF(parent), dst);
    dir_create_recursive(parent);
    return MoveFileExW(src, dst, 0) || copy_tree(src, dst, 0);
}

static BOOL relative_safe(const wchar_t *s)
{
    if (!s[0] || s[0] == L'/' || s[0] == L'\\' || wcschr(s, L':') || wcschr(s, L'\\')) return FALSE;
    if (wcsstr(s, L"../") || wcsstr(s, L"/..") || !wcscmp(s, L"..")) return FALSE;
    return TRUE;
}

/* a checked archive unpacked into a new folder under <data>\format-temp */
static BOOL unpack(const wchar_t *path, wchar_t *stage, size_t cap)
{
    if (!archive_safe(path) || !new_stage(stage, cap)) return FALSE;
    if (!run_tar(L"extract", path, stage)) { remove_tree(stage); return FALSE; }
    return TRUE;
}

/* adds the wallpaper unpacked in stage to the library; the stage is used up either way */
static int import_spf_stage(const wchar_t *stage, const wchar_t *collection)
{
    wchar_t manifest[SP_MAX_PATH], src[SP_MAX_PATH], dest[SP_MAX_PATH],
            rel[SP_MAX_PATH], thumbRel[SP_MAX_PATH], name[SP_NAME_LEN], id[SP_ID_LEN], props[SP_MAX_PATH];
    path_join(manifest, SP_COUNTOF(manifest), stage, L"manifest.json");
    JVal *j = json_parse_file(manifest);
    int ok = j && !strcmp(json_str(json_get(j, "screenplus"), ""), "spf") && json_num(json_get(j, "version"), 0) == 1;
    if (ok) {
        json_str_w(json_get(j, "path"), rel, SP_COUNTOF(rel));
        json_str_w(json_get(j, "thumb"), thumbRel, SP_COUNTOF(thumbRel));
        json_str_w(json_get(j, "name"), name, SP_COUNTOF(name));
        ok = (wallpaper_is_url(rel) || relative_safe(rel)) && (!thumbRel[0] || relative_safe(thumbRel));
    }
    BOOL link = ok && wallpaper_is_url(rel);
    if (ok && !link) {                       /* archive paths use '/', the library stores native ones */
        for (wchar_t *p = rel; *p; p++) if (*p == L'/') *p = SP_SEP_W[0];
        for (wchar_t *p = thumbRel; *p; p++) if (*p == L'/') *p = SP_SEP_W[0];
    }
    if (ok && !link) {
        path_join(src, SP_COUNTOF(src), stage, rel);
        ok = file_exists(src);
    }
    if (!ok) { json_free(j); remove_tree(stage); return 0; }
    WallpaperType type = (WallpaperType)(int)json_num(json_get(j, "type"), 0);
    int flags = (int)json_num(json_get(j, "flags"), 0) & ~WPFLAG_PPTX;
    if (link) flags = (flags & ~(WPFLAG_DEPTH)) | WPFLAG_URL;
    else flags &= ~(WPFLAG_URL | WPFLAG_YTDLP);
    if (type < WP_IMAGE || type > FORMAT_MAX_TYPE || ((flags & WPFLAG_DEPTH) && type != WP_HTML) ||
        (link && (type != WP_HTML && type != WP_VIDEO))) {
        json_free(j); remove_tree(stage); return 0;
    }
    do { random_id(id); } while (config_find(&g_app.cfg, id));
    if (link) str_copy(src, SP_COUNTOF(src), rel);
    else {
        path_join(dest, SP_COUNTOF(dest), paths_root(), (flags & WPFLAG_DEPTH) ? L"depth" : L"packages");
        path_join(dest, SP_COUNTOF(dest), dest, id);
    }
    if (link) ok = TRUE;
    else if (flags & WPFLAG_DEPTH) {
        wchar_t media[SP_MAX_PATH];
        path_join(media, SP_COUNTOF(media), stage, L"media/wallpaper");
        ok = move_tree(media, dest);
        path_join(src, SP_COUNTOF(src), dest, path_filename(rel));
    } else {
        ok = move_tree(stage, dest);       /* the stage becomes the package: no second copy of a big video */
        path_join(src, SP_COUNTOF(src), dest, rel);
        if (ok) { wchar_t m[SP_MAX_PATH]; path_join(m, SP_COUNTOF(m), dest, L"manifest.json"); DeleteFileW(m); }
    }
    if (!ok || (!link && !file_exists(src))) {
        if (!link) remove_tree(dest);
        json_free(j); remove_tree(stage); return 0;
    }
    WallpaperEntry *e = config_add(&g_app.cfg, type, src, name);
    if (!e) {
        if (!link) remove_tree(dest);
        json_free(j); remove_tree(stage); return 0;
    }
    str_copy(e->id, SP_COUNTOF(e->id), id);
    e->format = SP_CLAMP((int)json_num(json_get(j, "format"), WPF_PLAIN), WPF_PLAIN, WPF_WE);
    e->flags = flags;
    e->volCut = SP_CLAMP((int)json_num(json_get(j, "volCut"), 0), 0, 100);
    e->slideSec = SP_CLAMP((int)json_num(json_get(j, "slideSec"), 0), 0, 3600);
    if (!link && thumbRel[0]) {
        if (flags & WPFLAG_DEPTH) {
            wchar_t from[SP_MAX_PATH];
            path_join(from, SP_COUNTOF(from), stage, thumbRel);
            path_join(src, SP_COUNTOF(src), dest, path_filename(thumbRel));
            CopyFileW(from, src, FALSE);
        } else path_join(src, SP_COUNTOF(src), dest, thumbRel);
        if (file_exists(src)) str_copy(e->thumb, SP_COUNTOF(e->thumb), src);
    }
    if (collection && collection[0]) config_tag_set(e, collection, TRUE);
    path_join(src, SP_COUNTOF(src), (link || (flags & WPFLAG_DEPTH)) ? stage : dest, L"props.json");
    const JVal *custom = link ? json_get(j, "customisation") : NULL;
    if ((custom && custom->t == J_OBJ) || file_exists(src)) {
        wpapi_overrides_path(e->id, props, SP_COUNTOF(props));
        wchar_t dir[SP_MAX_PATH]; path_dirname(dir, SP_COUNTOF(dir), props);
        if (dir_create_recursive(dir)) {
            if (custom && custom->t == J_OBJ) {
                StrBuf b = { 0 };
                json_write(&b, custom);
                if (!b.failed) write_text(props, &b);
                sb_free(&b);
            } else CopyFileW(src, props, FALSE);
        }
    }
    json_free(j); remove_tree(stage);
    return 1;
}

/* ---------------------------------------------------------------- preview
 * Opening or dropping an archive shows what is in it before anything reaches the library. The archive is unpacked
 * once on a worker; "Add to library" then imports from that copy (a rename, not a second copy), and Cancel deletes it. */

static int64_t tree_bytes(const wchar_t *dir, int depth)
{
    wchar_t pattern[SP_MAX_PATH]; path_join(pattern, SP_COUNTOF(pattern), dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE || depth > 24) { if (h != INVALID_HANDLE_VALUE) FindClose(h); return 0; }
    int64_t total = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t p[SP_MAX_PATH]; path_join(p, SP_COUNTOF(p), dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += tree_bytes(p, depth + 1);
        else total += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static int64_t one_file_bytes(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) ? ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow : 0;
}

/* keys of a JSON object, joined for a row: "brightness, contrast and 2 more" */
static void key_list(const JVal *o, BOOL skipOwn, wchar_t *out, size_t cap, int *count)
{
    out[0] = 0; *count = 0;
    if (!o || o->t != J_OBJ) return;
    int shown = 0;
    for (int i = 0; i < o->n; i++) {
        if (skipOwn && !strncmp(o->keys[i], "_screenplus", 11)) continue;
        if (++*count > 3) continue;
        wchar_t *k = wide_from_utf8(o->keys[i], -1);
        if (shown++) str_cat(out, cap, L", ");
        str_cat(out, cap, k ? k : L"?");
        free(k);
    }
    if (*count > 3) { wchar_t more[32]; str_printf(more, SP_COUNTOF(more), L" and %d more", *count - 3); str_cat(out, cap, more); }
}

static void read_item(FormatItem *it)
{
    wchar_t manifest[SP_MAX_PATH], rel[SP_MAX_PATH], thumbRel[SP_MAX_PATH], p[SP_MAX_PATH];
    path_join(manifest, SP_COUNTOF(manifest), it->stage, L"manifest.json");
    JVal *j = json_parse_file(manifest);
    if (!j || strcmp(json_str(json_get(j, "screenplus"), ""), "spf") || json_num(json_get(j, "version"), 0) != 1) {
        json_free(j); return;
    }
    WallpaperEntry *e = &it->e;
    json_str_w(json_get(j, "name"), e->name, SP_COUNTOF(e->name));
    json_str_w(json_get(j, "path"), rel, SP_COUNTOF(rel));
    json_str_w(json_get(j, "thumb"), thumbRel, SP_COUNTOF(thumbRel));
    e->type = (WallpaperType)(int)json_num(json_get(j, "type"), WP_IMAGE);
    if (e->type < WP_IMAGE || e->type > FORMAT_MAX_TYPE) { json_free(j); return; }
    e->format = SP_CLAMP((int)json_num(json_get(j, "format"), WPF_PLAIN), WPF_PLAIN, WPF_WE);
    e->flags = (int)json_num(json_get(j, "flags"), 0);
    e->volCut = SP_CLAMP((int)json_num(json_get(j, "volCut"), 0), 0, 100);
    random_id(e->id);                                  /* its own thumbnail cache slot while shown */
    it->link = wallpaper_is_url(rel);
    if (!it->link && (!relative_safe(rel) || (thumbRel[0] && !relative_safe(thumbRel)))) { json_free(j); return; }
    for (wchar_t *c = rel; !it->link && *c; c++) if (*c == L'/') *c = SP_SEP_W[0];
    for (wchar_t *c = thumbRel; *c; c++) if (*c == L'/') *c = SP_SEP_W[0];
    if (it->link) str_copy(e->path, SP_COUNTOF(e->path), rel);
    else {
        path_join(e->path, SP_COUNTOF(e->path), it->stage, rel);
        if (!file_exists(e->path)) { json_free(j); return; }
        if (e->type == WP_HTML || e->type == WP_APP) { path_dirname(p, SP_COUNTOF(p), e->path); it->bytes = tree_bytes(p, 0); }
        else it->bytes = one_file_bytes(e->path);
        if (thumbRel[0]) path_join(e->thumb, SP_COUNTOF(e->thumb), it->stage, thumbRel);
        if (!file_exists(e->thumb)) e->thumb[0] = 0;
    }
    it->depthModel = (e->flags & WPFLAG_DEPTH) ? depth_entry_model(e) : -1;
    if (it->depthModel < 0 && (e->flags & WPFLAG_DEPTH)) it->depthModel = -2;      /* a model this build doesn't know */

    JVal *own = NULL;
    const JVal *custom = json_get(j, "customisation");
    if (!custom) {
        path_join(p, SP_COUNTOF(p), it->stage, L"props.json");
        if (file_exists(p)) custom = own = json_parse_file(p);
    }
    if (custom && custom->t == J_OBJ) {
        it->upscale = SP_CLAMP((int)json_num(json_geti(custom, "_screenplus_upscale"), 0), UPSCALE_OFF, UPSCALE_REALESRGAN);
        key_list(json_geti(custom, "_screenplus_adjust"), FALSE, it->looks, SP_COUNTOF(it->looks), &it->lookCount);
        key_list(custom, TRUE, it->props, SP_COUNTOF(it->props), &it->propCount);
        const JVal *rec = json_geti(custom, "_screenplus_rec");
        static const char *const sections[3] = { "look", "props", "volume" };
        for (int s = 0; s < 3; s++) {
            const JVal *r = json_get(rec, sections[s]);
            int n = (int)json_num(json_get(r, "n"), 0);
            const JVal *tr = json_get(r, "tracks");
            if (n < 2 || !tr || tr->t != J_ARR || !tr->n) continue;
            it->recSec[s] = (float)n / REC_RATE;
            it->recTracks[s] = tr->n;
            for (int k = 0; k < tr->n && k < 3; k++) {
                wchar_t *key = wide_from_utf8(json_str(json_get(&tr->kids[k], "key"), "?"), -1);
                if (k) str_cat(it->recKeys[s], SP_COUNTOF(it->recKeys[s]), L", ");
                str_cat(it->recKeys[s], SP_COUNTOF(it->recKeys[s]), key ? key : L"?");
                free(key);
            }
            if (tr->n > 3) {
                wchar_t more[32]; str_printf(more, SP_COUNTOF(more), L" and %d more", tr->n - 3);
                str_cat(it->recKeys[s], SP_COUNTOF(it->recKeys[s]), more);
            }
        }
    }
    json_free(own);
    json_free(j);
    it->ok = 1;
}

static void size_text(int64_t b, wchar_t *out, size_t cap)
{
    if (b >= 1024LL * 1024 * 1024) str_printf(out, cap, L"%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) str_printf(out, cap, L"%.1f MB", b / (1024.0 * 1024));
    else str_printf(out, cap, L"%d KB", (int)SP_MAX(1, (b + 1023) / 1024));
}

int sp_preview_rows(const FormatItem *it, wchar_t rows[][2][200], int max)
{
    int n = 0;
#define ROW(label, ...) do { if (n < max) { str_copy(rows[n][0], 200, label); str_printf(rows[n][1], 200, __VA_ARGS__); n++; } } while (0)
    const WallpaperEntry *e = &it->e;
    static const wchar_t *const types[] = { L"", L"Image", L"Video", L"Web page (HTML)", L"Program (.exe)" };
    static const wchar_t *const sources[] = { L"", L" · from Lively", L" · from Wallpaper Engine" };
    const wchar_t *kind = (e->flags & WPFLAG_DEPTH) ? ((e->flags & WPFLAG_DEPTHVIDEO) ? L"Interactive video (depth)"
                                                                                     : L"Interactive image (depth)")
                        : types[SP_CLAMP((int)e->type, 1, 4)];
    ROW(L"Type", L"%ls%ls%ls", kind, sources[SP_CLAMP(e->format, 0, 2)],
        (e->flags & WPFLAG_AUDIO) ? L" · reacts to sound" : L"");
    if (e->type == WP_APP) ROW(L"Warning", L"It runs as a program on this PC. Add it only if you trust where it came from.");
    if (it->link) ROW(L"Link", L"%ls%ls", e->path, (e->flags & WPFLAG_YTDLP) ? L" (streamed)" : L"");
    else {
        wchar_t sz[32]; size_text(it->bytes, sz, SP_COUNTOF(sz));
        ROW(L"File", L"%ls · %ls", path_filename(e->path), sz);
    }
    if (e->flags & WPFLAG_DEPTH) {
        if (it->depthModel >= 0) {
            const DepthModel *m = depth_model(it->depthModel);
            ROW(L"Depth model", L"%ls%ls", m->name, depth_model_ready(it->depthModel) ? L"" : L" (not downloaded here; not needed to play)");
        } else ROW(L"Depth model", L"A model this version of Screen+ doesn't know");
    }
    static const wchar_t *const ups[] = { L"Off", L"Smooth", L"Sharp", L"Real-ESRGAN" };
    if (it->upscale) ROW(L"Upscaling", L"%ls", ups[it->upscale]);
    if (it->lookCount) ROW(L"Look", L"%d adjusted: %ls", it->lookCount, it->looks);
    if (it->propCount) ROW(L"Properties", L"%d changed: %ls", it->propCount, it->props);
    if (e->volCut) ROW(L"Volume", L"%d%% quieter", e->volCut);
    static const wchar_t *const secs[3] = { L"Look recording", L"Properties recording", L"Volume recording" };
    for (int s = 0; s < 3; s++)
        if (it->recTracks[s]) ROW(secs[s], L"%.1f s · %d setting%ls: %ls", it->recSec[s], it->recTracks[s],
                                  it->recTracks[s] == 1 ? L"" : L"s", it->recKeys[s]);
    if (!it->upscale && !it->lookCount && !it->propCount && !it->recTracks[0] && !it->recTracks[1] && !it->recTracks[2])
        ROW(L"Customise", L"Default settings");
#undef ROW
    return n;
}

FormatPreview *sp_preview_load(const wchar_t *file)
{
    FormatPreview *p = (FormatPreview *)calloc(1, sizeof *p);
    if (!p) return NULL;
    str_copy(p->file, SP_COUNTOF(p->file), file);
    p->bytes = one_file_bytes(file);
    p->playlist = str_ieq(path_ext(file), L".spp");
    if (!p->playlist) {
        p->items = (FormatItem *)calloc(1, sizeof *p->items);
        if (!p->items) return p;
        p->count = 1;
        if (unpack(file, p->items[0].stage, SP_MAX_PATH)) read_item(&p->items[0]);
        p->ok = p->items[0].ok;
        if (p->ok) str_copy(p->name, SP_COUNTOF(p->name), p->items[0].e.name);
        return p;
    }
    if (!unpack(file, p->stage, SP_COUNTOF(p->stage))) return p;
    wchar_t manifest[SP_MAX_PATH], member[64], target[SP_MAX_PATH];
    path_join(manifest, SP_COUNTOF(manifest), p->stage, L"manifest.json");
    JVal *j = json_parse_file(manifest);
    int count = j && !strcmp(json_str(json_get(j, "screenplus"), ""), "spp") &&
                json_num(json_get(j, "version"), 0) == 1 ? (int)json_num(json_get(j, "count"), 0) : 0;
    p->minutes = j ? (int)json_num(json_get(j, "intervalMinutes"), 0) : 0;
    json_str_w(j ? json_get(j, "name") : NULL, p->name, SP_COUNTOF(p->name));
    json_free(j);
    if (count < 1 || count > FORMAT_MAX_FILES) return p;
    p->items = (FormatItem *)calloc((size_t)count, sizeof *p->items);
    if (!p->items) return p;
    p->count = count;
    for (int i = 0; i < count; i++) {
        str_printf(member, SP_COUNTOF(member), L"%04d.spf", i);
        path_join(target, SP_COUNTOF(target), p->stage, member);
        if (file_exists(target) && unpack(target, p->items[i].stage, SP_MAX_PATH)) read_item(&p->items[i]);
        DeleteFileW(target);                       /* unpacked: the member archive isn't needed again */
        p->ok += p->items[i].ok;
    }
    return p;
}

void sp_preview_free(FormatPreview *p)
{
    if (!p) return;
    for (int i = 0; i < p->count; i++) if (p->items[i].stage[0]) remove_tree(p->items[i].stage);
    if (p->stage[0]) remove_tree(p->stage);
    free(p->items); free(p);
}

int sp_preview_import(FormatPreview *p)
{
    if (!p || !p->ok) return 0;
    int added = 0;
    for (int i = 0; i < p->count; i++) {
        if (!p->items[i].ok) continue;
        added += import_spf_stage(p->items[i].stage, p->playlist ? p->name : NULL);
        p->items[i].stage[0] = 0;                  /* used up */
    }
    if (added && p->playlist && p->minutes > 0 && p->minutes <= 1440) g_app.cfg.s.slideshowMin = p->minutes;
    return added;
}

/* GUI: load on a worker; WM_SP_FORMAT (2) tells the window. The window owns one reference, the worker the other. */
static DWORD WINAPI preview_worker(LPVOID arg)
{
    FormatPreviewJob *job = (FormatPreviewJob *)arg;
    FormatPreview *p = sp_preview_load(job->file);
    if (InterlockedCompareExchange(&job->closed, 0, 0)) sp_preview_free(p);
    else {
        job->preview = p;
        InterlockedExchange(&job->ready, 1);
        PostMessageW(g_app.shellWnd, WM_SP_FORMAT, 2, 0);
    }
    if (InterlockedDecrement(&job->refs) == 0) { sp_preview_free(job->preview); free(job); }
    return 0;
}

FormatPreviewJob *sp_preview_start(const wchar_t *file)
{
    FormatPreviewJob *job = (FormatPreviewJob *)calloc(1, sizeof *job);
    if (!job) return NULL;
    str_copy(job->file, SP_COUNTOF(job->file), file);
    job->bytes = one_file_bytes(file);
    job->refs = 2;
    HANDLE h = CreateThread(NULL, 0, preview_worker, job, 0, NULL);
    if (!h) { free(job); return NULL; }
    CloseHandle(h);
    return job;
}

void sp_preview_close(FormatPreviewJob *job, BOOL import)
{
    if (!job) return;
    if (import && InterlockedCompareExchange(&job->ready, 0, 0) && job->preview) {
        FormatPreview *p = job->preview;
        int added = sp_preview_import(p);
        if (added) {
            app_save_config();
            ui_on_config_changed();
            if (p->playlist) app_notice(NOTICE_INFO, L"Added %d wallpaper%ls to the collection '%ls'.", added,
                                        added == 1 ? L"" : L"s", p->name);
            else app_notice(NOTICE_INFO, L"Added '%ls' to the library.", p->name);
        } else app_notice(NOTICE_WARN, L"Could not add '%ls'.", path_filename(job->file));
    }
    InterlockedExchange(&job->closed, 1);
    if (InterlockedCompareExchange(&job->ready, 0, 0)) { sp_preview_free(job->preview); job->preview = NULL; }
    if (InterlockedDecrement(&job->refs) == 0) { sp_preview_free(job->preview); free(job); }
}

int sp_format_import(const wchar_t *path, const wchar_t *collection)
{
    if (str_ieq(path_ext(path), L".spf")) {
        wchar_t stage[SP_MAX_PATH];
        return unpack(path, stage, SP_COUNTOF(stage)) ? import_spf_stage(stage, collection) : 0;
    }
    if (!str_ieq(path_ext(path), L".spp")) return 0;
    FormatPreview *p = sp_preview_load(path);
    if (p && collection && collection[0]) str_copy(p->name, SP_COUNTOF(p->name), collection);
    int added = sp_preview_import(p);
    sp_preview_free(p);
    return added;
}

#ifdef _WIN32
BOOL sp_format_save_dialog(HWND owner, const wchar_t *suggested, const wchar_t *extension, wchar_t *path, size_t cap)
{
    IFileSaveDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&dlg))) return FALSE;
    static const COMDLG_FILTERSPEC playlist[] = { { L"Screen+ playlist", L"*.spp" } };
    static const COMDLG_FILTERSPEC wallpaper[] = { { L"Screen+ wallpaper", L"*.spf" } };
    BOOL isPlaylist = str_ieq(extension, L"spp");
    IFileSaveDialog_SetFileTypes(dlg, 1, isPlaylist ? playlist : wallpaper);
    IFileSaveDialog_SetDefaultExtension(dlg, isPlaylist ? L"spp" : L"spf");
    IFileSaveDialog_SetFileName(dlg, suggested);
    IFileSaveDialog_SetTitle(dlg, isPlaylist ? L"Export Screen+ playlist" : L"Export Screen+ wallpaper");
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
