/* Import from a web link (the link button in the Library). One job at a time, on a worker thread, so
 * the window never waits on the network. What a link becomes:
 *   - an image: downloaded;
 *   - a video (a direct link, or a page yt-dlp knows): short ones (up to 10 minutes and 300 MB) are
 *     downloaded once, the cheapest way to loop them forever; longer ones, like the hours-long loops on
 *     YouTube, become stream wallpapers that the video renderer plays from small byte ranges kept in
 *     RAM (renderer/video/video_stream.c), the way mpv or Lively do: nothing is saved;
 *   - a page without a video: a live web wallpaper (the existing WebView2 renderer).
 * "Download" saves any video, "Web page" always shows the page. yt-dlp picks one ready-to-play stream
 * no larger than the biggest display, avoiding AV1 and HDR (common/ytdlp.c), so the hardware-decoding
 * video renderer plays it. Downloads live in <data>\downloads and are deleted with their library entry;
 * nothing is sent with a request but the address (no cookies, no sign-in). */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../common/ytdlp.h"
#include <winhttp.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define DOWNLOAD_MAX ((int64_t)4 << 30)    /* direct downloads; yt-dlp gets --max-filesize 4G */
#define SAVE_SECONDS 600.0                 /* Automatic: videos up to this long ... */
#define SAVE_BYTES   ((int64_t)300 << 20)  /* ... and this large are downloaded, longer ones streamed */
#define TITLE_SCAN   65536                 /* bytes of a page searched for its <title> */

enum { RES_FAIL = 0, RES_FILE, RES_PAGE, RES_STREAM, RES_CANCELLED };
enum { YT_OK = 0, YT_FAIL, YT_UNSUPPORTED, YT_CANCELLED };

typedef struct UrlJob {
    wchar_t url[SP_MAX_PATH];
    int     mode;
    int     maxRes;                 /* smaller side of the largest display, pixels */
    HANDLE  cancel;                 /* manual-reset event */
    double  lastPost;
    int     result;                 /* RES_* */
    wchar_t path[SP_MAX_PATH];      /* RES_FILE: the downloaded file; RES_PAGE / RES_STREAM: the link */
    int     flags;                  /* RES_STREAM: WPFLAG_* */
    wchar_t thumb[SP_MAX_PATH];     /* RES_STREAM: preview image */
    wchar_t title[SP_NAME_LEN];
    wchar_t name[SP_NAME_LEN];      /* given by the caller (the Store): the entry's name and the file's */
    wchar_t error[300];
    int     fellBack;               /* RES_PAGE because yt-dlp found no video */
} UrlJob;

static SRWLOCK        g_lock = SRWLOCK_INIT;
static UrlImportState g_state = { 0, -1 };
static UrlJob        *g_job;        /* running job; freed on the shell thread when it ends */
static HANDLE         g_thread;

static int cancelled(UrlJob *j) { return WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0; }

static void set_status(int percent, const wchar_t *text)
{
    AcquireSRWLockExclusive(&g_lock);
    g_state.percent = percent;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), text);
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageW(g_app.shellWnd, WM_SP_URLIMPORT, 0, 0);
}

static void stage(UrlJob *j, const wchar_t *text)
{
    if (!cancelled(j)) set_status(-1, text);
}

static void progress(UrlJob *j, const wchar_t *what, int64_t done, int64_t total)
{
    double now = time_ms();
    if (now - j->lastPost < 200.0 || cancelled(j)) return;     /* a few repaints a second at most */
    j->lastPost = now;
    wchar_t text[SP_COUNTOF(g_state.text)];
    int pct = total > 0 ? (int)SP_MIN(100, done * 100 / total) : -1;
    if (pct >= 0) str_printf(text, SP_COUNTOF(text), L"%ls… %d%%  ·  %.1f of %.1f MB", what, pct, done / 1048576.0, total / 1048576.0);
    else str_printf(text, SP_COUNTOF(text), L"%ls… %.1f MB", what, done / 1048576.0);
    set_status(pct, text);
}

static void fail(UrlJob *j, const wchar_t *why)
{
    j->result = cancelled(j) ? RES_CANCELLED : RES_FAIL;
    str_copy(j->error, SP_COUNTOF(j->error), why);
}

/* ---------------------------------------------------------------- files */

