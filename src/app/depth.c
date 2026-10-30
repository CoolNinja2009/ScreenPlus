/* "Make wallpaper cooler" (Customise page, or the "interactive images" setting): turns an image wallpaper
 * into an interactive depth wallpaper that follows the mouse. One job at a time, on a worker thread:
 *   1. The first time only, and only because the user asked, it downloads what the feature needs into
 *      <data>\depth and keeps it: the DirectML build of ONNX Runtime (from its official NuGet package;
 *      onnxruntime.dll is unpacked with Windows' own tar.exe) and the unquantized Depth Anything V2 Small
 *      ONNX model (Hugging Face, onnx-community). Both are pinned by size and SHA-256; nothing is bundled.
 *   2. It runs `ScreenPlus.exe --depth` (depth_infer.c) on the picture: GPU first, CPU as the fallback, and
 *      a retry on the CPU if the child process dies (a driver fault must not take the tray app down).
 *   3. It writes <data>\depth\<id>\: the page (resources/depth.html), its Lively properties, the picture
 *      (hard-linked, or copied) and depth.png. The entry is then converted in place into a plain web
 *      wallpaper (WP_HTML + WPFLAG_DEPTH, `thumb` = the original picture, which also lets it go back), so
 *      everything else is the existing machinery: the engine's WebView2 renderer, mouse forwarding,
 *      Customise properties (depth strength, auto move), grading.
 * The engine and the library never wait for any of it: the plain image keeps showing until the page is ready. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../../resources/resource.h"
#include <stdlib.h>
#include <string.h>

#define ORT_URL      L"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg"
#define ORT_PKG_SIZE ((int64_t)12458649)
#define ORT_PKG_SHA  "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
#define ORT_DLL_SIZE ((int64_t)17328152)
#define ORT_DLL_SHA  "e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40"
#define MODEL_URL    L"https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx"
#define MODEL_SIZE   ((int64_t)99060839)
#define MODEL_SHA    "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c"
#define INFER_MS     (15 * 60 * 1000)

/* Properties of the generated page (Lively format, so Customise shows them like any package's). */
static const char k_props[] =
    "{\"strength\":{\"type\":\"slider\",\"text\":\"Depth strength\",\"min\":0,\"max\":100,\"step\":1,\"value\":40},"
    "\"autoMove\":{\"type\":\"checkbox\",\"text\":\"Auto move\",\"value\":false},"
    "\"autoSpeed\":{\"type\":\"slider\",\"text\":\"Auto move speed\",\"min\":10,\"max\":300,\"step\":10,\"value\":100}}";

typedef struct DepthJob {
    wchar_t id[SP_ID_LEN];          /* wallpaper to convert; empty: only fetch the files */
    wchar_t source[SP_MAX_PATH];    /* its picture */
    wchar_t dir[SP_MAX_PATH];       /* <data>\depth\<id> */
    HANDLE  cancel;                 /* manual-reset event */
    double  lastPost;
    int     autoStart;              /* started by the setting rather than a button: quiet when it works */
    int     ok;
    wchar_t error[300];
} DepthJob;

static SRWLOCK    g_lock = SRWLOCK_INIT;
static DepthState g_state = { 0, -1 };
static DepthJob  *g_job;            /* running job; freed on the shell thread when it ends */
static HANDLE     g_thread;

static void depth_root(wchar_t *out, size_t cap) { path_join(out, cap, paths_root(), L"depth"); }

static void in_depth(wchar_t *out, size_t cap, const wchar_t *rel)
{
    wchar_t d[SP_MAX_PATH];
    depth_root(d, SP_COUNTOF(d));
    path_join(out, cap, d, rel);
}

static int cancelled(DepthJob *j) { return WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0; }

static void set_status(int percent, const wchar_t *text)
{
    AcquireSRWLockExclusive(&g_lock);
    g_state.percent = percent;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), text);
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageW(g_app.shellWnd, WM_SP_DEPTH, 0, 0);
}

static void fail(DepthJob *j, const wchar_t *why)
{
    str_copy(j->error, SP_COUNTOF(j->error), cancelled(j) ? L"Cancelled." : why);
}

