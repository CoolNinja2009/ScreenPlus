/* "Make wallpaper cooler" (Customise page, or the "interactive images" setting): turns an image or video
 * wallpaper into an interactive depth wallpaper that follows the mouse. One job at a time, on a worker thread:
 *   1. Only because the user asked, it downloads what the feature needs into <data>\depth and keeps it: the
 *      DirectML build of ONNX Runtime (from its official NuGet package; onnxruntime.dll is unpacked with
 *      Windows' own tar.exe) and the chosen depth model (k_models: Depth Anything 2 and 3, Small to Large, the
 *      unquantized onnx-community exports on Hugging Face). All pinned by size and SHA-256; nothing is bundled.
 *   2. It runs `ScreenPlus.exe --depth` (depth_infer.c) on the picture, or on every frame of the video (it
 *      writes depth.bin and posts its progress here): GPU first, CPU as the fallback, and a retry on the CPU
 *      if the child process dies (a driver fault must not take the tray app down).
 *   3. It writes <data>\depth\<id>\: the page (resources/depth.html), its Lively properties, the picture or
 *      video (hard-linked, or copied) and the depth, the last two named after the model. The entry is then
 *      converted in place into a plain web wallpaper (WP_HTML + WPFLAG_DEPTH, `thumb` = the original file,
 *      which also lets it go back), so everything else is the existing machinery: the engine's WebView2
 *      renderer, mouse forwarding, Customise properties (depth strength, auto move), grading.
 * The engine and the library never wait for any of it: the plain wallpaper keeps showing until the page is ready.
 * depth_estimate() rates each model for this PC (from what the child measured here, else from the GPU's memory
 * and the CPU's threads) for the model list on the Customise page. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../common/json.h"
#include "../renderer/renderer.h"
#include "../../resources/resource.h"
#include <dxgi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ORT_URL      L"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg"
#define ORT_PKG_SIZE ((int64_t)12458649)
#define ORT_PKG_SHA  "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
#define ORT_DLL_SIZE ((int64_t)17328152)
#define ORT_DLL_SHA  "e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40"
#define HF           L"https://huggingface.co/onnx-community/"
#define INFER_MS     (15 * 60 * 1000)          /* a picture */
#define VIDEO_MS     (6 * 60 * 60 * 1000)      /* a whole video on a slow machine */

/* Depth Anything 2 Small stays first: the default, and the one 1.6 installed as depth\model.onnx. */
static const DepthModel k_models[] = {
    { L"da2-small", L"Depth Anything 2 Small", L"Quick and light. Good for most pictures.",
      HF L"depth-anything-v2-small/resolve/main/onnx/model.onnx", NULL, 99060839, 0,
      "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c", NULL, 0, 518, 1.0f, 0.6f },
    { L"da2-base", L"Depth Anything 2 Base", L"Sharper edges and finer detail.",
      HF L"depth-anything-v2-base/resolve/main/onnx/model.onnx", NULL, 388917010, 0,
      "dab40f67d5ead5e0ed52acfae41a717a708b02bc57f6cb761c5de828f26a7289", NULL, 0, 518, 3.6f, 1.5f },
    { L"da2-large", L"Depth Anything 2 Large", L"The most detailed V2. Wants a strong GPU.",
      HF L"depth-anything-v2-large/resolve/main/onnx/model.onnx", NULL, 1336922232, 0,
      "a93aa89b5e92e30e0afbe0f7c3ec692b35cfca791ae9004a190fb0ca2010e905", NULL, 0, 518, 12.0f, 4.0f },
    { L"da3-small", L"Depth Anything 3 Small", L"Newer and steadier. The pick for videos.",
      HF L"depth-anything-v3-small/resolve/main/onnx/model.onnx", HF L"depth-anything-v3-small/resolve/main/onnx/model.onnx_data",
      640691, 104702464, "396008798244a074297fd88e450433b1357fc687f534939375c804ded86e7b2a",
      "802bb24741e67f5bb2b369fc64d40afe11439cc895d676d658d65cfb75c9860f", 1, 504, 1.1f, 0.7f },
    { L"da3-base", L"Depth Anything 3 Base", L"Real depth with clean, solid shapes.",
      HF L"depth-anything-v3-base/resolve/main/onnx/model.onnx", HF L"depth-anything-v3-base/resolve/main/onnx/model.onnx_data",
      646912, 412068352, "d9e7775d8b0340ce9416cfdb4356651ef72abab75eb1485f0aeefa352d78255c",
      "09de4dcff3fefe48b04fedbf146151f3339e6115416a72bf9b36dee3aafd03bd", 1, 504, 3.8f, 1.6f },
    { L"da3-large", L"Depth Anything 3 Large", L"The most accurate. The heavyweight.",
      HF L"depth-anything-v3-large/resolve/main/onnx/model.onnx", HF L"depth-anything-v3-large/resolve/main/onnx/model.onnx_data",
      1093118, 1381741568, "2c02d70b301b4b51c357e7f04c371e27b5d4a5520a0ef11b8c657627e86b4a7e",
      "3aee647d45b208cbff14f4a738737a4bd9f9e5fbce2c48e7a6db311ea807060c", 1, 504, 12.5f, 4.5f },
};

