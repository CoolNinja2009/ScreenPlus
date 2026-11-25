/* live.c: the Screen+ window on Bare, fed by the running service.
 *
 * A worker thread sends commands straight to the service (sp_ipc.c) and keeps
 * the data fresh; the window thread only draws. The window is frameless and
 * see-through: its background is the live desktop behind it, which the glass
 * bends. The service stays the only owner of the config and of playback. */
#include "screenplus_bare.h"
#include "sp_ipc.h"
#include "sp_kit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
typedef SRWLOCK Lock;
#define lock_init(l)   InitializeSRWLock(l)
#define lock(l)        AcquireSRWLockExclusive(l)
#define unlock(l)      ReleaseSRWLockExclusive(l)
typedef CONDITION_VARIABLE Cond;
#define cond_init(c)   InitializeConditionVariable(c)
#define cond_signal(c) WakeConditionVariable(c)
#else
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
typedef pthread_mutex_t Lock;
#define lock_init(l)   pthread_mutex_init((l), NULL)
#define lock(l)        pthread_mutex_lock(l)
#define unlock(l)      pthread_mutex_unlock(l)
typedef pthread_cond_t Cond;
#define cond_init(c)   pthread_cond_init((c), NULL)
#define cond_signal(c) pthread_cond_signal(c)
#endif

enum { DOC_STATUS, DOC_SETTINGS, DOC_LIST, DOC_LOOK, DOC_STORE, DOC_COUNT };
#define MAX_JOBS 32
#define MAX_ARGS 12

typedef struct Job { int argc; char *argv[MAX_ARGS]; } Job;

typedef struct Shared {
    Lock lock;
    Cond wake;
    Job jobs[MAX_JOBS];
    int job_head, job_count;
    JVal *fresh[DOC_COUNT];   /* new answers the window hasn't taken yet */
    bool have[DOC_COUNT];
    bool connected;
    float cpu;
    char message[512];
    bool message_new, message_failed;
    int page;                 /* the page on screen (the Store is fetched only while it's shown) */
    bool store_asked;
    bool quit;
} Shared;

static Shared g;

/* ---- the worker ------------------------------------------------------------ */

static void wait_for_work(int ms) {
#ifdef _WIN32
    SleepConditionVariableSRW(&g.wake, &g.lock, (DWORD)ms, 0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&g.wake, &g.lock, &ts);
#endif
}

/* Answers' "data", detached from the reply so the rest can be freed. */
static JVal *take_data(JVal *reply) {
    if (!reply) return NULL;
    JVal *data = NULL;
    for (int i = 0; reply->t == J_OBJ && i < reply->n; i++)
        if (!strcmp(reply->keys[i], "data")) {
            data = malloc(sizeof *data);
            if (data) { *data = reply->kids[i]; reply->kids[i] = (JVal){ 0 }; }
        }
    json_free(reply);
    return data;
}

static void publish(int slot, JVal *doc) {
    lock(&g.lock);
    json_free(g.fresh[slot]);
    g.fresh[slot] = doc;
    g.have[slot] = true;
    unlock(&g.lock);
}

#ifdef _WIN32
static float renderer_cpu(DWORD pid) {
    static DWORD last_pid;
    static ULONGLONG last_busy, last_wall;
    if (!pid) return -1;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return -1;
    FILETIME c, e, k, u, now;
    float result = -1;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        GetSystemTimeAsFileTime(&now);
        ULONGLONG busy = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        ULONGLONG wall = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (pid == last_pid && wall > last_wall)
            result = (float)(100.0 * (double)(busy - last_busy) / (double)(wall - last_wall) / (double)si.dwNumberOfProcessors);
        last_pid = pid; last_busy = busy; last_wall = wall;
    }
    CloseHandle(h);
    return result;
}
#endif

static void refresh(bool with_store) {
    static const char *status[] = { "status", NULL }, *settings[] = { "settings", NULL }, *list[] = { "list", NULL },
                      *look[] = { "look", NULL }, *store[] = { "store", "--previews", NULL };
    JVal *st = take_data(sp_request(status));
    bool up = st != NULL;
    float cpu = -1;
#ifdef _WIN32
    if (st) {
        float now = renderer_cpu((DWORD)json_num(json_get(json_get(st, "renderer"), "pid"), 0));
        static float smooth = -1;
        if (now >= 0) smooth = smooth < 0 ? now : smooth * 0.5f + now * 0.5f;
        cpu = smooth;
    }
#endif
    publish(DOC_STATUS, st);
    if (up) {
        publish(DOC_SETTINGS, take_data(sp_request(settings)));
        publish(DOC_LIST, take_data(sp_request(list)));
        publish(DOC_LOOK, take_data(sp_request(look)));
        if (with_store) publish(DOC_STORE, take_data(sp_request(store)));
    }
    lock(&g.lock);
    g.connected = up;
    g.cpu = cpu;
    unlock(&g.lock);
    bare_wake();
}

