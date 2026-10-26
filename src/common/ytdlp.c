#include "ytdlp.h"
#include "webfetch.h"
#include "util.h"
#include "paths.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

/* Stopping: end yt-dlp's worker processes but not its launcher, which then deletes the temporary
 * files of the one-file build (killing it first would leave ~40 MB in %TEMP% each time). */
static void stop_tree(HANDLE job, DWORD launcher)
{
    struct { JOBOBJECT_BASIC_PROCESS_ID_LIST l; ULONG_PTR more[63]; } ids;
    int killed = 0;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, &ids, sizeof ids, NULL)) {
        for (DWORD i = 0; i < ids.l.NumberOfProcessIdsInList; i++) {
            DWORD pid = (DWORD)ids.l.ProcessIdList[i];
            HANDLE p = pid != launcher ? OpenProcess(PROCESS_TERMINATE, FALSE, pid) : NULL;
            if (p) {
                TerminateProcess(p, 1);
                CloseHandle(p);
                killed++;
            }
        }
    }
    if (!killed) TerminateJobObject(job, 1);
}

int ytdlp_run(const wchar_t *args, const wchar_t *workDir, HANDLE cancel, volatile LONG *quit,
              YtLine cb, void *ctx, wchar_t *err, size_t errCap)
{
    wchar_t ytdlp[SP_MAX_PATH], qjs[SP_MAX_PATH], cache[SP_MAX_PATH], pipeName[96];
    if (!urltools_find(TOOL_YTDLP, ytdlp, SP_COUNTOF(ytdlp)) || !urltools_find(TOOL_QJS, qjs, SP_COUNTOF(qjs))) {
        str_copy(err, errCap, L"yt-dlp is not installed. Turn on 'Import wallpapers from web links' in Settings.");
        return -1;
    }
    path_join(cache, SP_COUNTOF(cache), paths_cache(), L"yt-dlp");
    size_t cmdCap = wcslen(args) + 4 * SP_MAX_PATH;
    wchar_t *cmd = (wchar_t *)malloc(cmdCap * sizeof(wchar_t));
    if (!cmd) {
        str_copy(err, errCap, L"Out of memory.");
        return -1;
    }
    /* --encoding: the one-file build ignores PYTHON* variables, and its console encoding drops
     * characters (titles, file names) it can't represent */
    str_printf(cmd, cmdCap,
               L"\"%ls\" --ignore-config --no-update --no-playlist --encoding utf-8 --color no_color --no-js-runtimes "
               L"--js-runtimes \"quickjs:%ls\" --cache-dir \"%ls\" --socket-timeout 20 %ls",
               ytdlp, qjs, cache, args);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    str_printf(pipeName, SP_COUNTOF(pipeName), L"\\\\.\\pipe\\ScreenPlus.yt-dlp.%lu.%lu.%llu", GetCurrentProcessId(),
               GetCurrentThreadId(), (unsigned long long)GetTickCount64());
    HANDLE rd = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1 << 16, 0, NULL);
    HANDLE wr = rd != INVALID_HANDLE_VALUE ? CreateFileW(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL)
                                           : INVALID_HANDLE_VALUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    ZeroMemory(&li, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   /* nothing outlives us */
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);

    /* the child inherits exactly its three standard handles */
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = wr;
    HANDLE inherit[2] = { nul, wr };
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(asz);
    BOOL attrOk = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &asz);
    if (attrOk && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL))
        si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL started = rd != INVALID_HANDLE_VALUE && wr != INVALID_HANDLE_VALUE && nul != INVALID_HANDLE_VALUE && job &&
                   si.lpAttributeList &&
                   CreateProcessW(ytdlp, cmd, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                                  NULL, workDir, &si.StartupInfo, &pi);
    DWORD startErr = GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(attrs);
    free(attrs);
    free(cmd);
    if (wr != INVALID_HANDLE_VALUE) CloseHandle(wr);   /* EOF arrives once every yt-dlp process has exited */
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
        if (job) CloseHandle(job);
        str_printf(err, errCap, L"Could not start yt-dlp (error %lu).", startErr);
        return -1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    char buf[8192];
    size_t have = 0;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE waits[2] = { ov.hEvent, cancel };
    int pending = 0, stopping = 0;
    while (ov.hEvent) {
        if (!pending) {
            ResetEvent(ov.hEvent);
            if (!ReadFile(rd, buf + have, (DWORD)(sizeof buf - 1 - have), NULL, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            pending = 1;
        }
        DWORD w = WaitForMultipleObjects(!stopping && cancel ? 2 : 1, waits, FALSE,
                                         stopping ? 8000 : quit ? 250 : INFINITE);
        if (!stopping && (w == WAIT_OBJECT_0 + 1 || (w == WAIT_TIMEOUT && quit && *quit))) {
            stopping = 1;
            stop_tree(job, pi.dwProcessId);
            continue;
        }
        if (w == WAIT_TIMEOUT && !stopping) continue;
        if (w != WAIT_OBJECT_0) {                  /* the launcher didn't finish its clean-up in time */
            TerminateJobObject(job, 1);
            break;
        }
        DWORD got = 0;
        pending = 0;
        if (!GetOverlappedResult(rd, &ov, &got, FALSE)) break;   /* broken pipe: everything exited */
        have += got;
        buf[have] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
            if (cb && !stopping) cb(ctx, line);
            line = nl + 1;
        }
        have = (size_t)(buf + have - line);
        if (have == sizeof buf - 1) have = 0;      /* an endless line: drop it */
        memmove(buf, line, have);
    }
    if (pending) {
        DWORD got = 0;
        CancelIoEx(rd, &ov);
        GetOverlappedResult(rd, &ov, &got, TRUE);
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(rd);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateJobObject(job, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    if (stopping) {
        str_copy(err, errCap, L"Cancelled");
        return -1;
    }
    return (int)code;
}

void ytdlp_format(wchar_t *out, size_t cap, int maxRes, int stream)
{
    if (stream)
        str_printf(out, cap,
                   L"-f \"bv*[protocol~='^https?$'][vcodec!^=?av01][dynamic_range=?SDR]/bv*[protocol~='^https?$'][vcodec!^=?av01]/"
                   L"bv*[protocol~='^https?$']/b[protocol~='^https?$']/bv*[vcodec!^=?av01]/bv*/b\" -S \"res:%d\"", maxRes);
    else
        str_printf(out, cap, L"-f \"bv*[vcodec!^=?av01][dynamic_range=?SDR]/bv*[vcodec!^=?av01]/bv*/b\" -S \"res:%d\"", maxRes);
}

void ytdlp_tidy_error(wchar_t *s)
{
    static const wchar_t *advice[] = { L" Use --", L" See  http", L" See http", L"; please report", L" Confirm you are on" };
    wchar_t *p = s, *e;
    if (*p == L'[' && (e = wcsstr(p, L"] ")) != NULL) {
        p = e + 2;
        wchar_t *colon = wcsstr(p, L": ");
        if (colon && wcschr(p, L' ') == colon + 1) p = colon + 2;    /* "<video id>: " */
    }
    for (size_t i = 0; i < SP_COUNTOF(advice); i++)
        if ((e = wcsstr(p, advice[i])) != NULL) *e = 0;
    memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
}

/* ---------------------------------------------------------------- streams */

static void put_wide(wchar_t *dst, size_t cap, const char *utf8)
{
    wchar_t *w = wide_from_utf8(utf8, -1);
    str_copy(dst, cap, w && wcscmp(w, L"NA") ? w : L"");
    free(w);
}

static void resolve_line(void *ctx, char *line)
{
    YtStream *s = (YtStream *)ctx;
    if (strncmp(line, "[sp-url] ", 9) == 0) {
        put_wide(s->url, SP_COUNTOF(s->url), line + 9);
    } else if (strncmp(line, "[sp-fmt] ", 9) == 0) {
        char *sp = strchr(line + 9, ' ');
        if (sp) {
            *sp = 0;
            put_wide(s->protocol, SP_COUNTOF(s->protocol), sp + 1);
        }
        put_wide(s->format, SP_COUNTOF(s->format), line + 9);
    } else if (strncmp(line, "[sp-meta] ", 10) == 0) {
        char *end = NULL;
        s->duration = strtod(line + 10, &end);
        if (end && *end == ' ' && end[1] >= '0' && end[1] <= '9') s->size = (int64_t)strtod(end + 1, NULL);
        s->live = strstr(line + 10, " True") != NULL;
    } else if (strncmp(line, "[sp-title] ", 11) == 0) {
        put_wide(s->title, SP_COUNTOF(s->title), line + 11);
    } else if (strncmp(line, "[sp-thumb] ", 11) == 0) {
        put_wide(s->thumb, SP_COUNTOF(s->thumb), line + 11);
    } else if (strncmp(line, "[sp-hdr] ", 9) == 0) {
        JVal *j = json_parse(line + 9, strlen(line + 9));
        for (int i = 0; j && j->t == J_OBJ && i < j->n; i++) {
            const char *k = j->keys[i], *v = j->kids[i].t == J_STR ? j->kids[i].str : NULL;
            if (!v || strpbrk(k, "\r\n:") || strpbrk(v, "\r\n") || !_stricmp(k, "Accept-Encoding") || !_stricmp(k, "Range") ||
                !_stricmp(k, "Host"))
                continue;
            wchar_t *wk = wide_from_utf8(k, -1), *wv = wide_from_utf8(v, -1);
            if (wk && wv && wcslen(s->headers) + wcslen(wk) + wcslen(wv) + 5 < SP_COUNTOF(s->headers)) {
                str_cat(s->headers, SP_COUNTOF(s->headers), wk);
                str_cat(s->headers, SP_COUNTOF(s->headers), L": ");
                str_cat(s->headers, SP_COUNTOF(s->headers), wv);
                str_cat(s->headers, SP_COUNTOF(s->headers), L"\r\n");
            }
            free(wk);
            free(wv);
        }
        json_free(j);
    } else if (strncmp(line, "ERROR: ", 7) == 0) {
        put_wide(s->error, SP_COUNTOF(s->error), line + 7);
    }
}

BOOL ytdlp_resolve(const wchar_t *page, int maxRes, const wchar_t *formatId, HANDLE cancel, volatile LONG *quit,
                   YtStream *out)
{
    ZeroMemory(out, sizeof *out);
    out->size = -1;
    wchar_t fmt[400];
    if (formatId && formatId[0]) str_printf(fmt, SP_COUNTOF(fmt), L"-f \"%ls\"", formatId);
    else ytdlp_format(fmt, SP_COUNTOF(fmt), maxRes, 1);
    size_t cap = wcslen(page) + 1024;
    wchar_t *args = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!args) return FALSE;
    str_printf(args, cap,
               L"--simulate %ls --print \"[sp-url] %%(url)s\" --print \"[sp-fmt] %%(format_id)s %%(protocol)s\" "
               L"--print \"[sp-meta] %%(duration)s %%(filesize,filesize_approx)s %%(is_live)s\" --print \"[sp-title] %%(title)s\" "
               L"--print \"[sp-thumb] %%(thumbnail)s\" --print \"[sp-hdr] %%(http_headers)j\" -- \"%ls\"",
               fmt, page);
    wchar_t why[300] = L"";
    int code = ytdlp_run(args, paths_cache(), cancel, quit, resolve_line, out, why, SP_COUNTOF(why));
    free(args);
    if (code != 0) {
        out->url[0] = 0;
        if (!out->error[0]) {
            if (why[0]) str_copy(out->error, SP_COUNTOF(out->error), why);
            else str_printf(out->error, SP_COUNTOF(out->error), L"yt-dlp stopped unexpectedly (exit code %d).", code);
        }
        out->unsupported = wcsstr(out->error, L"Unsupported URL") != NULL;
        ytdlp_tidy_error(out->error);
        return FALSE;
    }
    out->streamable = str_istarts(out->url, L"http") && (str_ieq(out->protocol, L"https") || str_ieq(out->protocol, L"http"));
    const wchar_t *ex = wcsstr(out->url, L"expire=");     /* YouTube addresses say when they stop working */
    if (ex && (ex[-1] == L'?' || ex[-1] == L'&')) out->expires = _wtoi64(ex + 7);
    return TRUE;
}

void ytdlp_cache_path(const wchar_t *page, const wchar_t *ext, wchar_t *out, size_t cap)
{
    wchar_t name[40];
    str_printf(name, SP_COUNTOF(name), L"streams\\%08x%ls", hash_fnv1a_w(page), ext);
    path_join(out, cap, paths_cache(), name);
}

void ytdlp_cache_save(const wchar_t *page, const YtStream *s)
{
    wchar_t path[SP_MAX_PATH], dir[SP_MAX_PATH], tmp[SP_MAX_PATH];
    if (!s->url[0]) return;
    ytdlp_cache_path(page, L".url", path, SP_COUNTOF(path));
    path_dirname(dir, SP_COUNTOF(dir), path);
    dir_create_recursive(dir);
    StrBuf b = { 0 };
    sb_printf(&b, "v1\n%lld\n%lld\n", (long long)s->expires, (long long)s->size);
    const wchar_t *fields[] = { page, s->format, s->protocol, s->url };
    for (size_t i = 0; i < SP_COUNTOF(fields); i++) {
        sb_put_wide(&b, fields[i]);
        sb_puts(&b, "\n");
    }
    sb_put_wide(&b, s->headers);                   /* one header per line */
    str_printf(tmp, SP_COUNTOF(tmp), L"%ls.tmp", path);
    HANDLE f = b.failed ? INVALID_HANDLE_VALUE : CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        BOOL ok = WriteFile(f, b.p, (DWORD)b.len, &w, NULL) && w == b.len;
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
    }
    sb_free(&b);
}

