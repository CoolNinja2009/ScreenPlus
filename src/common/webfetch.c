#include "webfetch.h"
#include "util.h"
#include "paths.h"
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION        118
#define WINHTTP_DECOMPRESSION_FLAG_ALL      0x3
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 147
#define WINHTTP_PROTOCOL_FLAG_HTTP2         0x1
#endif

#define USER_AGENT L"Mozilla/5.0 (compatible; ScreenPlus/" SP_VERSION_W L")"

struct HttpGet {
    HINTERNET session, conn, req;
    int       shared;     /* session is g_shared: never closed */
    int       status;
    int       ranges;     /* Accept-Ranges: bytes */
    int64_t   length;
    wchar_t   type[96];
};

/* Pinned official release assets. To update: change the version in the URL and take size + SHA-256
 * from the release (yt-dlp publishes SHA2-256SUMS; GitHub lists a digest for every asset). The same
 * pins are used by the release workflow for the portable zip. QuickJS-NG (2 MB, a plain .exe) is the
 * JavaScript runtime yt-dlp needs for YouTube; Deno, its default, is a ~45 MB zip. */
const UrlTool k_urlTools[TOOL_COUNT] = {
    { L"yt-dlp.exe", L"yt-dlp 2026.10.21",
      L"https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.21/yt-dlp.exe",
      "33121d192d706e62f684a06f80918e3f3a8ba52ac3a65213b6951c39c7d9f555", 17880111 },
    { L"qjs.exe", L"QuickJS-NG 0.17.0",
      L"https://github.com/quickjs-ng/quickjs/releases/download/v0.17.0/qjs-windows-x86_64.exe",
      "2aeabf0092c3262d6b2609824418f7dd7ed1f1df939f73b2b15645230cac0d77", 2151363 },
};

/* ---------------------------------------------------------------- SHA-256 */

