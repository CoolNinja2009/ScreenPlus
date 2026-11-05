/* See compat.h. */
#include "compat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FD_HANDLE(fd) ((HANDLE)(intptr_t)((fd) + 1))
#define HANDLE_FD(h)  ((int)((intptr_t)(h) - 1))

/* ------------------------------------------------------------------ text */

static int put_utf8(uint32_t c, char *o)
{
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) c = 0xFFFD;
    if (c < 0x80)    { o[0] = (char)c; return 1; }
    if (c < 0x800)   { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Like Win32: wn < 0 counts the NUL too; sn == 0 only measures. */
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *w, int wn, char *s, int sn, const char *d, BOOL *u)
{
    (void)cp; (void)flags; (void)d; (void)u;
    int n = wn < 0 ? (int)wcslen(w) + 1 : wn, out = 0;
    for (int i = 0; i < n; i++) {
        char b[4];
        int k = put_utf8((uint32_t)w[i], b);
        if (sn) {
            if (out + k > sn) return 0;
            memcpy(s + out, b, (size_t)k);
        }
        out += k;
    }
    return out;
}

int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int sn, wchar_t *w, int wn)
{
    (void)cp; (void)flags;
    int n = sn < 0 ? (int)strlen(s) + 1 : sn, out = 0;
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80 && !more) c = 0xFFFD;
        else if (more) {
            c &= 0x3Fu >> more;
            while (more-- && p < e && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63u);
        }
        if (wn) {
            if (out >= wn) return 0;
            w[out] = (wchar_t)c;
        }
        out++;
    }
    return out;
}

int CompareStringOrdinal(const wchar_t *a, int an, const wchar_t *b, int bn, BOOL ignoreCase)
{
    size_t la = an < 0 ? wcslen(a) : (size_t)an, lb = bn < 0 ? wcslen(b) : (size_t)bn;
    if (la != lb) return la < lb ? 1 : 3;
    for (size_t i = 0; i < la; i++) {
        wint_t x = (wint_t)a[i], y = (wint_t)b[i];
        if (ignoreCase) { x = towlower(x); y = towlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return CSTR_EQUAL;
}

wchar_t *StrStrIW(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (wcsncasecmp(hay, needle, n) == 0) return (wchar_t *)hay;
    return n ? NULL : (wchar_t *)hay;
}

/* spec: "*.png;*.jpg" style patterns, any case. */
BOOL PathMatchSpecW(const wchar_t *name, const wchar_t *spec)
{
    char *n = sp_path_utf8(name), *s = sp_path_utf8(spec), *ctx = NULL;
    BOOL hit = FALSE;
    for (char *t = s ? strtok_r(s, ";", &ctx) : NULL; n && t && !hit; t = strtok_r(NULL, ";", &ctx)) {
        while (*t == ' ') t++;
        hit = fnmatch(t, n, FNM_CASEFOLD) == 0;
    }
    free(n); free(s);
    return hit;
}

int _vsnwprintf_s(wchar_t *dst, size_t cap, size_t count, const wchar_t *fmt, va_list ap)
{
    (void)count;
    if (!dst || !cap) return -1;
    int n = vswprintf(dst, cap, fmt, ap);
    dst[cap - 1] = 0;                       /* glibc returns -1 on overflow; the text so far stays */
    return n;
}

int _snwprintf_s(wchar_t *dst, size_t cap, size_t count, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(dst, cap, count, fmt, ap);
    va_end(ap);
    return n;
}

int _vsnprintf_s(char *dst, size_t cap, size_t count, const char *fmt, va_list ap)
{
    (void)count;
    int n = vsnprintf(dst, cap, fmt, ap);
    return n >= (int)cap ? -1 : n;          /* truncated: -1, like _TRUNCATE on Windows */
}

void wcsncpy_s(wchar_t *dst, size_t cap, const wchar_t *src, size_t count)
{
    (void)count;
    size_t n = wcslen(src);
    if (n >= cap) n = cap - 1;
    wmemcpy(dst, src, n);
    dst[n] = 0;
}

void wcsncat_s(wchar_t *dst, size_t cap, const wchar_t *src, size_t count)
{
    size_t have = wcslen(dst);
    if (have < cap) wcsncpy_s(dst + have, cap - have, src, count);
}

wchar_t *wcsdup_sp(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *sp_path_utf8(const wchar_t *path)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s, n, NULL, NULL);
    for (char *c = s; *c; c++) if (*c == '\\') *c = '/';
    return s;
}

/* ------------------------------------------------------------------ files */

DWORD GetLastError(void)
{
    switch (errno) {
    case 0:      return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR:return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:  return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default:     return (DWORD)errno + 10000u;
    }
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *sec, DWORD disposition, DWORD flags, HANDLE tmpl)
{
    (void)share; (void)sec; (void)flags; (void)tmpl;
    int o = O_CLOEXEC;
    if (access & FILE_APPEND_DATA) o |= O_WRONLY | O_APPEND;
    else if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) o |= O_RDWR;
    else if (access & GENERIC_WRITE) o |= O_WRONLY;
    else o |= O_RDONLY;
    if (disposition == CREATE_ALWAYS) o |= O_CREAT | O_TRUNC;
    else if (disposition == OPEN_ALWAYS) o |= O_CREAT;
    char *p = sp_path_utf8(path);
    int fd = p ? open(p, o, 0644) : -1;
    free(p);
    return fd < 0 ? INVALID_HANDLE_VALUE : FD_HANDLE(fd);
}

