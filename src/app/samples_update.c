/* Sample wallpaper updates: the "Sample wallpaper updates" setting (off by default) and Settings' Check now / Update.
 * GitHub's latest release lists every sample file's SHA-256 in ScreenPlus-<version>-samples.json (CMakeLists.txt).
 * A check hashes the files of the samples in <exe>\samples that the list names (samples the user never downloaded
 * are skipped, files the user added are never looked at) and reports the samples that changed by their titles.
 * Update downloads the release's samples zip (size and SHA-256 as GitHub lists them) and unpacks only the changed
 * files over the old ones with Windows' tar.exe. Customise settings live in the data folder, so they stay; the
 * library is never changed. With everything current, samples\samples.id gets the release's id, so setup stops
 * offering the update too. With the setting on, the first check runs a few minutes after start, then every
 * 12 hours (an hour after a failed one), and updates by itself. The work runs on a worker thread that reports
 * via WM_SP_SAMPLES. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../common/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_MS     (4 * 60 * 1000)
#define EVERY_MS     (12 * 60 * 60 * 1000)
#define RETRY_MS     (60 * 60 * 1000)
#define SOON_MS      3000
#define LIST_MAX     ((int64_t)8 << 20)
#define ZIP_MAX      ((int64_t)1 << 30)
#define MAX_FOLDERS  64
#define TAR_CMD_MAX  30000                      /* CreateProcess allows 32767 characters */

typedef struct SamplesJob {
    int     install;                            /* update the changed samples (else only report them) */
    int     user;                               /* asked for (button) rather than the timer */
    HANDLE  cancel;                             /* manual-reset event */
    double  lastPost;
    int     ok;
    int     installed;                          /* the changed samples were updated */
    int     changed;                            /* samples that differ from the release's */
    int     present;                            /* downloaded samples the list names */
    int     noList;                             /* the latest release has no samples list */
    wchar_t version[32];
    wchar_t titles[200];                        /* "Rain, Aurora" */
    wchar_t error[SP_ERR_LEN];
} SamplesJob;

static SRWLOCK      g_lock = SRWLOCK_INIT;
static SamplesState g_state = { 0, -1 };
static SamplesJob  *g_job;
static HANDLE       g_thread;

static void set_status(int busy, int percent, int pending, const wchar_t *text)
{
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = busy;
    g_state.percent = percent;
    g_state.pending = pending;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), text);
    ReleaseSRWLockExclusive(&g_lock);
}

static void samples_dir(wchar_t *out, size_t cap) { path_join(out, cap, paths_exe_dir(), L"samples"); }

/* ---------------------------------------------------------------- worker thread */

static int dl_progress(void *ctx, int64_t done, int64_t total)
{
    SamplesJob *j = (SamplesJob *)ctx;
    if (WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0) return 0;
    double now = time_ms();
    if (now - j->lastPost < 250) return 1;
    j->lastPost = now;
    int pct = total > 0 ? (int)SP_MIN(100, done * 100 / total) : -1;
    wchar_t text[SP_COUNTOF(g_state.text)];
    str_printf(text, SP_COUNTOF(text), L"Downloading the sample wallpapers… %d%%", SP_MAX(pct, 0));
    set_status(1, pct, 0, text);
    PostMessageW(g_app.shellWnd, WM_SP_SAMPLES, 0, 0);
    return 1;
}

/* A verified download of a release asset into <data>\updates\<name>. */
static BOOL fetch(SamplesJob *j, const JVal *root, const wchar_t *name, int64_t max, FetchProgress cb, wchar_t *dest, size_t cap)
{
    wchar_t url[SP_MAX_PATH], dir[SP_MAX_PATH], why[SP_ERR_LEN] = L"";
    char sha[65], hex[65] = "";
    int64_t size;
    if (!release_asset(root, j->version, name, max, url, SP_COUNTOF(url), &size, sha)) {
        str_printf(j->error, SP_COUNTOF(j->error), L"Screen+ %ls has no %ls this copy can verify", j->version, name);
        return FALSE;
    }
    path_join(dir, SP_COUNTOF(dir), paths_root(), L"updates");
    dir_create_recursive(dir);
    path_join(dest, cap, dir, name);
    HttpGet *g = http_open(url, 0, why, SP_COUNTOF(why));
    BOOL ok = g && http_status(g) == 200;
    if (g && !ok) str_printf(why, SP_COUNTOF(why), L"the server answered HTTP %d", http_status(g));
    if (ok) ok = http_save(g, dest, size, cb, j, hex, why, SP_COUNTOF(why));
    http_close(g);
    if (ok && strcmp(hex, sha) != 0) {
        str_copy(why, SP_COUNTOF(why), L"the file did not match the SHA-256 checksum the release lists for it");
        DeleteFileW(dest);
        ok = FALSE;
    }
    if (!ok) str_printf(j->error, SP_COUNTOF(j->error), L"Could not download %ls: %ls", name, why);
    return ok;
}