static void partial_dir(wchar_t *out, size_t cap)
{
    path_join(out, cap, paths_downloads(), L".partial");
}

/* Leftovers of an interrupted download (only one job runs at a time). */
static void clean_partial(void)
{
    wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH], f[SP_MAX_PATH];
    partial_dir(dir, SP_COUNTOF(dir));
    path_join(pat, SP_COUNTOF(pat), dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
        DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL in_folder(const wchar_t *path, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    return n && path && _wcsnicmp(path, dir, n) == 0 && path[n] == L'\\' && !wcschr(path + n + 1, L'\\');
}

BOOL url_import_is_download(const wchar_t *path)
{
    return in_folder(path, paths_downloads());
}

/* A download is in use as an entry's file, or as the picture behind a depth wallpaper (its `thumb`). */
static BOOL in_library(const wchar_t *f)
{
    if (config_find_path(&g_app.cfg, f)) return TRUE;
    for (int i = 0; i < g_app.cfg.count; i++)
        if (str_ieq(g_app.cfg.items[i].thumb, f)) return TRUE;
    return FALSE;
}

BOOL url_import_forget(const wchar_t *path, const wchar_t *thumb)
{
    wchar_t f[SP_MAX_PATH], streams[SP_MAX_PATH];
    if (in_library(path)) return FALSE;                       /* another entry still uses it */
    path_join(streams, SP_COUNTOF(streams), paths_cache(), L"streams");
    if (thumb && in_folder(thumb, streams)) DeleteFileW(thumb);
    if (wallpaper_is_url(path)) {
        ytdlp_cache_path(path, L".url", f, SP_COUNTOF(f));
        DeleteFileW(f);
        return FALSE;
    }
    /* if the file is still being played, the next start removes it */
    return in_folder(path, paths_downloads()) && DeleteFileW(path);
}

void url_import_sweep(void)
{
    wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH], f[SP_MAX_PATH], hx[12];
    WIN32_FIND_DATAW fd;
    int n = 0;
    clean_partial();
    path_join(pat, SP_COUNTOF(pat), paths_downloads(), L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path_join(f, SP_COUNTOF(f), paths_downloads(), fd.cFileName);
            if (!in_library(f) && DeleteFileW(f)) n++;   /* removed while it was playing */
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    /* stream addresses and previews (cache\streams\<hash of the link>.*) of links no longer in the library */
    path_join(dir, SP_COUNTOF(dir), paths_cache(), L"streams");
    path_join(pat, SP_COUNTOF(pat), dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            int used = 0;
            for (int i = 0; i < g_app.cfg.count && !used; i++) {
                const WallpaperEntry *e = &g_app.cfg.items[i];
                str_printf(hx, SP_COUNTOF(hx), L"%08x", hash_fnv1a_w(e->path));
                used = wallpaper_is_url(e->path) && _wcsnicmp(fd.cFileName, hx, 8) == 0;
            }
            path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
            if (!used && DeleteFileW(f)) n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n) LOGI(L"url import: deleted %d file%ls of links no longer in the library", n, n == 1 ? L"" : L"s");
}

/* ---------------------------------------------------------------- direct links */

/* Last path segment of a URL, decoded and made safe as a file name ("" if none). */
/* in as a file name: characters Windows refuses become '_', and it doesn't end in a dot or a space */
static void file_safe(const wchar_t *in, wchar_t *out, size_t cap)
{
    size_t j = 0;
    for (const wchar_t *c = in; *c && j + 1 < cap; c++) out[j++] = (*c < 32 || wcschr(L"<>:\"/\\|?*", *c)) ? L'_' : *c;
    while (j && (out[j - 1] == L'.' || out[j - 1] == L' ')) j--;
    out[j] = 0;
}

static void url_leaf(const wchar_t *url, wchar_t *out, size_t cap)
{
    out[0] = 0;
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || !uc.lpszUrlPath) return;
    wchar_t path[SP_MAX_PATH];
    str_copyn(path, SP_COUNTOF(path), uc.lpszUrlPath, uc.dwUrlPathLength);
    UrlUnescapeW(path, NULL, NULL, URL_UNESCAPE_INPLACE);
    const wchar_t *leaf = wcsrchr(path, L'/');
    file_safe(leaf ? leaf + 1 : path, out, cap);
}

