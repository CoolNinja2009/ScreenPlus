/* App wallpapers: a program (.exe) draws the wallpaper in its own window, which lives inside the slot's host window.
 *
 * Launch: `<exe> --screenplus-parent <hwnd> --screenplus-size <w>x<h>` (also SCREENPLUS_PARENT in the environment),
 * working folder = the exe's, stdin = a pipe from Screen+. A program that knows Screen+ creates its window as a
 * WS_CHILD of that hwnd; any other program's first top-level window (from any process of its job) is made a child
 * and stretched over the host. Either way the program must show a window within 15 s.
 *
 * Screen+ -> program, stdin, one UTF-8 JSON object per line:
 *   {"type":"property","name":"speed","value":1.5}   every value at start, then each change (Customise; the
 *                                                     controls come from LivelyProperties.json beside the exe)
 *   {"type":"pause","value":true}                    covered / paused: stop drawing; false: carry on
 *   {"type":"volume","value":0..100}                 0: silent
 * Mouse: the desktop's mouse is posted to the window under the pointer as ordinary WM_MOUSEMOVE / WM_xBUTTONDOWN /
 * UP / WM_MOUSEWHEEL / WM_MOUSELEAVE (client coordinates), like the HTML renderer does for pages.
 *
 * The program runs in a job object that dies with the renderer. Writes to stdin go through a small writer thread so
 * a program that never reads stdin can't block the engine. Exit before or after showing a window: REV_FAILED. */
#include "../renderer.h"
#include "../html/wallpaper_api.h"
#include "../../common/util.h"
#include "../../common/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_APP_FIND   0x5A01   /* looking for the program's window (startup only) */
#define APP_FIND_MS  100
#define APP_WAIT_MS  15000
#define APP_QUEUE_CAP (1 << 20)

typedef struct AppRenderer {
    Renderer           base;
    HWND               host, notify, child;
    WallpaperEntry     wp;
    int                w, h, volume, paused, ready, found;
    HANDLE             job, proc, exitWait;
    DWORD              pid;
    ULONGLONG          startedAt;
    JVal              *sent;           /* property values the program has */
    /* stdin writer */
    HANDLE             pipe, thread;
    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE cv;
    char              *q;
    size_t             qlen, qcap;
    int                quit;
} AppRenderer;

/* ------------------------------------------------------------------ stdin */

static DWORD WINAPI writer_main(void *arg)
{
    AppRenderer *r = (AppRenderer *)arg;
    for (;;) {
        EnterCriticalSection(&r->lock);
        while (!r->qlen && !r->quit) SleepConditionVariableCS(&r->cv, &r->lock, INFINITE);
        if (r->quit) { LeaveCriticalSection(&r->lock); return 0; }
        char *buf = r->q;
        size_t n = r->qlen;
        r->q = NULL, r->qlen = r->qcap = 0;
        LeaveCriticalSection(&r->lock);
        size_t off = 0;
        while (off < n) {
            DWORD wrote = 0;
            if (!WriteFile(r->pipe, buf + off, (DWORD)SP_MIN(n - off, (size_t)65536), &wrote, NULL) || !wrote) break;
            off += wrote;
        }
        free(buf);
        if (off < n) return 0;   /* the program closed stdin (or exited) */
    }
}

static void send_text(AppRenderer *r, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || !r->pipe) return;
    EnterCriticalSection(&r->lock);
    if (r->qlen + n > APP_QUEUE_CAP) {   /* the program doesn't read stdin: drop rather than grow */
        LeaveCriticalSection(&r->lock);
        return;
    }
    if (r->qlen + n > r->qcap) {
        size_t cap = SP_MAX(r->qcap * 2, r->qlen + n + 256);
        char *p = (char *)realloc(r->q, cap);
        if (!p) { LeaveCriticalSection(&r->lock); return; }
        r->q = p, r->qcap = cap;
    }
    memcpy(r->q + r->qlen, s, n);
    r->qlen += n;
    LeaveCriticalSection(&r->lock);
    WakeConditionVariable(&r->cv);
}

static void send_props(AppRenderer *r)
{
    char *lines = wpapi_properties_lines(&r->wp, &r->sent);
    send_text(r, lines);
    free(lines);
}

