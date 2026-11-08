/* Auto update: the "Auto update" setting (on by default), Settings' Check now and `screenplus update`.
 * The latest release of Screen+ (GitHub, else the GitLab mirror) (never a pre-release) is compared with this build. A newer release's
 * installer is downloaded into <data>\updates and kept only if its size and SHA-256 match what the release lists
 * for that asset; it then runs silently with /UPDATE: setup closes Screen+, updates it in place (library and
 * settings are kept) and starts it again. Only installed copies update (uninstall.exe beside the exe):
 * portable and development builds are never replaced. With the setting on, the first check runs a few
 * minutes after start (the network may not be up at sign-in, and sign-in must not wait for it), then every
 * 12 hours, or an hour after a failed one. A downloaded update waits while the window is open, so nothing
 * closes under the user's hands. The network work runs on a worker thread that reports via WM_SP_UPDATE. */
#include "app.h"
#include "../ui/ui.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../common/json.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <ctype.h>

#ifdef _WIN32
#define SETUP_NAME   L"ScreenPlus-Setup-%ls.exe"
#else
#define SETUP_NAME   L"ScreenPlus-%ls-linux-x86_64.tar.gz"
#endif
#define RELEASE_API  L"https://api.github.com/repos/CoolNinja2009/ScreenPlus/releases/latest"
#define RELEASE_DL   L"https://github.com/CoolNinja2009/ScreenPlus/releases/download/v"
/* The GitLab mirror, asked when GitHub doesn't answer: its assets live in the project's package registry and
 * SHA256SUMS.txt beside them lists every file's SHA-256. */
#define GITLAB_API   L"https://gitlab.com/api/v4/projects/88596566/releases/permalink/latest"
#define GITLAB_DL    L"https://gitlab.com/api/v4/projects/88596566/packages/generic/screenplus/"
#define SUMS_MAX     (64 * 1024)
#define FIRST_MS     (3 * 60 * 1000)
#define EVERY_MS     (12 * 60 * 60 * 1000)
#define RETRY_MS     (60 * 60 * 1000)
#define SOON_MS      3000
#define API_MAX      (1 << 20)                  /* the release's JSON, notes included */
#define SETUP_MAX    ((int64_t)256 << 20)

typedef struct UpdateJob {
    int     install;                            /* download a newer installer (else only report it) */
    int     user;                               /* asked for (button, CLI) rather than the timer */
    HANDLE  cancel;                             /* manual-reset event */
    double  lastPost;
    int     ok;                                 /* the check (and download) worked */
    int     newer;                              /* the latest release is newer than this build */
    wchar_t version[32];                        /* the latest release, "1.8.4" */
    wchar_t setup[SP_MAX_PATH];                 /* its verified installer, "" when not downloaded */
    wchar_t error[SP_ERR_LEN];
} UpdateJob;

static SRWLOCK     g_lock = SRWLOCK_INIT;
static UpdateState g_state = { 0, -1 };
static UpdateJob  *g_job;                       /* running job; freed on the shell thread when it ends */
static HANDLE      g_thread;
static wchar_t     g_ready[SP_MAX_PATH];        /* a verified installer waiting for the window to close */
static wchar_t     g_readyVersion[32];
static int         g_readyUser;

static void set_status(int busy, int percent, const wchar_t *text)
{
    AcquireSRWLockExclusive(&g_lock);
    g_state.busy = busy;
    g_state.percent = percent;
    str_copy(g_state.text, SP_COUNTOF(g_state.text), text);
    ReleaseSRWLockExclusive(&g_lock);
}

static BOOL parse_version(const wchar_t *s, int v[3])
{
    if (*s == L'v' || *s == L'V') s++;
    for (int i = 0; i < 3; i++) {
        if (!iswdigit(*s)) return FALSE;
        wchar_t *end;
        v[i] = (int)wcstol(s, &end, 10);
        s = end;
        if (i < 2 && *s++ != L'.') return FALSE;
    }
    return *s == 0;
}

static int newer_than_this(const int v[3])
{
    int cur[3];
    if (!parse_version(SP_VERSION_W, cur)) return 0;
    for (int i = 0; i < 3; i++)
        if (v[i] != cur[i]) return v[i] > cur[i];
    return 0;
}

static void updates_dir(wchar_t *out, size_t cap) { path_join(out, cap, paths_root(), L"updates"); }