/* Properties of the generated page (Lively format, so Customise shows them like any package's). */
static const char k_props[] =
    "{\"strength\":{\"type\":\"slider\",\"text\":\"Depth strength\",\"min\":0,\"max\":100,\"step\":1,\"value\":40},"
    "\"autoMove\":{\"type\":\"checkbox\",\"text\":\"Auto move\",\"value\":false},"
    "\"autoSpeed\":{\"type\":\"slider\",\"text\":\"Auto move speed\",\"min\":10,\"max\":300,\"step\":10,\"value\":100}}";

typedef struct DepthJob {
    wchar_t id[SP_ID_LEN];          /* wallpaper to convert; empty: only fetch the files */
    wchar_t source[SP_MAX_PATH];    /* its picture or video */
    int     model;                  /* k_models */
    int     video;
    wchar_t dir[SP_MAX_PATH];       /* <data>\depth\<id> */
    HANDLE  cancel;                 /* manual-reset event */
    double  lastPost;
    int     autoStart;              /* started by the setting rather than a button: quiet when it works */
    wchar_t outFile[SP_MAX_PATH];   /* depth_make_file: the depth map goes here, no wallpaper is converted */
    int     ok;
    wchar_t error[300];
} DepthJob;

static SRWLOCK    g_lock = SRWLOCK_INIT;
static DepthState g_state = { 0, -1 };
static DepthJob  *g_job;            /* running job; freed on the shell thread when it ends */
static HANDLE     g_thread;
static int        g_gen = 1;        /* bumped when a job ends: the cached measurements and model names are re-read */

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

/* ---------------------------------------------------------------- models */

int depth_model_count(void) { return (int)SP_COUNTOF(k_models); }

const DepthModel *depth_model(int i) { return &k_models[i >= 0 && i < (int)SP_COUNTOF(k_models) ? i : 0]; }

int depth_model_find(const wchar_t *key)
{
    for (int i = 0; i < (int)SP_COUNTOF(k_models); i++) if (key && str_ieq(key, k_models[i].key)) return i;
    return -1;
}

static int chosen_model(void) { return SP_MAX(0, depth_model_find(g_app.cfg.s.depthModel)); }

void depth_model_dir(int i, wchar_t *out, size_t cap)
{
    wchar_t rel[64];
    if (i <= 0) { depth_root(out, cap); return; }           /* 1.6's place for the default model */
    str_printf(rel, SP_COUNTOF(rel), L"models\\%ls", depth_model(i)->key);
    in_depth(out, cap, rel);
}

void depth_model_file(int i, wchar_t *out, size_t cap)
{
    wchar_t d[SP_MAX_PATH];
    depth_model_dir(i, d, SP_COUNTOF(d));
    path_join(out, cap, d, L"model.onnx");
}

/* ONNX Runtime finds the weights next to the graph under the name the export gave them. */
static void model_data_file(int i, wchar_t *out, size_t cap)
{
    wchar_t d[SP_MAX_PATH];
    depth_model_dir(i, d, SP_COUNTOF(d));
    path_join(out, cap, d, L"model.onnx_data");
}

BOOL depth_model_ready(int i)
{
    const DepthModel *m = depth_model(i);
    wchar_t f[SP_MAX_PATH];
    depth_model_file(i, f, SP_COUNTOF(f));
    if (!file_is(f, m->size, NULL)) return FALSE;
    if (!m->dataUrl) return TRUE;
    model_data_file(i, f, SP_COUNTOF(f));
    return file_is(f, m->dataSize, NULL);
}

