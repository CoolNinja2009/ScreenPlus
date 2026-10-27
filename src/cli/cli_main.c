/* ScreenPlus.com: Screen+ from a terminal (cmd, PowerShell, Windows Terminal).
 *
 * The console twin of ScreenPlus.exe, installed next to it. A terminal resolves `screenplus` to
 * the .com first (PATHEXT order) while shortcuts and the startup entry name the .exe, the same
 * arrangement as devenv.com/devenv.exe. Commands run inside the running app: each one is sent as
 * IPC_CLI_REQUEST and the app's IPC_CLI_REPLY is printed (src/app/cli_server.c). A few work
 * without the app: help, version, start, logs, paths, path, tools, vcredist, screensaver (but use). No arguments: an interactive prompt.
 *
 * Exit codes: 0 ok, 1 the command failed, 2 bad usage, 3 Screen+ is not running. */
#include "../common/common.h"
#include "../common/ipc.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../common/paths.h"
#include "../common/webfetch.h"
#include "../common/scrnsave.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_NOT_RUNNING = 3 };

#define MAX_ARGS 64

static HANDLE        g_out, g_err, g_in;
static int           g_outCon, g_errCon, g_inCon, g_color;
static int           g_json;               /* --json: print the reply's data as JSON */
static int           g_quiet;              /* --quiet: no output (installer calls) */
static volatile LONG g_interrupted;        /* Ctrl+C while watching/following */
static volatile LONG g_catchCtrl;          /* the prompt or a watch loop is running: Ctrl+C stops that */

/* ------------------------------------------------------------------ console output */

static void write_to(HANDLE h, int con, const wchar_t *s, size_t n)
{
    DWORD w;
    if (!n || !h || h == INVALID_HANDLE_VALUE) return;
    if (con) {
        WriteConsoleW(h, s, (DWORD)n, &w, NULL);
        return;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, NULL, 0, NULL, NULL);   /* redirected: UTF-8 */
    char *u = len > 0 ? (char *)malloc((size_t)len) : NULL;
    if (!u) return;
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, u, len, NULL, NULL);
    WriteFile(h, u, (DWORD)len, &w, NULL);
    free(u);
}

static void vput(HANDLE h, int con, const wchar_t *fmt, va_list ap)
{
    wchar_t buf[4096];
    int n = _vsnwprintf_s(buf, SP_COUNTOF(buf), _TRUNCATE, fmt, ap);
    write_to(h, con, buf, n < 0 ? wcslen(buf) : (size_t)n);
}

static void out(const wchar_t *fmt, ...)
{
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vput(g_out, g_outCon, fmt, ap);
    va_end(ap);
}

static void err(const wchar_t *fmt, ...)
{
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vput(g_err, g_errCon, fmt, ap);
    va_end(ap);
}

static void out_utf8(const char *s, int toErr)
{
    if (g_quiet || !s || !s[0]) return;
    wchar_t *w = wide_from_utf8(s, -1);
    if (!w) return;
    if (toErr) write_to(g_err, g_errCon, w, wcslen(w));
    else write_to(g_out, g_outCon, w, wcslen(w));
    free(w);
}

/* ANSI colours, only on a real console with VT processing (and no NO_COLOR) */
static const wchar_t *c_red(void)   { return g_color ? L"\x1b[91m" : L""; }
static const wchar_t *c_green(void) { return g_color ? L"\x1b[92m" : L""; }
static const wchar_t *c_dim(void)   { return g_color ? L"\x1b[90m" : L""; }
static const wchar_t *c_bold(void)  { return g_color ? L"\x1b[1m" : L""; }
static const wchar_t *c_off(void)   { return g_color ? L"\x1b[0m" : L""; }

static BOOL WINAPI on_ctrl(DWORD type)
{
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && g_catchCtrl) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE;                       /* loops stop; the prompt survives */
    }
    return FALSE;                          /* a one-shot command: default handling ends it */
}

