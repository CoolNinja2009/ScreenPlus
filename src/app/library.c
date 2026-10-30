/* Library management. Imports only what the user explicitly asks for: picked files, the top
 * level of a picked folder, or the wallpaper libraries of Lively Wallpaper / Wallpaper Engine
 * (known locations only, never a drive scan). Entries store paths; wallpaper files are never
 * copied, modified or deleted, except those Screen+ made itself (<data>\downloads, cache\streams:
 * url_import.c; <data>\depth: depth.c), which are deleted with their entry (library_discard). */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/json.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

#define FOLDER_IMPORT_MAX  500
#define PACKAGE_IMPORT_MAX 2000

typedef struct ImportStats {
    int added, dup, bad, unsupported, packages;
} ImportStats;

BOOL library_html_entry(const wchar_t *folder, wchar_t *out, size_t cap)
{
    /* A folder import deliberately records the document inside its own folder.
     * html_renderer maps that entire folder to a virtual HTTPS host, so CSS,
     * JavaScript, images, fonts, media and relative subfolders all resolve as
     * they did in the original wallpaper. Nothing needs to be copied. */
    static const wchar_t *names[] = { L"index.html", L"index.htm", L"wallpaper.html", L"wallpaper.htm",
                                      L"main.html", L"main.htm", L"start.html" };
    for (size_t i = 0; i < SP_COUNTOF(names); i++) {
        path_join(out, cap, folder, names[i]);
        if (file_exists(out)) return TRUE;
    }
    /* exactly one top-level HTML file */
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), folder, L"*.htm*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wallpaper_type_from_path(fd.cFileName) == WP_HTML) {
            if (++n == 1) path_join(out, cap, folder, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n == 1;
}

static int add_entry(WallpaperType t, const wchar_t *path, const wchar_t *name, int format, int flags,
                     const wchar_t *thumb, ImportStats *st)
{
    if (config_find_path(&g_app.cfg, path)) {
        st->dup++;
        return 0;
    }
    WallpaperEntry *e = config_add(&g_app.cfg, t, path, name && name[0] ? name : NULL);
    if (!e) {
        st->bad++;
        return 0;
    }
    e->format = format;
    e->flags = flags;
    if (thumb && thumb[0] && file_exists(thumb)) str_copy(e->thumb, SP_COUNTOF(e->thumb), thumb);
    st->added++;
    return 1;
}

static int add_file(const wchar_t *path, ImportStats *st)
{
    WallpaperType t = wallpaper_type_from_path(path);
    if (t == WP_NONE) {
        st->bad++;
        return 0;
    }
    wchar_t name[SP_NAME_LEN] = L"";
    if (t == WP_HTML) {                      /* HTML wallpapers are named after their folder */
        wchar_t dir[SP_MAX_PATH];
        path_dirname(dir, SP_COUNTOF(dir), path);
        str_copy(name, SP_COUNTOF(name), path_filename(dir));
    }
    return add_entry(t, path, name, WPF_PLAIN, 0, NULL, st);
}

/* ---------------------------------------------------------------- packages */

/* Lively's own library and Screen+'s samples hold the same packages under the same folder names
 * (0wj1biqk.f41...), so a package folder already in the library is a duplicate wherever it lives. */
static BOOL lively_known(const wchar_t *folder)
{
    const wchar_t *name = path_filename(folder);
    if (!name[0]) return FALSE;
    wchar_t seg[SP_MAX_PATH];
    str_printf(seg, SP_COUNTOF(seg), L"\\%ls\\", name);
    for (int i = 0; i < g_app.cfg.count; i++)
        if (g_app.cfg.items[i].format == WPF_LIVELY && str_icontains(g_app.cfg.items[i].path, seg)) return TRUE;
    return FALSE;
}