static BOOL ensure_assets(DepthJob *j)
{
    wchar_t ortDir[SP_MAX_PATH], dll[SP_MAX_PATH], dir[SP_MAX_PATH], file[SP_MAX_PATH], what[160];
    const DepthModel *m = depth_model(j->model);
    in_depth(ortDir, SP_COUNTOF(ortDir), L"ort");
    path_join(dll, SP_COUNTOF(dll), ortDir, L"onnxruntime.dll");
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
    if (depth_model_ready(j->model)) return TRUE;
    depth_model_dir(j->model, dir, SP_COUNTOF(dir));
    if (!dir_create_recursive(dir)) {
        fail(j, L"Could not create the depth folder in the data folder.");
        return FALSE;
    }
    str_printf(what, SP_COUNTOF(what), L"Downloading %ls", m->name);
    depth_model_file(j->model, file, SP_COUNTOF(file));                 /* data, hashed when it is downloaded */
    if (!file_is(file, m->size, NULL) && !download(j, what, m->url, m->size, m->sha, file)) return FALSE;
    model_data_file(j->model, file, SP_COUNTOF(file));
    if (m->dataUrl && !file_is(file, m->dataSize, NULL) && !download(j, what, m->dataUrl, m->dataSize, m->dataSha, file)) return FALSE;
    LOGI(L"depth: installed %ls", m->name);
    return TRUE;
}

static BOOL is_local_video(const WallpaperEntry *e)
{
    return e->type == WP_VIDEO && !(e->flags & (WPFLAG_URL | WPFLAG_YTDLP)) && !wallpaper_is_url(e->path);
}

BOOL depth_can_cool(const WallpaperEntry *e)
{
    return (e->type == WP_IMAGE || is_local_video(e)) && !e->missing && !str_ieq(path_ext(e->path), L".gif");
}

BOOL depth_ready(void)
{
    wchar_t dll[SP_MAX_PATH];
    in_depth(dll, SP_COUNTOF(dll), L"ort\\onnxruntime.dll");
    return file_exists(dll) && depth_model_ready(chosen_model());
}

/* ---------------------------------------------------------------- how well this PC runs a model */

typedef struct Measured { double gpuMs, cpuMs, gpuRun, cpuRun; } Measured;   /* the child's backend.json */

static void measured(int i, Measured *out)
{
    static Measured cache[SP_COUNTOF(k_models)];
    static int gen[SP_COUNTOF(k_models)];
    i = SP_CLAMP(i, 0, (int)SP_COUNTOF(k_models) - 1);
    if (gen[i] != g_gen) {
        wchar_t p[SP_MAX_PATH];
        Measured m = { 0 };
        depth_model_dir(i, p, SP_COUNTOF(p));
        path_join(p, SP_COUNTOF(p), p, L"backend.json");
        JVal *j = file_exists(p) ? json_parse_file(p) : NULL;
        if (j) {
            m.gpuMs = json_num(json_get(j, "gpu_ms"), 0.0);
            m.cpuMs = json_num(json_get(j, "cpu_ms"), 0.0);
            m.gpuRun = json_num(json_get(j, "gpu_run_ms"), 0.0);
            m.cpuRun = json_num(json_get(j, "cpu_run_ms"), 0.0);
            json_free(j);
        }
        cache[i] = m;
        gen[i] = g_gen;
    }
    *out = cache[i];
}

typedef struct Pc { double vramGb, ramGb; int threads, discrete; } Pc;

static const Pc *pc(void)
{
    static Pc p;
    static int done;
    if (done) return &p;
    done = 1;
    MEMORYSTATUSEX ms = { sizeof ms };
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    p.threads = SP_MAX(1, (int)si.dwNumberOfProcessors);
    if (GlobalMemoryStatusEx(&ms)) p.ramGb = ms.ullTotalPhys / 1073741824.0;
    IDXGIFactory1 *fac = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) {    /* the GPU with the most memory of its own */
        IDXGIAdapter1 *ad = NULL;
        for (UINT i = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(fac, i, &ad)); i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                double own = d.DedicatedVideoMemory / 1073741824.0;
                int discrete = own >= 1.5;          /* integrated GPUs report a small carve-out and share the RAM */
                double usable = discrete ? own : d.SharedSystemMemory / 1073741824.0 * 0.5;
                if (discrete > p.discrete || (discrete == p.discrete && usable > p.vramGb)) {
                    p.discrete = discrete;
                    p.vramGb = usable;
                }
            }
            SP_RELEASE(ad);
        }
        SP_RELEASE(fac);
    }
    LOGI(L"depth: this PC: %.1f GB %ls GPU memory, %.1f GB RAM, %d threads", p.vramGb, p.discrete ? L"dedicated" : L"shared",
         p.ramGb, p.threads);
    return &p;
}

/* Rough figures for Depth Anything 2 Small (one 924x518 frame) where nothing was measured yet. */
static double guess_gpu_run(const Pc *p)
{
    return !p->discrete ? 300.0 : p->vramGb >= 11.0 ? 25.0 : p->vramGb >= 7.0 ? 40.0 : p->vramGb >= 3.5 ? 70.0 : 130.0;
}

