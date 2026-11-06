/* Web links (`screenplus import <url>`), the Linux counterpart of src/app/url_import.c: a live page is added
 * as it is; a media file comes down with curl; anything else goes through yt-dlp. Both are system tools,
 * run as child processes the service's loop checks on. */
#include "service.h"
#include "desktop.h"
#include "../app/app.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static UrlImportState g_st;
static pid_t   g_pid;
static wchar_t g_stem[SP_ID_LEN];         /* downloads/<stem>.* */
static wchar_t g_title[SP_NAME_LEN];
static wchar_t g_link[SP_MAX_PATH];     /* AUTO: what becomes a stream when yt-dlp leaves it undownloaded */

static void set_failed(const wchar_t *why)
{
    g_st.busy = 0;
    g_st.failed = 1;
    g_st.lastOk = 0;
    str_copy(g_st.text, SP_COUNTOF(g_st.text), why);
    LOGW(L"link: %ls", why);
}

static int direct_media(const wchar_t *url)
{
    static const wchar_t *const exts[] = { L".mp4", L".webm", L".mkv", L".mov", L".gif", L".jpg", L".jpeg", L".png", L".webp" };
    wchar_t clean[SP_MAX_PATH];
    str_copy(clean, SP_COUNTOF(clean), url);
    wchar_t *q = wcschr(clean, L'?');
    if (q) *q = 0;
    const wchar_t *ext = path_ext(clean);
    for (size_t i = 0; i < SP_COUNTOF(exts); i++)
        if (str_ieq(ext, exts[i])) return 1;
    return 0;
}

static pid_t spawn(const char *const *argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    return pid;
}

BOOL url_import_start(const wchar_t *url, int mode)
{
    if (g_st.busy) return FALSE;
    memset(&g_st, 0, sizeof g_st);
    g_st.percent = -1;
    if (!wallpaper_is_url(url)) {
        set_failed(L"That isn't a web link.");
        return FALSE;
    }
    if (mode == URLMODE_PAGE) {
        WallpaperEntry *e = config_add(&g_app.cfg, WP_HTML, url, url);
        if (!e) {
            set_failed(L"The library is full.");
            return FALSE;
        }
        e->flags |= WPFLAG_URL;
        g_st.finished++;
        g_st.lastOk = 1;
        app_save_config();
        return TRUE;
    }

    wchar_t dir[SP_MAX_PATH];
    str_copy(dir, SP_COUNTOF(dir), paths_downloads());
    dir_create_recursive(dir);
    random_id(g_stem);
    char *u = utf8_from_wide(url, NULL), *d = sp_path_utf8(dir), *stem = utf8_from_wide(g_stem, NULL);
    char out[MAX_PATH + 64];
    pid_t pid = -1;
    if (u && d && stem) {
        if (direct_media(url)) {
            const char *ext = "";
            char *dot = strrchr(u, '.');
            char e[16] = "";
            if (dot) {
                snprintf(e, sizeof e, "%s", dot);
                e[strcspn(e, "?#")] = 0;
                ext = e;
            }
            snprintf(out, sizeof out, "%s/%s%s", d, stem, ext);
            if (!g_title[0]) {
                wchar_t clean[SP_MAX_PATH];
                str_copy(clean, SP_COUNTOF(clean), url);
                clean[wcscspn(clean, L"?#")] = 0;
                path_stem(g_title, SP_COUNTOF(g_title), clean);
            }
            const char *argv[] = { "curl", "-fsSL", "--max-time", "1800", "-o", out, u, NULL };
            pid = sp_on_path("curl") ? spawn(argv) : -1;
            if (pid < 0) set_failed(L"curl isn't installed.");
        } else {
            snprintf(out, sizeof out, "%s/%s.%%(title).100B.%%(ext)s", d, stem);   /* url_import_tick names the entry from it */
            /* Automatic: up to 10 minutes and 300 MB are saved, anything longer plays as a stream */
            const char *argv[] = { "yt-dlp", "--no-playlist", "--no-part", "--merge-output-format", "mp4", "-o", out, u, NULL };
            const char *brief[] = { "yt-dlp", "--no-playlist", "--no-part", "--merge-output-format", "mp4", "--match-filter", "duration<=600",
                                    "--max-filesize", "300M", "-o", out, u, NULL };
            str_copy(g_link, SP_COUNTOF(g_link), mode == URLMODE_AUTO ? url : L"");
            pid = sp_on_path("yt-dlp") ? spawn(mode == URLMODE_AUTO ? brief : argv) : -1;
            if (pid < 0) set_failed(L"yt-dlp isn't installed. Install it from your package manager.");
        }
    }
    free(u); free(d); free(stem);
    if (pid <= 0) return FALSE;
    g_pid = pid;
    g_st.busy = 1;
    str_copy(g_st.text, SP_COUNTOF(g_st.text), L"Downloading");
    LOGI(L"link: downloading %ls", url);
    return TRUE;
}