/* Lively Wallpaper package: <folder>\LivelyInfo.json. Returns 1 if the folder is a Lively package. */
static int import_lively_package(const wchar_t *folder, ImportStats *st)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), folder, L"LivelyInfo.json");
    if (!file_exists(p)) return 0;
    JVal *j = json_parse_file(p);
    st->packages++;
    if (!j) {
        LOGW(L"library: unreadable %ls", p);
        st->bad++;
        return 1;
    }
    int type = (int)json_num(json_geti(j, "Type"), -1);
    /* Lively enables event feeds through Arguments in LivelyInfo.json; honor the
     * known switches so a wallpaper that registers its listener late (or after
     * async init) still receives the data even if the page probe misses it. */
    wchar_t args[SP_NAME_LEN];
    json_str_w(json_geti(j, "Arguments"), args, SP_COUNTOF(args));
    int argAudio = str_icontains(args, L"--audio");
    int argSysInfo = str_icontains(args, L"--system-information");
    wchar_t title[SP_NAME_LEN], file[SP_MAX_PATH], tf[SP_MAX_PATH], thumb[SP_MAX_PATH] = L"", target[SP_MAX_PATH];
    json_str_w(json_geti(j, "Title"), title, SP_COUNTOF(title));
    json_str_w(json_geti(j, "FileName"), file, SP_COUNTOF(file));
    int absolute = json_bool(json_geti(j, "IsAbsolutePath"), 0);
    json_str_w(json_geti(j, "Thumbnail"), tf, SP_COUNTOF(tf));
    if (tf[0]) path_join(thumb, SP_COUNTOF(thumb), folder, tf);
    if (!thumb[0] || !file_exists(thumb)) {
        json_str_w(json_geti(j, "Preview"), tf, SP_COUNTOF(tf));
        if (tf[0]) path_join(thumb, SP_COUNTOF(thumb), folder, tf);
    }
    json_free(j);
    if (!title[0]) str_copy(title, SP_COUNTOF(title), path_filename(folder));
    if (type == 3 || wallpaper_is_url(file)) str_copy(target, SP_COUNTOF(target), file);
    else if (absolute || (file[0] && file[1] == L':')) str_copy(target, SP_COUNTOF(target), file);
    else path_join(target, SP_COUNTOF(target), folder, file);

    WallpaperType t;
    int flags = 0;
    if (argAudio) flags |= WPFLAG_AUDIO;
    if (argSysInfo) flags |= WPFLAG_SYSINFO;
    switch (type) {   /* Lively WallpaperType */
    case 1:  t = WP_HTML; break;                          /* web */
    case 2:  t = WP_HTML; flags = WPFLAG_AUDIO; break;    /* webaudio */
    case 3:  t = WP_HTML; flags = WPFLAG_URL; break;      /* url */
    case 7:  t = WP_VIDEO; break;                         /* video */
    case 8:  t = WP_VIDEO; break;                         /* gif (played by FFmpeg) */
    case 11: t = WP_IMAGE; break;                         /* picture */
    default:
        /* app, bizhawk, unity, godot, unityaudio, videostream: external programs or streams */
        LOGI(L"library: skipping Lively '%ls' (type %d is not supported)", title, type);
        st->unsupported++;
        return 1;
    }
    if (!wallpaper_is_url(target) && !file_exists(target)) {
        LOGW(L"library: Lively '%ls' entry file missing: %ls", title, target);
        st->bad++;
        return 1;
    }
    if (lively_known(folder)) st->dup++;
    else add_entry(t, target, title, WPF_LIVELY, flags, thumb, st);
    return 1;
}

/* Wallpaper Engine package: <folder>\project.json (web and video types). */
static int import_we_package(const wchar_t *folder, ImportStats *st)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), folder, L"project.json");
    if (!file_exists(p)) return 0;
    JVal *j = json_parse_file(p);
    st->packages++;
    if (!j) {
        st->bad++;
        return 1;
    }
    const char *type = json_str(json_geti(j, "type"), "");
    wchar_t title[SP_NAME_LEN], file[SP_MAX_PATH], pv[SP_MAX_PATH], thumb[SP_MAX_PATH] = L"", target[SP_MAX_PATH];
    json_str_w(json_geti(j, "title"), title, SP_COUNTOF(title));
    json_str_w(json_geti(j, "file"), file, SP_COUNTOF(file));
    json_str_w(json_geti(j, "preview"), pv, SP_COUNTOF(pv));
    if (pv[0]) path_join(thumb, SP_COUNTOF(thumb), folder, pv);
    WallpaperType t = !_stricmp(type, "web") ? WP_HTML : !_stricmp(type, "video") ? WP_VIDEO : WP_NONE;
    json_free(j);
    if (!title[0]) str_copy(title, SP_COUNTOF(title), path_filename(folder));
    if (t == WP_NONE || !file[0]) {
        LOGI(L"library: skipping Wallpaper Engine '%ls' (type '%hs' is not supported)", title, type);
        st->unsupported++;   /* scene / application wallpapers use Wallpaper Engine's own renderer */
        return 1;
    }
    path_join(target, SP_COUNTOF(target), folder, file);
    if (!file_exists(target)) {
        st->bad++;
        return 1;
    }
    add_entry(t, target, title, WPF_WE, 0, thumb, st);
    return 1;
}

