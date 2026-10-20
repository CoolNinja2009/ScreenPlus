#include "diag.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include <psapi.h>
#include <dbghelp.h>
#include <stdlib.h>

static wchar_t g_component[32] = L"app";

typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static void prune_dumps(void)
{
    wchar_t pattern[SP_MAX_PATH];
    path_join(pattern, SP_COUNTOF(pattern), paths_crash(), L"*.dmp");
    WIN32_FIND_DATAW fd;
    FILETIME times[64];
    wchar_t names[64][MAX_PATH];
    int n = 0;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64) {
            times[n] = fd.ftLastWriteTime;
            str_copy(names[n], MAX_PATH, fd.cFileName);
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    while (n > 5) {                   /* delete the oldest of our own dumps */
        int oldest = 0;
        for (int i = 1; i < n; i++)
            if (CompareFileTime(&times[i], &times[oldest]) < 0) oldest = i;
        wchar_t p[SP_MAX_PATH];
        path_join(p, SP_COUNTOF(p), paths_crash(), names[oldest]);
        DeleteFileW(p);
        times[oldest] = times[n - 1];
        str_copy(names[oldest], MAX_PATH, names[n - 1]);
        n--;
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;
    HMODULE mod = NULL;
    wchar_t modName[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod))
        GetModuleFileNameW(mod, modName, MAX_PATH);
    LOGE(L"FATAL: unhandled exception 0x%08lX at %p in %ls", code, addr, path_filename(modName));

    dir_create_recursive(paths_crash());
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96], file[SP_MAX_PATH];
    str_printf(name, SP_COUNTOF(name), L"%ls-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_component, st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    path_join(file, SP_COUNTOF(file), paths_crash(), name);
    HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    MiniDumpWriteDumpFn write = dbg ? (MiniDumpWriteDumpFn)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    if (write) {
        HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
            if (write(GetCurrentProcess(), GetCurrentProcessId(), f,
                      (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, NULL, NULL))
                LOGE(L"minidump written: %ls", file);
            CloseHandle(f);
        }
        prune_dumps();
    }
    log_shutdown();
    return EXCEPTION_EXECUTE_HANDLER;   /* terminate without the WER dialog */
}

void diag_install_crash_handler(const wchar_t *component)
{
    str_copy(g_component, SP_COUNTOF(g_component), component);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
}

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static double cpu_update(CpuSampler *s, uint64_t cpu)
{
    uint64_t wall = filetime_now();
    if (s->lastWall && wall > s->lastWall && cpu >= s->lastCpu) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        double pct = (double)(cpu - s->lastCpu) * 100.0 / (double)(wall - s->lastWall) / (double)SP_MAX(si.dwNumberOfProcessors, 1);
        s->percent = SP_CLAMP(pct, 0.0, 100.0);
    }
    s->lastWall = wall;
    s->lastCpu = cpu;
    return s->percent;
}

double diag_cpu_process(CpuSampler *s, HANDLE process)
{
    FILETIME c, e, k, u;
    if (!process || !GetProcessTimes(process, &c, &e, &k, &u)) return s->percent;
    return cpu_update(s, ft64(k) + ft64(u));
}

double diag_cpu_job(CpuSampler *s, HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai;
    if (!job || !QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof ai, NULL)) return s->percent;
    return cpu_update(s, (uint64_t)ai.TotalKernelTime.QuadPart + (uint64_t)ai.TotalUserTime.QuadPart);
}

/* PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 21H2+); declared locally for older SDK headers. */
typedef struct SpMemCountersEx2 {
    DWORD   cb, PageFaultCount;
    SIZE_T  PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage,
            PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
} SpMemCountersEx2;

size_t diag_mem_process(HANDLE process)
{
    SpMemCountersEx2 pmc;
    ZeroMemory(&pmc, sizeof pmc);
    if (process && GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return pmc.PrivateWorkingSetSize ? pmc.PrivateWorkingSetSize : pmc.WorkingSetSize;
    PROCESS_MEMORY_COUNTERS basic;
    if (process && GetProcessMemoryInfo(process, &basic, sizeof basic)) return basic.WorkingSetSize;
    return 0;
}

size_t diag_mem_job(HANDLE job, int *processCount)
{
    if (processCount) *processCount = 0;
    if (!job) return 0;
    struct { JOBOBJECT_BASIC_PROCESS_ID_LIST h; ULONG_PTR more[127]; } list;
    ZeroMemory(&list, sizeof list);
    list.h.NumberOfAssignedProcesses = 128;
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &list, sizeof list, NULL)) return 0;
    size_t total = 0;
    for (DWORD i = 0; i < list.h.NumberOfProcessIdsInList && i < 128; i++) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)list.h.ProcessIdList[i]);
        if (!p) continue;
        total += diag_mem_process(p);
        CloseHandle(p);
        if (processCount) (*processCount)++;
    }
    return total;
}
