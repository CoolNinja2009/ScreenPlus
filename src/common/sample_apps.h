/* sample_apps.h: the sample wallpapers as Bare programs (Windows). Each was an HTML page in samples\<html> and is
 * now samples\<name>: <exe>, its assets, LivelyProperties.json and a LivelyInfo.json (Type 0, an app), unpacked from
 * the release's <name>.spf (cmake/sample_apps.cmake). The page's Customise names are the program's, so an entry
 * moved from the page to the program (library_migrate_samples) keeps its settings. Linux keeps the pages: it has
 * no app host. Shared by the app (library.c, samples_update.c) and ScreenPlus.com (`samples install`). */
#ifndef SP_SAMPLE_APPS_H
#define SP_SAMPLE_APPS_H

#include <wchar.h>

typedef struct SampleApp {
    const wchar_t *name;    /* samples\<name>, <name>.spf */
    const wchar_t *exe;     /* the program in it */
    const wchar_t *html;    /* the page's folder before (the same as name for Screen+'s own) */
    const wchar_t *title;   /* the library's name for it */
    const wchar_t *old;     /* the page's title, when the library had another */
} SampleApp;

static const SampleApp k_sample_apps[] = {
    { L"Aurora",            L"aurora.exe",      L"Aurora",       L"Aurora",            NULL },
    { L"Singularity",       L"singularity.exe", L"Singularity",  L"Singularity",       NULL },
    { L"KoiPond",           L"koi.exe",         L"KoiPond",      L"Koi Pond",          NULL },
    { L"Fluids",            L"fluid.exe",       L"0wj1biqk.f41", L"Fluids",            NULL },
    { L"MatrixRain",        L"matrix.exe",      L"xn0quq52.bq2", L"Matrix Rain",       L"Matrix Rain Customizable" },
    { L"MusicTunnel",       L"tunnel.exe",      L"spfaevge.qza", L"Music Tunnel",      NULL },
    { L"SimpleSystem",      L"system.exe",      L"4yvhquwl.pcv", L"Simple System",     NULL },
    { L"Rain",              L"rain.exe",        L"abqqpcp4.hv3", L"Rain",              NULL },
    { L"TrianglesAndLight", L"triangles.exe",   L"iqdvd4pt.jyo", L"Triangles & Light", NULL },
    { L"TheHill",           L"hill.exe",        L"lgz4xpht.sjn", L"The Hill",          NULL },
    { L"Parallax",          L"parallax.exe",    L"rtnm43pj.wyl", L"Parallax.js",       NULL },
    { L"PeriodicTable",     L"periodic.exe",    L"ptpcxvcd.hlz", L"Periodic Table",    NULL },
    { L"Medusae",           L"medusae.exe",     L"nps35xrp.5eh", L"Medusae",           NULL },
    { L"LivingRoom",        L"livingroom.exe",  L"mylulkt4.dn1", L"Living Room",       NULL },
    { L"MusicTV",           L"musictv.exe",     L"zmijuhra.1n3", L"Music TV",          L"Music TV (LQ)" },
    { L"NullSaint",         L"nullsaint.exe",   L"NullSaint",    L"NULL//SAINT",       NULL },
};
#define SP_SAMPLE_APP_COUNT ((int)(sizeof k_sample_apps / sizeof k_sample_apps[0]))

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

/* <root>\<folder> into out. */
static inline void sample_path(wchar_t *out, size_t cap, const wchar_t *root, const wchar_t *folder, const wchar_t *file)
{
    if (file) swprintf(out, cap, L"%ls\\%ls\\%ls", root, folder, file);
    else swprintf(out, cap, L"%ls\\%ls", root, folder);
}

/* The page is still there: its folder holds no program (a folder that is only the program's is not a page). */
static inline BOOL sample_is_page(const wchar_t *root, const SampleApp *s)
{
    wchar_t p[MAX_PATH * 2];
    sample_path(p, MAX_PATH * 2, root, s->html, NULL);
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    if (wcscmp(s->html, s->name) != 0) return TRUE;   /* its own folder: whatever is in it is the page's */
    sample_path(p, MAX_PATH * 2, root, s->name, s->exe);
    return GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES;
}

/* The program is there. */
static inline BOOL sample_has_app(const wchar_t *root, const SampleApp *s)
{
    wchar_t p[MAX_PATH * 2];
    sample_path(p, MAX_PATH * 2, root, s->name, s->exe);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* A folder and everything in it, quietly (the Recycle Bin is not involved). */
static inline BOOL sample_delete_dir(const wchar_t *dir)
{
    wchar_t from[MAX_PATH * 2 + 2] = L"";
    size_t n = wcslen(dir);
    if (!n || n >= MAX_PATH * 2) return FALSE;
    wmemcpy(from, dir, n);
    from[n] = from[n + 1] = 0;                        /* double-terminated */
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/* A running program can't be replaced but can be renamed: <exe> -> <exe>.old, so an update can write the new one
 * while the wallpaper still runs the old (sample_drop_aside removes them once nothing runs them). */
static inline void sample_exe_aside(const wchar_t *root, const SampleApp *s)
{
    wchar_t p[MAX_PATH * 2], q[MAX_PATH * 2 + 8];
    sample_path(p, MAX_PATH * 2, root, s->name, s->exe);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    swprintf(q, MAX_PATH * 2 + 8, L"%ls.old", p);
    MoveFileExW(p, q, MOVEFILE_REPLACE_EXISTING);
}

static inline void sample_drop_aside(const wchar_t *root, const SampleApp *s)
{
    wchar_t p[MAX_PATH * 2 + 8];
    sample_path(p, MAX_PATH * 2, root, s->name, s->exe);
    wcscat_s(p, MAX_PATH * 2 + 8, L".old");
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) DeleteFileW(p);
}
#else   /* Linux keeps the pages (no app host): no sample is ever a program there */
static inline void sample_path(wchar_t *out, size_t cap, const wchar_t *root, const wchar_t *folder, const wchar_t *file)
{
    (void)root; (void)folder; (void)file;
    if (cap) out[0] = L'\0';
}
static inline int sample_is_page(const wchar_t *root, const SampleApp *s) { (void)root; (void)s; return 0; }
static inline int sample_delete_dir(const wchar_t *dir) { (void)dir; return 0; }
static inline void sample_exe_aside(const wchar_t *root, const SampleApp *s) { (void)root; (void)s; }
#endif

#endif