static int import_package(const wchar_t *folder, ImportStats *st)
{
    return import_lively_package(folder, st) || import_we_package(folder, st);
}

/* A folder whose subfolders are packages (a Lively "wallpapers" folder, a Steam workshop folder). */
static int import_package_library(const wchar_t *root, ImportStats *st)
{
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), root, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int seen = 0, n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (++seen > PACKAGE_IMPORT_MAX) break;
        wchar_t sub[SP_MAX_PATH];
        path_join(sub, SP_COUNTOF(sub), root, fd.cFileName);
        n += import_package(sub, st);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void add_folder(const wchar_t *folder, ImportStats *st)
{
    if (import_package(folder, st)) return;
    if (import_package_library(folder, st)) return;
    wchar_t entry[SP_MAX_PATH];
    if (library_html_entry(folder, entry, SP_COUNTOF(entry))) {
        add_file(entry, st);
        return;
    }
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), folder, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int seen = 0;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        WallpaperType t = wallpaper_type_from_path(fd.cFileName);
        if (t != WP_IMAGE && t != WP_VIDEO) continue;
        if (++seen > FOLDER_IMPORT_MAX) break;
        wchar_t p[SP_MAX_PATH];
        path_join(p, SP_COUNTOF(p), folder, fd.cFileName);
        add_file(p, st);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void report(const ImportStats *st, const wchar_t *source, int before, BOOL applyFirst)
{
    if (st->added) {
        LOGI(L"library: %ls: added %d, already present %d, unsupported %d, failed %d", source, st->added, st->dup,
             st->unsupported, st->bad);
        if (applyFirst && !app_wallpaper_for(NULL)[0]) app_apply_wallpaper(g_app.cfg.items[before].id, NULL);
        else app_save_config();
    }
    wchar_t extra[160] = L"";
    if (st->unsupported)
        str_printf(extra, SP_COUNTOF(extra), L" %d unsupported.",
                   st->unsupported);
    if (st->added)
        app_notice(st->bad ? NOTICE_WARN : NOTICE_INFO, L"Added %d wallpaper%ls%ls%ls.%ls%ls", st->added,
                   st->added == 1 ? L"" : L"s", source[0] ? L" from " : L"", source, extra,
                   st->bad ? L" Some could not be read." : L"");
    else if (st->dup && !st->bad)
        app_notice(NOTICE_INFO, L"Everything%ls%ls is already in your library.%ls", source[0] ? L" from " : L"", source, extra);
    else if (st->unsupported && !st->bad)
        app_notice(NOTICE_WARN, L"Nothing to import.%ls", extra);
    else
        app_notice(NOTICE_WARN, L"Nothing was imported. These files are not supported.");
    ui_on_config_changed();
}

int library_import(const wchar_t *const *paths, int count, BOOL applyFirst)
{
    ImportStats st = { 0 };
    int before = g_app.cfg.count;
    for (int i = 0; i < count; i++) {
        if (dir_exists(paths[i])) add_folder(paths[i], &st);
        else if (file_exists(paths[i])) {
            if (str_ieq(path_filename(paths[i]), L"LivelyInfo.json") || str_ieq(path_filename(paths[i]), L"project.json")) {
                wchar_t dir[SP_MAX_PATH];
                path_dirname(dir, SP_COUNTOF(dir), paths[i]);
                if (!import_package(dir, &st)) st.bad++;
            } else {
                add_file(paths[i], &st);
            }
        } else st.bad++;
    }
    if (count) report(&st, L"", before, applyFirst);
    return st.added;
}