/* An image or video Screen+ plays, judged by Content-Type and the link's file extension. */
static int media_ext(const wchar_t *type, const wchar_t *leaf, wchar_t *ext, size_t cap)
{
    static const struct { const wchar_t *type, *ext; } k[] = {
        { L"video/mp4", L".mp4" }, { L"video/webm", L".webm" }, { L"video/x-matroska", L".mkv" },
        { L"video/quicktime", L".mov" }, { L"video/x-msvideo", L".avi" }, { L"video/mp2t", L".ts" },
        { L"video/x-ms-wmv", L".wmv" }, { L"video/mpeg", L".mpg" }, { L"video/3gpp", L".3gp" },
        { L"image/jpeg", L".jpg" }, { L"image/png", L".png" }, { L"image/webp", L".webp" }, { L"image/gif", L".gif" },
        { L"image/avif", L".avif" }, { L"image/bmp", L".bmp" }, { L"image/tiff", L".tiff" },
        { L"image/heic", L".heic" }, { L"image/heif", L".heif" },
    };
    WallpaperType t = wallpaper_type_from_path(leaf);
    int known = t == WP_IMAGE || t == WP_VIDEO;
    for (size_t i = 0; i < SP_COUNTOF(k); i++) {
        if (str_ieq(type, k[i].type)) {
            str_copy(ext, cap, known ? path_ext(leaf) : k[i].ext);
            return 1;
        }
    }
    if (known && (!type[0] || wcsstr(type, L"octet-stream"))) {   /* servers often send media as binary */
        str_copy(ext, cap, path_ext(leaf));
        return 1;
    }
    return 0;
}

static int direct_progress(void *ctx, int64_t done, int64_t total)
{
    UrlJob *j = (UrlJob *)ctx;
    progress(j, L"Downloading", done, total);
    return !cancelled(j);
}

static void download_direct(UrlJob *j, HttpGet *g, const wchar_t *stem, const wchar_t *ext)
{
    wchar_t name[160], dir[SP_MAX_PATH], part[SP_MAX_PATH], why[300];
    /* the same link always maps to the same file, so importing it again never downloads it again */
    str_printf(name, SP_COUNTOF(name), L"%.80ls-%08x%ls", stem, hash_fnv1a_w(j->url), ext);
    path_join(j->path, SP_COUNTOF(j->path), paths_downloads(), name);
    if (file_exists(j->path)) {
        j->result = RES_FILE;
        return;
    }
    ULARGE_INTEGER avail;
    if (http_length(g) > 0 && GetDiskFreeSpaceExW(paths_root(), &avail, NULL, NULL) &&
        avail.QuadPart < (ULONGLONG)http_length(g) + (256ull << 20)) {
        str_printf(why, SP_COUNTOF(why), L"Not enough free disk space for this %.0f MB download.", http_length(g) / 1048576.0);
        fail(j, why);
        return;
    }
    partial_dir(dir, SP_COUNTOF(dir));
    path_join(part, SP_COUNTOF(part), dir, name);
    stage(j, L"Downloading…");
    if (!http_save(g, part, DOWNLOAD_MAX, direct_progress, j, NULL, why, SP_COUNTOF(why))) {
        fail(j, why);
        return;
    }
    if (!MoveFileExW(part, j->path, MOVEFILE_REPLACE_EXISTING)) {
        str_printf(why, SP_COUNTOF(why), L"Could not save the download (error %lu).", GetLastError());
        DeleteFileW(part);
        fail(j, why);
        return;
    }
    j->result = RES_FILE;
}

/* ---------------------------------------------------------------- web pages */

