#include "log.h"
#include "paths.h"
#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#define LOG_MAX_BYTES (1024 * 1024)

static CRITICAL_SECTION g_lock;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static wchar_t g_path[SP_MAX_PATH];
static wchar_t g_old[SP_MAX_PATH];
static LONGLONG g_size;
static int g_init;

static void open_file(void)
{
    LARGE_INTEGER sz = { 0 };
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_path, GetFileExInfoStandard, &fad)) {
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = (LONG)fad.nFileSizeHigh;
        if (sz.QuadPart > LOG_MAX_BYTES) {
            MoveFileExW(g_path, g_old, MOVEFILE_REPLACE_EXISTING);
            sz.QuadPart = 0;
        }
    }
    g_file = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_size = sz.QuadPart;
}

void log_init(const wchar_t *component)
{
    if (g_init) return;
    InitializeCriticalSection(&g_lock);
    g_init = 1;
    wchar_t name[64];
    str_printf(name, SP_COUNTOF(name), L"%ls.log", component);
    path_join(g_path, SP_COUNTOF(g_path), paths_logs(), name);
    str_printf(name, SP_COUNTOF(name), L"%ls.1.log", component);
    path_join(g_old, SP_COUNTOF(g_old), paths_logs(), name);
    open_file();
}

void log_path(wchar_t *dst, size_t cap) { str_copy(dst, cap, g_path); }

void log_shutdown(void)
{
    if (!g_init) return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_lock);
}

void log_write(LogLevel level, const wchar_t *fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, SP_COUNTOF(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t lv[] = L"DIWE";
    wchar_t line[1200];
    str_printf(line, SP_COUNTOF(line), L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lc] [%lu] %ls\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               lv[level & 3], GetCurrentThreadId(), msg);
#ifndef NDEBUG
    OutputDebugStringW(line);
#endif
    if (!g_init) return;
    int n = 0;
    char *u = utf8_from_wide(line, &n);
    if (!u) return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(g_file, u, (DWORD)n, &wr, NULL);
        g_size += wr;
        if (g_size > LOG_MAX_BYTES) {
            CloseHandle(g_file);
            g_file = INVALID_HANDLE_VALUE;
            open_file();
        }
    }
    LeaveCriticalSection(&g_lock);
    free(u);
}
