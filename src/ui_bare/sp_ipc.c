/* sp_ipc.c: one request to the Screen+ service, the way ScreenPlus.com sends it.
 *
 * Windows: WM_COPYDATA straight to the shell window (IPC_CLI_REQUEST, src/common/ipc.h),
 * no process per request. The shell answers with IPC_CLI_REPLY to our message window
 * while SendMessageTimeout waits, so the answer is there when it returns. Use from one
 * thread (the window belongs to the thread that made it).
 * Linux: the service's socket ($XDG_RUNTIME_DIR/screenplus.sock), as the `screenplus` CLI talks to it: the
 * request, then the whole answer up to the end. */
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
    wc.lpszClassName = SP_CLI_CLASS;   /* the shell answers only this class (cli_server_handle) */
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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_service(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    int n = dir && *dir ? snprintf(a.sun_path, sizeof a.sun_path, "%s/screenplus.sock", dir)   /* src/linux/run.c */
                        : snprintf(a.sun_path, sizeof a.sun_path, "/tmp/screenplus.sock-%u", (unsigned)getuid());
    if (n <= 0 || (size_t)n >= sizeof a.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool sp_service_running(void) {
    int fd = connect_service();
    if (fd < 0) return false;
    close(fd);
    return true;
}

JVal *sp_request(const char *const *argv) {
    int fd = connect_service();
    if (fd < 0) return NULL;
    StrBuf q = request_json(argv);
    size_t off = 0;
    while (!q.failed && off < q.len) {
        ssize_t w = write(fd, q.p + off, q.len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    sb_free(&q);
    shutdown(fd, SHUT_WR);
    StrBuf in = { 0 };
    char buf[8192];
    ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) > 0 && in.len < 64u << 20) sb_append(&in, buf, (size_t)got);
    close(fd);
    JVal *doc = in.len && !in.failed ? json_parse(in.p, in.len) : NULL;
    sb_free(&in);
    return doc;
}
#endif