static void run_job(Job *job) {
    JVal *reply = sp_request((const char *const *)job->argv);
    const char *text = reply ? json_str(json_get(reply, "text"), "") : "Screen+ isn't running";
    bool ok = reply && json_bool(json_get(reply, "ok"), 0);
    bool quiet = !strcmp(job->argv[0], "preview");   /* asked by the library, not by you */
    lock(&g.lock);
    size_t n = strcspn(text, "\r\n");
    if (n >= sizeof g.message) n = sizeof g.message - 1;
    memcpy(g.message, text, n);
    g.message[n] = 0;
    if (!n) snprintf(g.message, sizeof g.message, "%s", ok ? "Done" : "That didn't work");
    g.message_new = !quiet;
    g.message_failed = !ok;
    unlock(&g.lock);
    json_free(reply);
    for (int i = 0; i < job->argc; i++) free(job->argv[i]);
    bare_wake();
}

/* Seconds on a steady clock (the worker can't use bare_time). */
static double clock_now(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

#ifdef _WIN32
static DWORD WINAPI worker(void *arg) {
#else
static void *worker(void *arg) {
#endif
    (void)arg;
    double next = 0;
#ifdef _WIN32
    /* Opened on its own (not by Screen+): start the service beside it, in the tray. */
    if (!getenv("BARE_CAPTURE") && !sp_service_running()) {   /* a capture is a test: never start one */
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
        if (slash && (size_t)(slash - exe) + 16 < MAX_PATH) {
            wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - exe), L"ScreenPlus.exe");
            _snwprintf_s(cmd, MAX_PATH + 16, _TRUNCATE, L"\"%ls\" --tray", exe);
            STARTUPINFOW si = { sizeof si };
            PROCESS_INFORMATION pi;
            if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES &&
                CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                for (int i = 0; i < 50 && !sp_service_running(); i++) Sleep(100);
            }
        }
    }
#endif
    for (;;) {
        lock(&g.lock);
        if (g.quit) { unlock(&g.lock); break; }
        Job job = { 0 };
        bool have_job = g.job_count > 0;
        if (have_job) {
            job = g.jobs[g.job_head];
            g.job_head = (g.job_head + 1) % MAX_JOBS;
            g.job_count--;
        }
        bool store = g.page == SP_STORE;
        bool asked = g.store_asked;
        g.store_asked = false;
        unlock(&g.lock);
        if (have_job) {
            run_job(&job);
            /* quick follow-up so the result shows; more jobs first if queued */
            next = 0;
            continue;
        }
        if (clock_now() >= next || asked) {
            refresh(store);
            next = clock_now() + (store ? 1.0 : 2.0);
        }
        lock(&g.lock);
        if (!g.quit && !g.job_count && !g.store_asked) {
            int ms = (int)((next - clock_now()) * 1000);
            wait_for_work(ms < 10 ? 10 : ms);
        }
        unlock(&g.lock);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- the window's side -------------------------------------------------------- */

typedef struct Live {
    SpView view;
    SpModel model;
    JVal *docs[DOC_COUNT];
    char message[512];
    char theme_key[64];
} Live;

static void enqueue(void *user, const char *const *argv) {
    (void)user;
    Job job = { 0 };
    for (int i = 0; argv[i] && job.argc < MAX_ARGS - 1; i++) {
        size_t n = strlen(argv[i]);
        job.argv[job.argc] = malloc(n + 1);
        if (!job.argv[job.argc]) break;
        memcpy(job.argv[job.argc++], argv[i], n + 1);
    }
    lock(&g.lock);
    if (g.job_count < MAX_JOBS) {
        g.jobs[(g.job_head + g.job_count) % MAX_JOBS] = job;
        g.job_count++;
        job.argc = 0;
    }
    cond_signal(&g.wake);
    unlock(&g.lock);
    for (int i = 0; i < job.argc; i++) free(job.argv[i]);
}

static void open_path(void *user, const char *path) {
    (void)user;
    if (!path || !*path) return;
#ifdef _WIN32
    wchar_t w[2048], args[2100];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) return;
    _snwprintf_s(args, 2100, _TRUNCATE, L"/select,\"%ls\"", w);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
#else
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (!fork()) { execlp("xdg-open", "xdg-open", dir, (char *)NULL); _exit(127); }
#endif
}

static bool system_dark(void) {
#ifdef _WIN32
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS)
        return v == 0;
#endif
    return true;
}

static void apply_theme(Live *app) {
    const char *theme = sp_setting(&app->model, "theme", "system");
    const char *accent = sp_setting(&app->model, "accent_color", "#00D705");
    const char *motion = sp_setting(&app->model, "ui_motion", "full");
    bool dark = !strcmp(theme, "dark") || (strcmp(theme, "light") && system_dark());
    char key[64];
    snprintf(key, sizeof key, "%d|%s|%s", dark, accent, motion);
    if (!strcmp(key, app->theme_key)) return;
    snprintf(app->theme_key, sizeof app->theme_key, "%s", key);
    unsigned rgb = 0x00D705;
    if (accent[0] == '#') rgb = (unsigned)strtoul(accent + 1, NULL, 16);
    sp_theme_set(dark, BARE_RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255),
                 !strcmp(motion, "off") ? 2 : !strcmp(motion, "reduced") ? 1 : 0);
}