/* Earlier downloads: an update that already ran, or a release that has been replaced since. */
static void sweep(const wchar_t *dir, const wchar_t *keep)
{
    wchar_t pat[SP_MAX_PATH], f[SP_MAX_PATH];
    WIN32_FIND_DATAW fd;
    path_join(pat, SP_COUNTOF(pat), dir, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (keep && str_ieq(fd.cFileName, keep))) continue;
        path_join(f, SP_COUNTOF(f), dir, fd.cFileName);
        DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------------------------------------------------------------- worker thread */

static int dl_progress(void *ctx, int64_t done, int64_t total)
{
    UpdateJob *j = (UpdateJob *)ctx;
    if (WaitForSingleObject(j->cancel, 0) == WAIT_OBJECT_0) return 0;
    double now = time_ms();
    if (now - j->lastPost < 250) return 1;
    j->lastPost = now;
    int pct = total > 0 ? (int)SP_MIN(100, done * 100 / total) : -1;
    wchar_t text[SP_COUNTOF(g_state.text)];
    str_printf(text, SP_COUNTOF(text), L"Downloading Screen+ %ls… %d%%", j->version, SP_MAX(pct, 0));
    set_status(1, pct, text);
    PostMessageW(g_app.shellWnd, WM_SP_UPDATE, 0, 0);
    return 1;
}

/* A small body over HTTPS (at most cap bytes), or NULL with *status set (0: not reached, why says why). */
static char *get_body(const wchar_t *url, int cap, int *n, int *status, wchar_t *why, size_t whyCap)
{
    HttpGet *g = http_open(url, 0, why, whyCap);
    *n = 0;
    *status = g ? http_status(g) : 0;
    if (!g) return NULL;
    int got;
    char *body = *status == 200 ? (char *)malloc((size_t)cap + 1) : NULL;
    while (body && *n < cap && (got = http_read(g, body + *n, cap - *n)) > 0) *n += got;
    http_close(g);
    if (body) body[*n] = 0;
    return body;
}

static JVal *get_json(const wchar_t *url, const wchar_t *site, wchar_t *err, size_t errCap)
{
    wchar_t why[SP_ERR_LEN];
    int n, status;
    char *body = get_body(url, API_MAX, &n, &status, why, SP_COUNTOF(why));
    JVal *root = body ? json_parse(body, (size_t)n) : NULL;
    free(body);
    if (!root) {
        if (!status) str_printf(err, errCap, L"Could not reach %ls: %ls", site, why);
        else if (status != 200) str_printf(err, errCap, L"%ls answered HTTP %d", site, status);
        else str_printf(err, errCap, L"%ls's answer could not be read", site);
    }
    return root;
}

/* The latest release (also samples_update.c): its JSON and version ("1.8.4"), or NULL with err set. GitHub
 * first; when it doesn't answer (the account was hidden in 2026-11), the GitLab mirror. */
JVal *release_latest(wchar_t *version, size_t cap, int v[3], wchar_t *err, size_t errCap)
{
    wchar_t hubErr[SP_ERR_LEN], labErr[SP_ERR_LEN];
    JVal *root = get_json(RELEASE_API, L"GitHub", hubErr, SP_COUNTOF(hubErr));
    if (!root) {
        root = get_json(GITLAB_API, L"GitLab", labErr, SP_COUNTOF(labErr));
        if (!root) {
            str_printf(err, errCap, L"%ls; %ls", hubErr, labErr);
            return NULL;
        }
        LOGI(L"update: %ls; asked GitLab instead", hubErr);
    }
    wchar_t tag[40];
    json_str_w(json_get(root, "tag_name"), tag, SP_COUNTOF(tag));
    if (!parse_version(tag, v)) {
        str_copy(err, errCap, L"The latest release has no version number");
        json_free(root);
        return NULL;
    }
    str_printf(version, cap, L"%d.%d.%d", v[0], v[1], v[2]);
    return root;
}

/* GitLab: the asset's link in the package registry for that very version, and its line in SHA256SUMS.txt.
 * GitLab lists no size, so the download is capped at max and the checksum decides. */
static BOOL gitlab_asset(const JVal *links, const wchar_t *version, const wchar_t *name, int64_t max,
                         wchar_t *url, size_t urlCap, int64_t *size, char sha[65])
{
    wchar_t prefix[160], sums[SP_MAX_PATH] = L"", why[SP_ERR_LEN];
    str_printf(prefix, SP_COUNTOF(prefix), GITLAB_DL L"%ls/", version);
    for (int i = 0; links->t == J_ARR && i < links->n; i++) {
        wchar_t an[96];
        json_str_w(json_get(&links->kids[i], "name"), an, SP_COUNTOF(an));
        if (str_ieq(an, name)) json_str_w(json_get(&links->kids[i], "url"), url, urlCap);
        else if (str_ieq(an, L"SHA256SUMS.txt")) json_str_w(json_get(&links->kids[i], "url"), sums, SP_COUNTOF(sums));
    }
    if (!str_istarts(url, prefix) || !str_istarts(sums, prefix)) return FALSE;
    int n, status;
    char *body = get_body(sums, SUMS_MAX, &n, &status, why, SP_COUNTOF(why));
    if (!body) return FALSE;
    char want[96];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, want, sizeof want, NULL, NULL);
    BOOL ok = FALSE;
    for (char *line = body; line && *line && !ok;) {       /* "<64 hex>  <name>" (or " *<name>") */
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (len > 66 && line[64] == ' ' && (line[65] == ' ' || line[65] == '*') && !_stricmp(line + 66, want)) {
            ok = TRUE;
            for (int k = 0; ok && k < 64; k++) {
                ok = isxdigit((unsigned char)line[k]) != 0;
                sha[k] = (char)tolower((unsigned char)line[k]);
            }
        }
        line = next;
    }
    free(body);
    sha[64] = 0;
    *size = max;
    return ok;
}