/* ---------------------------------------------------------------- files and processes */

/* Size matches and, when sha is given, so does the SHA-256. */
static BOOL file_is(const wchar_t *path, int64_t size, const char *sha)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    char hex[65];
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    if ((int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) != size) return FALSE;
    return !sha || (file_sha256(path, hex) && strcmp(hex, sha) == 0);
}

static BOOL write_file(const wchar_t *path, const void *data, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = WriteFile(f, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    return ok;
}

/* Runs a command hidden at below-normal priority; FALSE when it could not start, ran out of time or was
 * cancelled (the process is then killed). */
static BOOL run_hidden(DepthJob *j, wchar_t *cmd, DWORD timeoutMs, DWORD *code)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    *code = (DWORD)-1;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        LOGE(L"depth: could not start %ls (error %lu)", cmd, GetLastError());
        return FALSE;
    }
    CloseHandle(pi.hThread);
    HANDLE h[2] = { pi.hProcess, j->cancel };
    DWORD w = WaitForMultipleObjects(2, h, FALSE, timeoutMs);
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0;
}

static void remove_folder(const wchar_t *dir)
{
    wchar_t pat[SP_MAX_PATH], f[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    path_join(pat, SP_COUNTOF(pat), dir, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);      /* still in use (the page is open): the next start sweeps it */
}

/* ---------------------------------------------------------------- downloads */

typedef struct Dl { DepthJob *j; const wchar_t *what; } Dl;

static int dl_progress(void *c, int64_t done, int64_t total)
{
    Dl *d = (Dl *)c;
    double now = time_ms();
    if (cancelled(d->j)) return 0;
    if (now - d->j->lastPost < 200.0) return 1;               /* a few repaints a second at most */
    d->j->lastPost = now;
    wchar_t text[SP_COUNTOF(g_state.text)];
    int pct = total > 0 ? (int)SP_MIN(100, done * 100 / total) : -1;
    if (pct >= 0) str_printf(text, SP_COUNTOF(text), L"%ls… %d%%  ·  %.1f of %.1f MB", d->what, pct, done / 1048576.0, total / 1048576.0);
    else str_printf(text, SP_COUNTOF(text), L"%ls… %.1f MB", d->what, done / 1048576.0);
    set_status(pct, text);
    return 1;
}

/* Pinned download: nothing is kept (or later loaded) that isn't the published file. */
static BOOL download(DepthJob *j, const wchar_t *what, const wchar_t *url, int64_t size, const char *sha, const wchar_t *dest)
{
    wchar_t part[SP_MAX_PATH], why[SP_ERR_LEN];
    char hex[65] = "";
    Dl d = { j, what };
    str_printf(part, SP_COUNTOF(part), L"%ls.download", dest);
    HttpGet *g = http_open(url, 0, why, SP_COUNTOF(why));
    BOOL ok = g && http_status(g) == 200;
    if (g && !ok) str_printf(why, SP_COUNTOF(why), L"the server answered HTTP %d", http_status(g));
    if (ok) ok = http_save(g, part, size, dl_progress, &d, hex, why, SP_COUNTOF(why));
    http_close(g);
    if (ok && strcmp(hex, sha) != 0) {
        str_copy(why, SP_COUNTOF(why), L"the file did not match its published SHA-256 checksum");
        ok = FALSE;
    }
    if (ok && !MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        str_printf(why, SP_COUNTOF(why), L"it could not be saved (error %lu)", GetLastError());
        ok = FALSE;
    }
    if (!ok) {
        DeleteFileW(part);
        wchar_t msg[SP_COUNTOF(j->error)];
        str_printf(msg, SP_COUNTOF(msg), L"Could not download %ls: %ls", what, why);
        fail(j, msg);
    }
    return ok;
}

static BOOL ensure_assets(DepthJob *j)
{
    wchar_t ortDir[SP_MAX_PATH], dll[SP_MAX_PATH], model[SP_MAX_PATH];
    in_depth(ortDir, SP_COUNTOF(ortDir), L"ort");
    path_join(dll, SP_COUNTOF(dll), ortDir, L"onnxruntime.dll");
    in_depth(model, SP_COUNTOF(model), L"model.onnx");
    if (!file_is(dll, ORT_DLL_SIZE, ORT_DLL_SHA)) {              /* the DLL is code we load: always verified */
        wchar_t pkg[SP_MAX_PATH], tar[SP_MAX_PATH], cmd[2200];
        DWORD code = 1;
        if (!dir_create_recursive(ortDir)) {
            fail(j, L"Could not create the depth folder in the data folder.");
            return FALSE;
        }
        path_join(pkg, SP_COUNTOF(pkg), ortDir, L"runtime.nupkg");
        if (!download(j, L"Downloading ONNX Runtime", ORT_URL, ORT_PKG_SIZE, ORT_PKG_SHA, pkg)) return FALSE;
        set_status(-1, L"Unpacking ONNX Runtime…");
        GetSystemDirectoryW(tar, SP_COUNTOF(tar));
        path_join(tar, SP_COUNTOF(tar), tar, L"tar.exe");
        str_printf(cmd, SP_COUNTOF(cmd),
                   L"\"%ls\" -xf \"%ls\" -C \"%ls\" --strip-components 3 runtimes/win-x64/native/onnxruntime.dll "
                   L"runtimes/win-x64/native/onnxruntime_providers_shared.dll", tar, pkg, ortDir);
        BOOL ok = run_hidden(j, cmd, 120000, &code) && code == 0 && file_is(dll, ORT_DLL_SIZE, ORT_DLL_SHA);
        DeleteFileW(pkg);
        if (!ok) {
            fail(j, L"ONNX Runtime could not be unpacked (Windows' tar.exe is needed).");
            return FALSE;
        }
        LOGI(L"depth: installed ONNX Runtime 1.24.4 (DirectML)");
    }
    if (!file_is(model, MODEL_SIZE, NULL)) {                     /* data, hashed when it is downloaded */
        if (!dir_create_recursive(ortDir) || !download(j, L"Downloading the depth model", MODEL_URL, MODEL_SIZE, MODEL_SHA, model)) {
            if (!j->error[0]) fail(j, L"Could not create the depth folder in the data folder.");
            return FALSE;
        }
        LOGI(L"depth: installed the Depth Anything V2 Small model");
    }
    return TRUE;
}

BOOL depth_can_cool(const WallpaperEntry *e)
{
    return e->type == WP_IMAGE && !e->missing && !str_ieq(path_ext(e->path), L".gif");
}

BOOL depth_ready(void)
{
    wchar_t dll[SP_MAX_PATH], model[SP_MAX_PATH];
    in_depth(dll, SP_COUNTOF(dll), L"ort\\onnxruntime.dll");
    in_depth(model, SP_COUNTOF(model), L"model.onnx");
    return file_exists(dll) && file_exists(model);
}

/* ---------------------------------------------------------------- the page */

/* The extension a browser can show for a picture of this kind; NULL for HEIC, TIFF and the like (the child
 * process writes a PNG of those). */
static const wchar_t *web_ext(const wchar_t *ext)
{
    static const wchar_t *k[][2] = { { L".jpg", L".jpg" }, { L".jpeg", L".jpg" }, { L".jpe", L".jpg" }, { L".jfif", L".jpg" },
                                     { L".png", L".png" }, { L".bmp", L".bmp" }, { L".webp", L".webp" }, { L".avif", L".avif" } };
    for (size_t i = 0; i < SP_COUNTOF(k); i++) if (str_ieq(ext, k[i][0])) return k[i][1];
    return NULL;
}

/* The depth map is current when it is newer than the picture (switching back to cool is then instant). */
static BOOL depth_current(const wchar_t *source, const wchar_t *depthPng)
{
    WIN32_FILE_ATTRIBUTE_DATA s, d;
    return GetFileAttributesExW(source, GetFileExInfoStandard, &s) && GetFileAttributesExW(depthPng, GetFileExInfoStandard, &d) &&
           CompareFileTime(&d.ftLastWriteTime, &s.ftLastWriteTime) >= 0;
}

static BOOL write_page(DepthJob *j)
{
    wchar_t dst[SP_MAX_PATH], name[24];
    const wchar_t *ext = web_ext(path_ext(j->source));
    str_printf(name, SP_COUNTOF(name), L"image%ls", ext ? ext : L".png");
    path_join(dst, SP_COUNTOF(dst), j->dir, name);
    if (ext) {
        DeleteFileW(dst);
        if (!CreateHardLinkW(dst, j->source, NULL) && !CopyFileW(j->source, dst, FALSE)) {
            fail(j, L"The picture could not be copied next to its depth map.");
            return FALSE;
        }
    } else if (!file_exists(dst)) {
        fail(j, L"The picture could not be converted for the web page.");
        return FALSE;
    }
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_DEPTH_HTML), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const char *tpl = g ? (const char *)LockResource(g) : NULL;
    if (!tpl) {
        fail(j, L"The page template is missing from this build.");
        return FALSE;
    }
    char *img = utf8_from_wide(name, NULL);
    StrBuf b = { 0 };
    const char *p = tpl, *end = tpl + SizeofResource(NULL, r), *hit;
    while (img && p < end && (hit = (const char *)memchr(p, '_', (size_t)(end - p))) != NULL) {   /* every __IMG__ */
        if (end - hit >= 7 && memcmp(hit, "__IMG__", 7) == 0) {
            sb_append(&b, p, (size_t)(hit - p));
            sb_puts(&b, img);
            p = hit + 7;
        } else {
            sb_append(&b, p, (size_t)(hit - p) + 1);
            p = hit + 1;
        }
    }
    if (img && p < end) sb_append(&b, p, (size_t)(end - p));
    free(img);
    wchar_t page[SP_MAX_PATH], props[SP_MAX_PATH];
    path_join(page, SP_COUNTOF(page), j->dir, L"index.html");
    path_join(props, SP_COUNTOF(props), j->dir, L"LivelyProperties.json");
    BOOL ok = !b.failed && write_file(page, b.p, b.len) && write_file(props, k_props, sizeof k_props - 1);
    sb_free(&b);
    if (!ok) fail(j, L"The wallpaper page could not be written to the data folder.");
    return ok;
}