/* New answers from the worker: swap them in before the frame is drawn. */
static void take_updates(Live *app) {
    lock(&g.lock);
    for (int i = 0; i < DOC_COUNT; i++)
        if (g.have[i]) {
            json_free(app->docs[i]);
            app->docs[i] = g.fresh[i];
            g.fresh[i] = NULL;
            g.have[i] = false;
        }
    app->model.connected = g.connected;
    app->model.cpu = g.cpu;
    if (g.message_new) {
        snprintf(app->message, sizeof app->message, "%s", g.message);
        app->model.message = app->message;
        app->model.message_time = bare_time();
        app->model.message_failed = g.message_failed;
        g.message_new = false;
    }
    app->model.busy = g.job_count > 0;
    if (g.page != (int)app->view.page) {
        if (app->view.page == SP_STORE) { g.store_asked = true; cond_signal(&g.wake); }
        g.page = (int)app->view.page;
    }
    unlock(&g.lock);
    app->model.status = app->docs[DOC_STATUS];
    app->model.settings = app->docs[DOC_SETTINGS];
    app->model.list = app->docs[DOC_LIST];
    app->model.look = app->docs[DOC_LOOK];
    app->model.store = app->docs[DOC_STORE];
}

static void frame(void *user) {
    Live *app = user;
    take_updates(app);
    apply_theme(app);
    app->model.backdrop = bare_window_backdrop();
    bare_background(sp.dark ? BARE_RGB(14, 22, 24) : BARE_RGB(214, 224, 222));
    sp_draw(&app->view, &app->model);
}

#ifdef _WIN32
/* One window: a second launch brings the first forward. */
static bool already_open(void) {
    CreateMutexW(NULL, TRUE, L"Local\\ScreenPlus.BareUi");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    HWND w = FindWindowW(NULL, L"Screen+ ");
    if (w) {
        ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(w);
    }
    return true;
}
#else
/* One window: a lock held while it's open (Wayland lets no one else raise it). */
static bool already_open(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/screenplus-bare-ui.lock", dir && *dir ? dir : "/tmp");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) && errno == EWOULDBLOCK;   /* fd stays open: the lock lasts */
}
#endif

/* Where the service keeps pages' recorded previews (src/common/paths.c), for one too old to say. */
static void previews_dir(char *out, size_t size) {
    out[0] = 0;
#ifdef _WIN32
    const wchar_t *over = _wgetenv(L"SCREENPLUS_DATA_DIR"), *local = _wgetenv(L"LOCALAPPDATA");
    wchar_t dir[1024];
    if (over && *over) swprintf(dir, 1024, L"%ls\\cache\\previews", over);
    else if (local && *local) swprintf(dir, 1024, L"%ls\\ScreenPlus\\cache\\previews", local);
    else return;
    if (!WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, (int)size, NULL, NULL)) out[0] = 0;
#else
    const char *over = getenv("SCREENPLUS_DATA_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (over && *over) snprintf(out, size, "%s/cache/previews", over);
    else if (xdg && *xdg) snprintf(out, size, "%s/screenplus/cache/previews", xdg);
    else if (home && *home) snprintf(out, size, "%s/.local/share/screenplus/cache/previews", home);
#endif
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!getenv("BARE_CAPTURE") && already_open()) return 0;
    static Live app;
    lock_init(&g.lock);
    cond_init(&g.wake);
    g.page = SP_LIBRARY;
    app.model.user = &app;
    app.model.run = enqueue;
    app.model.open_path = open_path;
    app.model.cpu = -1;
    static char previews[1024];
    previews_dir(previews, sizeof previews);
    app.model.previews = previews;
    sp_theme_set(system_dark(), BARE_RGB(0, 215, 5), 0);
    /* The title ends in a space so a second launch finds this window and not the classic one. */
    /* the classic window's size (1180 x 760), the layout drawn at 80% */
    if (!bare_window_with(.width = 1475, .height = 950, .zoom = 0.8f, .title = "Screen+ ", .frameless = true,
                          .min_width = 1020, .min_height = 660))
        return 1;
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
#endif
    /* bare_run, not a bare_frame loop: it keeps drawing while the window is dragged,
     * so the glass follows what's under it. */
    bare_run(frame, &app);
    lock(&g.lock);
    g.quit = true;
    cond_signal(&g.wake);
    unlock(&g.lock);
#ifdef _WIN32
    /* A request can wait on the service for a while; don't hold the exit for it. */
    WaitForSingleObject(thread, 1500);
#else
    pthread_join(thread, NULL);
#endif
    bare_quit();
    return 0;
}
