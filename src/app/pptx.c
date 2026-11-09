/* PowerPoint presentations as wallpapers (.pptx, .ppsx, .pptm, .ppt, .pps).
 * PowerPoint itself renders the presentation to an MP4 (Presentation.CreateVideo), so every transition (Morph
 * included) and animation looks as it does in a slide show; each slide stays up for the entry's slideSec seconds.
 * The video then plays as an ordinary looping video wallpaper (WP_VIDEO + WPFLAG_PPTX).
 *  - A job is a hidden PowerShell child driving PowerPoint over COM (-EncodedCommand, paths as quoted literals),
 *    waited on by a worker thread; the result comes back to the shell as WM_SP_PPTX.
 *  - Files: <data>\slides\<folder>\slides-<sec>.mp4 and source.txt (the presentation's path, UTF-8). Changing the
 *    seconds in Customise renders again from source.txt and swaps the entry's path; the old video is discarded.
 *  - Removing the entry deletes its folder (library.c). Linux: no PowerPoint, so a notice says so. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include <stdlib.h>

static const wchar_t *const k_exts[] = { L".pptx", L".ppsx", L".pptm", L".ppt", L".pps", L".potx" };

BOOL pptx_is_file(const wchar_t *path)
{
    const wchar_t *ext = path_ext(path);
    for (size_t i = 0; i < SP_COUNTOF(k_exts); i++) if (str_ieq(ext, k_exts[i])) return TRUE;
    return FALSE;
}

#ifndef _WIN32

BOOL pptx_import(const wchar_t *path, BOOL apply)
{
    (void)apply;
    app_notice(NOTICE_WARN, L"'%ls' needs PowerPoint, which Linux doesn't have. Export it as a video instead.", path_filename(path));
    return FALSE;
}
BOOL pptx_set_seconds(const wchar_t *id, int sec) { (void)id; (void)sec; return FALSE; }
void pptx_shutdown(void) {}
BOOL pptx_busy(const wchar_t *id) { (void)id; return FALSE; }
void pptx_on_event(WPARAM wp, LPARAM lp) { (void)wp; (void)lp; }

#else

typedef struct PptxJob {
    wchar_t id[SP_ID_LEN];          /* "" : a new import */
    wchar_t name[SP_NAME_LEN];
    wchar_t src[SP_MAX_PATH], out[SP_MAX_PATH];
    int     sec, apply;
    DWORD   code;                   /* the child's exit code: 0 done, 2 no PowerPoint, else failed */
    HANDLE  proc;
} PptxJob;

static PptxJob *g_jobs[16];

static void b64(const BYTE *in, size_t n, wchar_t *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = (wchar_t)t[v >> 18 & 63];
        out[o++] = (wchar_t)t[v >> 12 & 63];
        out[o++] = i + 1 < n ? (wchar_t)t[v >> 6 & 63] : L'=';
        out[o++] = i + 2 < n ? (wchar_t)t[v & 63] : L'=';
    }
    out[o] = 0;
}

/* a PowerShell single-quoted literal */
static void ps_quote(wchar_t *dst, size_t cap, const wchar_t *s)
{
    size_t o = 0;
    if (cap < 3) return;
    dst[o++] = L'\'';
    for (; *s && o + 3 < cap; s++) {
        if (*s == L'\'') dst[o++] = L'\'';
        dst[o++] = *s;
    }
    dst[o++] = L'\'';
    dst[o] = 0;
}

static int video_height(void)
{
    int h = 1080;
    for (int i = 0; i < g_app.monCount; i++) h = SP_MAX(h, (int)(g_app.mons[i].rect.bottom - g_app.mons[i].rect.top));
    return h > 1440 ? 2160 : h > 1080 ? 1440 : 1080;
}