void depth_estimate(int model, double videoSeconds, DepthEstimate *out)
{
    const DepthModel *m = depth_model(model);
    const Pc *p = pc();
    Measured me, ref;
    measured(model, &me);
    measured(0, &ref);
    int canGpu = p->vramGb >= m->gb, canCpu = p->ramGb >= m->gb * 1.5 + 2.0;
    double scale = m->cost / k_models[0].cost, frames = 1.0, extra = videoSeconds > 0 ? 15.0 : 0.0;   /* video: decode + encode */
    /* per frame: what was measured with this model, else Depth Anything 2 Small's measurement scaled, else a guess */
    double gpuRun = me.gpuRun > 0 ? me.gpuRun : ref.gpuRun > 0 ? ref.gpuRun * scale : guess_gpu_run(p) * scale;
    double cpuRun = me.cpuRun > 0 ? me.cpuRun : ref.cpuRun > 0 ? ref.cpuRun * scale : 9000.0 / p->threads * scale;
    double gpuStart = me.gpuMs > me.gpuRun && me.gpuRun > 0 ? me.gpuMs - me.gpuRun : 1500.0 + 400.0 * m->cost;   /* DirectML compiles the graph */
    double cpuStart = me.cpuMs > me.cpuRun && me.cpuRun > 0 ? me.cpuMs - me.cpuRun : 300.0 + 100.0 * m->cost;
    memset(out, 0, sizeof *out);
    if (!canGpu && !canCpu) return;
    if (videoSeconds > 0) frames = SP_MIN(ceil(videoSeconds * DEPTH_VIDEO_FPS), (double)DEPTH_VIDEO_FRAMES);
    double gpu = canGpu ? gpuStart + frames * (gpuRun + extra) : 1e18;
    double cpu = canCpu ? cpuStart + frames * (cpuRun + extra) : 1e18;
    out->gpu = gpu <= cpu;
    out->seconds = SP_MIN(gpu, cpu) / 1000.0;
    /* pictures by their own time; videos by the time per second of video */
    double r = videoSeconds > 0 ? out->seconds / videoSeconds : out->seconds;
    out->level = videoSeconds > 0 ? (r <= 1.0 ? 5 : r <= 3.0 ? 4 : r <= 8.0 ? 3 : r <= 25.0 ? 2 : 1)
                                  : (r <= 3.0 ? 5 : r <= 8.0 ? 4 : r <= 20.0 ? 3 : r <= 60.0 ? 2 : 1);
}

double depth_video_seconds(const wchar_t *path)
{
    static wchar_t last[SP_MAX_PATH];
    static double secs;
    if (!str_ieq(last, path)) {
        str_copy(last, SP_COUNTOF(last), path);
        secs = ffmpeg_duration(path);
    }
    return secs;
}

