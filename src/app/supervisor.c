/* Engine supervision: the engine runs in a child process inside a job object (kill-on-close,
 * so it and its WebView2 helpers never outlive the shell). Unexpected exits are recovered with
 * bounded exponential backoff, then safe mode (images only), then a stopped state that the
 * user can restart explicitly. No polling: exits arrive via a registered wait. */
#include "app.h"
#include "../ui/ui.h"
#include "../tray/tray.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"

#define CRASH_WINDOW_MS (10.0 * 60.0 * 1000.0)
#define MAX_RESTARTS    3

static int g_stopping;

static void CALLBACK engine_exit_cb(void *ctx, BOOLEAN timedOut)
{
    (void)timedOut;
    HANDLE proc = (HANDLE)ctx;
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    PostMessageW(g_app.shellWnd, WM_SP_ENGINE_EXITED, code, (LPARAM)GetProcessId(proc));
}

static void release_engine_handles(void)
{
    if (g_app.engineWait) {
        UnregisterWaitEx(g_app.engineWait, INVALID_HANDLE_VALUE);
        g_app.engineWait = NULL;
    }
    if (g_app.engineProcess) {
        CloseHandle(g_app.engineProcess);
        g_app.engineProcess = NULL;
    }
    g_app.engineWnd = NULL;
    g_app.enginePid = 0;
    g_app.haveStatus = 0;
}