static void console_init(void)
{
    DWORD mode = 0;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = GetStdHandle(STD_ERROR_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_outCon = g_out && GetConsoleMode(g_out, &mode);
    if (g_outCon && SetConsoleMode(g_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) g_color = 1;
    g_errCon = g_err && GetConsoleMode(g_err, &mode);
    if (g_errCon) SetConsoleMode(g_err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    g_inCon = g_in && GetConsoleMode(g_in, &mode);
    if (GetEnvironmentVariableW(L"NO_COLOR", NULL, 0)) g_color = 0;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}

/* ------------------------------------------------------------------ talking to the app */

static char  *g_reply;          /* last IPC_CLI_REPLY payload, NUL-terminated */
static size_t g_replyLen;

static LRESULT CALLBACK reply_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds->dwData != IPC_CLI_REPLY || !cds->lpData || !cds->cbData) return FALSE;
        free(g_reply);
        g_reply = (char *)malloc((size_t)cds->cbData + 1);
        if (!g_reply) return FALSE;
        memcpy(g_reply, cds->lpData, cds->cbData);
        g_reply[cds->cbData] = 0;
        g_replyLen = cds->cbData;
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND reply_window(void)
{
    static HWND w;
    if (w) return w;
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = reply_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SP_CLI_CLASS;
    RegisterClassExW(&wc);
    w = CreateWindowExW(0, SP_CLI_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (w) ChangeWindowMessageFilterEx(w, WM_COPYDATA, MSGFLT_ALLOW, NULL);   /* elevated terminal, normal app */
    return w;
}

static HWND app_window(void) { return FindWindowW(SP_SHELL_CLASS, NULL); }

/* path-looking arguments are made absolute: the app resolves nothing against our folder */
static int looks_like_path(const wchar_t *a)
{
    return wcschr(a, L'\\') || wcschr(a, L'/') || wcschr(a, L':') || a[0] == L'.';
}

/* Sends argv to the app. *reply gets the parsed answer; returns an exit code. */
static int request(int argc, wchar_t **argv, JVal **reply)
{
    *reply = NULL;
    HWND app = app_window();
    if (!app) {
        err(L"%lsScreen+ is not running.%ls Start it with: screenplus start\n", c_red(), c_off());
        return EXIT_NOT_RUNNING;
    }
    HWND me = reply_window();
    if (!me) {
        err(L"%lsCould not open a reply channel (error %lu).%ls\n", c_red(), GetLastError(), c_off());
        return EXIT_FAILED;
    }
    int pathArgs = argc && (str_ieq(argv[0], L"import") || str_ieq(argv[0], L"add"));
    int maybePath = argc && (str_ieq(argv[0], L"apply") || str_ieq(argv[0], L"set"));
    StrBuf b = { 0 };
    sb_puts(&b, "{\"argv\":[");
    for (int i = 0; i < argc; i++) {
        wchar_t full[SP_MAX_PATH];
        const wchar_t *a = argv[i];
        if (i && a[0] != L'-' && !str_istarts(a, L"https://") && !str_istarts(a, L"http://") &&
            (pathArgs || (maybePath && looks_like_path(a))) &&
            GetFullPathNameW(a, SP_COUNTOF(full), full, NULL))
            a = full;
        char *u = utf8_from_wide(a, NULL);
        if (i) sb_puts(&b, ",");
        json_write_string(&b, u ? u : "");
        free(u);
    }
    sb_puts(&b, "]}");
    free(g_reply);
    g_reply = NULL;
    COPYDATASTRUCT cds = { IPC_CLI_REQUEST, (DWORD)b.len, b.p };
    DWORD_PTR res = 0;
    LRESULT sent = b.failed ? 0 : SendMessageTimeoutW(app, WM_COPYDATA, (WPARAM)me, (LPARAM)&cds,
                                                      SMTO_NORMAL | SMTO_ABORTIFHUNG, 30000, &res);
    sb_free(&b);
    if (!sent) {
        err(L"%lsScreen+ did not answer (error %lu).%ls\n", c_red(), GetLastError(), c_off());
        return EXIT_FAILED;
    }
    if (!res || !g_reply) {
        /* an app from before the terminal interface: the switches it always understood still work */
        static const struct { const wchar_t *cmd; uint32_t id; const char *done; } legacy[] = {
            { L"show", CMD_SHOW_UI, "Window opened.\n" },         { L"pause", CMD_PAUSE, "Wallpapers paused.\n" },
            { L"resume", CMD_RESUME, "Wallpapers playing.\n" },   { L"toggle", CMD_TOGGLE_PAUSE, "Pause toggled.\n" },
            { L"next", CMD_NEXT_WALLPAPER, "Next wallpaper.\n" }, { L"stop", CMD_EXIT, "Screen+ is closing.\n" },
        };
        for (size_t i = 0; argc && i < SP_COUNTOF(legacy); i++) {
            if (!str_ieq(argv[0], legacy[i].cmd)) continue;
            IpcCommand c = { legacy[i].id, 0 };
            if (!ipc_send(me, app, IPC_SHELL_CMD, &c, sizeof c, 3000)) break;
            StrBuf r = { 0 };
            sb_puts(&r, "{\"ok\":true,\"text\":");
            json_write_string(&r, legacy[i].done);
            sb_puts(&r, ",\"data\":null}");
            *reply = r.failed ? NULL : json_parse(r.p, r.len);
            sb_free(&r);
            return *reply ? EXIT_OK : EXIT_FAILED;
        }
        err(L"%lsThe running Screen+ is an older version without terminal commands.%ls\n"
            L"Restart it with this one: screenplus stop, then screenplus start\n", c_red(), c_off());
        return EXIT_FAILED;
    }
    *reply = json_parse(g_reply, g_replyLen);
    if (!*reply) {
        err(L"%lsScreen+ sent an unreadable answer.%ls\n", c_red(), c_off());
        return EXIT_FAILED;
    }
    return EXIT_OK;
}

static int print_reply(const JVal *rep)
{
    int ok = json_bool(json_get(rep, "ok"), 0);
    const char *text = json_str(json_get(rep, "text"), "");
    if (g_json) {
        StrBuf b = { 0 };
        if (ok) {
            json_write(&b, json_get(rep, "data"));
        } else {
            sb_puts(&b, "{\"error\":");
            json_write_string(&b, text);
            sb_puts(&b, "}");
        }
        sb_puts(&b, "\n");
        if (!b.failed) out_utf8(b.p, 0);
        sb_free(&b);
        return ok ? EXIT_OK : EXIT_FAILED;
    }
    if (ok) {
        out_utf8(text, 0);
    } else {
        err(L"%ls", c_red());
        out_utf8(text, 1);
        err(L"%ls", c_off());
    }
    return ok ? EXIT_OK : EXIT_FAILED;
}

static int remote(int argc, wchar_t **argv)
{
    JVal *rep = NULL;
    int rc = request(argc, argv, &rep);
    if (rc == EXIT_OK) rc = print_reply(rep);
    json_free(rep);
    return rc;
}

/* ------------------------------------------------------------------ help */

static const wchar_t k_help[] =
    L"Screen+ command line\n"
    L"\n"
    L"Usage: screenplus <command> [arguments] [--json]\n"
    L"       screenplus                        interactive prompt ('exit' leaves it)\n"
    L"\n"
    L"Playback\n"
    L"  status [--watch]                       what runs on each display; --watch refreshes every second\n"
    L"  pause | resume | toggle                pause or resume all wallpapers\n"
    L"  next                                   the next wallpaper in the library\n"
    L"  audio [on | off | toggle]              wallpaper sound\n"
    L"  restart                                restart the renderer process\n"
    L"\n"
    L"Library\n"
    L"  list [filter]                          wallpapers (* = on screen); filter: image video html lively\n"
    L"                                         missing active, or part of a name\n"
    L"  apply <wallpaper> [--display N]        by name, number from 'list', id, or a file/folder path\n"
    L"  import <path>... [--apply]             add files or folders to the library\n"
    L"  import <link> [--download | --page]    a web link: long videos stream, short ones and images are saved\n"
    L"  remove <wallpaper>                     remove from the library (the file stays)\n"
    L"  displays                               monitors and what they show\n"
    L"\n"
    L"Customise (the wallpaper on screen, or the one on --display N)\n"
    L"  look                                   colour grading\n"
    L"  look set <key> <value> ...             brightness contrast saturation gamma (-100..100),\n"
    L"                                         hue (-180..180), tint #RRGGBB, tint-strength tint-brightness (0..100)\n"
    L"  look reset                             back to neutral\n"
    L"  props                                  the wallpaper's own settings (Lively / Wallpaper Engine)\n"
    L"  props set <name> <value>               e.g. props set rainIntensity 80 | props set Scaling Fill\n"
    L"  props reset                            the wallpaper's defaults\n"
    L"\n"
    L"App\n"
    L"  start [--show]                         start Screen+ if it is not running\n"
    L"  show | hide                            open or close the window\n"
    L"  stop                                   quit Screen+\n"
    L"  settings [set <key> <value>]           app settings, e.g. settings set pause_on_battery off\n"
    L"  logs [app | engine] [-n N] [-f]        recent log lines (both logs by default); -f keeps following\n"
    L"  paths                                  where Screen+ keeps its data, config and logs\n"
    L"  path [add | remove]                    put screenplus on your PATH (current user)\n"
    L"  tools [install | remove]               yt-dlp + QuickJS-NG for importing videos from web links\n"
    L"  vcredist [install]                     the Microsoft Visual C++ runtime that Make wallpaper cooler needs\n"
    L"  screensaver [on | off | preview]       Screen+ as the Windows screensaver\n"
    L"  screensaver timeout <minutes>          idle time before it starts (a Windows setting)\n"
    L"  screensaver lock [on | off]            sign-in screen when it ends (a Windows setting)\n"
    L"  screensaver use <wallpaper | desktop>  its wallpaper (needs the app running)\n"
    L"  ping | version | help [command]\n"
    L"\n"
    L"Options\n"
    L"  --json                                 machine-readable output, e.g. screenplus status --json | ConvertFrom-Json\n"
    L"  --display N, -d N                      one display (1-based, see 'displays')\n"
    L"\n"
    L"Exit codes: 0 ok, 1 the command failed, 2 bad usage, 3 Screen+ is not running.\n";

static int cmd_help(const wchar_t *topic)
{
    if (!topic || !topic[0]) {
        out(L"%ls", k_help);
        return EXIT_OK;
    }
    /* help <command>: the lines that document it (and their continuation lines) */
    int shown = 0, cont = 0;
    const wchar_t *p = k_help;
    while (*p) {
        const wchar_t *e = wcschr(p, L'\n');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        wchar_t line[256];
        str_copyn(line, SP_COUNTOF(line), p, n);
        int indent = line[0] == L' ' && line[1] == L' ' && line[2] != L' ';
        int match = indent && str_istarts(line + 2, topic);
        if (match || (cont && !indent && line[0] == L' ')) {
            out(L"%ls\n", line);
            shown++;
            cont = 1;
        } else {
            cont = match;
        }
        p += n + (e ? 1 : 0);
    }
    if (!shown) {
        err(L"No command \"%ls\". See: screenplus help\n", topic);
        return EXIT_USAGE;
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ local commands */

static void sibling_exe(wchar_t *out, size_t cap)
{
    path_join(out, cap, paths_exe_dir(), L"ScreenPlus.exe");
}

static int cmd_version(void)
{
    out(L"Screen+ %ls (command line)\n", SP_VERSION_W);
    HWND app = app_window();
    DWORD pid = 0;
    if (app) GetWindowThreadProcessId(app, &pid);
    if (app) out(L"The app is running (pid %lu).\n", pid);
    else out(L"The app is not running.\n");
    return EXIT_OK;
}

static int cmd_start(int show)
{
    if (app_window()) {
        if (show) {
            wchar_t *argv[] = { L"show" };
            return remote(1, argv);
        }
        out(L"Screen+ is already running.\n");
        return EXIT_OK;
    }
    wchar_t exe[SP_MAX_PATH], cmd[SP_MAX_PATH + 32];
    sibling_exe(exe, SP_COUNTOF(exe));
    if (!file_exists(exe)) {
        err(L"%lsScreenPlus.exe was not found next to this program (%ls).%ls\n", c_red(), exe, c_off());
        return EXIT_FAILED;
    }
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" %ls", exe, show ? L"--show" : L"--tray");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP, NULL, paths_exe_dir(), &si, &pi)) {
        err(L"%lsCould not start Screen+ (error %lu).%ls\n", c_red(), GetLastError(), c_off());
        return EXIT_FAILED;
    }
    CloseHandle(pi.hThread);
    out(L"Starting Screen+...");
    int up = 0;
    for (int i = 0; i < 150 && !up; i++) {           /* the app answers once its window exists */
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (i % 10 == 9) out(L".");
        up = app_window() != NULL;
    }
    CloseHandle(pi.hProcess);
    if (!up) {
        err(L"\n%lsScreen+ did not come up. See: screenplus logs app%ls\n", c_red(), c_off());
        return EXIT_FAILED;
    }
    out(L" %lsrunning%ls.\n", c_green(), c_off());
    return EXIT_OK;
}

static int cmd_paths(void)
{
    wchar_t props[SP_MAX_PATH];
    path_join(props, SP_COUNTOF(props), paths_root(), L"props");
    out(L"Data       %ls\n", paths_root());
    out(L"Config     %ls\n", paths_config());
    out(L"Logs       %ls\n", paths_logs());
    out(L"Customise  %ls  (one <wallpaper id>.json each)\n", props);
    out(L"Cache      %ls\n", paths_cache());
    out(L"Downloads  %ls  (wallpapers imported from web links)\n", paths_downloads());
    out(L"Program    %ls\n", paths_exe_dir());
    return EXIT_OK;
}

/* ---- tools: the optional URL-import downloaders (setup runs `tools install` when chosen) */

static int tools_progress(void *ctx, int64_t done, int64_t total)
{
    int *last = (int *)ctx, pct = total > 0 ? (int)(done * 100 / total) : 0;
    if (pct / 10 != *last / 10) out(L"  %d%%\n", pct);
    *last = pct;
    return 1;
}

static int cmd_tools(const wchar_t *sub)
{
    wchar_t p[SP_MAX_PATH], e[320];
    if (!sub || !sub[0] || str_ieq(sub, L"status")) {
        for (int i = 0; i < TOOL_COUNT; i++)
            if (urltools_find(i, p, SP_COUNTOF(p))) out(L"%-18ls %ls\n", k_urlTools[i].name, p);
            else out(L"%-18ls not installed (screenplus tools install)\n", k_urlTools[i].name);
        return EXIT_OK;
    }
    if (str_ieq(sub, L"install")) {
        int last = -1;
        out(L"Downloading %ls and %ls (official releases, SHA-256 verified)...\n", k_urlTools[TOOL_YTDLP].name,
            k_urlTools[TOOL_QJS].name);
        if (!urltools_install(tools_progress, &last, e, SP_COUNTOF(e))) {
            err(L"%ls%ls%ls\n", c_red(), e, c_off());
            return EXIT_FAILED;
        }
        out(L"Ready: videos can be imported from web links.\n");
        return EXIT_OK;
    }
    if (str_ieq(sub, L"remove")) {
        urltools_remove();
        out(L"Removed the downloaded tools.\n");
        return EXIT_OK;
    }
    err(L"Usage: screenplus tools [status | install | remove]\n");
    return EXIT_USAGE;
}

/* ---- screensaver: the Windows side works without the app (the uninstaller runs "screensaver off") */
static int cmd_screensaver(int n, wchar_t **argv)
{
    const wchar_t *sub = n > 1 ? argv[1] : L"";
    wchar_t e[256] = L"";
    ScrState st;
    if (str_ieq(sub, L"on") || str_ieq(sub, L"off")) {
        BOOL on = str_ieq(sub, L"on");
        if (!scr_enable(on, e, SP_COUNTOF(e))) {
            err(L"%ls\n", e);
            return EXIT_FAILED;
        }
        if (e[0]) err(L"%ls\n", e);
    } else if (str_ieq(sub, L"timeout") && n > 2) {
        int m = _wtoi(argv[2]);
        if (m < 1 || m > 1440 || !scr_set_timeout(m * 60)) {
            err(L"Usage: screenplus screensaver timeout <1-1440 minutes>\n");
            return EXIT_USAGE;
        }
    } else if (str_ieq(sub, L"lock") && n > 2 && (str_ieq(argv[2], L"on") || str_ieq(argv[2], L"off"))) {
        scr_set_secure(str_ieq(argv[2], L"on"));
    } else if (str_ieq(sub, L"preview")) {
        if (!scr_launch(L"/s")) {
            err(L"Could not start ScreenPlus.exe.\n");
            return EXIT_FAILED;
        }
        out(L"Screensaver preview started: press a key to end it.\n");
        return EXIT_OK;
    } else if (sub[0] && !str_ieq(sub, L"status")) {
        err(L"Usage: screenplus screensaver [status | on | off | preview | timeout <minutes> | lock on|off | use <wallpaper>]\n");
        return EXIT_USAGE;
    }
    scr_query(&st);
    out(L"Screen+ screensaver: %ls\n", st.ours && st.active ? L"on" : st.active ? L"off (Windows uses another one)" : L"off");
    out(L"Starts after %d s idle; sign-in screen on resume: %ls\n", st.timeoutSec, st.secure ? L"on" : L"off");
    if (st.current[0]) out(L"SCRNSAVE.EXE = %ls\n", st.current);
    if (st.policy) out(L"Group Policy sets the screensaver on this PC: Windows may ignore Screen+'s.\n");
    return EXIT_OK;
}

/* ---- vcredist: the Microsoft Visual C++ runtime ONNX Runtime needs (setup offers it when it is missing) */

static int cmd_vcredist(const wchar_t *sub)
{
    wchar_t found[40], e[320], tmp[SP_MAX_PATH];
    BOOL ok = vcruntime_ok(found, SP_COUNTOF(found));
    if (!sub || !sub[0] || str_ieq(sub, L"status")) {
        if (ok) out(L"Microsoft Visual C++ runtime %ls: new enough for Make wallpaper cooler.\n", found);
        else if (found[0]) out(L"Microsoft Visual C++ runtime %ls is older than the 14.44 that Make wallpaper cooler needs (screenplus vcredist install).\n", found);
        else out(L"The Microsoft Visual C++ runtime (x64) is not installed (screenplus vcredist install).\n");
        return ok ? EXIT_OK : EXIT_FAILED;
    }
    if (!str_ieq(sub, L"install")) {
        err(L"Usage: screenplus vcredist [status | install]\n");
        return EXIT_USAGE;
    }
    if (ok) {
        out(L"Already installed (%ls): nothing to do.\n", found);
        return EXIT_OK;
    }
    int last = -1;
    GetTempPathW(SP_COUNTOF(tmp), tmp);
    str_cat(tmp, SP_COUNTOF(tmp), L"ScreenPlus-VC_redist.x64.exe");
    out(L"Downloading the Microsoft Visual C++ Redistributable (x64; official, SHA-256 verified)...\n");
    if (!vcruntime_fetch(tmp, tools_progress, &last, e, SP_COUNTOF(e))) {
        err(L"%ls%ls%ls\n", c_red(), e, c_off());
        return EXIT_FAILED;
    }
    out(L"Installing it: Windows asks for administrator permission...\n");
    BOOL done = vcruntime_run(tmp, e, SP_COUNTOF(e));
    DeleteFileW(tmp);
    if (!done) {
        err(L"%ls%ls%ls\n", c_red(), e, c_off());
        return EXIT_FAILED;
    }
    out(L"Installed.\n");
    return EXIT_OK;
}

/* ---- logs */

static char *read_range(const wchar_t *path, LONGLONG from, LONGLONG *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *buf = NULL;
    if (GetFileSizeEx(f, &sz)) {
        *size = sz.QuadPart;
        if (from > sz.QuadPart) from = 0;                  /* rotated */
        LONGLONG n = SP_MIN(sz.QuadPart - from, (LONGLONG)(8 << 20));
        buf = (char *)malloc((size_t)n + 1);
        LARGE_INTEGER pos;
        pos.QuadPart = sz.QuadPart - n;
        DWORD rd = 0;
        if (buf && SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && ReadFile(f, buf, (DWORD)n, &rd, NULL)) {
            buf[rd] = 0;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(f);
    return buf;
}

static void print_log_text(const char *text, const wchar_t *tag)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p + 1) : strlen(p);
        char line[2048];
        size_t k = SP_MIN(n, sizeof line - 1);
        memcpy(line, p, k);
        line[k] = 0;
        const wchar_t *colour = strstr(line, "[E]") ? c_red() : strstr(line, "[W]") ? L"\x1b[93m" : L"";
        if (!g_color) colour = L"";
        if (tag) out(L"%ls%ls%ls ", c_dim(), tag, c_off());
        out(L"%ls", colour);
        out_utf8(line, 0);
        if (colour[0]) out(L"%ls", c_off());
        if (!e) out(L"\n");
        p += n;
    }
}

static int cmd_logs(int argc, wchar_t **argv)
{
    int lines = 30, follow = 0, which = 3;           /* 1 app, 2 engine, 3 both */
    for (int i = 1; i < argc; i++) {
        if ((str_ieq(argv[i], L"-n") || str_ieq(argv[i], L"--lines")) && i + 1 < argc) lines = _wtoi(argv[++i]);
        else if (str_ieq(argv[i], L"-f") || str_ieq(argv[i], L"--follow")) follow = 1;
        else if (str_ieq(argv[i], L"app") || str_ieq(argv[i], L"shell")) which = 1;
        else if (str_ieq(argv[i], L"engine") || str_ieq(argv[i], L"renderer")) which = 2;
        else if (str_ieq(argv[i], L"all")) which = 3;
        else {
            err(L"Usage: screenplus logs [app | engine] [-n N] [-f]\n");
            return EXIT_USAGE;
        }
    }
    lines = SP_CLAMP(lines, 1, 100000);
    const wchar_t *names[2] = { L"screenplus.log", L"engine.log" }, *tags[2] = { L"[app]   ", L"[engine]" };
    wchar_t path[2][SP_MAX_PATH];
    LONGLONG size[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        path_join(path[k], SP_COUNTOF(path[k]), paths_logs(), names[k]);
        if (!(which & (1 << k))) continue;
        char *all = read_range(path[k], 0, &size[k]);
        if (!all) {
            err(L"%lsNo log yet: %ls%ls\n", c_dim(), path[k], c_off());
            continue;
        }
        const char *end = all + strlen(all), *start = end;   /* back up `lines` lines from the end */
        int seen = 0;
        while (start > all) {
            if (start[-1] == '\n' && start != end && ++seen >= lines) break;
            start--;
        }
        if (which == 3) out(L"%ls== %ls ==%ls\n", c_bold(), path[k], c_off());
        print_log_text(start, NULL);
        free(all);
    }
    if (!follow) return EXIT_OK;
    out(L"%ls-- following, Ctrl+C to stop --%ls\n", c_dim(), c_off());
    InterlockedExchange(&g_interrupted, 0);
    InterlockedIncrement(&g_catchCtrl);
    while (!g_interrupted) {
        Sleep(250);
        for (int k = 0; k < 2; k++) {
            if (!(which & (1 << k))) continue;
            LONGLONG now = 0;
            HANDLE f = CreateFileW(path[k], FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz;
            if (GetFileSizeEx(f, &sz)) now = sz.QuadPart;
            CloseHandle(f);
            if (now == size[k]) continue;
            if (now < size[k]) size[k] = 0;            /* rotated: start over */
            LONGLONG total = 0;
            char *add = read_range(path[k], size[k], &total);
            if (add) {
                print_log_text(add, which == 3 ? tags[k] : NULL);
                free(add);
            }
            size[k] = total;
        }
    }
    InterlockedDecrement(&g_catchCtrl);
    out(L"\n");
    return EXIT_OK;
}

/* ---- PATH (current user) */

static int path_has(const wchar_t *list, const wchar_t *dir)
{
    const wchar_t *p = list;
    size_t dl = wcslen(dir);
    while (p && *p) {
        const wchar_t *e = wcschr(p, L';');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        wchar_t item[SP_MAX_PATH], exp[SP_MAX_PATH];
        str_copyn(item, SP_COUNTOF(item), p, n);
        ExpandEnvironmentStringsW(item, exp, SP_COUNTOF(exp));
        size_t il = wcslen(exp);
        while (il && (exp[il - 1] == L'\\' || exp[il - 1] == L'/')) exp[--il] = 0;
        if (il == dl && !_wcsnicmp(exp, dir, dl)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

static int cmd_path(const wchar_t *sub)
{
    wchar_t dir[SP_MAX_PATH];
    str_copy(dir, SP_COUNTOF(dir), paths_exe_dir());
    size_t dl = wcslen(dir);
    while (dl && dir[dl - 1] == L'\\') dir[--dl] = 0;
    HKEY env = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &env) != ERROR_SUCCESS) {
        err(L"%lsCould not open HKCU\\Environment.%ls\n", c_red(), c_off());
        return EXIT_FAILED;
    }
    DWORD type = REG_EXPAND_SZ, bytes = 0;
    LSTATUS st = RegQueryValueExW(env, L"Path", NULL, &type, NULL, &bytes);
    wchar_t *cur = (wchar_t *)calloc((size_t)bytes / sizeof(wchar_t) + 2, sizeof(wchar_t));
    if (!cur) {
        RegCloseKey(env);
        return EXIT_FAILED;
    }
    if (st == ERROR_SUCCESS && bytes) RegQueryValueExW(env, L"Path", NULL, &type, (BYTE *)cur, &bytes);
    else type = REG_EXPAND_SZ;
    int present = path_has(cur, dir), rc = EXIT_OK;
    if (!sub || !sub[0] || str_ieq(sub, L"status")) {
        out(present ? L"%ls is on your PATH.\n" : L"%ls is not on your PATH. Add it: screenplus path add\n", dir);
    } else if (str_ieq(sub, L"add") || str_ieq(sub, L"remove")) {
        int add = str_ieq(sub, L"add");
        if (add == present) {
            out(add ? L"Already on your PATH: %ls\n" : L"Not on your PATH: %ls\n", dir);
        } else {
            size_t cap = wcslen(cur) + wcslen(dir) + 4;
            wchar_t *next = (wchar_t *)calloc(cap, sizeof(wchar_t));
            if (!next) rc = EXIT_FAILED;
            else if (add) {
                wcscpy_s(next, cap, cur);
                size_t n = wcslen(next);
                if (n && next[n - 1] != L';') wcscat_s(next, cap, L";");
                wcscat_s(next, cap, dir);
            } else {
                const wchar_t *p = cur;            /* keep every entry except ours, in order */
                while (p && *p) {
                    const wchar_t *e = wcschr(p, L';');
                    size_t n = e ? (size_t)(e - p) : wcslen(p);
                    wchar_t item[SP_MAX_PATH];
                    str_copyn(item, SP_COUNTOF(item), p, n);
                    if (item[0] && !path_has(item, dir)) {
                        if (next[0]) wcscat_s(next, cap, L";");
                        wcscat_s(next, cap, item);
                    }
                    p = e ? e + 1 : NULL;
                }
            }
            if (next && RegSetValueExW(env, L"Path", 0, type == REG_SZ ? REG_SZ : REG_EXPAND_SZ, (const BYTE *)next,
                                       (DWORD)((wcslen(next) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
                SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, NULL);
                out(add ? L"Added %ls to your PATH. Open a new terminal to use 'screenplus' anywhere.\n"
                        : L"Removed %ls from your PATH.\n", dir);
            } else {
                err(L"%lsCould not update your PATH.%ls\n", c_red(), c_off());
                rc = EXIT_FAILED;
            }
            free(next);
        }
    } else {
        err(L"Usage: screenplus path [add | remove]\n");
        rc = EXIT_USAGE;
    }
    free(cur);
    RegCloseKey(env);
    return rc;
}

/* ---- status --watch */

static int cmd_watch(int argc, wchar_t **argv)
{
    if (!g_outCon) return remote(argc, argv);         /* redirected: one snapshot */
    InterlockedExchange(&g_interrupted, 0);
    InterlockedIncrement(&g_catchCtrl);
    int rc = EXIT_OK;
    while (!g_interrupted) {
        JVal *rep = NULL;
        rc = request(argc, argv, &rep);
        out(L"\x1b[H\x1b[2J");
        if (rc == EXIT_OK) print_reply(rep);
        json_free(rep);
        SYSTEMTIME t;
        GetLocalTime(&t);
        out(L"\n%ls%02u:%02u:%02u - refreshing every second, Ctrl+C to stop%ls\n", c_dim(), t.wHour, t.wMinute, t.wSecond, c_off());
        if (rc == EXIT_NOT_RUNNING) out(L"%ls(waiting for Screen+)%ls\n", c_dim(), c_off());
        for (int i = 0; i < 10 && !g_interrupted; i++) Sleep(100);
    }
    InterlockedDecrement(&g_catchCtrl);
    return rc == EXIT_NOT_RUNNING ? rc : EXIT_OK;
}

/* ------------------------------------------------------------------ dispatch */

static int run_command(int argc, wchar_t **in)
{
    wchar_t *argv[MAX_ARGS];
    int n = 0, watch = 0;
    g_json = 0;
    for (int i = 0; i < argc && n < MAX_ARGS; i++) {   /* global switches may appear anywhere */
        if (str_ieq(in[i], L"--json")) g_json = 1;
        else if (str_ieq(in[i], L"--quiet")) g_quiet = 1;
        else if (str_ieq(in[i], L"--watch") || str_ieq(in[i], L"-w")) watch = 1;
        else argv[n++] = in[i];
    }
    if (!n) return cmd_help(NULL);
    const wchar_t *c = argv[0];
    /* the GUI executable's switches work here too */
    static const struct { const wchar_t *sw, *cmd; } legacy[] = {
        { L"--show", L"show" }, { L"--pause", L"pause" }, { L"--resume", L"resume" }, { L"--toggle-pause", L"toggle" },
        { L"--next", L"next" }, { L"--exit", L"stop" }, { L"--status", L"status" },
    };
    for (size_t i = 0; i < SP_COUNTOF(legacy); i++)
        if (str_ieq(c, legacy[i].sw)) argv[0] = (wchar_t *)legacy[i].cmd;
    c = argv[0];
    if (str_ieq(c, L"--tray")) return cmd_start(0);
    if (str_ieq(c, L"help") || str_ieq(c, L"-h") || str_ieq(c, L"--help") || str_ieq(c, L"/?") || str_ieq(c, L"-?"))
        return cmd_help(n > 1 ? argv[1] : NULL);
    if (str_ieq(c, L"version") || str_ieq(c, L"--version") || str_ieq(c, L"-v")) return cmd_version();
    if (str_ieq(c, L"start")) return cmd_start(n > 1 && str_ieq(argv[1], L"--show"));
    if (str_ieq(c, L"logs") || str_ieq(c, L"log")) return cmd_logs(n, argv);
    if (str_ieq(c, L"paths")) return cmd_paths();
    if (str_ieq(c, L"path")) return cmd_path(n > 1 ? argv[1] : NULL);
    if (str_ieq(c, L"tools")) return cmd_tools(n > 1 ? argv[1] : NULL);
    if (str_ieq(c, L"vcredist")) return cmd_vcredist(n > 1 ? argv[1] : NULL);
    if (str_ieq(c, L"screensaver") && !(n > 1 && str_ieq(argv[1], L"use"))) return cmd_screensaver(n, argv);
    if (str_ieq(c, L"watch") || (watch && str_ieq(c, L"status"))) {
        argv[0] = L"status";
        return cmd_watch(1, argv);
    }
    return remote(n, argv);
}

/* split a typed line: whitespace separates, "double quotes" group */
static int split_line(wchar_t *line, wchar_t **argv, int max)
{
    int n = 0;
    wchar_t *p = line;
    while (*p && n < max) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        wchar_t *dst = p;
        argv[n++] = p;
        int quoted = 0;
        while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') quoted = !quoted;
            else *dst++ = *p;
            p++;
        }
        if (*p) p++;
        *dst = 0;
    }
    return n;
}

static int read_line(wchar_t *buf, size_t cap)
{
    if (g_inCon) {
        DWORD got = 0;
        InterlockedExchange(&g_interrupted, 0);
        BOOL ok = ReadConsoleW(g_in, buf, (DWORD)cap - 1, &got, NULL);
        if (!ok) Sleep(50);                           /* let the Ctrl+C handler run first */
        if (g_interrupted || (ok && !got)) return 0;   /* Ctrl+C at the prompt: a new prompt */
        if (!ok) return -1;
        buf[got] = 0;
        if (buf[0] == 0x1A) return -1;                 /* Ctrl+Z, Enter: end of input */
    } else {
        /* piped or redirected commands: UTF-8 bytes (PowerShell may lead with a BOM) */
        static int first = 1;
        char raw[4096];
        if (!fgets(raw, sizeof raw, stdin)) return -1;
        const char *p = raw;
        if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        first = 0;
        if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, buf, (int)cap)) buf[0] = 0;
    }
    size_t n = wcslen(buf);
    while (n && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r')) buf[--n] = 0;
    return 1;
}

static int repl(void)
{
    if (g_inCon) {                                     /* commands piped in run silently, like a script */
        out(L"%lsScreen+ %ls%ls command line. Type %lshelp%ls for commands, %lsexit%ls to leave.\n", c_bold(), SP_VERSION_W,
            c_off(), c_bold(), c_off(), c_bold(), c_off());
        if (!app_window()) out(L"%lsScreen+ is not running: type 'start' to start it.%ls\n", c_dim(), c_off());
    }
    wchar_t line[2048];
    int rc = EXIT_OK;
    InterlockedIncrement(&g_catchCtrl);
    for (;;) {
        if (g_inCon) out(L"%lsscreen+>%ls ", c_green(), c_off());
        int got = read_line(line, SP_COUNTOF(line));
        if (got < 0) break;
        if (!got) {
            out(L"\n");
            continue;
        }
        wchar_t *argv[MAX_ARGS];
        int n = split_line(line, argv, MAX_ARGS);
        if (!n) continue;
        if (str_ieq(argv[0], L"exit") || str_ieq(argv[0], L"quit") || str_ieq(argv[0], L"q")) break;
        if (str_ieq(argv[0], L"cls") || str_ieq(argv[0], L"clear")) {
            out(L"\x1b[H\x1b[2J\x1b[3J");
            continue;
        }
        rc = run_command(n, argv);
        g_quiet = 0;
    }
    return rc;
}

int wmain(int argc, wchar_t **argv)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    console_init();
    paths_init();
    int rc = argc > 1 ? run_command(argc - 1, argv + 1) : repl();
    free(g_reply);
    return rc;
}