BOOL ReadFile(HANDLE f, void *buf, DWORD n, DWORD *got, void *ov)
{
    (void)ov;
    ssize_t r = read(HANDLE_FD(f), buf, n);
    if (got) *got = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}

BOOL WriteFile(HANDLE f, const void *buf, DWORD n, DWORD *wrote, void *ov)
{
    (void)ov;
    ssize_t r = write(HANDLE_FD(f), buf, n);
    if (wrote) *wrote = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}

BOOL CloseHandle(HANDLE h)
{
    return h && h != INVALID_HANDLE_VALUE && close(HANDLE_FD(h)) == 0;
}

BOOL GetFileSizeEx(HANDLE f, LARGE_INTEGER *size)
{
    struct stat st;
    if (fstat(HANDLE_FD(f), &st)) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE f) { return fsync(HANDLE_FD(f)) == 0; }

#define WITH_PATH(path, expr) do { char *p_ = sp_path_utf8(path); int r_ = p_ ? (expr) : -1; free(p_); return r_ == 0; } while (0)

BOOL DeleteFileW(LPCWSTR path)      { WITH_PATH(path, unlink(p_)); }
BOOL RemoveDirectoryW(LPCWSTR path) { WITH_PATH(path, rmdir(p_)); }
BOOL CreateDirectoryW(LPCWSTR path, void *sec) { (void)sec; WITH_PATH(path, mkdir(p_, 0755)); }

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    (void)flags;
    char *a = sp_path_utf8(from), *b = sp_path_utf8(to);
    int r = a && b ? rename(a, b) : -1;
    free(a); free(b);
    return r == 0;
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    char *a = sp_path_utf8(from), *b = sp_path_utf8(to);
    int in = a ? open(a, O_RDONLY | O_CLOEXEC) : -1;
    int out = b && in >= 0 ? open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (failIfExists ? O_EXCL : O_TRUNC), 0644) : -1;
    BOOL ok = out >= 0;
    char buf[65536];
    ssize_t n;
    while (ok && (n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) ok = FALSE;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    free(a); free(b);
    return ok;
}

/* Atomic: `with` becomes `path`, the old `path` is kept as `backup`. */
BOOL ReplaceFileW(LPCWSTR path, LPCWSTR with, LPCWSTR backup, DWORD flags, void *a, void *b)
{
    (void)flags; (void)a; (void)b;
    if (backup) CopyFileW(path, backup, FALSE);
    return MoveFileExW(with, path, MOVEFILE_REPLACE_EXISTING);
}