int depth_entry_model(const WallpaperEntry *e)
{
    const wchar_t *f = path_filename(e->path);
    wchar_t key[40];
    if (!(e->flags & WPFLAG_DEPTH)) return -1;
    if (str_ieq(f, L"index.html")) return 0;
    if (!str_istarts(f, L"index-")) return -1;
    str_copy(key, SP_COUNTOF(key), f + 6);
    wchar_t *dot = wcsrchr(key, L'.');
    if (dot) *dot = 0;
    return depth_model_find(key);
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

/* Each model's depth and page have their own names (the default keeps 1.6's index.html and depth.png), so a
 * switch of model changes the entry's path, which makes the engine load the page again, and switching back to
 * a model used before is instant. */
static void model_name(int model, const wchar_t *base, const wchar_t *ext, wchar_t *out, size_t cap)
{
    if (model <= 0) str_printf(out, cap, L"%ls%ls", base, ext);
    else str_printf(out, cap, L"%ls-%ls%ls", base, depth_model(model)->key, ext);
}

/* The depth is current when it is newer than the picture (switching back to cool is then instant). */
static BOOL depth_current(const wchar_t *source, const wchar_t *depthFile)
{
    WIN32_FILE_ATTRIBUTE_DATA s, d;
    return GetFileAttributesExW(source, GetFileExInfoStandard, &s) && GetFileAttributesExW(depthFile, GetFileExInfoStandard, &d) &&
           CompareFileTime(&d.ftLastWriteTime, &s.ftLastWriteTime) >= 0;
}

/* The page from the template (RCDATA): 1 written, 0 already the same (onlyIfChanged), -1 not written, -2 no template. */
static int render_page(const wchar_t *dir, const wchar_t *media, int model, int video, BOOL onlyIfChanged)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_DEPTH_HTML), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const char *tpl = g ? (const char *)LockResource(g) : NULL;
    if (!tpl) return -2;
    wchar_t depthName[64], pageName[64];
    model_name(model, L"depth", video ? L".bin" : L".png", depthName, SP_COUNTOF(depthName));
    model_name(model, L"index", L".html", pageName, SP_COUNTOF(pageName));
    char *img = utf8_from_wide(media, NULL), *dep = utf8_from_wide(depthName, NULL);
    const char *vars[][2] = { { "__IMG__", img }, { "__DEPTH__", dep }, { "__VIDEO__", video ? "1" : "0" } };
    StrBuf b = { 0 };
    const char *p = tpl, *end = tpl + SizeofResource(NULL, r), *hit;
    while (img && dep && p < end && (hit = (const char *)memchr(p, '_', (size_t)(end - p))) != NULL) {   /* every __NAME__ */
        size_t v = 0;
        while (v < SP_COUNTOF(vars) && !((size_t)(end - hit) >= strlen(vars[v][0]) && memcmp(hit, vars[v][0], strlen(vars[v][0])) == 0)) v++;
        if (v < SP_COUNTOF(vars)) {
            sb_append(&b, p, (size_t)(hit - p));
            sb_puts(&b, vars[v][1]);
            p = hit + strlen(vars[v][0]);
        } else {
            sb_append(&b, p, (size_t)(hit - p) + 1);
            p = hit + 1;
        }
    }
    if (img && dep && p < end) sb_append(&b, p, (size_t)(end - p));
    BOOL built = img && dep && !b.failed;
    free(img);
    free(dep);
    wchar_t page[SP_MAX_PATH], props[SP_MAX_PATH];
    path_join(page, SP_COUNTOF(page), dir, pageName);
    path_join(props, SP_COUNTOF(props), dir, L"LivelyProperties.json");
    int rc = -1;
    if (built && onlyIfChanged) {                  /* the same bytes already there: leave the file (and its time) alone */
        HANDLE f = CreateFileW(page, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            char *old = GetFileSizeEx(f, &sz) && (size_t)sz.QuadPart == b.len ? (char *)malloc(b.len + 1) : NULL;
            DWORD n = 0;
            if (old && ReadFile(f, old, (DWORD)b.len, &n, NULL) && n == b.len && memcmp(old, b.p, b.len) == 0) rc = 0;
            free(old);
            CloseHandle(f);
        }
    }
    if (built && rc != 0) rc = write_file(page, b.p, b.len) && write_file(props, k_props, sizeof k_props - 1) ? 1 : -1;
    sb_free(&b);
    return rc;
}

static void reencode_progress(int percent) { set_status(percent, L"Making the video playable on this PC…"); }

/* A video WebView2 can't play here (webvideo.c) becomes video.mp4 (H.264), made once: kept while it is newer
 * than the source and isn't the source itself (the hard link has the same size). */
static int page_video(DepthJob *j, wchar_t *name, size_t cap, wchar_t *dst, size_t dcap)
{
    WIN32_FILE_ATTRIBUTE_DATA s, d;
    if (!webvideo_needed(j->source)) return 0;
    str_copy(name, cap, L"video.mp4");
    path_join(dst, dcap, j->dir, name);
    if (GetFileAttributesExW(j->source, GetFileExInfoStandard, &s) && GetFileAttributesExW(dst, GetFileExInfoStandard, &d) &&
        CompareFileTime(&d.ftLastWriteTime, &s.ftLastWriteTime) > 0 &&
        (d.nFileSizeLow != s.nFileSizeLow || d.nFileSizeHigh != s.nFileSizeHigh))
        return 1;
    wchar_t old[SP_MAX_PATH], part[SP_MAX_PATH], other[24];
    str_printf(other, SP_COUNTOF(other), L"video%.8ls", path_ext(j->source));
    path_join(old, SP_COUNTOF(old), j->dir, other);
    DeleteFileW(old);                              /* an earlier link to the source, whatever its extension */
    path_join(part, SP_COUNTOF(part), j->dir, L"reencode.part.mp4");
    reencode_progress(0);
    if (!webvideo_make(j->source, part, j->cancel, reencode_progress) || !MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part);
        return -1;
    }
    return 1;
}