/* ---------------------------------------------------------------- the job */

/* mode: DEPTH_AUTO (the child picks the faster of GPU and CPU, testing both the first time) or DEPTH_CRASHED. */
static BOOL infer(DepthJob *j, int mode, int png, DWORD *code)
{
    wchar_t cmd[2200];
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --depth \"%ls\" \"%ls\"%ls%ls", paths_exe(), j->source, j->dir,
               mode == DEPTH_CRASHED ? L" --crashed" : L"", png ? L" --png" : L"");
    BOOL ok = run_hidden(j, cmd, INFER_MS, code) && *code == 0;
    if (!ok && !cancelled(j)) LOGW(L"depth: the depth process ended with code 0x%lx", *code);
    return ok;
}

/* Why it failed, in words: a Visual C++ runtime that is missing or too old is the usual reason ONNX Runtime
 * can't start or dies. */
static const wchar_t *failure_text(DWORD code)
{
    if (code == 2) return L"This picture could not be read.";
    if (!vcruntime_ok(NULL, 0))
        return L"This needs the Microsoft Visual C++ Redistributable 14.44 or newer. Run screenplus vcredist install, then try again.";
    return code == 3 ? L"ONNX Runtime could not start. See depth.log."
                     : L"The depth could not be calculated. See depth.log.";
}