static void send_state(AppRenderer *r)
{
    char line[96];
    snprintf(line, sizeof line, "{\"type\":\"pause\",\"value\":%s}\n{\"type\":\"volume\",\"value\":%d}\n",
             r->paused > 0 ? "true" : "false", r->volume);
    send_text(r, line);
}

/* ------------------------------------------------------------------ the program's window */

typedef struct FindCtx { AppRenderer *r; DWORD pids[64]; int n; HWND hit; } FindCtx;

static int pid_in(const FindCtx *f, DWORD pid)
{
    for (int i = 0; i < f->n; i++) if (f->pids[i] == pid) return 1;
    return 0;
}

static BOOL CALLBACK find_top(HWND hwnd, LPARAM lp)
{
    FindCtx *f = (FindCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid_in(f, pid) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, SP_COUNTOF(cls));
    if (str_ieq(cls, L"IME") || str_ieq(cls, L"MSCTFIME UI") || str_ieq(cls, L"ConsoleWindowClass")) return TRUE;
    LONG st = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (!IsWindowVisible(hwnd) && !(st & (WS_CAPTION | WS_POPUP))) return TRUE;
    RECT rc;
    if (!GetWindowRect(hwnd, &rc) || rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return TRUE;
    f->hit = hwnd;
    return FALSE;
}

static void job_pids(AppRenderer *r, FindCtx *f)
{
    struct { JOBOBJECT_BASIC_PROCESS_ID_LIST l; ULONG_PTR more[63]; } ids;
    ZeroMemory(&ids, sizeof ids);
    f->n = 0;
    if (r->job && QueryInformationJobObject(r->job, JobObjectBasicProcessIdList, &ids, sizeof ids, NULL))
        for (DWORD i = 0; i < ids.l.NumberOfProcessIdsInList && f->n < 64; i++) f->pids[f->n++] = (DWORD)ids.l.ProcessIdList[i];
    if (!f->n) f->pids[f->n++] = r->pid;
}

static void fit_child(AppRenderer *r)
{
    if (r->child && IsWindow(r->child))
        SetWindowPos(r->child, NULL, 0, 0, r->w, r->h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static int adopt_window(AppRenderer *r)
{
    HWND own = GetWindow(r->host, GW_CHILD);   /* the program parented itself */
    if (!own) {
        FindCtx f = { r, { 0 }, 0, NULL };
        job_pids(r, &f);
        EnumWindows(find_top, (LPARAM)&f);
        if (!f.hit) return 0;
        own = f.hit;
        LONG st = GetWindowLongW(own, GWL_STYLE);
        st &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        SetWindowLongW(own, GWL_STYLE, st | WS_CHILD);
        LONG ex = GetWindowLongW(own, GWL_EXSTYLE) & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
        SetWindowLongW(own, GWL_EXSTYLE, ex);
        if (!SetParent(own, r->host)) {
            DWORD e = GetLastError();
            renderer_post(r->notify, r->base.rid, REV_FAILED, HRESULT_FROM_WIN32(e),
                          L"Could not take in the program's window (error %lu)", e);
            return -1;
        }
    }
    r->child = own;
    SetWindowPos(own, NULL, 0, 0, r->w, r->h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    LOGI(L"app: '%ls' window %p in the host after %llu ms", r->wp.name, (void *)own, GetTickCount64() - r->startedAt);
    return 1;
}

static VOID CALLBACK on_exit(PVOID arg, BOOLEAN timedOut)
{
    (void)timedOut;
    AppRenderer *r = (AppRenderer *)arg;
    DWORD code = 0;
    GetExitCodeProcess(r->proc, &code);
    renderer_post(r->notify, r->base.rid, REV_FAILED, E_FAIL, L"The program closed (exit code %lu)", code);
}

/* ------------------------------------------------------------------ vtable */

static void app_set_state(Renderer *base, RenderState st, BOOL keepFrame)
{
    AppRenderer *r = (AppRenderer *)base;
    int paused = st != RS_ACTIVE;
    if (r->child && IsWindow(r->child)) ShowWindow(r->child, paused && !keepFrame ? SW_HIDE : SW_SHOWNA);
    if (paused == r->paused) return;
    r->paused = paused;
    send_state(r);
}

static void app_resize(Renderer *base, int w, int h)
{
    AppRenderer *r = (AppRenderer *)base;
    r->w = w, r->h = h;
    fit_child(r);
}

static void app_set_audio(Renderer *base, BOOL on)
{
    AppRenderer *r = (AppRenderer *)base;
    if (r->volume == (int)on) return;
    r->volume = (int)on;
    send_state(r);
}

static BOOL app_paint(Renderer *base, HDC dc, const RECT *rc)
{
    (void)base;
    FillRect(dc, rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    return TRUE;
}

static void app_describe(Renderer *base, wchar_t *buf, size_t cap)
{
    AppRenderer *r = (AppRenderer *)base;
    str_printf(buf, cap, L"App: %ls (pid %lu%ls)", path_filename(r->wp.path), r->pid, r->child ? L"" : L", no window yet");
}

static int app_wants(Renderer *base)
{
    return ((AppRenderer *)base)->child ? RENDER_WANT_INPUT : 0;
}

static void app_input(Renderer *base, const RendererInput *in)
{
    AppRenderer *r = (AppRenderer *)base;
    if (!r->child || !IsWindow(r->child)) return;
    POINT p = { in->x, in->y };   /* host-relative = child-relative (the child covers the host) */
    HWND target = r->child;
    for (int depth = 0; depth < 8; depth++) {   /* the deepest child under the pointer gets it */
        HWND sub = ChildWindowFromPointEx(target, p, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!sub || sub == target) break;
        MapWindowPoints(target, sub, &p, 1);
        target = sub;
    }
    WPARAM mk = (in->buttons & 1 ? MK_LBUTTON : 0) | (in->buttons & 2 ? MK_RBUTTON : 0) | (in->buttons & 4 ? MK_MBUTTON : 0);
    LPARAM xy = MAKELPARAM((short)p.x, (short)p.y);
    static const UINT down[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN }, up[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    switch (in->kind) {
    case RIN_MOVE:  PostMessageW(target, WM_MOUSEMOVE, mk, xy); break;
    case RIN_DOWN:  if (in->button < 3) PostMessageW(target, down[in->button], mk, xy); break;
    case RIN_UP:    if (in->button < 3) PostMessageW(target, up[in->button], mk, xy); break;
    case RIN_WHEEL: {
        POINT s = { in->x, in->y };
        ClientToScreen(r->host, &s);
        PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(mk, (short)in->wheel), MAKELPARAM((short)s.x, (short)s.y));
        break;
    }
    case RIN_LEAVE: PostMessageW(target, WM_MOUSELEAVE, 0, 0); break;
    default: break;
    }
}

static void app_on_timer(Renderer *base, UINT_PTR id)
{
    AppRenderer *r = (AppRenderer *)base;
    if (id != T_APP_FIND) return;
    int got = adopt_window(r);
    if (got == 0 && GetTickCount64() - r->startedAt < APP_WAIT_MS) return;
    KillTimer(r->host, T_APP_FIND);
    if (got < 0) return;
    if (!got) {
        renderer_post(r->notify, r->base.rid, REV_FAILED, E_FAIL, L"The program showed no window within %d s", APP_WAIT_MS / 1000);
        return;
    }
    r->ready = 1;
    renderer_post(r->notify, r->base.rid, REV_WANTS, S_OK, NULL);
    renderer_post(r->notify, r->base.rid, REV_READY, S_OK, NULL);
}

static void app_apply(Renderer *base)
{
    send_props((AppRenderer *)base);
}

static void app_destroy(Renderer *base)
{
    AppRenderer *r = (AppRenderer *)base;
    KillTimer(r->host, T_APP_FIND);
    if (r->exitWait) UnregisterWaitEx(r->exitWait, INVALID_HANDLE_VALUE);   /* no exit event for our own kill */
    if (r->child && IsWindow(r->child)) {
        ShowWindow(r->child, SW_HIDE);
        PostMessageW(r->child, WM_CLOSE, 0, 0);
    }
    if (r->job) TerminateJobObject(r->job, 0);
    else if (r->proc) TerminateProcess(r->proc, 0);
    if (r->thread) {
        EnterCriticalSection(&r->lock);
        r->quit = 1;
        LeaveCriticalSection(&r->lock);
        WakeConditionVariable(&r->cv);
        CancelSynchronousIo(r->thread);
        WaitForSingleObject(r->thread, 2000);
        CloseHandle(r->thread);
    }
    if (r->pipe) CloseHandle(r->pipe);
    if (r->proc) CloseHandle(r->proc);
    if (r->job) CloseHandle(r->job);
    DeleteCriticalSection(&r->lock);
    free(r->q);
    json_free(r->sent);
    LOGI(L"app: '%ls' stopped", r->wp.name);
    free(r);
}

static const RendererVtbl k_appVtbl = { app_set_state, app_resize, app_set_audio, app_paint, app_describe, app_destroy,
                                        app_wants, app_input, NULL, app_on_timer, app_apply };

Renderer *app_renderer_create(const RendererContext *ctx, wchar_t *err, size_t errCap)
{
    if (!file_exists(ctx->wp->path)) {
        str_copy(err, errCap, L"Program not found");
        return NULL;
    }
    AppRenderer *r = (AppRenderer *)calloc(1, sizeof *r);
    if (!r) { str_copy(err, errCap, L"Out of memory"); return NULL; }
    r->base.vt = &k_appVtbl;
    r->base.rid = ctx->rid;
    r->base.type = WP_APP;
    r->host = ctx->hwnd;
    r->notify = ctx->notify;
    r->wp = *ctx->wp;
    r->w = ctx->width, r->h = ctx->height;
    r->volume = (int)ctx->audio;
    r->paused = -1;  /* the first set_state sends the first pause and volume lines */
    InitializeCriticalSection(&r->lock);
    InitializeConditionVariable(&r->cv);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE readEnd = NULL;
    if (!CreatePipe(&readEnd, &r->pipe, &sa, 65536)) r->pipe = NULL;
    else SetHandleInformation(r->pipe, HANDLE_FLAG_INHERIT, 0);

    r->job = CreateJobObjectW(NULL, NULL);
    if (r->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(r->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    wchar_t dir[SP_MAX_PATH], cmd[SP_MAX_PATH + 128], env[32];
    path_dirname(dir, SP_COUNTOF(dir), r->wp.path);
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --screenplus-parent %llu --screenplus-size %dx%d", r->wp.path,
               (unsigned long long)(ULONG_PTR)r->host, r->w, r->h);
    str_printf(env, SP_COUNTOF(env), L"%llu", (unsigned long long)(ULONG_PTR)r->host);
    SetEnvironmentVariableW(L"SCREENPLUS_PARENT", env);   /* inherited by the child, then dropped from ours */
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | (readEnd ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_SHOWNOACTIVATE;
    si.hStdInput = readEnd;
    si.hStdOutput = si.hStdError = NULL;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessW(r->wp.path, cmd, NULL, NULL, readEnd != NULL, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL,
                             dir, &si, &pi);
    DWORD e = GetLastError();
    SetEnvironmentVariableW(L"SCREENPLUS_PARENT", NULL);
    if (readEnd) CloseHandle(readEnd);
    if (!ok) {
        str_printf(err, errCap, L"Could not start the program (error %lu)", e);
        if (r->pipe) CloseHandle(r->pipe);
        if (r->job) CloseHandle(r->job);
        DeleteCriticalSection(&r->lock);
        free(r);
        return NULL;
    }
    if (r->job && !AssignProcessToJobObject(r->job, pi.hProcess)) {
        CloseHandle(r->job);   /* nested jobs need Windows 8; the process is still killed on destroy */
        r->job = NULL;
    }
    r->proc = pi.hProcess;
    r->pid = pi.dwProcessId;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    r->startedAt = GetTickCount64();
    if (r->pipe) r->thread = CreateThread(NULL, 0, writer_main, r, 0, NULL);
    send_props(r);
    RegisterWaitForSingleObject(&r->exitWait, r->proc, on_exit, r, INFINITE, WT_EXECUTEONLYONCE);
    SetTimer(r->host, T_APP_FIND, APP_FIND_MS, NULL);
    LOGI(L"app: started '%ls' (pid %lu)", r->wp.path, r->pid);
    return &r->base;
}