/* ---------------------------------------------------------------- known libraries */

static int add_root(wchar_t roots[][SP_MAX_PATH], int n, int max, const wchar_t *dir)
{
    if (n >= max || !dir_exists(dir)) return n;
    for (int i = 0; i < n; i++)
        if (str_ieq(roots[i], dir)) return n;
    str_copy(roots[n], SP_MAX_PATH, dir);
    return n + 1;
}

static int lively_roots(wchar_t roots[][SP_MAX_PATH], int max)
{
    PWSTR local = NULL;
    int n = 0;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)) || !local) return 0;
    wchar_t bases[4][SP_MAX_PATH];
    int nb = 0;
    path_join(bases[nb++], SP_MAX_PATH, local, L"Lively Wallpaper");                 /* installer build */
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), local, L"Packages\\*rocksdanister.LivelyWallpaper*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);                                         /* Microsoft Store build */
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nb < 4) {
                wchar_t pkg[SP_MAX_PATH];
                path_join(pkg, SP_COUNTOF(pkg), local, L"Packages");
                path_join(pkg, SP_COUNTOF(pkg), pkg, fd.cFileName);
                path_join(bases[nb++], SP_MAX_PATH, pkg, L"LocalCache\\Local\\Lively Wallpaper");
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int b = 0; b < nb; b++) {
        wchar_t p[SP_MAX_PATH];
        path_join(p, SP_COUNTOF(p), bases[b], L"Library\\wallpapers");
        n = add_root(roots, n, max, p);
        /* custom library location (Settings.json "WallpaperDir") */
        path_join(p, SP_COUNTOF(p), bases[b], L"Settings.json");
        JVal *s = json_parse_file(p);
        if (s) {
            wchar_t dir[SP_MAX_PATH];
            json_str_w(json_geti(s, "WallpaperDir"), dir, SP_COUNTOF(dir));
            json_free(s);
            if (dir[0]) {
                path_join(p, SP_COUNTOF(p), dir, L"wallpapers");
                n = add_root(roots, n, max, p);
                /* Store builds see %LOCALAPPDATA% virtualized into their package LocalCache */
                wchar_t prefix[SP_MAX_PATH];
                path_join(prefix, SP_COUNTOF(prefix), local, L"Lively Wallpaper");
                if (b > 0 && str_istarts(dir, prefix)) {
                    path_join(p, SP_COUNTOF(p), bases[b], dir + wcslen(prefix) + (dir[wcslen(prefix)] == L'\\'));
                    path_join(p, SP_COUNTOF(p), p, L"wallpapers");
                    n = add_root(roots, n, max, p);
                }
            }
        }
    }
    CoTaskMemFree(local);
    return n;
}

/* Runs on every normal launch. This is intentionally quiet and incremental:
 * config_find_path prevents duplicates, and only Lively's known library paths
 * are enumerated. It does not scan arbitrary disks or assign a new wallpaper. */
int library_auto_import_lively(void)
{
    wchar_t roots[8][SP_MAX_PATH];
    int n = lively_roots(roots, SP_COUNTOF(roots));
    if (!n) return 0;                 /* Lively is not installed, or has no library yet */
    ImportStats st = { 0 };
    for (int i = 0; i < n; i++) {
        LOGI(L"library: auto-discovering Lively wallpapers in %ls", roots[i]);
        import_package_library(roots[i], &st);
    }
    if (st.added) {
        if (!config_save(&g_app.cfg, paths_config()))
            LOGW(L"library: could not save auto-imported Lively wallpapers");
        else
            LOGI(L"library: automatically added %d Lively wallpaper%ls (%d already known, %d unsupported, %d unreadable)",
                 st.added, st.added == 1 ? L"" : L"s", st.dup, st.unsupported, st.bad);
    }
    return st.added;
}

/* The wallpapers in <exe>\samples: Aurora (always bundled) and the others (in the portable zip, or downloaded by
 * setup: `screenplus samples install`), Lively packages or folders with an HTML entry. Added silently on the
 * first run and by setup's default installation; Aurora goes first. */