static BOOL write_page(DepthJob *j)
{
    wchar_t dst[SP_MAX_PATH], name[24];
    const wchar_t *ext = j->video ? path_ext(j->source) : web_ext(path_ext(j->source));
    int re = j->video ? page_video(j, name, SP_COUNTOF(name), dst, SP_COUNTOF(dst)) : 0;
    if (re < 0) {
        fail(j, L"This PC's graphics can't play this HEVC video in the interactive page, and it could not be re-encoded. See screenplus.log.");
        return FALSE;
    }
    if (!re) {
        str_printf(name, SP_COUNTOF(name), L"%ls%.8ls", j->video ? L"video" : L"image", ext ? ext : L".png");
        path_join(dst, SP_COUNTOF(dst), j->dir, name);
    }
    if (re) {
        /* video.mp4 is ready */
    } else if (ext) {
        DeleteFileW(dst);
        if (!CreateHardLinkW(dst, j->source, NULL) && !CopyFileW(j->source, dst, FALSE)) {
            fail(j, j->video ? L"The video could not be copied next to its depth." : L"The picture could not be copied next to its depth map.");
            return FALSE;
        }
    } else if (!file_exists(dst)) {
        fail(j, L"The picture could not be converted for the web page.");
        return FALSE;
    }
    int r = render_page(j->dir, name, j->model, j->video, FALSE);
    if (r < 0) fail(j, r == -2 ? L"The page template is missing from this build." : L"The wallpaper page could not be written to the data folder.");
    return r >= 0;
}

/* Pages made by an older build get this build's page (its fixes) at the next start; nothing is touched when
 * they already match. */
void depth_refresh_pages(void)
{
    for (int i = 0; i < g_app.cfg.count; i++) {
        const WallpaperEntry *e = &g_app.cfg.items[i];
        wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH];
        WIN32_FIND_DATAW fd;
        int video = (e->flags & WPFLAG_DEPTHVIDEO) != 0, model = depth_entry_model(e);
        if (!(e->flags & WPFLAG_DEPTH) || e->type != WP_HTML || model < 0) continue;
        path_dirname(dir, SP_COUNTOF(dir), e->path);
        path_join(pat, SP_COUNTOF(pat), dir, video ? L"video.*" : L"image.*");
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        if (render_page(dir, fd.cFileName, model, video, TRUE) == 1) LOGI(L"depth: updated the page of '%ls'", e->name);
    }
}

/* ---------------------------------------------------------------- the job */

/* mode: DEPTH_AUTO (the child picks the faster of GPU and CPU, testing both the first time) or DEPTH_CRASHED. */
static BOOL infer(DepthJob *j, int mode, int png, DWORD *code)
{
    wchar_t cmd[2400];
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --depth \"%ls\" \"%ls\" --model %ls --notify %llx%ls%ls%ls", paths_exe(), j->source, j->dir,
               depth_model(j->model)->key, (unsigned long long)(uintptr_t)g_app.shellWnd, j->video ? L" --video" : L"",
               mode == DEPTH_CRASHED ? L" --crashed" : L"", png ? L" --png" : L"");
    BOOL ok = run_hidden(j, cmd, j->video ? VIDEO_MS : INFER_MS, code) && *code == 0;
    if (!ok && !cancelled(j)) LOGW(L"depth: the depth process ended with code 0x%lx", *code);
    return ok;
}

/* Why it failed, in words: a Visual C++ runtime that is missing or too old is the usual reason ONNX Runtime
 * can't start or dies. */
static const wchar_t *failure_text(DepthJob *j, DWORD code)
{
    if (code == 2) return j->video ? L"This video could not be read." : L"This picture could not be read.";
    if (code == 5) return L"This video's format can't play in the interactive page (H.264, HEVC, VP9 or AV1 can).";
    if (!vcruntime_ok(NULL, 0))
        return L"This needs the Microsoft Visual C++ Redistributable 14.44 or newer. Run screenplus vcredist install, then try again.";
    return code == 3 ? L"ONNX Runtime could not start. See depth.log."
                     : L"The depth could not be calculated. See depth.log.";
}