static BCRYPT_HASH_HANDLE sha_begin(BCRYPT_ALG_HANDLE *alg)
{
    BCRYPT_HASH_HANDLE h = NULL;
    *alg = NULL;
    if (BCryptOpenAlgorithmProvider(alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return NULL;
    if (BCryptCreateHash(*alg, &h, NULL, 0, NULL, 0, 0) != 0) h = NULL;
    return h;
}

static void sha_end(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE h, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    UCHAR d[32];
    if (hex) hex[0] = 0;
    if (h && hex && BCryptFinishHash(h, d, sizeof d, 0) == 0) {
        for (int i = 0; i < 32; i++) {
            hex[i * 2] = digits[d[i] >> 4];
            hex[i * 2 + 1] = digits[d[i] & 15];
        }
        hex[64] = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
}

BOOL file_sha256(const wchar_t *path, char hex[65])
{
    hex[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h = sha_begin(&alg);
    char *buf = (char *)malloc(1 << 20);
    BOOL ok = h && buf;
    DWORD n = 0;
    while (ok && ReadFile(f, buf, 1 << 20, &n, NULL) && n) ok = BCryptHashData(h, (PUCHAR)buf, n, 0) == 0;
    free(buf);
    CloseHandle(f);
    sha_end(alg, h, ok ? hex : NULL);
    return ok && hex[0];
}

/* ---------------------------------------------------------------- HTTP */

static void net_err(wchar_t *err, size_t cap, const wchar_t *what)
{
    DWORD e = GetLastError();
    const wchar_t *m = NULL;
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  m = L"the server name could not be found (check the address and your connection)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT:     m = L"could not connect to the server"; break;
    case ERROR_WINHTTP_TIMEOUT:            m = L"the connection timed out"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:     m = L"the secure connection failed"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR:   m = L"the connection was interrupted"; break;
    case ERROR_WINHTTP_REDIRECT_FAILED:    m = L"the server redirected to an address that isn't allowed"; break;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: m = L"the address is not a valid web link"; break;
    default: break;
    }
    if (m) str_printf(err, cap, L"%ls: %ls", what, m);
    else str_printf(err, cap, L"%ls (network error %lu)", what, e);
}

void http_close(HttpGet *g)
{
    if (!g) return;
    if (g->req) WinHttpCloseHandle(g->req);
    if (g->conn) WinHttpCloseHandle(g->conn);
    if (g->session && !g->shared) WinHttpCloseHandle(g->session);
    free(g);
}

/* One session for http_open_shared: WinHTTP pools its connections, so later requests skip the TCP and TLS
 * handshakes, and HTTP/2 runs parallel requests over one connection. */
static INIT_ONCE g_sharedOnce = INIT_ONCE_STATIC_INIT;
static HINTERNET g_shared;

static BOOL CALLBACK shared_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    g_shared = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (g_shared) {
        DWORD v = WINHTTP_PROTOCOL_FLAG_HTTP2;      /* Windows 10 1607+; older ones keep HTTP/1.1 keep-alive */
        WinHttpSetOption(g_shared, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &v, sizeof v);
        v = WINHTTP_DECOMPRESSION_FLAG_ALL;         /* gzip pages are a fifth of the size */
        WinHttpSetOption(g_shared, WINHTTP_OPTION_DECOMPRESSION, &v, sizeof v);
        WinHttpSetTimeouts(g_shared, 10000, 10000, 15000, 15000);
    }
    return TRUE;
}

static HttpGet *open_get(const wchar_t *url, int allowHttp, int shared, wchar_t *err, size_t errCap)
{
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    wchar_t host[256];
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || !uc.dwHostNameLength || uc.dwHostNameLength >= SP_COUNTOF(host) ||
        (uc.nScheme != INTERNET_SCHEME_HTTPS && !(allowHttp && uc.nScheme == INTERNET_SCHEME_HTTP))) {
        str_copy(err, errCap, allowHttp ? L"That is not a valid http:// or https:// link" : L"Only https:// links are allowed");
        return NULL;
    }
    str_copyn(host, SP_COUNTOF(host), uc.lpszHostName, uc.dwHostNameLength);
    /* path + query are contiguous in the URL; the #fragment is never sent */
    const wchar_t *start = uc.lpszUrlPath ? uc.lpszUrlPath : uc.lpszExtraInfo;
    size_t plen = start ? (size_t)uc.dwUrlPathLength + uc.dwExtraInfoLength : 0;
    wchar_t *object = (wchar_t *)calloc(plen + 2, sizeof(wchar_t));
    HttpGet *g = (HttpGet *)calloc(1, sizeof *g);
    if (!object || !g) {
        free(object);
        free(g);
        str_copy(err, errCap, L"Out of memory");
        return NULL;
    }
    str_copyn(object, plen + 2, start, plen);
    wchar_t *frag = wcschr(object, L'#');
    if (frag) *frag = 0;
    if (!object[0]) str_copy(object, plen + 2, L"/");

    g->length = -1;
    if (shared) {
        InitOnceExecuteOnce(&g_sharedOnce, shared_init, NULL, NULL);
        g->session = g_shared;
        g->shared = 1;
    } else {
        g->session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (g->session) WinHttpSetTimeouts(g->session, 15000, 15000, 30000, 30000);
    }
    if (g->session) g->conn = WinHttpConnect(g->session, host, uc.nPort, 0);
    if (g->conn)   /* default redirect policy: followed, but never from https to http */
        g->req = WinHttpOpenRequest(g->conn, L"GET", object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (g->req && shared) {
        DWORD v = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(g->req, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &v, sizeof v);
    }
    free(object);
    if (!g->req || !WinHttpSendRequest(g->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(g->req, NULL)) {
        net_err(err, errCap, L"Could not reach the server");
        http_close(g);
        return NULL;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(g->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    g->status = (int)status;
    wchar_t len[32];
    size = sizeof len;
    if (WinHttpQueryHeaders(g->req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX))
        g->length = _wcstoi64(len, NULL, 10);
    size = sizeof len;
    g->ranges = WinHttpQueryHeaders(g->req, WINHTTP_QUERY_ACCEPT_RANGES, WINHTTP_HEADER_NAME_BY_INDEX, len, &size,
                                    WINHTTP_NO_HEADER_INDEX) && str_icontains(len, L"bytes");
    size = sizeof g->type;
    if (WinHttpQueryHeaders(g->req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, g->type, &size, WINHTTP_NO_HEADER_INDEX)) {
        wchar_t *semi = wcschr(g->type, L';');
        if (semi) *semi = 0;
        str_trim(g->type);
        CharLowerW(g->type);
    } else {
        g->type[0] = 0;
    }
    return g;
}

HttpGet *http_open(const wchar_t *url, int allowHttp, wchar_t *err, size_t errCap)
{
    return open_get(url, allowHttp, 0, err, errCap);
}

HttpGet *http_open_shared(const wchar_t *url, wchar_t *err, size_t errCap)
{
    return open_get(url, 0, 1, err, errCap);
}

int            http_status(const HttpGet *g)       { return g->status; }
int            http_ranges(const HttpGet *g)       { return g->ranges; }
int64_t        http_length(const HttpGet *g)       { return g->length; }
const wchar_t *http_content_type(const HttpGet *g) { return g->type; }

int http_read(HttpGet *g, char *buf, int cap)
{
    int got = 0;
    while (got < cap) {
        DWORD n = 0;
        if (!WinHttpReadData(g->req, buf + got, (DWORD)(cap - got), &n) || !n) break;
        got += (int)n;
    }
    return got;
}

BOOL http_save(HttpGet *g, const wchar_t *dest, int64_t maxBytes, FetchProgress cb, void *ctx,
               char *sha256Hex, wchar_t *err, size_t errCap)
{
    if (g->length > maxBytes) {
        str_printf(err, errCap, L"The file is too large (%lld MB)", g->length >> 20);
        return FALSE;
    }
    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        str_printf(err, errCap, L"Could not create %ls (error %lu)", path_filename(dest), GetLastError());
        return FALSE;
    }
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = sha256Hex ? sha_begin(&alg) : NULL;
    char *buf = (char *)malloc(1 << 16);
    int64_t done = 0;
    BOOL ok = buf && (h || !sha256Hex);
    if (!ok) str_copy(err, errCap, L"Out of memory");
    while (ok) {
        DWORD n = 0, w = 0;
        if (!WinHttpReadData(g->req, buf, 1 << 16, &n)) {
            net_err(err, errCap, L"The download was interrupted");
            ok = FALSE;
        } else if (!n) {
            break;
        } else if ((done += n) > maxBytes) {
            str_printf(err, errCap, L"The file is larger than %lld MB", maxBytes >> 20);
            ok = FALSE;
        } else if (!WriteFile(f, buf, n, &w, NULL) || w != n) {
            str_printf(err, errCap, L"Could not write the file (error %lu); is the disk full?", GetLastError());
            ok = FALSE;
        } else if (h && BCryptHashData(h, (PUCHAR)buf, n, 0) != 0) {
            str_copy(err, errCap, L"Could not verify the download");
            ok = FALSE;
        } else if (cb && !cb(ctx, done, g->length)) {
            str_copy(err, errCap, L"Cancelled");
            ok = FALSE;
        }
    }
    if (ok && g->length >= 0 && done != g->length) {
        str_copy(err, errCap, L"The download ended early");
        ok = FALSE;
    }
    free(buf);
    CloseHandle(f);
    if (sha256Hex) sha_end(alg, h, ok ? sha256Hex : NULL);
    if (!ok) DeleteFileW(dest);
    return ok;
}

/* ---------------------------------------------------------------- URL-import tools */

static void tools_dir(wchar_t *out, size_t cap, int portable)
{
    path_join(out, cap, portable ? paths_exe_dir() : paths_root(), L"tools");
}

static BOOL tool_ok(const wchar_t *path, const UrlTool *t)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    if ((int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) != t->size) return FALSE;
    char hex[65];
    return file_sha256(path, hex) && strcmp(hex, t->sha256) == 0;
}

BOOL urltools_find(int tool, wchar_t *out, size_t cap)
{
    wchar_t dir[SP_MAX_PATH];
    for (int portable = 1; portable >= 0; portable--) {
        tools_dir(dir, SP_COUNTOF(dir), portable);
        path_join(out, cap, dir, k_urlTools[tool].file);
        if (tool_ok(out, &k_urlTools[tool])) return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

BOOL urltools_present(void)
{
    wchar_t dir[SP_MAX_PATH], p[SP_MAX_PATH];
    for (int i = 0; i < TOOL_COUNT; i++) {
        int found = 0;
        for (int portable = 1; portable >= 0 && !found; portable--) {
            tools_dir(dir, SP_COUNTOF(dir), portable);
            path_join(p, SP_COUNTOF(p), dir, k_urlTools[i].file);
            found = file_exists(p);
        }
        if (!found) return FALSE;
    }
    return TRUE;
}

/* Downloads t into dest (through dest.download); nothing is kept that isn't the pinned file. */
BOOL fetch_pinned(const UrlTool *t, const wchar_t *dest, FetchProgress cb, void *ctx, wchar_t *err, size_t errCap)
{
    wchar_t part[SP_MAX_PATH], why[200];
    char hex[65] = "";
    str_printf(part, SP_COUNTOF(part), L"%ls.download", dest);
    HttpGet *g = http_open(t->url, 0, why, SP_COUNTOF(why));
    BOOL ok = g && http_status(g) == 200;
    if (g && !ok) str_printf(why, SP_COUNTOF(why), L"the server answered HTTP %d", http_status(g));
    if (ok) ok = http_save(g, part, t->size, cb, ctx, hex, why, SP_COUNTOF(why));
    http_close(g);
    if (ok && strcmp(hex, t->sha256) != 0) {   /* never keep (or run) a binary that isn't the pinned one */
        str_copy(why, SP_COUNTOF(why), L"the file did not match its published SHA-256 checksum");
        ok = FALSE;
    }
    if (ok && !MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        str_printf(why, SP_COUNTOF(why), L"it could not be saved (error %lu)", GetLastError());
        ok = FALSE;
    }
    if (!ok) {
        DeleteFileW(part);
        str_printf(err, errCap, L"Could not download %ls: %ls", t->name, why);
    }
    return ok;
}

typedef struct InstallProgress {
    FetchProgress cb;
    void         *ctx;
    int64_t       base, total;
} InstallProgress;

static int install_progress(void *c, int64_t done, int64_t total)
{
    InstallProgress *p = (InstallProgress *)c;
    (void)total;
    return !p->cb || p->cb(p->ctx, p->base + done, p->total);
}

BOOL urltools_install(FetchProgress cb, void *ctx, wchar_t *err, size_t errCap)
{
    wchar_t dir[SP_MAX_PATH], path[SP_MAX_PATH];
    int need[TOOL_COUNT];
    InstallProgress p = { cb, ctx, 0, 0 };
    for (int i = 0; i < TOOL_COUNT; i++) {
        need[i] = !urltools_find(i, path, SP_COUNTOF(path));
        if (need[i]) p.total += k_urlTools[i].size;
    }
    if (!p.total) return TRUE;
    tools_dir(dir, SP_COUNTOF(dir), 0);
    if (!dir_create_recursive(dir)) {
        str_printf(err, errCap, L"Could not create %ls (error %lu)", dir, GetLastError());
        return FALSE;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (!need[i]) continue;
        const UrlTool *t = &k_urlTools[i];
        path_join(path, SP_COUNTOF(path), dir, t->file);
        if (!fetch_pinned(t, path, install_progress, &p, err, errCap)) return FALSE;
        p.base += t->size;
    }
    return TRUE;
}

void urltools_remove(void)
{
    wchar_t dir[SP_MAX_PATH], p[SP_MAX_PATH];
    tools_dir(dir, SP_COUNTOF(dir), 0);
    for (int i = 0; i < TOOL_COUNT; i++) {
        path_join(p, SP_COUNTOF(p), dir, k_urlTools[i].file);
        DeleteFileW(p);
        str_cat(p, SP_COUNTOF(p), L".download");
        DeleteFileW(p);
    }
    RemoveDirectoryW(dir);
}

/* ---------------------------------------------------------------- Visual C++ runtime */

#define VCRUN_KEY       L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64"
#define VCRUN_MIN_MINOR 44     /* ONNX Runtime 1.24.4 is built with the 14.44 toolset; keep installer/screenplus.nsi in step */

/* Microsoft's own installer, the build https://aka.ms/vs/17/release/vc_redist.x64.exe pointed to on 2026-10-26
 * (14.44.35211, signed by Microsoft; the address carries the SHA-256). Newer runtimes run 14.44 programs too. */
static const UrlTool k_vcRedist = {
    L"VC_redist.x64.exe", L"Microsoft Visual C++ Redistributable 14.44",
    L"https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/"
    L"CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe",
    "cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b", 25635768 };

BOOL vcruntime_ok(wchar_t *found, size_t cap)
{
    HKEY k;
    DWORD v[4] = { 0 }, size;                     /* Installed, Major, Minor, Bld */
    static const wchar_t *names[4] = { L"Installed", L"Major", L"Minor", L"Bld" };
    if (found && cap) found[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VCRUN_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return FALSE;
    for (int i = 0; i < 4; i++) {
        size = sizeof v[i];
        if (RegQueryValueExW(k, names[i], NULL, NULL, (BYTE *)&v[i], &size) != ERROR_SUCCESS) v[i] = 0;
    }
    RegCloseKey(k);
    if (v[0] != 1) return FALSE;
    if (found && cap) str_printf(found, cap, L"%lu.%lu.%lu", v[1], v[2], v[3]);
    return v[1] > 14 || (v[1] == 14 && v[2] >= VCRUN_MIN_MINOR);
}

BOOL vcruntime_fetch(const wchar_t *dest, FetchProgress cb, void *ctx, wchar_t *err, size_t errCap)
{
    return fetch_pinned(&k_vcRedist, dest, cb, ctx, err, errCap);
}

BOOL vcruntime_run(const wchar_t *installer, wchar_t *err, size_t errCap)
{
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";                        /* it needs administrator rights: Windows asks first */
    sei.lpFile = installer;
    sei.lpParameters = L"/install /passive /norestart";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        if (GetLastError() == ERROR_CANCELLED) str_copy(err, errCap, L"Windows' administrator prompt was declined");
        else str_printf(err, errCap, L"The installer could not be started (error %lu)", GetLastError());
        return FALSE;
    }
    DWORD code = 1;
    WaitForSingleObject(sei.hProcess, 15 * 60 * 1000);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code == 0 || code == 3010 || code == 1638) return TRUE;   /* installed; installed, restart advised; a newer one is there */
    str_printf(err, errCap, L"The Visual C++ installer stopped with code %lu", code);
    return FALSE;
}