/* The release's files that differ here, as zip member names ("abqqpcp4.hv3/js/script.js"), and their folders. */
typedef struct Diff {
    StrBuf  members;                            /* quoted, space separated, for tar */
    int     count;
    wchar_t folders[MAX_FOLDERS][64];
    int     nFolders;
} Diff;

static void compare(SamplesJob *j, const JVal *files, Diff *d)
{
    wchar_t root[SP_MAX_PATH], path[SP_MAX_PATH], folder[64], seen[MAX_FOLDERS][64];
    int nSeen = 0;
    samples_dir(root, SP_COUNTOF(root));
    for (int i = 0; files && files->t == J_OBJ && i < files->n; i++) {
        if (!files->keys[i] || WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0) continue;
        wchar_t *rel = wide_from_utf8(files->keys[i], -1);
        if (!rel || wcsstr(rel, L"..") || rel[0] == L'/' || wcschr(rel, L':')) { free(rel); continue; }
        const wchar_t *slash = wcschr(rel, L'/');
        if (!slash || slash - rel >= (ptrdiff_t)SP_COUNTOF(folder)) { free(rel); continue; }
        wcsncpy_s(folder, SP_COUNTOF(folder), rel, (size_t)(slash - rel));
        path_join(path, SP_COUNTOF(path), root, folder);
        if (!dir_exists(path)) { free(rel); continue; }   /* never downloaded: not ours to add */
        int known = 0;
        for (int k = 0; k < nSeen && !known; k++) known = str_ieq(seen[k], folder);
        if (!known && nSeen < MAX_FOLDERS) str_copy(seen[nSeen++], SP_COUNTOF(seen[0]), folder);
        path_join(path, SP_COUNTOF(path), root, rel);
        for (wchar_t *c = path; *c; c++) if (*c == L'/') *c = L'\\';
        char have[65] = "";
        const char *want = files->kids[i].t == J_STR && files->kids[i].str ? files->kids[i].str : "";
        if (!file_exists(path) || !file_sha256(path, have) || _stricmp(have, want) != 0) {
            d->count++;
            sb_printf(&d->members, " \"%s\"", files->keys[i]);
            int dup = 0;
            for (int k = 0; k < d->nFolders && !dup; k++) dup = str_ieq(d->folders[k], folder);
            if (!dup && d->nFolders < MAX_FOLDERS) str_copy(d->folders[d->nFolders++], SP_COUNTOF(d->folders[0]), folder);
        }
        free(rel);
    }
    j->present = nSeen;
}

/* "Rain, Aurora": each changed sample's Lively title, else its folder name. */
static void name_folders(SamplesJob *j, const Diff *d)
{
    wchar_t root[SP_MAX_PATH], info[SP_MAX_PATH], title[80];
    samples_dir(root, SP_COUNTOF(root));
    j->titles[0] = 0;
    for (int i = 0; i < d->nFolders; i++) {
        path_join(info, SP_COUNTOF(info), root, d->folders[i]);
        path_join(info, SP_COUNTOF(info), info, L"LivelyInfo.json");
        JVal *v = file_exists(info) ? json_parse_file(info) : NULL;
        json_str_w(v ? json_geti(v, "Title") : NULL, title, SP_COUNTOF(title));
        json_free(v);
        if (i) str_cat(j->titles, SP_COUNTOF(j->titles), L", ");
        str_cat(j->titles, SP_COUNTOF(j->titles), title[0] ? title : d->folders[i]);
    }
}

