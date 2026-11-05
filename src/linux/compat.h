/* Linux stand-ins for the small part of Win32 the shared code uses (config, json, log, library,
 * cli_server, wallpaper_api), so those files build unchanged. wchar_t is 32-bit here; text is UTF-8
 * again the moment it leaves the process. Every file call turns '\' into '/', so the shared code's
 * L"a\\b" path literals work. Windows builds never see this file. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <pthread.h>

#define WINAPI
#define __inline inline
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 4096
#define WM_APP 0x8000
#define _TRUNCATE ((size_t)-1)

typedef int            BOOL;
typedef uint32_t       DWORD, UINT, ULONG;
typedef int32_t        LONG, HRESULT;
typedef uint8_t        BYTE;
typedef uint16_t       WORD;
typedef uintptr_t      ULONG_PTR, DWORD_PTR, WPARAM;
typedef intptr_t       LPARAM, LRESULT;
typedef void          *HANDLE, *HWND, *HINSTANCE, *HMONITOR, *HMODULE, *PVOID, *LPVOID;
typedef wchar_t        WCHAR, *PWSTR, *LPWSTR;
typedef const wchar_t *PCWSTR, *LPCWSTR;
typedef long long      LONGLONG;

typedef struct RECT { LONG left, top, right, bottom; } RECT;
typedef union LARGE_INTEGER { LONGLONG QuadPart; struct { DWORD LowPart; LONG HighPart; }; } LARGE_INTEGER;
typedef struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef pthread_mutex_t CRITICAL_SECTION;
typedef struct MSG { int unused; } MSG;
typedef struct WIN32_FIND_DATAW {
    DWORD         dwFileAttributes;
    LARGE_INTEGER size;
    wchar_t       cFileName[1024];
} WIN32_FIND_DATAW;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;

#define INVALID_HANDLE_VALUE       ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES    ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY   0x10u
#define FILE_ATTRIBUTE_NORMAL      0x80u
#define FILE_ATTRIBUTE_HIDDEN      0x02u
#define FILE_ATTRIBUTE_SYSTEM      0x04u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define FILE_APPEND_DATA           0x4u
#define FILE_SHARE_READ            1u
#define FILE_SHARE_WRITE           2u
#define FILE_SHARE_DELETE          4u
#define CREATE_ALWAYS              2u
#define OPEN_EXISTING              3u
#define OPEN_ALWAYS                4u
#define FILE_FLAG_WRITE_THROUGH    0u
#define FILE_FLAG_SEQUENTIAL_SCAN  0u
#define MOVEFILE_REPLACE_EXISTING  1u
#define MOVEFILE_WRITE_THROUGH     8u
#define REPLACEFILE_IGNORE_MERGE_ERRORS 2u
#define REPLACEFILE_IGNORE_ACL_ERRORS   4u
#define ERROR_SUCCESS              0u
#define ERROR_FILE_NOT_FOUND       2u
#define ERROR_PATH_NOT_FOUND       3u
#define ERROR_ACCESS_DENIED        5u
#define ERROR_ALREADY_EXISTS       183u
#define CP_UTF8                    65001u
#define CSTR_EQUAL                 2
#define WC_ERR_INVALID_CHARS       0u
#define MB_ERR_INVALID_CHARS       0u
#define SUCCEEDED(h)               ((h) >= 0)
#define FAILED(h)                  ((h) < 0)

#define ZeroMemory(p, n)           memset((p), 0, (n))
#define CopyMemory(d, s, n)        memcpy((d), (s), (n))
#define OutputDebugStringW(s)      ((void)0)
#define _stricmp                   strcasecmp
#define _strnicmp                  strncasecmp
#define _wcsicmp                   wcscasecmp
#define _wcsnicmp                  wcsncasecmp
#define wcstok_s(s, d, c)          wcstok((s), (d), (c))
#define strtok_s(s, d, c)          strtok_r((s), (d), (c))
#define _wtoi(s)                   ((int)wcstol((s), NULL, 10))
#define _wtof(s)                   wcstod((s), NULL)
#define _wcstoi64                  wcstoll
#define _wcsdup                    wcsdup_sp
#define _strdup                    strdup
#define InterlockedExchange(p, v)  __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p)    __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p)    __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)

/* files */
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *sec, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL   ReadFile(HANDLE f, void *buf, DWORD n, DWORD *got, void *ov);
BOOL   WriteFile(HANDLE f, const void *buf, DWORD n, DWORD *wrote, void *ov);
BOOL   CloseHandle(HANDLE h);
BOOL   GetFileSizeEx(HANDLE f, LARGE_INTEGER *size);
BOOL   FlushFileBuffers(HANDLE f);
BOOL   DeleteFileW(LPCWSTR path);
BOOL   RemoveDirectoryW(LPCWSTR path);
BOOL   CreateDirectoryW(LPCWSTR path, void *sec);
BOOL   MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL   CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
BOOL   ReplaceFileW(LPCWSTR path, LPCWSTR with, LPCWSTR backup, DWORD flags, void *a, void *b);
DWORD  GetFileAttributesW(LPCWSTR path);
BOOL   GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *out);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *out);
BOOL   FindNextFileW(HANDLE h, WIN32_FIND_DATAW *out);
BOOL   FindClose(HANDLE h);
FILE  *_wfopen(const wchar_t *path, const wchar_t *mode);
DWORD  GetLastError(void);

/* text */
int    WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *w, int wn, char *s, int sn, const char *d, BOOL *u);
int    MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int sn, wchar_t *w, int wn);
int    CompareStringOrdinal(const wchar_t *a, int an, const wchar_t *b, int bn, BOOL ignoreCase);
wchar_t *StrStrIW(const wchar_t *hay, const wchar_t *needle);
BOOL   PathMatchSpecW(const wchar_t *name, const wchar_t *spec);
int    _vsnwprintf_s(wchar_t *dst, size_t cap, size_t count, const wchar_t *fmt, va_list ap);
int    _snwprintf_s(wchar_t *dst, size_t cap, size_t count, const wchar_t *fmt, ...);
int    _vsnprintf_s(char *dst, size_t cap, size_t count, const char *fmt, va_list ap);
void   wcsncpy_s(wchar_t *dst, size_t cap, const wchar_t *src, size_t count);
void   wcsncat_s(wchar_t *dst, size_t cap, const wchar_t *src, size_t count);
wchar_t *wcsdup_sp(const wchar_t *s);
char  *sp_path_utf8(const wchar_t *path);   /* UTF-8, '\' -> '/'; free() */

/* process, time, sync */
DWORD  GetCurrentProcessId(void);
DWORD  GetCurrentThreadId(void);
DWORD  GetEnvironmentVariableW(LPCWSTR name, wchar_t *out, DWORD cap);
DWORD  GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD cap);
void   GetLocalTime(SYSTEMTIME *t);
void   GetSystemTimePreciseAsFileTime(FILETIME *ft);
BOOL   QueryPerformanceCounter(LARGE_INTEGER *v);
BOOL   QueryPerformanceFrequency(LARGE_INTEGER *v);
void   sp_random_bytes(void *p, size_t n);
#define InitializeCriticalSection(c) pthread_mutex_init((c), NULL)
#define EnterCriticalSection(c)      pthread_mutex_lock(c)
#define LeaveCriticalSection(c)      pthread_mutex_unlock(c)
#define DeleteCriticalSection(c)     pthread_mutex_destroy(c)