BOOL url_import_start_titled(const wchar_t *url, int mode, const wchar_t *title)
{
    str_copy(g_title, SP_COUNTOF(g_title), title ? title : L"");
    return url_import_start(url, mode);
}

/* The usual named entities and numeric references, in place. */
void html_unescape(wchar_t *s)
{
    static const struct { const wchar_t *name; wchar_t c; } k[] = { { L"amp;", L'&' }, { L"lt;", L'<' }, { L"gt;", L'>' }, { L"quot;", L'"' }, { L"apos;", L'\'' }, { L"nbsp;", L' ' } };
    wchar_t *o = s;
    for (wchar_t *p = s; *p; ) {
        int done = 0;
        if (*p == L'&') {
            for (size_t i = 0; i < SP_COUNTOF(k) && !done; i++) {
                size_t n = wcslen(k[i].name);
                if (!wcsncmp(p + 1, k[i].name, n)) { *o++ = k[i].c; p += n + 1; done = 1; }
            }
            if (!done && p[1] == L'#') {
                wchar_t *end;
                long v = p[2] == L'x' || p[2] == L'X' ? wcstol(p + 3, &end, 16) : wcstol(p + 2, &end, 10);
                if (*end == L';' && v > 0) { *o++ = (wchar_t)v; p = end + 1; done = 1; }
            }
        }
        if (!done) *o++ = *p++;
    }
    *o = 0;
}

void url_import_cancel(void)
{
    if (g_pid > 0) kill(g_pid, SIGTERM);
}

void url_import_get(UrlImportState *out) { *out = g_st; }

void url_import_tick(void)
{
    int st = 0;
    if (!g_pid || waitpid(g_pid, &st, WNOHANG) != g_pid) return;
    g_pid = 0;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        set_failed(L"The download failed.");
        return;
    }
    wchar_t pattern[SP_MAX_PATH], file[SP_MAX_PATH], leaf[64];
    WIN32_FIND_DATAW fd;
    str_printf(leaf, SP_COUNTOF(leaf), L"%ls*", g_stem);
    path_join(pattern, SP_COUNTOF(pattern), paths_downloads(), leaf);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (g_link[0] && !g_title[0]) {             /* once, and quick: yt-dlp has it cached */
            char *u = utf8_from_wide(g_link, NULL), name[256] = "";
            const char *argv[] = { "yt-dlp", "--no-playlist", "--skip-download", "--print", "title", u, NULL };
            if (u && !sp_run(argv, name, sizeof name)) {
                name[strcspn(name, "\r\n")] = 0;
                wchar_t *w = wide_from_utf8(name, -1);
                if (w) str_copy(g_title, SP_COUNTOF(g_title), w);
                free(w);
            }
            free(u);
        }
        WallpaperEntry *e = g_link[0] ? config_add(&g_app.cfg, WP_VIDEO, g_link, g_title[0] ? g_title : g_link) : NULL;
        if (!e) {
            set_failed(L"The download left no file.");
            return;
        }
        e->flags |= WPFLAG_URL | WPFLAG_YTDLP;
        g_link[0] = g_title[0] = 0;
        g_st.busy = 0;
        g_st.finished++;
        g_st.lastOk = 1;
        app_save_config();
        return;
    }
    path_join(file, SP_COUNTOF(file), paths_downloads(), fd.cFileName);
    if (!g_title[0] && wcslen(fd.cFileName) > wcslen(g_stem) + 1) {     /* <stem>.<title>.<ext> */
        path_stem(g_title, SP_COUNTOF(g_title), fd.cFileName + wcslen(g_stem) + 1);
    }
    FindClose(h);
    const wchar_t *paths[1] = { file };
    int added = library_import(paths, 1, FALSE);
    g_st.busy = 0;
    g_st.finished++;
    g_st.lastOk = added > 0;
    if (!added) set_failed(L"That file isn't a wallpaper Screen+ can use.");
    else if (g_title[0]) {
        WallpaperEntry *e = config_find_path(&g_app.cfg, file);
        if (e) str_copy(e->name, SP_COUNTOF(e->name), g_title);
    }
    g_title[0] = 0;
    app_save_config();
}

BOOL url_import_is_download(const wchar_t *path)
{
    return path && str_istarts(path, paths_downloads());
}

BOOL url_import_forget(const wchar_t *path, const wchar_t *thumb)
{
    (void)thumb;
    if (!url_import_is_download(path)) return FALSE;
    library_discard(path, FALSE);
    return TRUE;
}