/* tar.exe unpacks only the changed files (or, when that list is too long, the changed folders). */
static BOOL unpack(SamplesJob *j, const wchar_t *zip, const Diff *d)
{
    wchar_t dir[SP_MAX_PATH], tar[SP_MAX_PATH];
    samples_dir(dir, SP_COUNTOF(dir));
    GetSystemDirectoryW(tar, SP_COUNTOF(tar));
    path_join(tar, SP_COUNTOF(tar), tar, L"tar.exe");
    StrBuf b = { 0 };
    char *t = utf8_from_wide(tar, NULL), *z = utf8_from_wide(zip, NULL), *o = utf8_from_wide(dir, NULL);
    sb_printf(&b, "\"%s\" -xf \"%s\" -C \"%s\"", t ? t : "", z ? z : "", o ? o : "");
    free(t); free(z); free(o);
    if (b.len + d->members.len < TAR_CMD_MAX) sb_puts(&b, d->members.p ? d->members.p : "");
    else for (int i = 0; i < d->nFolders; i++) {
        char *f = utf8_from_wide(d->folders[i], NULL);
        sb_printf(&b, " \"%s\"", f ? f : "");
        free(f);
    }
    wchar_t *cmd = b.failed ? NULL : wide_from_utf8(b.p, -1);
    sb_free(&b);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (cmd && CreateProcessW(tar, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 300000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(cmd);
    if (code != 0)
        str_copy(j->error, SP_COUNTOF(j->error), L"The sample wallpapers could not be unpacked (a file may be in use; try again)");
    return code == 0;
}

static void mark_current(const char *id)
{
    wchar_t dir[SP_MAX_PATH], p[SP_MAX_PATH];
    samples_dir(dir, SP_COUNTOF(dir));
    path_join(p, SP_COUNTOF(p), dir, L"samples.id");
    FILE *f = _wfopen(p, L"wb");
    if (f) {
        fputs(id, f);
        fclose(f);
    }
    path_join(p, SP_COUNTOF(p), dir, L"download.sha256");  /* 1.9.4 and older: the zip's hash */
    DeleteFileW(p);
}

static void run(SamplesJob *j)
{
    wchar_t name[96], list[SP_MAX_PATH], zip[SP_MAX_PATH];
    int v[3];
    JVal *root = release_latest(j->version, SP_COUNTOF(j->version), v, j->error, SP_COUNTOF(j->error));
    if (!root) return;
    str_printf(name, SP_COUNTOF(name), L"ScreenPlus-%ls-samples.json", j->version);
    JVal *man = NULL;
    wchar_t url[SP_MAX_PATH];
    char sha[65];
    int64_t size;
    if (!release_asset(root, j->version, name, LIST_MAX, url, SP_COUNTOF(url), &size, sha)) {
        j->ok = j->noList = 1;                     /* released before the list existed: nothing to compare */
        json_free(root);
        return;
    }
    if (fetch(j, root, name, LIST_MAX, NULL, list, SP_COUNTOF(list))) {
        man = json_parse_file(list);
        DeleteFileW(list);
        if (!man) str_printf(j->error, SP_COUNTOF(j->error), L"%ls could not be read", name);
    }
    if (!man) { json_free(root); return; }
    Diff d = { 0 };
    compare(j, json_get(man, "files"), &d);
    j->changed = d.nFolders;
    name_folders(j, &d);
    j->ok = 1;
    if (d.count && j->install) {
        str_printf(name, SP_COUNTOF(name), L"ScreenPlus-%ls-samples.zip", j->version);
        j->ok = fetch(j, root, name, ZIP_MAX, dl_progress, zip, SP_COUNTOF(zip)) && unpack(j, zip, &d);
        DeleteFileW(zip);
        if (j->ok) {
            Diff after = { 0 };
            compare(j, json_get(man, "files"), &after);   /* what tar could not replace */
            j->ok = after.count == 0;
            if (!j->ok) str_copy(j->error, SP_COUNTOF(j->error), L"Some sample files could not be replaced (in use?); try again");
            sb_free(&after.members);
        }
        j->installed = j->ok;
    }
    if (j->ok && j->present && (!d.count || j->installed)) {   /* none downloaded: setup must not think so */
        const JVal *id = json_get(man, "id");
        if (id && id->t == J_STR && id->str) mark_current(id->str);
    }
    sb_free(&d.members);
    json_free(man);
    json_free(root);
}

static DWORD WINAPI worker(LPVOID p)
{
    SamplesJob *j = (SamplesJob *)p;
    run(j);
    PostMessageW(g_app.shellWnd, WM_SP_SAMPLES, 1, (LPARAM)j);
    return 0;
}

/* ---------------------------------------------------------------- shell thread */

static BOOL start(BOOL install, BOOL user)
{
    if (g_job) return FALSE;
    SamplesJob *j = (SamplesJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    j->install = install;
    j->user = user;
    j->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    set_status(1, -1, 0, install ? L"Updating the sample wallpapers…" : L"Checking the sample wallpapers…");
    g_job = j;
    g_thread = j->cancel ? CreateThread(NULL, 0, worker, j, 0, NULL) : NULL;
    if (!g_thread) {
        g_job = NULL;
        if (j->cancel) CloseHandle(j->cancel);
        free(j);
        set_status(0, -1, 0, L"");
        return FALSE;
    }
    ui_on_status();
    return TRUE;
}

void samples_init(void) { samples_schedule(FALSE); }

void samples_schedule(BOOL soon)
{
    if (!g_app.shellWnd) return;
    if (!g_app.cfg.s.autoUpdateSamples) {
        KillTimer(g_app.shellWnd, T_SHELL_SAMPLES);
        return;
    }
    SetTimer(g_app.shellWnd, T_SHELL_SAMPLES, soon ? SOON_MS : FIRST_MS, NULL);
}

BOOL samples_check(BOOL install) { return start(install, TRUE); }

void samples_on_timer(void)
{
    KillTimer(g_app.shellWnd, T_SHELL_SAMPLES);
    if (!g_app.cfg.s.autoUpdateSamples) return;
    if (!start(TRUE, FALSE)) SetTimer(g_app.shellWnd, T_SHELL_SAMPLES, RETRY_MS, NULL);
}

void samples_get(SamplesState *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!out->text[0])
        str_copy(out->text, SP_COUNTOF(out->text), g_app.cfg.s.autoUpdateSamples ? L"Updated automatically" : L"Checked when you press Check now");
}

void samples_on_event(WPARAM wp, LPARAM lp)
{
    SamplesJob *j = (SamplesJob *)lp;
    if (!wp || !j || j != g_job) {
        ui_on_status();
        return;
    }
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_job = NULL;
    wchar_t text[SP_COUNTOF(g_state.text)];
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (!j->ok) {
        LOGW(L"samples: %ls", j->error);
        set_status(0, -1, j->changed > 0, j->error);
        if (j->user) app_notice(NOTICE_ERROR, L"%ls", j->error);
    } else if (j->noList) {
        LOGI(L"samples: Screen+ %ls lists no samples", j->version);
        str_printf(text, SP_COUNTOF(text), L"Screen+ %ls has no sample list yet · checked at %02u:%02u", j->version, now.wHour, now.wMinute);
        set_status(0, -1, 0, text);
    } else if (!j->present) {
        set_status(0, -1, 0, L"No downloaded sample wallpapers to update");
    } else if (!j->changed) {
        LOGI(L"samples: up to date with Screen+ %ls", j->version);
        str_printf(text, SP_COUNTOF(text), L"Up to date · checked at %02u:%02u", now.wHour, now.wMinute);
        set_status(0, -1, 0, text);
    } else if (!j->installed) {
        LOGI(L"samples: updates for %ls (Screen+ %ls)", j->titles, j->version);
        str_printf(text, SP_COUNTOF(text), L"Updates for %ls · checked at %02u:%02u", j->titles, now.wHour, now.wMinute);
        set_status(0, -1, 1, text);
    } else {
        LOGI(L"samples: updated %ls (Screen+ %ls)", j->titles, j->version);
        str_printf(text, SP_COUNTOF(text), L"Updated %ls at %02u:%02u", j->titles, now.wHour, now.wMinute);
        set_status(0, -1, 0, text);
        app_restart_renderer();                    /* a showing sample picks up its new files */
        if (j->user) app_notice(NOTICE_INFO, L"Updated %ls.", j->titles);
    }
    if (g_app.cfg.s.autoUpdateSamples && !g_app.exiting)
        SetTimer(g_app.shellWnd, T_SHELL_SAMPLES, j->ok ? EVERY_MS : RETRY_MS, NULL);
    CloseHandle(j->cancel);
    free(j);
    ui_on_status();
}

void samples_shutdown(void)
{
    if (!g_job) return;
    SetEvent(g_job->cancel);
    if (g_thread && WaitForSingleObject(g_thread, 5000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        CloseHandle(g_job->cancel);
        free(g_job);
    }
    g_thread = NULL;
    g_job = NULL;
}