BOOL ytdlp_cache_load(const wchar_t *page, YtStream *out)
{
    wchar_t path[SP_MAX_PATH];
    ZeroMemory(out, sizeof *out);
    out->size = -1;
    ytdlp_cache_path(page, L".url", path, SP_COUNTOF(path));
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    char raw[24576];
    DWORD n = 0;
    BOOL read = ReadFile(f, raw, sizeof raw - 1, &n, NULL);
    CloseHandle(f);
    if (!read) return FALSE;
    raw[n] = 0;
    wchar_t *text = wide_from_utf8(raw, -1);
    if (!text) return FALSE;
    wchar_t *lines[64];
    int count = 0;
    for (wchar_t *p = text; p && count < (int)SP_COUNTOF(lines);) {
        lines[count++] = p;
        p = wcschr(p, L'\n');
        if (p) *p++ = 0;
    }
    BOOL ok = count >= 7 && wcscmp(lines[0], L"v1") == 0 && str_ieq(lines[3], page);
    if (ok) {
        out->expires = _wtoi64(lines[1]);
        out->size = _wtoi64(lines[2]);
        str_copy(out->format, SP_COUNTOF(out->format), lines[4]);
        str_copy(out->protocol, SP_COUNTOF(out->protocol), lines[5]);
        str_copy(out->url, SP_COUNTOF(out->url), lines[6]);
        for (int i = 7; i < count; i++) {
            size_t l = wcslen(lines[i]);
            if (l && lines[i][l - 1] == L'\r') lines[i][--l] = 0;
            if (!l) continue;
            str_cat(out->headers, SP_COUNTOF(out->headers), lines[i]);
            str_cat(out->headers, SP_COUNTOF(out->headers), L"\r\n");
        }
        /* reused only while the address has a while left (YouTube's last about six hours) */
        ok = out->url[0] && out->size > 0 && (!out->expires || out->expires > time_unix_now() + 900);
        out->streamable = ok;
    }
    free(text);
    return ok;
}