static void run(DepthJob *j)
{
    wchar_t depthPng[SP_MAX_PATH];
    if (!ensure_assets(j)) return;
    if (!j->id[0]) {
        j->ok = 1;
        return;
    }
    if (!dir_create_recursive(j->dir)) {
        fail(j, L"Could not create the wallpaper's folder in the data folder.");
        return;
    }
    path_join(depthPng, SP_COUNTOF(depthPng), j->dir, L"depth.png");
    if (!depth_current(j->source, depthPng)) {
        set_status(-1, L"Finding the depth of the picture…");
        double t0 = time_ms();
        int png = web_ext(path_ext(j->source)) == NULL;
        DWORD code = 0;
        BOOL ok = infer(j, DEPTH_AUTO, png, &code);
        if (!ok && !cancelled(j) && code != 2 && code != 3 && code != 4)    /* the process died, the GPU path most likely: */
            ok = infer(j, DEPTH_CRASHED, png, &code);                        /* once more on the CPU, which then stays the choice */
        if (!ok) {
            fail(j, failure_text(code));
            return;
        }
        LOGI(L"depth: analysed '%ls' in %.1f s", path_filename(j->source), (time_ms() - t0) / 1000.0);
    }
    set_status(-1, L"Building the interactive wallpaper…");
    j->ok = write_page(j);
}