static DWORD WINAPI worker(void *arg)
{
    PptxJob *j = (PptxJob *)arg;
    if (WaitForSingleObject(j->proc, 60 * 60 * 1000) != WAIT_OBJECT_0) {
        TerminateProcess(j->proc, 9);
        j->code = 9;
    } else if (!GetExitCodeProcess(j->proc, &j->code)) j->code = 8;
    PostMessageW(g_app.shellWnd, WM_SP_PPTX, 1, (LPARAM)j);
    return 0;
}

static BOOL start_job(PptxJob *j)
{
    static const wchar_t fmt[] =
        L"$ErrorActionPreference='Stop';"
        L"try{$app=New-Object -ComObject PowerPoint.Application}catch{exit 2};"
        L"$p=$app.Presentations.Open(%ls,-1,0,0);"
        L"try{$p.CreateVideo(%ls,$false,%d,%d,30,90);"
        L"while($p.CreateVideoStatus -eq 1 -or $p.CreateVideoStatus -eq 2){Start-Sleep -Milliseconds 500;"
        L"if(Test-Path -LiteralPath %ls){exit 5}};"
        L"if($p.CreateVideoStatus -ne 3){exit 3}}"
        L"finally{$p.Close();if($app.Presentations.Count -eq 0){$app.Quit()}};exit 0";
    wchar_t qs[SP_MAX_PATH * 2 + 4], qo[SP_MAX_PATH * 2 + 4], qc[SP_MAX_PATH * 2 + 16], cancel[SP_MAX_PATH + 8];
    ps_quote(qs, SP_COUNTOF(qs), j->src);
    ps_quote(qo, SP_COUNTOF(qo), j->out);
    str_printf(cancel, SP_COUNTOF(cancel), L"%ls.cancel", j->out);   /* pptx_shutdown makes it: the job stops */
    ps_quote(qc, SP_COUNTOF(qc), cancel);
    size_t cap = wcslen(fmt) + wcslen(qs) + wcslen(qo) + wcslen(qc) + 64;
    wchar_t *script = (wchar_t *)malloc(cap * sizeof(wchar_t));
    wchar_t *enc = script ? (wchar_t *)malloc((cap * 2 / 3 * 4 + 8) * sizeof(wchar_t) + 64) : NULL;
    wchar_t *cmd = enc ? (wchar_t *)malloc((cap * 2 / 3 * 4 + SP_MAX_PATH + 128) * sizeof(wchar_t) + 64) : NULL;
    BOOL ok = FALSE;
    if (cmd) {
        str_printf(script, cap, fmt, qs, qo, j->sec, video_height(), qc);
        b64((const BYTE *)script, wcslen(script) * sizeof(wchar_t), enc);
        wchar_t ps[SP_MAX_PATH];
        GetSystemDirectoryW(ps, SP_MAX_PATH);
        str_cat(ps, SP_COUNTOF(ps), L"\\WindowsPowerShell\\v1.0\\powershell.exe");
        size_t ccap = wcslen(enc) + SP_MAX_PATH + 128;
        str_printf(cmd, ccap, L"\"%ls\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand %ls", ps, enc);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(ps, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            j->proc = pi.hProcess;
            HANDLE t = CreateThread(NULL, 0, worker, j, 0, NULL);
            if (t) {
                CloseHandle(t);
                ok = TRUE;
            } else {
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
            }
        }
    }
    free(cmd);
    free(enc);
    free(script);
    if (!ok) return FALSE;
    for (size_t i = 0; i < SP_COUNTOF(g_jobs); i++) if (!g_jobs[i]) { g_jobs[i] = j; break; }
    LOGI(L"slides: PowerPoint is making a video of '%ls' (%d s a slide)", j->src, j->sec);
    ui_on_status();
    return TRUE;
}

static void write_source(const wchar_t *dir, const wchar_t *src)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), dir, L"source.txt");
    char u[SP_MAX_PATH * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, u, (int)sizeof u, NULL, NULL);
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    if (n > 1) WriteFile(f, u, (DWORD)(n - 1), &wr, NULL);
    CloseHandle(f);
}