static DWORD attrs_of(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    struct stat st;
    char *p = sp_path_utf8(path);
    int r = p ? stat(p, &st) : -1;
    free(p);
    return r ? INVALID_FILE_ATTRIBUTES : attrs_of(&st);
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    (void)level;
    struct stat st;
    char *p = sp_path_utf8(path);
    int r = p ? stat(p, &st) : -1;
    free(p);
    if (r) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)out;
    d->dwFileAttributes = attrs_of(&st);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

typedef struct Find {
    DIR  *d;
    char  dir[MAX_PATH];
    char  mask[256];
} Find;

static int find_next(Find *f, WIN32_FIND_DATAW *out)
{
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->mask, e->d_name, FNM_CASEFOLD)) continue;
        char full[MAX_PATH + 256];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        memset(out, 0, sizeof *out);
        if (!stat(full, &st)) {
            out->dwFileAttributes = attrs_of(&st);
            out->size.QuadPart = st.st_size;
        } else {
            out->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        MultiByteToWideChar(CP_UTF8, 0, e->d_name, -1, out->cFileName, 1023);
        return 1;
    }
    return 0;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *out)
{
    char *p = sp_path_utf8(pattern);
    Find *f = (Find *)calloc(1, sizeof *f);
    if (!p || !f) goto fail;
    char *slash = strrchr(p, '/');
    snprintf(f->mask, sizeof f->mask, "%s", slash ? slash + 1 : p);
    if (slash) *slash = 0;
    snprintf(f->dir, sizeof f->dir, "%s", slash ? (p[0] ? p : "/") : ".");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, out)) goto fail;
    free(p);
    return f;
fail:
    if (f && f->d) closedir(f->d);
    free(f);
    free(p);
    return INVALID_HANDLE_VALUE;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *out) { return find_next((Find *)h, out); }

BOOL FindClose(HANDLE h)
{
    Find *f = (Find *)h;
    if (!f || h == INVALID_HANDLE_VALUE) return FALSE;
    closedir(f->d);
    free(f);
    return TRUE;
}

FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char m[8] = "";
    for (int i = 0; mode[i] && i < 7; i++) m[i] = (char)mode[i];
    char *p = sp_path_utf8(path);
    FILE *f = p ? fopen(p, m) : NULL;
    free(p);
    return f;
}

/* ------------------------------------------------------------------ process, time */

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetCurrentThreadId(void)  { return (DWORD)syscall(SYS_gettid); }

DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t *out, DWORD cap)
{
    char *n = sp_path_utf8(name);
    const char *v = n ? getenv(n) : NULL;
    free(n);
    if (!v) return 0;
    int len = MultiByteToWideChar(CP_UTF8, 0, v, -1, NULL, 0);
    if ((DWORD)len > cap) return (DWORD)len;
    MultiByteToWideChar(CP_UTF8, 0, v, -1, out, (int)cap);
    return (DWORD)len - 1;
}

DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD cap)
{
    (void)m;
    char b[MAX_PATH];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return 0;
    b[n] = 0;
    int len = MultiByteToWideChar(CP_UTF8, 0, b, -1, out, (int)cap);
    return len > 0 ? (DWORD)len - 1 : 0;
}

void GetLocalTime(SYSTEMTIME *t)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    t->wYear = (WORD)(tm.tm_year + 1900); t->wMonth = (WORD)(tm.tm_mon + 1); t->wDayOfWeek = (WORD)tm.tm_wday;
    t->wDay = (WORD)tm.tm_mday; t->wHour = (WORD)tm.tm_hour; t->wMinute = (WORD)tm.tm_min;
    t->wSecond = (WORD)tm.tm_sec; t->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}

void GetSystemTimePreciseAsFileTime(FILETIME *ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t v = (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ULL;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000000000LL; return TRUE; }

BOOL QueryPerformanceCounter(LARGE_INTEGER *v)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

void sp_random_bytes(void *p, size_t n)
{
    if (getrandom(p, n, 0) != (ssize_t)n) {
        uint64_t t = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        memcpy(p, &t, n < sizeof t ? n : sizeof t);
    }
}