static DWORD WINAPI worker(LPVOID p)
{
    DepthJob *j = (DepthJob *)p;
    run(j);
    PostMessageW(g_app.shellWnd, WM_SP_DEPTH, 1, (LPARAM)j);
    return 0;
}

/* ---------------------------------------------------------------- shell thread */

static BOOL start(const wchar_t *id, int autoStart)
{
    WallpaperEntry *e = NULL;
    if (g_job) return FALSE;
    if (id && id[0] && (!(e = config_find(&g_app.cfg, id)) || !depth_can_cool(e))) return FALSE;
    DepthJob *j = (DepthJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    if (e) {
        str_copy(j->id, SP_COUNTOF(j->id), e->id);
        str_copy(j->source, SP_COUNTOF(j->source), e->path);
        in_depth(j->dir, SP_COUNTOF(j->dir), e->id);
    }
    j->autoStart = autoStart;
    j->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = 1;
    g_state.failed = 0;
    g_state.percent = -1;
    str_copy(g_state.id, SP_COUNTOF(g_state.id), j->id);
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
        str_copy(g_state.text, SP_COUNTOF(g_state.text), L"Could not start.");
        ReleaseSRWLockExclusive(&g_lock);
        ui_on_status();
        return FALSE;
    }
    LOGI(L"depth: %ls%ls", e ? e->name : L"fetching the depth files", autoStart ? L" (interactive images setting)" : L"");
    ui_on_status();
    return TRUE;
}

BOOL depth_start(const wchar_t *wallpaperId) { return start(wallpaperId, 0); }

void depth_cancel(void)
{
    if (g_job) SetEvent(g_job->cancel);
}

void depth_get(DepthState *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_state;
    ReleaseSRWLockShared(&g_lock);
}

void depth_auto(const wchar_t *id)
{
    WallpaperEntry *e = id && id[0] ? config_find(&g_app.cfg, id) : NULL;
    if (g_app.cfg.s.depthAll && e && depth_can_cool(e) && !(e->flags & WPFLAG_NOCOOL) && depth_ready()) start(id, 1);
}

void depth_auto_all(BOOL fetchMissing)
{
    if (!g_app.cfg.s.depthAll) return;
    if (!depth_ready()) {                       /* the user just switched the setting on: that is the go-ahead to download */
        if (fetchMissing) start(L"", 1);
        return;
    }
    depth_auto(g_app.cfg.defaultWallpaper);
    for (int i = 0; i < g_app.cfg.assignCount; i++) depth_auto(g_app.cfg.assign[i].wallpaperId);
}

/* The job succeeded: the entry becomes the generated page. */
static BOOL convert(DepthJob *j, wchar_t *text, size_t cap)
{
    WallpaperEntry *e = config_find(&g_app.cfg, j->id);
    if (!e || e->type != WP_IMAGE) return TRUE;           /* removed or changed meanwhile: nothing to convert */
    str_copy(e->thumb, SP_COUNTOF(e->thumb), e->path);    /* the picture stays the library thumbnail and the way back */
    path_join(e->path, SP_COUNTOF(e->path), j->dir, L"index.html");
    e->type = WP_HTML;
    e->format = WPF_LIVELY;
    e->flags = (e->flags | WPFLAG_DEPTH) & ~WPFLAG_NOCOOL;
    str_printf(text, cap, L"'%ls' is now interactive.", e->name);
    app_save_config();
    ui_on_config_changed();
    ui_on_customise_changed();
    return TRUE;
}