/* An asset of that release: its address (this repository's download for that very version), its size (at most
 * max) and the SHA-256 the release lists for it. */
BOOL release_asset(const JVal *root, const wchar_t *version, const wchar_t *name, int64_t max,
                   wchar_t *url, size_t urlCap, int64_t *size, char sha[65])
{
    wchar_t prefix[160], digest[80] = L"";
    const JVal *assets = json_get(root, "assets"), *a = NULL;
    url[0] = 0;
    *size = 0;
    sha[0] = 0;
    if (assets && assets->t == J_OBJ) {                    /* a GitLab release */
        const JVal *links = json_get(assets, "links");
        return links && gitlab_asset(links, version, name, max, url, urlCap, size, sha);
    }
    str_printf(prefix, SP_COUNTOF(prefix), RELEASE_DL L"%ls/", version);
    for (int i = 0; assets && assets->t == J_ARR && i < assets->n && !a; i++) {
        wchar_t an[96];
        json_str_w(json_get(&assets->kids[i], "name"), an, SP_COUNTOF(an));
        if (str_ieq(an, name)) a = &assets->kids[i];
    }
    if (a) {
        json_str_w(json_get(a, "browser_download_url"), url, urlCap);
        json_str_w(json_get(a, "digest"), digest, SP_COUNTOF(digest));
        *size = (int64_t)json_num(json_get(a, "size"), 0);
    }
    BOOL ok = a && str_istarts(url, prefix) && *size > 0 && *size <= max && str_istarts(digest, L"sha256:") &&
              wcslen(digest) == 7 + 64;
    for (int k = 0; ok && k < 64; k++) {
        ok = iswxdigit(digest[7 + k]) != 0;
        sha[k] = (char)towlower(digest[7 + k]);
    }
    sha[64] = 0;
    return ok;
}

/* The latest release: its version and, when it is newer, its installer (address, size, SHA-256). */
static BOOL read_release(UpdateJob *j, wchar_t *url, size_t urlCap, int64_t *size, char sha[65])
{
    int v[3];
    JVal *root = release_latest(j->version, SP_COUNTOF(j->version), v, j->error, SP_COUNTOF(j->error));
    if (!root) return FALSE;
    j->newer = newer_than_this(v);
    BOOL ok = TRUE;
    if (j->newer) {
        wchar_t name[64];
        str_printf(name, SP_COUNTOF(name), SETUP_NAME, j->version);
        ok = release_asset(root, j->version, name, SETUP_MAX, url, urlCap, size, sha);
        if (!ok) str_printf(j->error, SP_COUNTOF(j->error), L"Screen+ %ls has no installer this copy can verify", j->version);
    }
    json_free(root);
    return ok;
}