void html_unescape(wchar_t *s)
{
    static const struct { const wchar_t *e; wchar_t c; } k[] = {
        { L"&amp;", L'&' }, { L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&quot;", L'"' }, { L"&#39;", L'\'' },
        { L"&#039;", L'\'' }, { L"&#x27;", L'\'' }, { L"&apos;", L'\'' }, { L"&nbsp;", L' ' },
    };
    wchar_t *w = s;
    for (const wchar_t *r = s; *r;) {
        int hit = 0;
        for (size_t i = 0; *r == L'&' && i < SP_COUNTOF(k) && !hit; i++) {
            size_t l = wcslen(k[i].e);
            if (wcsncmp(r, k[i].e, l) == 0) {
                *w++ = k[i].c;
                r += l;
                hit = 1;
            }
        }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}

static void read_title(HttpGet *g, wchar_t *out, size_t cap)
{
    char *buf = (char *)malloc(TITLE_SCAN + 1);
    if (!buf) return;
    buf[http_read(g, buf, TITLE_SCAN)] = 0;
    char *t = StrStrIA(buf, "<title");
    char *b = t ? strchr(t, '>') : NULL;
    char *e = b ? StrStrIA(b, "</title") : NULL;
    if (e) {
        *e = 0;
        wchar_t *w = wide_from_utf8(b + 1, -1);
        if (w) {
            html_unescape(w);
            str_sanitize_line(w);
            str_trim(w);
            str_copy(out, cap, w);
            free(w);
        }
    }
    free(buf);
}

static void host_name(const wchar_t *url, wchar_t *out, size_t cap)
{
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    out[0] = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || !uc.lpszHostName) return;
    str_copyn(out, cap, uc.lpszHostName, uc.dwHostNameLength);
    if (str_istarts(out, L"www.")) memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));
}

/* ---------------------------------------------------------------- yt-dlp */

static int tools_progress(void *ctx, int64_t done, int64_t total)
{
    UrlJob *j = (UrlJob *)ctx;
    progress(j, L"Getting video support (yt-dlp, once)", done, total);
    return !cancelled(j);
}

static int64_t field(const char **p)
{
    while (**p == ' ') (*p)++;
    if (**p < '0' || **p > '9') {                  /* "NA" */
        while (**p && **p != ' ') (*p)++;
        return -1;
    }
    char *end = NULL;
    int64_t v = _strtoi64(*p, &end, 10);
    while (*end && *end != ' ') end++;             /* estimates are floats */
    *p = end;
    return v;
}

typedef struct DlCtx {
    UrlJob *j;
    wchar_t err[600];
} DlCtx;

static void dl_line(void *ctx, char *line)
{
    DlCtx *c = (DlCtx *)ctx;
    UrlJob *j = c->j;
    if (strncmp(line, "[sp] ", 5) == 0) {
        const char *p = line + 5;
        int64_t done = field(&p), total = field(&p), estimate = field(&p);
        if (done >= 0) progress(j, L"Downloading video", done, total > 0 ? total : estimate);
        return;
    }
    wchar_t *w = NULL;
    if (strncmp(line, "[sp-file] ", 10) == 0 && (w = wide_from_utf8(line + 10, -1)) != NULL)
        str_copy(j->path, SP_COUNTOF(j->path), w);
    else if (strncmp(line, "[sp-title] ", 11) == 0 && (w = wide_from_utf8(line + 11, -1)) != NULL)
        str_copy(j->title, SP_COUNTOF(j->title), w);
    else if (strncmp(line, "ERROR: ", 7) == 0 && (w = wide_from_utf8(line + 7, -1)) != NULL)
        str_copy(c->err, SP_COUNTOF(c->err), w);
    free(w);
}

/* Downloads the whole video into <data>\downloads (Download, or short videos). */
static int run_download(UrlJob *j, wchar_t *why, size_t whyCap)
{
    wchar_t partial[SP_MAX_PATH], fmt[400];
    partial_dir(partial, SP_COUNTOF(partial));
    ytdlp_format(fmt, SP_COUNTOF(fmt), j->maxRes, 0);
    size_t cap = 4 * SP_MAX_PATH + 1024;
    wchar_t *args = (wchar_t *)malloc(cap * sizeof(wchar_t));
    DlCtx *c = (DlCtx *)calloc(1, sizeof *c);
    if (!args || !c) {
        free(args);
        free(c);
        str_copy(why, whyCap, L"Out of memory.");
        return YT_FAIL;
    }
    c->j = j;
    str_printf(args, cap,
               L"--newline --progress --progress-template \"download:[sp] %%(progress.downloaded_bytes)s "
               L"%%(progress.total_bytes)s %%(progress.total_bytes_estimate)s\" "
               L"--print \"after_move:[sp-file] %%(filepath)s\" --print \"after_move:[sp-title] %%(title)s\" "
               L"--max-filesize 4G %ls -P \"home:%ls\" -P \"temp:%ls\" -o \"%%(title).80B [%%(id).30B].%%(ext)s\" -- \"%ls\"",
               fmt, paths_downloads(), partial, j->url);
    int code = ytdlp_run(args, partial, j->cancel, NULL, dl_line, c, why, whyCap);
    free(args);
    int r = YT_FAIL;
    if (cancelled(j)) r = YT_CANCELLED;
    else if (code == 0 && j->path[0] && file_exists(j->path)) r = YT_OK;
    else if (wcsstr(c->err, L"Unsupported URL")) r = YT_UNSUPPORTED;
    else {
        LOGW(L"url import: yt-dlp exited with %d, file '%ls': %ls", code, j->path, c->err[0] ? c->err : L"no error");
        ytdlp_tidy_error(c->err);
        if (wcsstr(c->err, L"max-filesize") || wcsstr(c->err, L"larger than max"))
            str_copy(why, whyCap, L"The video is larger than 4 GB.");
        else if (c->err[0])
            str_printf(why, whyCap, L"Couldn't get the video: %ls", c->err);
        else if (code != -1)
            str_printf(why, whyCap, L"yt-dlp stopped unexpectedly (exit code %d).", code);
    }
    free(c);
    return r;
}