static BOOL read_source(const wchar_t *dir, wchar_t *out, size_t cap)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), dir, L"source.txt");
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    char u[SP_MAX_PATH * 3];
    DWORD rd = 0;
    BOOL ok = ReadFile(f, u, (DWORD)sizeof u - 1, &rd, NULL);
    CloseHandle(f);
    if (!ok || !rd) return FALSE;
    u[rd] = 0;
    return MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)cap) > 1;
}

static BOOL busy_on(const wchar_t *id, const wchar_t *src)
{
    for (size_t i = 0; i < SP_COUNTOF(g_jobs); i++)
        if (g_jobs[i] && ((id && id[0] && str_ieq(g_jobs[i]->id, id)) || (src && str_ieq(g_jobs[i]->src, src)))) return TRUE;
    return FALSE;
}

BOOL pptx_busy(const wchar_t *id) { return busy_on(id, NULL); }

BOOL pptx_import(const wchar_t *path, BOOL apply)
{
    wchar_t full[SP_MAX_PATH];
    if (!GetFullPathNameW(path, SP_COUNTOF(full), full, NULL)) str_copy(full, SP_COUNTOF(full), path);
    if (busy_on(NULL, full)) return TRUE;
    for (int i = 0; i < g_app.cfg.count; i++) {          /* already in the library */
        const WallpaperEntry *e = &g_app.cfg.items[i];
        wchar_t dir[SP_MAX_PATH], src[SP_MAX_PATH];
        if (!(e->flags & WPFLAG_PPTX)) continue;
        path_dirname(dir, SP_COUNTOF(dir), e->path);
        if (read_source(dir, src, SP_COUNTOF(src)) && str_ieq(src, full)) {
            app_notice(NOTICE_INFO, L"'%ls' is already in the library.", e->name);
            return FALSE;
        }
    }
    PptxJob *j = (PptxJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    wchar_t dir[SP_MAX_PATH], leaf[32], base[SP_MAX_PATH];
    path_join(base, SP_COUNTOF(base), paths_root(), L"slides");
    str_printf(leaf, SP_COUNTOF(leaf), L"%llx", (unsigned long long)filetime_now());
    path_join(dir, SP_COUNTOF(dir), base, leaf);
    dir_create_recursive(dir);
    write_source(dir, full);
    str_copy(j->src, SP_COUNTOF(j->src), full);
    path_stem(j->name, SP_COUNTOF(j->name), full);
    j->sec = SP_SLIDE_SEC_DEFAULT;
    j->apply = apply;
    wchar_t file[32];
    str_printf(file, SP_COUNTOF(file), L"slides-%d.mp4", j->sec);
    path_join(j->out, SP_COUNTOF(j->out), dir, file);
    if (!start_job(j)) {
        free(j);
        app_notice(NOTICE_ERROR, L"Could not start PowerPoint for '%ls'.", path_filename(full));
        return FALSE;
    }
    app_notice(NOTICE_INFO, L"PowerPoint is turning '%ls' into a wallpaper. This can take a few minutes.", path_filename(full));
    return TRUE;
}

BOOL pptx_set_seconds(const wchar_t *id, int sec)
{
    WallpaperEntry *e = config_find(&g_app.cfg, id);
    sec = SP_CLAMP(sec, 1, 3600);
    if (!e || !(e->flags & WPFLAG_PPTX) || busy_on(id, NULL)) return FALSE;
    wchar_t dir[SP_MAX_PATH], file[32], out[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), e->path);
    str_printf(file, SP_COUNTOF(file), L"slides-%d.mp4", sec);
    path_join(out, SP_COUNTOF(out), dir, file);
    e->slideSec = sec;
    if (file_exists(out)) {                              /* made before */
        str_copy(e->path, SP_COUNTOF(e->path), out);
        app_save_config();
        return TRUE;
    }
    PptxJob *j = (PptxJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    if (!read_source(dir, j->src, SP_COUNTOF(j->src)) || !file_exists(j->src)) {
        free(j);
        app_save_config();
        app_notice(NOTICE_WARN, L"The presentation behind '%ls' can't be found, so its slides can't be timed again.", e->name);
        return FALSE;
    }
    str_copy(j->id, SP_COUNTOF(j->id), id);
    str_copy(j->name, SP_COUNTOF(j->name), e->name);
    str_copy(j->out, SP_COUNTOF(j->out), out);
    j->sec = sec;
    app_save_config();
    if (!start_job(j)) {
        free(j);
        app_notice(NOTICE_ERROR, L"Could not start PowerPoint for '%ls'.", e->name);
        return FALSE;
    }
    return TRUE;
}

/* Screen+ is quitting: each running job closes its presentation (and PowerPoint, when it opened it) and stops;
 * a new import's half-made folder goes. Waits a few seconds at most. */
void pptx_shutdown(void)
{
    for (size_t i = 0; i < SP_COUNTOF(g_jobs); i++) {
        PptxJob *j = g_jobs[i];
        if (!j) continue;
        wchar_t cancel[SP_MAX_PATH + 8], dir[SP_MAX_PATH];
        str_printf(cancel, SP_COUNTOF(cancel), L"%ls.cancel", j->out);
        HANDLE f = CreateFileW(cancel, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (WaitForSingleObject(j->proc, 4000) != WAIT_OBJECT_0) TerminateProcess(j->proc, 9);
        DeleteFileW(cancel);
        DeleteFileW(j->out);
        path_dirname(dir, SP_COUNTOF(dir), j->out);
        if (!j->id[0]) library_discard(dir, TRUE);
        LOGI(L"slides: stopped PowerPoint's video of '%ls' (closing)", j->name);
        g_jobs[i] = NULL;                      /* the worker's message never arrives once the shell is gone */
    }
}

void pptx_on_event(WPARAM wp, LPARAM lp)
{
    PptxJob *j = (PptxJob *)lp;
    if (wp != 1 || !j) return;
    for (size_t i = 0; i < SP_COUNTOF(g_jobs); i++) if (g_jobs[i] == j) g_jobs[i] = NULL;
    CloseHandle(j->proc);
    if (j->code != 0 || !file_exists(j->out)) {
        LOGW(L"slides: PowerPoint failed on '%ls' (exit %lu)", j->src, (unsigned long)j->code);
        if (j->code == 2) app_notice(NOTICE_ERROR, L"'%ls' needs Microsoft PowerPoint, which isn't installed.", j->name);
        else app_notice(NOTICE_ERROR, L"PowerPoint couldn't make a video of '%ls'.", j->name);
        if (!j->id[0]) {                                  /* a new import: its empty folder goes */
            wchar_t dir[SP_MAX_PATH];
            path_dirname(dir, SP_COUNTOF(dir), j->out);
            library_discard(dir, TRUE);
        }
    } else if (!j->id[0]) {
        WallpaperEntry *e = config_add(&g_app.cfg, WP_VIDEO, j->out, j->name);
        if (e) {
            e->flags |= WPFLAG_PPTX;
            e->slideSec = j->sec;
            wchar_t id[SP_ID_LEN];
            str_copy(id, SP_COUNTOF(id), e->id);
            LOGI(L"slides: '%ls' added", j->name);
            if (j->apply) app_apply_wallpaper(id, NULL);
            else app_save_config();
            app_notice(NOTICE_INFO, L"Added '%ls'.", j->name);
        }
    } else {
        WallpaperEntry *e = config_find(&g_app.cfg, j->id);
        if (e && e->slideSec == j->sec) {                 /* still the wanted timing */
            wchar_t old[SP_MAX_PATH];
            str_copy(old, SP_COUNTOF(old), e->path);
            str_copy(e->path, SP_COUNTOF(e->path), j->out);
            app_save_config();
            if (!str_ieq(old, j->out)) library_discard(old, FALSE);
            LOGI(L"slides: '%ls' now %d s a slide", e->name, j->sec);
        } else if (!e) {
            library_discard(j->out, FALSE);
        }
    }
    free(j);
    ui_on_config_changed();
}

#endif
