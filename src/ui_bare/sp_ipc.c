/* sp_ipc.c: one request to the Screen+ service, the way ScreenPlus.com sends it.
 *
 * Windows: WM_COPYDATA straight to the shell window (IPC_CLI_REQUEST, src/common/ipc.h),
 * no process per request. The shell answers with IPC_CLI_REPLY to our message window
 * while SendMessageTimeout waits, so the answer is there when it returns. Use from one
 * thread (the window belongs to the thread that made it).
 * Linux: runs the `screenplus` CLI with --json. */
#include "sp_ipc.h"
#include "../common/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static StrBuf request_json(const char *const *argv) {
    StrBuf b = { 0 };
    sb_puts(&b, "{\"argv\":[");
    for (int i = 0; argv[i]; i++) {
        if (i) sb_puts(&b, ",");
        json_write_string(&b, argv[i]);
    }
    sb_puts(&b, "]}");
    return b;
}

#ifdef _WIN32
#include "../common/ipc.h"

static char  *g_reply;
static size_t g_reply_len;

static LRESULT CALLBACK reply_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds->dwData != IPC_CLI_REPLY || !cds->lpData || !cds->cbData) return FALSE;
        free(g_reply);
        g_reply = (char *)malloc((size_t)cds->cbData + 1);
        if (!g_reply) return FALSE;
        memcpy(g_reply, cds->lpData, cds->cbData);
        g_reply[cds->cbData] = 0;
        g_reply_len = cds->cbData;
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND reply_window(void) {
    static HWND w;
    if (w) return w;
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = reply_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ScreenPlus.BareUi.Reply";
    RegisterClassExW(&wc);
    w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (w) ChangeWindowMessageFilterEx(w, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    return w;
}

bool sp_service_running(void) { return FindWindowW(SP_SHELL_CLASS, NULL) != NULL; }

JVal *sp_request(const char *const *argv) {
    HWND app = FindWindowW(SP_SHELL_CLASS, NULL), me = reply_window();
    if (!app || !me) return NULL;
    StrBuf b = request_json(argv);
    if (b.failed) { sb_free(&b); return NULL; }
    free(g_reply);
    g_reply = NULL;
    COPYDATASTRUCT cds = { IPC_CLI_REQUEST, (DWORD)b.len, b.p };
    DWORD_PTR res = 0;
    LRESULT sent = SendMessageTimeoutW(app, WM_COPYDATA, (WPARAM)me, (LPARAM)&cds,
                                       SMTO_NORMAL | SMTO_ABORTIFHUNG, 30000, &res);
    sb_free(&b);
    if (!sent || !g_reply) return NULL;
    JVal *doc = json_parse(g_reply, g_reply_len);
    free(g_reply);
    g_reply = NULL;
    return doc;
}

#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_cli = "screenplus";
void sp_set_cli(const char *path) { if (path && *path) g_cli = path; }

bool sp_service_running(void) { return true; }

JVal *sp_request(const char *const *argv) {
    int pipes[2];
    if (pipe(pipes)) return NULL;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return NULL; }
    if (!child) {
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]); close(pipes[1]);
        char *av[20]; int n = 0;
        av[n++] = (char *)g_cli;
        for (int i = 0; argv[i] && n < 18; i++) av[n++] = (char *)argv[i];
        av[n++] = "--json"; av[n] = NULL;
        execvp(g_cli, av);
        _exit(127);
    }
    close(pipes[1]);
    size_t cap = 4096, used = 0;
    char *out = malloc(cap);
    if (out) for (;;) {
        if (used + 4096 + 1 > cap) {
            if (cap >= 16 * 1024 * 1024) break;
            cap *= 2;
            char *bigger = realloc(out, cap);
            if (!bigger) break;
            out = bigger;
        }
        ssize_t got = read(pipes[0], out + used, cap - used - 1);
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(pipes[0]);
    waitpid(child, NULL, 0);
    if (!out) return NULL;
    out[used] = 0;
    JVal *doc = json_parse(out, used);
    free(out);
    return doc;
}
#endif