static void run(DepthJob *j)
{
    wchar_t depthFile[SP_MAX_PATH], name[64];
    if (!ensure_assets(j)) return;
    if (j->outFile[0]) {                           /* one picture's depth map for a wallpaper that reads it */
        DWORD code = 0;
        set_status(-1, L"Finding the depth of the background…");
        model_name(j->model, L"depth", L".png", name, SP_COUNTOF(name));
        path_join(depthFile, SP_COUNTOF(depthFile), j->dir, name);
        if (!dir_create_recursive(j->dir)) {
            fail(j, L"Could not create the depth folder in the data folder.");
            return;
        }
        BOOL ok = infer(j, DEPTH_AUTO, 0, &code);
        if (!ok && !cancelled(j) && code != 2 && code != 3 && code != 4) ok = infer(j, DEPTH_CRASHED, 0, &code);
        if (!ok) {
            fail(j, failure_text(j, code));
            return;
        }
        if (!CopyFileW(depthFile, j->outFile, FALSE)) {
            fail(j, L"The depth map could not be saved next to the wallpaper.");
            return;
        }
        DeleteFileW(depthFile);
        LOGI(L"depth: depth map of '%ls' with %ls -> %ls", path_filename(j->source), depth_model(j->model)->name, j->outFile);
        j->ok = 1;
        return;
    }
    if (!j->id[0]) {
        j->ok = 1;
        return;
    }
    if (!dir_create_recursive(j->dir)) {
        fail(j, L"Could not create the wallpaper's folder in the data folder.");
        return;
    }
    model_name(j->model, L"depth", j->video ? L".bin" : L".png", name, SP_COUNTOF(name));
    path_join(depthFile, SP_COUNTOF(depthFile), j->dir, name);
    if (!depth_current(j->source, depthFile)) {
        set_status(-1, j->video ? L"Finding the depth of every frame…" : L"Finding the depth of the picture…");
        double t0 = time_ms();
        int png = !j->video && web_ext(path_ext(j->source)) == NULL;
        DWORD code = 0;
        BOOL ok = infer(j, DEPTH_AUTO, png, &code);
        if (!ok && !cancelled(j) && code != 2 && code != 3 && code != 4 && code != 5)  /* the process died, the GPU path most likely: */
            ok = infer(j, DEPTH_CRASHED, png, &code);                                   /* once more on the CPU, which then stays the choice */
        if (!ok) {
            fail(j, failure_text(j, code));
            return;
        }
        LOGI(L"depth: analysed '%ls' with %ls in %.1f s", path_filename(j->source), depth_model(j->model)->name, (time_ms() - t0) / 1000.0);
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
    if (id && id[0] && (!(e = config_find(&g_app.cfg, id)) || !(depth_can_cool(e) || (e->flags & WPFLAG_DEPTH)))) return FALSE;
    DepthJob *j = (DepthJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    j->model = chosen_model();
    if (e) {
        str_copy(j->id, SP_COUNTOF(j->id), e->id);
        /* an interactive wallpaper redone with another model: from its original file */
        str_copy(j->source, SP_COUNTOF(j->source), (e->flags & WPFLAG_DEPTH) ? e->thumb : e->path);
        j->video = (e->flags & WPFLAG_DEPTH) ? (e->flags & WPFLAG_DEPTHVIDEO) != 0 : e->type == WP_VIDEO;
        in_depth(j->dir, SP_COUNTOF(j->dir), e->id);
        if (!file_exists(j->source)) {
            free(j);
            app_notice(NOTICE_ERROR, L"The original file of '%ls' is gone.", e->name);
            return FALSE;
        }
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
    LOGI(L"depth: %ls with %ls%ls", e ? e->name : L"fetching the depth files", depth_model(j->model)->name,
         autoStart ? L" (interactive images setting)" : L"");
    ui_on_status();
    return TRUE;
}

BOOL depth_start(const wchar_t *wallpaperId) { return start(wallpaperId, 0); }

/* The model for a background's depth map: Depth Anything 3 Small when it is there, else the smallest model
 * already downloaded, else the smallest model of all (downloaded now). */
static int file_model(void)
{
    int best = -1, da3 = depth_model_find(L"da3-small");
    if (da3 >= 0 && depth_model_ready(da3)) return da3;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < (int)SP_COUNTOF(k_models); i++)
            if ((pass || depth_model_ready(i)) &&
                (best < 0 || k_models[i].size + k_models[i].dataSize < k_models[best].size + k_models[best].dataSize))
                best = i;
    return best;
}

BOOL depth_make_file(const wchar_t *image, const wchar_t *outPng)
{
    if (g_job) return FALSE;
    DepthJob *j = (DepthJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    j->model = file_model();
    str_copy(j->source, SP_COUNTOF(j->source), image);
    str_copy(j->outFile, SP_COUNTOF(j->outFile), outPng);
    in_depth(j->dir, SP_COUNTOF(j->dir), L"file-job");
    j->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = 1;
    g_state.failed = 0;
    g_state.percent = -1;
    g_state.id[0] = 0;
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
        ReleaseSRWLockExclusive(&g_lock);
        return FALSE;
    }
    LOGI(L"depth: depth map of '%ls' with %ls", path_filename(image), depth_model(j->model)->name);
    ui_on_status();
    return TRUE;
}

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
    /* pictures only: a video takes minutes, so it waits for its own button */
    if (g_app.cfg.s.depthAll && e && e->type == WP_IMAGE && depth_can_cool(e) && !(e->flags & WPFLAG_NOCOOL) && depth_ready()) start(id, 1);
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

/* The job succeeded: the entry becomes the generated page (or, redone with another model, simply reloads). */
static BOOL convert(DepthJob *j, wchar_t *text, size_t cap)
{
    WallpaperEntry *e = config_find(&g_app.cfg, j->id);
    wchar_t page[64];
    if (!e) return TRUE;                                   /* removed meanwhile: nothing to convert */
    model_name(j->model, L"index", L".html", page, SP_COUNTOF(page));
    if (e->flags & WPFLAG_DEPTH) {                         /* redone with another model: its own page */
        path_join(e->path, SP_COUNTOF(e->path), j->dir, page);
        str_printf(text, cap, L"'%ls' now uses %ls.", e->name, depth_model(j->model)->name);
        app_save_config();
        ui_on_config_changed();
        ui_on_customise_changed();
        return TRUE;
    }
    if (e->type != (j->video ? WP_VIDEO : WP_IMAGE)) return TRUE;   /* changed meanwhile */
    str_copy(e->thumb, SP_COUNTOF(e->thumb), e->path);    /* the original stays the library thumbnail and the way back */
    path_join(e->path, SP_COUNTOF(e->path), j->dir, page);
    e->type = WP_HTML;
    e->format = WPF_LIVELY;
    e->flags = (e->flags | WPFLAG_DEPTH | (j->video ? WPFLAG_DEPTHVIDEO : 0)) & ~WPFLAG_NOCOOL;
    str_printf(text, cap, L"'%ls' is now interactive.", e->name);
    app_save_config();
    ui_on_config_changed();
    ui_on_customise_changed();
    return TRUE;
}

void depth_on_event(WPARAM wp, LPARAM lp)
{
    DepthJob *j = (DepthJob *)lp;
    if (wp == 2) {                                 /* the child's frame count */
        int done = (int)(lp & 0xffff), total = (int)((lp >> 16) & 0xffff);
        if (!g_job || total <= 0) return;
        AcquireSRWLockExclusive(&g_lock);
        g_state.percent = SP_MIN(100, done * 100 / total);
        str_printf(g_state.text, SP_COUNTOF(g_state.text), L"Finding the depth of every frame… %d of %d", done, total);
        ReleaseSRWLockExclusive(&g_lock);
        ui_on_status();
        return;
    }
    if (!wp || !j || j != g_job) {                 /* progress: repaint */
        ui_on_status();
        return;
    }
    WaitForSingleObject(g_thread, INFINITE);       /* returns right after posting */
    CloseHandle(g_thread);
    g_thread = NULL;
    g_job = NULL;
    g_gen++;                                       /* new measurements */
    wchar_t text[400] = L"";
    int ok = j->ok && (!j->id[0] || convert(j, text, SP_COUNTOF(text)));
    int stopped = !ok && WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0;
    if (ok && j->outFile[0]) str_copy(text, SP_COUNTOF(text), L"The background now has depth.");
    else if (ok && !j->id[0]) str_copy(text, SP_COUNTOF(text), L"Interactive images are ready.");
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
    int video = (e->flags & WPFLAG_DEPTHVIDEO) != 0;
    str_copy(src, SP_COUNTOF(src), e->thumb);
    if (!file_exists(src)) {                       /* the original moved away: the copy next to the depth */
        wchar_t dir[SP_MAX_PATH], pat[SP_MAX_PATH];
        WIN32_FIND_DATAW fd;
        path_dirname(dir, SP_COUNTOF(dir), e->path);
        path_join(pat, SP_COUNTOF(pat), dir, video ? L"video.*" : L"image.*");
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            path_join(src, SP_COUNTOF(src), dir, fd.cFileName);
            FindClose(h);
        }
    }
    if (!file_exists(src)) {
        app_notice(NOTICE_ERROR, L"The %ls of '%ls' is gone, so it can't go back.", video ? L"video" : L"picture", e->name);
        return;
    }
    str_copy(e->path, SP_COUNTOF(e->path), src);
    e->thumb[0] = 0;
    e->type = video ? WP_VIDEO : WP_IMAGE;
    e->format = WPF_PLAIN;
    e->flags = (e->flags & ~(WPFLAG_DEPTH | WPFLAG_DEPTHVIDEO)) | WPFLAG_NOCOOL;   /* the setting must not undo this choice */
    app_save_config();
    ui_on_config_changed();
    ui_on_customise_changed();
}

void depth_forget(const WallpaperEntry *e)
{
    wchar_t dir[SP_MAX_PATH];
    in_depth(dir, SP_COUNTOF(dir), e->id);
    if (dir_exists(dir)) library_discard(dir, TRUE);   /* its page, or the picture "Use plain image" went back to */
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