int library_add_samples(void)
{
    wchar_t root[SP_MAX_PATH], sub[SP_MAX_PATH], entry[SP_MAX_PATH];
    path_join(root, SP_COUNTOF(root), paths_exe_dir(), L"samples");
    path_join(sub, SP_COUNTOF(sub), root, L"Aurora");
    ImportStats st = { 0 };
    if (library_html_entry(sub, entry, SP_COUNTOF(entry))) add_file(entry, &st);
    path_join(sub, SP_COUNTOF(sub), root, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(sub, &fd);
    if (h == INVALID_HANDLE_VALUE) return st.added;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        path_join(sub, SP_COUNTOF(sub), root, fd.cFileName);
        if (!import_package(sub, &st) && library_html_entry(sub, entry, SP_COUNTOF(entry))) add_file(entry, &st);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (st.added) LOGI(L"library: added %d sample wallpaper%ls", st.added, st.added == 1 ? L"" : L"s");
    return st.added;
}

static int steam_libraries(wchar_t libs[][SP_MAX_PATH], int max)
{
    int n = 0;
    wchar_t steam[SP_MAX_PATH];
    DWORD sz = sizeof steam;
    struct { HKEY root; const wchar_t *key, *value; } srcs[] = {
        { HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath" },
    };
    for (size_t s = 0; s < SP_COUNTOF(srcs); s++) {
        sz = sizeof steam;
        if (RegGetValueW(srcs[s].root, srcs[s].key, srcs[s].value, RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS) continue;
        for (wchar_t *c = steam; *c; c++) if (*c == L'/') *c = L'\\';
        if (!(steam[0] && steam[1] == L':')) continue;          /* need an absolute path */
        n = add_root(libs, n, max, steam);
        /* additional libraries: steamapps\libraryfolders.vdf  ("path"  "D:\\SteamLibrary") */
        wchar_t vdf[SP_MAX_PATH];
        path_join(vdf, SP_COUNTOF(vdf), steam, L"steamapps\\libraryfolders.vdf");
        HANDLE f = CreateFileW(vdf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        char buf[65536];
        DWORD rd = 0;
        ReadFile(f, buf, sizeof buf - 1, &rd, NULL);
        CloseHandle(f);
        buf[rd] = 0;
        for (char *q = strstr(buf, "\"path\""); q; q = strstr(q + 6, "\"path\"")) {
            char *a = strchr(q + 6, '"');
            if (!a) break;
            char path[SP_MAX_PATH];
            size_t k = 0;
            for (a++; *a && *a != '"' && k < sizeof path - 1; a++) {
                if (a[0] == '\\' && a[1] == '\\') a++;
                path[k++] = *a;
            }
            path[k] = 0;
            wchar_t *w = wide_from_utf8(path, -1);
            if (w) {
                n = add_root(libs, n, max, w);
                free(w);
            }
        }
    }
    return n;
}

void library_import_lively(void)
{
    wchar_t roots[8][SP_MAX_PATH];
    int n = lively_roots(roots, 8);
    ImportStats st = { 0 };
    int before = g_app.cfg.count;
    for (int i = 0; i < n; i++) {
        LOGI(L"library: scanning Lively library %ls", roots[i]);
        import_package_library(roots[i], &st);
    }
    if (!n) {
        app_notice(NOTICE_WARN, L"No Lively Wallpaper library was found.");
        return;
    }
    report(&st, L"Lively Wallpaper", before, TRUE);
}

void library_import_wallpaper_engine(void)
{
    wchar_t libs[8][SP_MAX_PATH];
    int n = steam_libraries(libs, 8), found = 0;
    ImportStats st = { 0 };
    int before = g_app.cfg.count;
    static const wchar_t *subs[] = { L"steamapps\\workshop\\content\\431960",
                                     L"steamapps\\common\\wallpaper_engine\\projects\\myprojects",
                                     L"steamapps\\common\\wallpaper_engine\\projects\\defaultprojects" };
    for (int i = 0; i < n; i++) {
        for (size_t s = 0; s < SP_COUNTOF(subs); s++) {
            wchar_t p[SP_MAX_PATH];
            path_join(p, SP_COUNTOF(p), libs[i], subs[s]);
            if (!dir_exists(p)) continue;
            found++;
            LOGI(L"library: scanning Wallpaper Engine folder %ls", p);
            import_package_library(p, &st);
        }
    }
    if (!found) {
        app_notice(NOTICE_WARN, L"No Wallpaper Engine library was found. Use Import folder on a workshop item or project folder.");
        return;
    }
    report(&st, L"Wallpaper Engine", before, TRUE);
}

void library_import_dialog(HWND owner, BOOL folder)
{
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return;
    FILEOPENDIALOGOPTIONS opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT;
    if (folder) opts |= FOS_PICKFOLDERS;
    else opts |= FOS_FILEMUSTEXIST;
    IFileOpenDialog_SetOptions(dlg, opts);
    if (folder) {
        IFileOpenDialog_SetTitle(dlg, L"Choose a wallpaper folder");
    } else {
        static const COMDLG_FILTERSPEC filters[] = {
            { L"Wallpapers", L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr;"
                             L"*.mp4;*.m4v;*.mov;*.wmv;*.webm;*.mkv;*.avi;*.mpg;*.mpeg;*.ts;*.m2ts;*.html;*.htm;"
                             L"LivelyInfo.json;project.json" },
            { L"Images", L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr" },
            { L"Videos", L"*.mp4;*.m4v;*.mov;*.wmv;*.webm;*.mkv;*.avi;*.mpg;*.mpeg;*.ts;*.m2ts;*.gif" },
            { L"HTML wallpaper", L"*.html;*.htm" },
            { L"Lively / Wallpaper Engine package", L"LivelyInfo.json;project.json" },
            { L"All files", L"*.*" },
        };
        IFileOpenDialog_SetFileTypes(dlg, (UINT)SP_COUNTOF(filters), filters);
        IFileOpenDialog_SetTitle(dlg, L"Import wallpapers");
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
        IShellItemArray *items = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items))) {
            DWORD n = 0;
            IShellItemArray_GetCount(items, &n);
            wchar_t **paths = (wchar_t **)calloc(n ? n : 1, sizeof(wchar_t *));
            int got = 0;
            for (DWORD i = 0; paths && i < n; i++) {
                IShellItem *it = NULL;
                if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &it))) {
                    PWSTR p = NULL;
                    if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) paths[got++] = p;
                    IShellItem_Release(it);
                }
            }
            if (got) library_import((const wchar_t *const *)paths, got, TRUE);
            for (int i = 0; i < got; i++) CoTaskMemFree(paths[i]);
            free(paths);
            IShellItemArray_Release(items);
        }
    }
    IFileOpenDialog_Release(dlg);
}