void depth_on_event(WPARAM wp, LPARAM lp)
{
    DepthJob *j = (DepthJob *)lp;
    if (!wp || !j || j != g_job) {                 /* progress: repaint */
        ui_on_status();
        return;
    }
    WaitForSingleObject(g_thread, INFINITE);       /* returns right after posting */
    CloseHandle(g_thread);
    g_thread = NULL;
    g_job = NULL;
    wchar_t text[400] = L"";
    int ok = j->ok && (!j->id[0] || convert(j, text, SP_COUNTOF(text)));
    int stopped = !ok && WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0;
    if (ok && !j->id[0]) str_copy(text, SP_COUNTOF(text), L"Interactive images are ready.");
    if (ok && text[0] && (!j->autoStart || !j->id[0])) app_notice(NOTICE_INFO, L"%ls", text);
    else if (!ok && !stopped) {
        LOGW(L"depth: failed: %ls", j->error);
        app_notice(NOTICE_ERROR, L"%ls", j->error[0] ? j->error : L"Making the wallpaper cooler failed.");
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = 0;
    g_state.percent = -1;
    g_state.failed = !ok && !stopped;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), g_state.failed ? j->error : L"");
    ReleaseSRWLockExclusive(&g_lock);
    CloseHandle(j->cancel);
    free(j);
    ui_on_status();
    if (ok) depth_auto_all(FALSE);                 /* the setting is on: the next picture waiting for its turn */
}

void depth_revert(const wchar_t *id)
{
    WallpaperEntry *e = config_find(&g_app.cfg, id);
    wchar_t src[SP_MAX_PATH];
    if (!e || !(e->flags & WPFLAG_DEPTH)) return;
    str_copy(src, SP_COUNTOF(src), e->thumb);
    if (!file_exists(src)) {                       /* the original moved away: the copy next to the depth map */
        wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH];
        WIN32_FIND_DATAW fd;
        path_dirname(dir, SP_COUNTOF(dir), e->path);
        path_join(pat, SP_COUNTOF(pat), dir, L"image.*");
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            path_join(src, SP_COUNTOF(src), dir, fd.cFileName);
            FindClose(h);
        }
    }
    if (!file_exists(src)) {
        app_notice(NOTICE_ERROR, L"The picture of '%ls' is gone, so it can't go back to a plain image.", e->name);
        return;
    }
    str_copy(e->path, SP_COUNTOF(e->path), src);
    e->thumb[0] = 0;
    e->type = WP_IMAGE;
    e->format = WPF_PLAIN;
    e->flags = (e->flags & ~WPFLAG_DEPTH) | WPFLAG_NOCOOL;   /* the setting must not undo this choice */
    app_save_config();
    ui_on_config_changed();
    ui_on_customise_changed();
}

void depth_forget(const WallpaperEntry *e)
{
    wchar_t dir[SP_MAX_PATH];
    if (!(e->flags & WPFLAG_DEPTH)) return;
    in_depth(dir, SP_COUNTOF(dir), e->id);
    remove_folder(dir);
}

void depth_sweep(void)
{
    wchar_t root[SP_MAX_PATH], pat[SP_MAX_PATH], d[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    depth_root(root, SP_COUNTOF(root));
    path_join(pat, SP_COUNTOF(pat), root, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != SP_ID_LEN - 1 ||
            config_find(&g_app.cfg, fd.cFileName))
            continue;
        path_join(d, SP_COUNTOF(d), root, fd.cFileName);
        remove_folder(d);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void depth_shutdown(void)
{
    if (!g_job) return;
    SetEvent(g_job->cancel);
    if (g_thread && WaitForSingleObject(g_thread, 10000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        CloseHandle(g_job->cancel);
        free(g_job);
    }
    g_thread = NULL;                               /* still running: process exit ends it */
    g_job = NULL;
}