/* The page's preview image, kept beside its stream address (cache\streams). */
static void fetch_thumb(UrlJob *j, const wchar_t *url)
{
    wchar_t why[160], dir[SP_MAX_PATH];
    if (!wallpaper_is_url(url)) return;
    HttpGet *g = http_open(url, 1, why, SP_COUNTOF(why));
    if (g && http_status(g) == 200 && str_istarts(http_content_type(g), L"image/")) {
        const wchar_t *type = http_content_type(g);
        ytdlp_cache_path(j->url, wcsstr(type, L"webp") ? L".webp" : wcsstr(type, L"png") ? L".png" : L".jpg",
                         j->thumb, SP_COUNTOF(j->thumb));
        path_dirname(dir, SP_COUNTOF(dir), j->thumb);
        dir_create_recursive(dir);
        if (!http_save(g, j->thumb, 8 << 20, NULL, NULL, NULL, why, SP_COUNTOF(why))) j->thumb[0] = 0;
    }
    http_close(g);
}

/* ---------------------------------------------------------------- the job */

static void run_video_page(UrlJob *j, int isPage, const wchar_t *pageTitle)
{
    wchar_t why[300] = L"";
    int r;
    if (j->mode == URLMODE_DOWNLOAD) {
        stage(j, L"Looking for a video on the page…");
        r = run_download(j, why, SP_COUNTOF(why));
    } else {
        stage(j, L"Looking for a video on the page…");
        YtStream *yt = (YtStream *)calloc(1, sizeof *yt);
        if (!yt) {
            fail(j, L"Out of memory.");
            return;
        }
        if (!ytdlp_resolve(j->url, j->maxRes, NULL, j->cancel, NULL, yt)) {
            r = cancelled(j) ? YT_CANCELLED : yt->unsupported ? YT_UNSUPPORTED : YT_FAIL;
            if (r == YT_FAIL) str_printf(why, SP_COUNTOF(why), L"Couldn't get the video: %ls", yt->error);
        } else if (yt->live) {
            r = YT_FAIL;
            str_copy(why, SP_COUNTOF(why), L"Live streams can't be used as wallpapers.");
        } else {
            int brief = yt->duration > 0 && yt->duration <= SAVE_SECONDS && (yt->size < 0 || yt->size <= SAVE_BYTES);
            if (brief || !yt->streamable) {
                if (!brief && yt->duration > SAVE_SECONDS) {
                    str_printf(why, SP_COUNTOF(why), L"This %.0f-minute video is only offered in pieces Screen+ can't "
                               L"stream. Choose Download to save it anyway.", yt->duration / 60.0);
                    r = YT_FAIL;
                } else {
                    r = run_download(j, why, SP_COUNTOF(why));
                }
            } else {                                   /* long: a stream wallpaper, nothing saved */
                str_copy(j->path, SP_COUNTOF(j->path), j->url);
                str_copy(j->title, SP_COUNTOF(j->title), yt->title);
                j->flags = WPFLAG_URL | WPFLAG_YTDLP;
                ytdlp_cache_save(j->url, yt);          /* its first play starts right away */
                fetch_thumb(j, yt->thumb);
                LOGI(L"url import: streams %ls (format %ls, %.0f min, %.0f MB)", j->url, yt->format, yt->duration / 60.0,
                     yt->size / 1048576.0);
                j->result = RES_STREAM;
                free(yt);
                return;
            }
        }
        free(yt);
    }
    switch (r) {
    case YT_OK:
        if (wallpaper_type_from_path(j->path) != WP_VIDEO && wallpaper_type_from_path(j->path) != WP_IMAGE) {
            str_printf(why, SP_COUNTOF(why), L"The site delivered a format Screen+ can't play (%ls).", path_ext(j->path));
            DeleteFileW(j->path);
            fail(j, why);
        } else {
            j->result = RES_FILE;
        }
        break;
    case YT_UNSUPPORTED:
        if (j->mode == URLMODE_AUTO && isPage) {
            str_copy(j->title, SP_COUNTOF(j->title), pageTitle);
            j->result = RES_PAGE;
            j->fellBack = 1;
        } else {
            fail(j, L"No video was found at this address. To show the page itself, choose Web page.");
        }
        break;
    case YT_CANCELLED:
        j->result = RES_CANCELLED;
        break;
    default:
        fail(j, why[0] ? why : L"The video could not be imported.");
        break;
    }
}