BOOL supervisor_start(void)
{
    if (g_app.engineProcess) return TRUE;
    if (!g_app.job) {
        g_app.job = CreateJobObjectW(NULL, NULL);
        if (g_app.job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
            ZeroMemory(&li, sizeof li);
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            SetInformationJobObject(g_app.job, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
    }
    wchar_t cmd[SP_MAX_PATH + 96];
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --engine --shell=%llx%ls", paths_exe(),
               (unsigned long long)(uintptr_t)g_app.shellWnd, g_app.safeMode ? L" --safe-mode" : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(paths_exe(), cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        str_printf(g_app.engineError, SP_COUNTOF(g_app.engineError), L"Could not start the renderer process (err %lu)", GetLastError());
        LOGE(L"%ls", g_app.engineError);
        return FALSE;
    }
    if (g_app.job && !AssignProcessToJobObject(g_app.job, pi.hProcess))
        LOGW(L"engine not placed in job object (err %lu); resource totals cover the engine process only", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_app.engineProcess = pi.hProcess;
    g_app.enginePid = pi.dwProcessId;
    g_app.engineWnd = NULL;
    g_app.engineSpawnTime = filetime_now();
    g_stopping = 0;
    if (!RegisterWaitForSingleObject(&g_app.engineWait, pi.hProcess, engine_exit_cb, pi.hProcess, INFINITE,
                                     WT_EXECUTEONLYONCE))
        g_app.engineWait = NULL;
    ZeroMemory(&g_app.cpuEngine, sizeof g_app.cpuEngine);
    LOGI(L"engine process started (pid %lu%ls)", pi.dwProcessId, g_app.safeMode ? L", safe mode" : L"");
    return TRUE;
}

void supervisor_on_hello(const IpcHello *h)
{
    if (h->version != SP_IPC_VERSION || h->pid != g_app.enginePid) {
        LOGW(L"ignoring hello from unexpected engine (pid %u)", h->pid);
        return;
    }
    g_app.engineWnd = (HWND)(uintptr_t)h->hwnd;
    LOGI(L"engine connected (pid %u)", h->pid);
}

void supervisor_on_exit(DWORD code, DWORD pid)
{
    if (!g_app.engineProcess || pid != g_app.enginePid) return;   /* stale or intentional stop */
    release_engine_handles();
    if (g_stopping || g_app.exiting) return;
    double now = time_ms();
    if (!g_app.crashes || now - g_app.firstCrashAt > CRASH_WINDOW_MS) {
        g_app.crashes = 0;
        g_app.firstCrashAt = now;
    }
    g_app.crashes++;
    str_printf(g_app.engineError, SP_COUNTOF(g_app.engineError), L"The renderer process exited unexpectedly (code 0x%08lX)", code);
    LOGE(L"%ls; crash %d in the current window", g_app.engineError, g_app.crashes);
    if (g_app.crashes <= MAX_RESTARTS) {
        UINT delay = 1000u << (g_app.crashes - 1);       /* 1 s, 2 s, 4 s */
        g_app.restartPending = 1;
        SetTimer(g_app.shellWnd, 1, delay, NULL);
        app_notice(NOTICE_WARN, L"The wallpaper renderer stopped unexpectedly and is restarting (attempt %d of %d).",
                   g_app.crashes, MAX_RESTARTS);
    } else if (!g_app.safeMode) {
        g_app.safeMode = 1;
        g_app.restartPending = 1;
        SetTimer(g_app.shellWnd, 1, 3000, NULL);
        app_notice(NOTICE_WARN, L"The renderer kept failing, so it restarted in safe mode (video and HTML wallpapers are paused).");
    } else {
        g_app.engineGaveUp = 1;
        app_notice(NOTICE_ERROR, L"The renderer stopped after repeated failures. Your normal Windows wallpaper is shown. "
                                 L"Use Restart renderer to try again.");
    }
    tray_update();
    ui_on_status();
}

void supervisor_on_timer(void)
{
    if (!g_app.restartPending || g_app.exiting) return;
    g_app.restartPending = 0;
    g_app.engineRestarts++;
    if (!supervisor_start()) {
        g_app.engineGaveUp = 1;
        app_notice(NOTICE_ERROR, L"%ls", g_app.engineError);
    }
    ui_on_status();
}

BOOL supervisor_send(uint32_t cmd, uint32_t arg)
{
    if (!g_app.engineWnd) return FALSE;
    IpcCommand c = { cmd, arg };
    if (ipc_send(g_app.shellWnd, g_app.engineWnd, IPC_ENGINE_CMD, &c, sizeof c, 2000)) return TRUE;
    if (IsHungAppWindow(g_app.engineWnd) && g_app.engineProcess) {
        LOGE(L"engine is not responding; terminating it for recovery");
        TerminateProcess(g_app.engineProcess, 0xE0000001);   /* exit callback schedules the restart */
    }
    return FALSE;
}

void supervisor_stop(void)
{
    if (!g_app.engineProcess) return;
    g_stopping = 1;
    g_app.restartPending = 0;
    KillTimer(g_app.shellWnd, 1);
    double t0 = time_ms();
    supervisor_send(CMD_SHUTDOWN, 0);
    if (WaitForSingleObject(g_app.engineProcess, 4000) != WAIT_OBJECT_0) {
        LOGW(L"engine did not exit in time; terminating");
        TerminateProcess(g_app.engineProcess, 1);
        WaitForSingleObject(g_app.engineProcess, 2000);
    }
    LOGI(L"engine stopped in %.0f ms", time_ms() - t0);
    release_engine_handles();
}

void supervisor_restart(void)
{
    LOGI(L"renderer restart requested");
    supervisor_stop();
    g_app.crashes = 0;
    g_app.safeMode = 0;
    g_app.engineGaveUp = 0;
    g_app.engineError[0] = 0;
    g_app.engineRestarts++;
    g_stopping = 0;
    supervisor_start();
    tray_update();
    ui_on_status();
}

void supervisor_sample_stats(void)
{
    g_app.shellCpu = diag_cpu_process(&g_app.cpuShell, GetCurrentProcess());
    g_app.shellMem = diag_mem_process(GetCurrentProcess());
    if (!g_app.engineProcess) {
        g_app.engineCpu = 0;
        g_app.engineMem = 0;
        g_app.engineProcs = 0;
        return;
    }
    BOOL inJob = FALSE;
    if (g_app.job && IsProcessInJob(g_app.engineProcess, g_app.job, &inJob) && inJob) {
        g_app.engineCpu = diag_cpu_job(&g_app.cpuEngine, g_app.job);
        g_app.engineMem = diag_mem_job(g_app.job, &g_app.engineProcs);
    } else {
        g_app.engineCpu = diag_cpu_process(&g_app.cpuEngine, g_app.engineProcess);
        g_app.engineMem = diag_mem_process(g_app.engineProcess);
        g_app.engineProcs = 1;
    }
}