static void run(UpdateJob *j)
{
    wchar_t url[SP_MAX_PATH] = L"", dir[SP_MAX_PATH], name[64], dest[SP_MAX_PATH], part[SP_MAX_PATH], why[SP_ERR_LEN];
    char sha[65] = "", hex[65] = "";
    int64_t size = 0;
    if (!read_release(j, url, SP_COUNTOF(url), &size, sha)) return;
    updates_dir(dir, SP_COUNTOF(dir));
    str_printf(name, SP_COUNTOF(name), SETUP_NAME, j->version);
    path_join(dest, SP_COUNTOF(dest), dir, name);
    sweep(dir, j->newer ? name : NULL);
    j->ok = 1;
    if (!j->newer || !j->install) return;
    if (file_exists(dest) && file_sha256(dest, hex) && strcmp(hex, sha) == 0) {   /* downloaded before, still waiting */
        str_copy(j->setup, SP_COUNTOF(j->setup), dest);
        return;
    }
    dir_create_recursive(dir);
    str_printf(part, SP_COUNTOF(part), L"%ls.download", dest);
    HttpGet *g = http_open(url, 0, why, SP_COUNTOF(why));
    BOOL ok = g && http_status(g) == 200;
    if (g && !ok) str_printf(why, SP_COUNTOF(why), L"the server answered HTTP %d", http_status(g));
    if (ok) ok = http_save(g, part, size, dl_progress, j, hex, why, SP_COUNTOF(why));
    http_close(g);
    if (ok && strcmp(hex, sha) != 0) {
        str_copy(why, SP_COUNTOF(why), L"the file did not match the SHA-256 checksum the release lists for it");
        ok = FALSE;
    }
    if (ok && !MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        str_printf(why, SP_COUNTOF(why), L"it could not be saved (error %lu)", GetLastError());
        ok = FALSE;
    }
    if (!ok) {
        DeleteFileW(part);
        j->ok = 0;
        str_printf(j->error, SP_COUNTOF(j->error), L"Could not download Screen+ %ls: %ls", j->version, why);
        return;
    }
    str_copy(j->setup, SP_COUNTOF(j->setup), dest);
}

static DWORD WINAPI worker(LPVOID p)
{
    UpdateJob *j = (UpdateJob *)p;
    run(j);
    PostMessageW(g_app.shellWnd, WM_SP_UPDATE, 1, (LPARAM)j);
    return 0;
}

/* ---------------------------------------------------------------- shell thread */

static int can_install(void) { return g_state.supported; }

/* Setup closes this process (--exit), replaces the files and starts the new version (/UPDATE). */
static void run_setup(void)
{
    wchar_t cmd[2 * SP_MAX_PATH + 64], text[SP_COUNTOF(g_state.text)];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
#ifdef _WIN32
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" /S /UPDATE", g_ready);
#else
    /* Under a systemd unit (sign-in starts it as screenplus.service) update.sh would sit in the service's cgroup,
     * and its `screenplus stop` would take it down with the service: it gets a scope of its own. */
    str_printf(cmd, SP_COUNTOF(cmd), L"%ls\"%ls/update.sh\" \"%ls\"",
               getenv("INVOCATION_ID") ? L"systemd-run --user --scope --quiet " : L"", paths_exe_dir(), g_ready);
#endif
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        LOGW(L"update: could not start the installer (err %lu)", GetLastError());
        set_status(0, -1, L"The update could not start");
    } else {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOGI(L"update: installing Screen+ %ls; setup closes Screen+ and starts the new version", g_readyVersion);
        str_printf(text, SP_COUNTOF(text), L"Installing Screen+ %ls…", g_readyVersion);
        set_status(1, -1, text);
    }
    g_ready[0] = 0;
    ui_on_status();
}

static void try_install(void)
{
    wchar_t text[SP_COUNTOF(g_state.text)];
    if (ui_is_open()) {
        str_printf(text, SP_COUNTOF(text), L"Screen+ %ls installs when this window closes", g_readyVersion);
        set_status(0, -1, text);
        return;
    }
    run_setup();
}

static BOOL start(BOOL install, BOOL user)
{
    if (g_job) return FALSE;
    UpdateJob *j = (UpdateJob *)calloc(1, sizeof *j);
    if (!j) return FALSE;
    j->install = install && can_install();
    j->user = user;
    j->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    set_status(1, -1, L"Checking for updates…");
    g_job = j;
    g_thread = j->cancel ? CreateThread(NULL, 0, worker, j, 0, NULL) : NULL;
    if (!g_thread) {
        g_job = NULL;
        if (j->cancel) CloseHandle(j->cancel);
        free(j);
        set_status(0, -1, L"");
        return FALSE;
    }
    ui_on_status();
    return TRUE;
}