void library_remove(const wchar_t *id)
{
    WallpaperEntry *e = config_find(&g_app.cfg, id);
    if (!e) return;
    wchar_t name[SP_NAME_LEN], path[SP_MAX_PATH], thumb[SP_MAX_PATH];
    WallpaperEntry gone = *e;                   /* depth_forget needs it once the entry is out of the list */
    str_copy(name, SP_COUNTOF(name), e->name);
    str_copy(path, SP_COUNTOF(path), e->path);
    str_copy(thumb, SP_COUNTOF(thumb), e->thumb);
    config_remove(&g_app.cfg, id);
    app_save_config();
    depth_forget(&gone);                        /* the page "Make wallpaper cooler" generated goes with its entry */
    /* what Screen+ downloaded for a web link goes with its entry */
    if (url_import_forget(path, thumb)) {
        LOGI(L"library: removed '%ls' and deleted its download", name);
        app_notice(NOTICE_INFO, L"Removed '%ls' and its downloaded file.", name);
    } else if (wallpaper_is_url(path)) {                       /* a web page or stream: no file */
        LOGI(L"library: removed '%ls'", name);
        app_notice(NOTICE_INFO, L"Removed '%ls' from the library.", name);
    } else {
        LOGI(L"library: removed '%ls' (file left untouched)", name);
        app_notice(NOTICE_INFO, L"Removed '%ls' from the library.", name);
    }
    ui_on_config_changed();
}