static void run(UrlJob *j)
{
    wchar_t why[300] = L"", ext[16], leaf[128], stem[128];
    int isPage = 0, status = 0, reached = 0;
    wchar_t partial[SP_MAX_PATH];
    partial_dir(partial, SP_COUNTOF(partial));
    clean_partial();
    if (!dir_create_recursive(partial)) {
        fail(j, L"Could not create the downloads folder.");
        return;
    }

    stage(j, L"Connecting…");
    HttpGet *g = http_open(j->url, 1, why, SP_COUNTOF(why));
    if (g) {
        reached = 1;
        status = http_status(g);
        url_leaf(j->url, leaf, SP_COUNTOF(leaf));
        if (status >= 200 && status < 300 && j->mode != URLMODE_PAGE &&
            media_ext(http_content_type(g), leaf, ext, SP_COUNTOF(ext))) {
            if (j->name[0]) file_safe(j->name, stem, SP_COUNTOF(stem));
            else path_stem(stem, SP_COUNTOF(stem), leaf);
            if (!stem[0]) str_copy(stem, SP_COUNTOF(stem), L"download");
            str_copy(j->title, SP_COUNTOF(j->title), stem);
            wchar_t probe[24];
            str_printf(probe, SP_COUNTOF(probe), L"x%ls", ext);
            if (j->mode == URLMODE_AUTO && wallpaper_type_from_path(probe) == WP_VIDEO && http_ranges(g) &&
                http_length(g) > SAVE_BYTES) {         /* a large video file: streamed from the link */
                str_copy(j->path, SP_COUNTOF(j->path), j->url);
                j->flags = WPFLAG_URL;
                j->result = RES_STREAM;
            } else {
                download_direct(j, g, stem, ext);
            }
            http_close(g);
            return;
        }
        isPage = status >= 200 && status < 300 && (str_istarts(http_content_type(g), L"text/html") ||
                                                    str_istarts(http_content_type(g), L"application/xhtml"));
        if (isPage) read_title(g, j->title, SP_COUNTOF(j->title));
        http_close(g);
    }
    if (cancelled(j)) {
        j->result = RES_CANCELLED;
        return;
    }
    if (j->mode == URLMODE_PAGE) {                 /* shown live; it may still load in the browser */
        j->result = RES_PAGE;
        return;
    }
    if (!reached) {                                /* name or connection failure: yt-dlp would fail too */
        fail(j, why);
        return;
    }
    if (status == 401 || status == 407) {
        fail(j, L"This link needs a sign-in. Screen+ only imports content that is publicly available.");
        return;
    }
    if (status == 404 || status == 410) {
        str_printf(why, SP_COUNTOF(why), L"Nothing was found at this address (HTTP %d).", status);
        fail(j, why);
        return;
    }

    wchar_t ytdlp[SP_MAX_PATH], qjs[SP_MAX_PATH];
    if (!urltools_find(TOOL_YTDLP, ytdlp, SP_COUNTOF(ytdlp)) || !urltools_find(TOOL_QJS, qjs, SP_COUNTOF(qjs))) {
        stage(j, L"Getting video support (yt-dlp, once)…");
        if (!urltools_install(tools_progress, j, why, SP_COUNTOF(why))) {
            fail(j, why[0] ? why : L"Video support could not be installed.");
            return;
        }
        LOGI(L"url import: installed %ls and %ls", k_urlTools[TOOL_YTDLP].name, k_urlTools[TOOL_QJS].name);
    }
    wchar_t pageTitle[SP_NAME_LEN];
    str_copy(pageTitle, SP_COUNTOF(pageTitle), j->title);
    j->title[0] = 0;
    run_video_page(j, isPage, pageTitle);
}