void update_init(void)
{
    wchar_t dir[SP_MAX_PATH], un[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), paths_exe());
#ifdef _WIN32
    path_join(un, SP_COUNTOF(un), dir, L"uninstall.exe");
#else
    path_join(un, SP_COUNTOF(un), dir, L"update.sh");     /* install.sh's copy, not a build folder */
#endif
    g_state.supported = file_exists(un);
    update_schedule(FALSE);
}

void update_schedule(BOOL soon)
{
    if (!g_app.shellWnd) return;
    if (!g_job) set_status(0, -1, L"");           /* the setting's own description until the next check */
    if (!g_app.cfg.s.autoUpdate || !g_state.supported) {
        KillTimer(g_app.shellWnd, T_SHELL_UPDATE);
        return;
    }
    SetTimer(g_app.shellWnd, T_SHELL_UPDATE, soon ? SOON_MS : FIRST_MS, NULL);
}

BOOL update_check(BOOL install)
{
    return start(install, TRUE);
}

void update_on_timer(void)
{
    KillTimer(g_app.shellWnd, T_SHELL_UPDATE);
    if (!g_app.cfg.s.autoUpdate || !g_state.supported) return;
    if (g_ready[0]) try_install();                  /* already downloaded: still waiting for the window */
    else if (!start(TRUE, FALSE)) SetTimer(g_app.shellWnd, T_SHELL_UPDATE, RETRY_MS, NULL);
}

void update_get(UpdateState *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (out->text[0]) return;
    if (!out->supported)
        str_copy(out->text, SP_COUNTOF(out->text), L"Portable copy: updates are not installed automatically");
    else if (g_app.cfg.s.autoUpdate)
        str_copy(out->text, SP_COUNTOF(out->text), L"On");
    else
        str_copy(out->text, SP_COUNTOF(out->text), L"Off");
}

void update_on_event(WPARAM wp, LPARAM lp)
{
    UpdateJob *j = (UpdateJob *)lp;
    if (!wp || !j || j != g_job) {                 /* progress: repaint */
        ui_on_status();
        return;
    }
    WaitForSingleObject(g_thread, INFINITE);       /* returns right after posting */
    CloseHandle(g_thread);
    g_thread = NULL;
    g_job = NULL;
    wchar_t text[SP_COUNTOF(g_state.text)];
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (!j->ok) {
        LOGW(L"update: %ls", j->error);
        set_status(0, -1, j->error);
        if (j->user) app_notice(NOTICE_ERROR, L"%ls", j->error);
    } else if (!j->newer) {
        LOGI(L"update: Screen+ %ls is up to date (latest release %ls)", SP_VERSION_W, j->version);
        str_printf(text, SP_COUNTOF(text), L"Up to date · checked at %02u:%02u", now.wHour, now.wMinute);
        set_status(0, -1, text);
    } else if (!j->setup[0] || (!j->user && !g_app.cfg.s.autoUpdate)) {
        LOGI(L"update: Screen+ %ls is available", j->version);
        str_printf(text, SP_COUNTOF(text), can_install() ? L"Screen+ %ls is available"
                                                         : L"Screen+ %ls is available", j->version);
        set_status(0, -1, text);
    } else {
        LOGI(L"update: Screen+ %ls downloaded and verified", j->version);
        str_copy(g_ready, SP_COUNTOF(g_ready), j->setup);
        str_copy(g_readyVersion, SP_COUNTOF(g_readyVersion), j->version);
        g_readyUser = j->user;
        try_install();
    }
    if (g_app.cfg.s.autoUpdate && g_state.supported && !g_app.exiting)
        SetTimer(g_app.shellWnd, T_SHELL_UPDATE, j->ok ? EVERY_MS : RETRY_MS, NULL);
    CloseHandle(j->cancel);
    free(j);
    ui_on_status();
}

void update_on_ui_closed(void)
{
    if (g_ready[0] && !g_app.exiting && (g_readyUser || g_app.cfg.s.autoUpdate)) run_setup();
}

void update_shutdown(void)
{
    if (!g_job) return;
    SetEvent(g_job->cancel);
    if (g_thread && WaitForSingleObject(g_thread, 5000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        CloseHandle(g_job->cancel);
        free(g_job);
    }
    g_thread = NULL;                               /* still running: process exit ends it */
    g_job = NULL;
}