void library_check_missing(void)
{
    for (int i = 0; i < g_app.cfg.count; i++)
        g_app.cfg.items[i].missing = !wallpaper_source_exists(&g_app.cfg.items[i]);
}

/* ---------------------------------------------------------------- deleting what Screen+ made */

/* A file or folder Screen+ made for an entry (a link's download, a depth page) goes with the entry. One
 * still open then (the wallpaper on screen) is listed in pending-delete.txt and retried at the next start.
 * Nothing is ever deleted for being missing from the library: one that loaded short (an old copy of
 * config.ini) must not cost the user the wallpapers it doesn't list. */
static void pending_list(wchar_t *out, size_t cap) { path_join(out, cap, paths_root(), L"pending-delete.txt"); }

static BOOL already_gone(void)
{
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

/* a file, or a folder of files (depth pages have no subfolders) */
static BOOL delete_now(const wchar_t *path, BOOL folder)
{
    if (!folder) return DeleteFileW(path) || already_gone();
    wchar_t pat[SP_MAX_PATH], f[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    path_join(pat, SP_COUNTOF(pat), path, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path_join(f, SP_COUNTOF(f), path, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path) || already_gone();
}

static void pending_add(const wchar_t *path, BOOL folder)
{
    wchar_t list[SP_MAX_PATH], line[SP_MAX_PATH + 4];
    pending_list(list, SP_COUNTOF(list));
    str_printf(line, SP_COUNTOF(line), L"%ls%ls\n", path, folder ? L"\\" : L"");
    int n = 0;
    char *u = utf8_from_wide(line, &n);
    HANDLE h = CreateFileW(list, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (u && h != INVALID_HANDLE_VALUE) WriteFile(h, u, (DWORD)n, &written, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(u);
}

void library_discard(const wchar_t *path, BOOL folder)
{
    if (delete_now(path, folder)) return;
    pending_add(path, folder);
    LOGI(L"library: '%ls' is in use; it is deleted at the next start", path);
}

/* only what Screen+ made: a direct child of <data>\downloads or <data>\depth */
static BOOL made_here(const wchar_t *path)
{
    wchar_t dir[SP_MAX_PATH], depth[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), path);
    path_join(depth, SP_COUNTOF(depth), paths_root(), L"depth");
    return str_ieq(dir, paths_downloads()) || str_ieq(dir, depth);
}

/* an entry uses it again: its file or picture is path, or lies inside the folder path (named after an entry) */
static BOOL used_again(const wchar_t *path, BOOL folder)
{
    size_t n = wcslen(path);
    if (folder && config_find(&g_app.cfg, path_filename(path))) return TRUE;
    for (int i = 0; i < g_app.cfg.count; i++) {
        const wchar_t *p[2] = { g_app.cfg.items[i].path, g_app.cfg.items[i].thumb };
        for (int k = 0; k < 2; k++)
            if (folder ? _wcsnicmp(p[k], path, n) == 0 && p[k][n] == L'\\' : str_ieq(p[k], path)) return TRUE;
    }
    return FALSE;
}

void library_purge(void)
{
    wchar_t list[SP_MAX_PATH];
    pending_list(list, SP_COUNTOF(list));
    HANDLE h = CreateFileW(list, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    char *buf = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20) ? (char *)malloc((size_t)size.QuadPart + 1) : NULL;
    DWORD got = 0;
    BOOL ok = buf && ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    wchar_t *text = ok ? wide_from_utf8(buf, (int)got) : NULL;
    free(buf);
    if (!text) return;
    DeleteFileW(list);                          /* what still can't go is listed again */
    int n = 0;
    for (wchar_t *line = text, *next; *line; line = next) {
        next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        else next = line + wcslen(line);
        size_t len = wcslen(line);
        if (len && line[len - 1] == L'\r') line[--len] = 0;
        BOOL folder = len && line[len - 1] == L'\\';
        if (folder) line[--len] = 0;
        if (!len || !made_here(line) || used_again(line, folder)) continue;
        if (delete_now(line, folder)) n++;
        else pending_add(line, folder);
    }
    free(text);
    if (n) LOGI(L"library: deleted %d file%ls of removed wallpapers", n, n == 1 ? L"" : L"s");
}