static DWORD WINAPI worker(LPVOID p)
{
    UrlJob *j = (UrlJob *)p;
    run(j);
    if (j->result != RES_FILE) clean_partial();
    PostMessageW(g_app.shellWnd, WM_SP_URLIMPORT, 1, (LPARAM)j);
    return 0;
}

/* ---------------------------------------------------------------- shell thread */

/* Accepts "example.com/x" too (https:// is assumed). */
static BOOL normalize_url(const wchar_t *in, wchar_t *out, size_t cap, wchar_t *why, size_t whyCap)
{
    static const wchar_t *refused[] = { L"javascript:", L"data:", L"file:", L"blob:", L"about:", L"mailto:", L"ftp:" };
    wchar_t s[SP_MAX_PATH];
    str_copy(s, SP_COUNTOF(s), in);
    str_trim(s);
    if (!s[0]) {
        str_copy(why, whyCap, L"Paste a link to a video, an image or a web page first.");
        return FALSE;
    }
    if (wcslen(s) > SP_MAX_PATH - 16) {
        str_copy(why, whyCap, L"That link is too long.");
        return FALSE;
    }
    if ((iswalpha(s[0]) && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) || str_istarts(s, L"\\\\")) {
        str_copy(why, whyCap, L"That is a file on this PC: use Import or the folder button instead.");
        return FALSE;
    }
    for (wchar_t *c = s; *c; c++) {
        if (*c == L'\\') *c = L'/';                /* as browsers do; also keeps the command line well-formed */
        if (*c <= 32 || *c == L'"' || *c == 127) {
            str_copy(why, whyCap, L"A link can't contain spaces or quotes.");
            return FALSE;
        }
    }
    for (size_t i = 0; i < SP_COUNTOF(refused); i++) {
        if (str_istarts(s, refused[i]) || (!wallpaper_is_url(s) && wcsstr(s, L"://"))) {
            str_copy(why, whyCap, L"Only http:// and https:// links can be imported.");
            return FALSE;
        }
    }
    if (wallpaper_is_url(s)) str_copy(out, cap, s);
    else str_printf(out, cap, L"https://%ls", s);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    if (!WinHttpCrackUrl(out, 0, 0, &uc) || !uc.dwHostNameLength) {
        str_copy(why, whyCap, L"That doesn't look like a web address.");
        return FALSE;
    }
    return TRUE;
}

BOOL url_import_start(const wchar_t *url, int mode)
{
    return url_import_start_titled(url, mode, NULL);
}

BOOL url_import_start_titled(const wchar_t *url, int mode, const wchar_t *title)
{
    wchar_t why[160];
    if (g_job) return FALSE;
    UrlJob *j = (UrlJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    if (title) str_copy(j->name, SP_COUNTOF(j->name), title);
    if (!normalize_url(url, j->url, SP_COUNTOF(j->url), why, SP_COUNTOF(why))) {
        free(j);
        AcquireSRWLockExclusive(&g_lock);
        g_state.failed = 1;
        g_state.percent = -1;
        str_copy(g_state.text, SP_COUNTOF(g_state.text), why);
        ReleaseSRWLockExclusive(&g_lock);
        ui_on_status();
        return FALSE;
    }
    j->mode = mode;
    for (int i = 0; i < g_app.monCount; i++) {
        const RECT *r = &g_app.mons[i].rect;
        j->maxRes = SP_MAX(j->maxRes, (int)SP_MIN(r->right - r->left, r->bottom - r->top));
    }
    if (j->maxRes <= 0) j->maxRes = 1080;
    j->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = 1;
    g_state.failed = 0;
    g_state.percent = -1;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), L"Starting…");
    ReleaseSRWLockExclusive(&g_lock);
    g_job = j;
    g_thread = j->cancel ? CreateThread(NULL, 0, worker, j, 0, NULL) : NULL;
    if (!g_thread) {
        g_job = NULL;
        if (j->cancel) CloseHandle(j->cancel);
        free(j);
        AcquireSRWLockExclusive(&g_lock);
        g_state.busy = 0;
        g_state.failed = 1;
        str_copy(g_state.text, SP_COUNTOF(g_state.text), L"Could not start the import.");
        ReleaseSRWLockExclusive(&g_lock);
        ui_on_status();
        return FALSE;
    }
    LOGI(L"url import: %ls (mode %d, up to %dp)", j->url, mode, j->maxRes);
    ui_on_status();
    return TRUE;
}

