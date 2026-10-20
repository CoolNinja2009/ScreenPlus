#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>

const wchar_t *wallpaper_type_name(WallpaperType t)
{
    switch (t) {
    case WP_IMAGE: return L"Image";
    case WP_VIDEO: return L"Video";
    case WP_HTML:  return L"HTML";
    default:       return L"None";
    }
}

const wchar_t *render_state_name(RenderState s)
{
    switch (s) {
    case RS_ACTIVE:     return L"Active";
    case RS_OCCLUDED:   return L"Occluded";
    case RS_SUSPENDED:  return L"Suspended";
    case RS_RECOVERING: return L"Recovering";
    default:            return L"Stopped";
    }
}

void str_copy(wchar_t *dst, size_t cap, const wchar_t *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    wcsncpy_s(dst, cap, src, _TRUNCATE);
}

void str_copyn(wchar_t *dst, size_t cap, const wchar_t *src, size_t n)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t m = SP_MIN(n, cap - 1);
    wmemcpy(dst, src, m);
    dst[m] = 0;
}

void str_cat(wchar_t *dst, size_t cap, const wchar_t *src)
{
    if (!dst || cap == 0 || !src) return;
    wcsncat_s(dst, cap, src, _TRUNCATE);
}

void str_printf(wchar_t *dst, size_t cap, const wchar_t *fmt, ...)
{
    if (!dst || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(dst, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

int str_ieq(const wchar_t *a, const wchar_t *b)
{
    if (!a || !b) return a == b;
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

int str_icontains(const wchar_t *hay, const wchar_t *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    return StrStrIW(hay, needle) != NULL;
}

int str_istarts(const wchar_t *s, const wchar_t *prefix)
{
    if (!s || !prefix) return 0;
    size_t n = wcslen(prefix);
    if (wcslen(s) < n) return 0;
    return CompareStringOrdinal(s, (int)n, prefix, (int)n, TRUE) == CSTR_EQUAL;
}

void str_trim(wchar_t *s)
{
    if (!s) return;
    size_t len = wcslen(s), start = 0;
    while (start < len && iswspace(s[start])) start++;
    while (len > start && iswspace(s[len - 1])) len--;
    if (start) wmemmove(s, s + start, len - start);
    s[len - start] = 0;
}

void str_sanitize_line(wchar_t *s)
{
    for (; s && *s; s++)
        if (*s < 0x20 || *s == 0x7f) *s = L' ';
}

char *utf8_from_wide(const wchar_t *w, int *outLen)
{
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    if (outLen) *outLen = n - 1;
    return s;
}

wchar_t *wide_from_utf8(const char *s, int len)
{
    if (!s) return NULL;
    if (len < 0) len = (int)strlen(s);
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n > 0 ? n : 0] = 0;
    return w;
}

const wchar_t *path_filename(const wchar_t *path)
{
    if (!path) return L"";
    const wchar_t *p = path, *last = path;
    for (; *p; p++)
        if (*p == L'\\' || *p == L'/') last = p + 1;
    return last;
}

const wchar_t *path_ext(const wchar_t *path)
{
    const wchar_t *name = path_filename(path);
    const wchar_t *dot = wcsrchr(name, L'.');
    return dot ? dot : L"";
}

void path_dirname(wchar_t *dst, size_t cap, const wchar_t *path)
{
    const wchar_t *name = path_filename(path);
    size_t n = (size_t)(name - path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    /* keep "C:\" form for drive roots */
    if (n == 2 && path[1] == L':') n = 3;
    str_copyn(dst, cap, path, n);
}

void path_join(wchar_t *dst, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    wchar_t tmp[SP_MAX_PATH];
    str_copy(tmp, SP_COUNTOF(tmp), dir);
    size_t n = wcslen(tmp);
    if (n && tmp[n - 1] != L'\\' && tmp[n - 1] != L'/') str_cat(tmp, SP_COUNTOF(tmp), L"\\");
    str_cat(tmp, SP_COUNTOF(tmp), name);
    str_copy(dst, cap, tmp);
}

void path_stem(wchar_t *dst, size_t cap, const wchar_t *path)
{
    const wchar_t *name = path_filename(path);
    const wchar_t *dot = wcsrchr(name, L'.');
    str_copyn(dst, cap, name, dot && dot != name ? (size_t)(dot - name) : wcslen(name));
}

int file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_create_recursive(const wchar_t *path)
{
    wchar_t tmp[SP_MAX_PATH];
    str_copy(tmp, SP_COUNTOF(tmp), path);
    size_t n = wcslen(tmp);
    for (size_t i = 3; i <= n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'/' || tmp[i] == 0) {
            wchar_t c = tmp[i];
            tmp[i] = 0;
            if (!dir_exists(tmp) && !CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return 0;
            tmp[i] = c;
        }
    }
    return dir_exists(path);
}

int64_t time_unix_now(void)
{
    return (int64_t)((filetime_now() - 116444736000000000ULL) / 10000000ULL);
}

double time_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

uint64_t filetime_now(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

uint64_t process_creation_filetime(HANDLE process)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return 0;
    return ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
}

void random_id(wchar_t out[SP_ID_LEN])
{
    uint8_t b[8];
    if (BCryptGenRandom(NULL, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        uint64_t t = filetime_now() ^ ((uint64_t)GetCurrentProcessId() << 32);
        memcpy(b, &t, sizeof b);
    }
    static const wchar_t hex[] = L"0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[16] = 0;
}

uint32_t hash_fnv1a_w(const wchar_t *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        wchar_t c = (wchar_t)towlower(*s);
        h ^= (uint32_t)(c & 0xff); h *= 16777619u;
        h ^= (uint32_t)(c >> 8);   h *= 16777619u;
    }
    return h;
}

int url_encode_path(char *dst, size_t cap, const wchar_t *path)
{
    int n = 0;
    char *u = utf8_from_wide(path, &n);
    if (!u) return 0;
    size_t o = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c == '\\') c = '/';
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (keep) {
            if (o + 1 >= cap) { free(u); return 0; }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= cap) { free(u); return 0; }
            dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15];
        }
    }
    dst[o] = 0;
    free(u);
    return 1;
}

DWORD windows_build_number(void)
{
    static DWORD build;
    if (build) return build;
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW vi = { sizeof vi };
    if (fn && fn(&vi) == 0) build = vi.dwBuildNumber;
    return build;
}

void shell_open(const wchar_t *target)
{
    ShellExecuteW(NULL, L"open", target, NULL, NULL, SW_SHOWNORMAL);
}

void shell_reveal(const wchar_t *path)
{
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
    } else {
        wchar_t dir[SP_MAX_PATH];
        path_dirname(dir, SP_COUNTOF(dir), path);
        shell_open(dir);
    }
}

void sb_append(StrBuf *b, const char *s, size_t n)
{
    if (b->failed || !s) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = (char *)realloc(b->p, nc);
        if (!p) { b->failed = 1; return; }
        b->p = p;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_puts(StrBuf *b, const char *s) { sb_append(b, s, strlen(s)); }

void sb_put_wide(StrBuf *b, const wchar_t *w)
{
    int n = 0;
    char *u = utf8_from_wide(w, &n);
    if (!u) { b->failed = 1; return; }
    sb_append(b, u, (size_t)n);
    free(u);
}

void sb_printf(StrBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(tmp);
    sb_append(b, tmp, (size_t)n);
}

void sb_free(StrBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}