void url_import_cancel(void)
{
    if (g_job) SetEvent(g_job->cancel);
}

void url_import_get(UrlImportState *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_state;
    ReleaseSRWLockShared(&g_lock);
}

static int add_entry(UrlJob *j, WallpaperType type, int flags, wchar_t *text, size_t cap)
{
    WallpaperEntry *e = config_find_path(&g_app.cfg, j->path);
    if (e) {
        str_printf(text, cap, L"'%ls' is already in your library.", e->name);
        return 1;
    }
    e = config_add(&g_app.cfg, type, j->path, j->name[0] ? j->name : j->title[0] ? j->title : NULL);
    if (!e) {
        str_copy(text, cap, L"The wallpaper could not be added to the library.");
        return 0;
    }
    e->flags = flags;
    if (j->thumb[0]) str_copy(e->thumb, SP_COUNTOF(e->thumb), j->thumb);
    LOGI(L"url import: added '%ls' (%ls)", e->name, j->path);
    if (j->result == RES_STREAM) str_printf(text, cap, L"Added '%ls'. It streams in small parts; nothing is saved.", e->name);
    else if (j->fellBack) str_printf(text, cap, L"Added '%ls' as a live web page (no video was found on it).", e->name);
    else str_printf(text, cap, L"Added '%ls'.", e->name);
    if (!app_wallpaper_for(NULL)[0]) app_apply_wallpaper(e->id, NULL);
    else app_save_config();
    ui_on_config_changed();
    return 1;
}

void url_import_on_event(WPARAM wp, LPARAM lp)
{
    UrlJob *j = (UrlJob *)lp;
    if (!wp || !j || j != g_job) {                 /* progress: repaint */
        ui_on_status();
        return;
    }
    WaitForSingleObject(g_thread, INFINITE);       /* returns right after posting */
    CloseHandle(g_thread);
    g_thread = NULL;
    g_job = NULL;
    wchar_t text[SP_COUNTOF(g_state.text)] = L"";
    int ok = 0;
    if (j->result == RES_FILE) {
        ok = add_entry(j, wallpaper_type_from_path(j->path), 0, text, SP_COUNTOF(text));
    } else if (j->result == RES_STREAM) {
        ok = add_entry(j, WP_VIDEO, j->flags, text, SP_COUNTOF(text));
    } else if (j->result == RES_PAGE) {
        str_copy(j->path, SP_COUNTOF(j->path), j->url);
        if (!j->title[0]) host_name(j->url, j->title, SP_COUNTOF(j->title));
        ok = add_entry(j, WP_HTML, WPFLAG_URL, text, SP_COUNTOF(text));
    } else if (j->result == RES_CANCELLED) {
        str_copy(text, SP_COUNTOF(text), L"Import cancelled.");
        LOGI(L"url import: cancelled");
    } else {
        str_copy(text, SP_COUNTOF(text), j->error[0] ? j->error : L"The import failed.");
        LOGW(L"url import: failed: %ls", text);
    }
    if (ok) app_notice(NOTICE_INFO, L"%ls", text);
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = 0;
    g_state.percent = -1;
    g_state.failed = !ok && j->result != RES_CANCELLED;
    g_state.finished++;
    g_state.lastOk = ok;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), ok ? L"" : text);
    ReleaseSRWLockExclusive(&g_lock);
    CloseHandle(j->cancel);
    free(j);
    ui_on_status();
}

void url_import_shutdown(void)
{
    if (!g_job) return;
    SetEvent(g_job->cancel);
    if (g_thread && WaitForSingleObject(g_thread, 10000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        CloseHandle(g_job->cancel);
        free(g_job);
    }
    g_thread = NULL;                               /* still running: process exit ends it (and yt-dlp's job) */
    g_job = NULL;
}
