/* Hyprland: displays and fullscreen state, read from its control socket. */
#include "desktop.h"
#include "../common/json.h"
#include "../common/util.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

char *hypr_query(const char *request)
{
    const char *run = getenv("XDG_RUNTIME_DIR"), *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!run || !sig) return NULL;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    int n = snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/.socket.sock", run, sig);
    if (n <= 0 || (size_t)n >= sizeof a.sun_path) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) || write(fd, request, strlen(request)) < 0) {
        close(fd);
        return NULL;
    }
    StrBuf b = { 0 };
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) sb_append(&b, buf, (size_t)r);
    close(fd);
    if (b.failed || !b.p) {
        sb_free(&b);
        return NULL;
    }
    return b.p;
}

static JVal *query_json(const char *request)
{
    char *t = hypr_query(request);
    JVal *j = t ? json_parse(t, strlen(t)) : NULL;
    free(t);
    return j;
}

int hypr_monitors(MonitorInfo *out, int max)
{
    JVal *j = query_json("j/monitors");
    int n = 0;
    for (int i = 0; j && j->t == J_ARR && i < j->n && n < max; i++) {
        const JVal *m = &j->kids[i];
        MonitorInfo *o = &out[n];
        memset(o, 0, sizeof *o);
        json_str_w(json_get(m, "name"), o->id, SP_COUNTOF(o->id));
        if (!o->id[0]) continue;
        json_str_w(json_get(m, "description"), o->name, SP_COUNTOF(o->name));
        if (!o->name[0]) str_copy(o->name, SP_COUNTOF(o->name), o->id);
        o->rect.left = (LONG)json_num(json_get(m, "x"), 0);
        o->rect.top = (LONG)json_num(json_get(m, "y"), 0);
        o->rect.right = o->rect.left + (LONG)json_num(json_get(m, "width"), 0);
        o->rect.bottom = o->rect.top + (LONG)json_num(json_get(m, "height"), 0);
        o->work = o->rect;
        o->dpi = (UINT)(96 * json_num(json_get(m, "scale"), 1));
        o->refreshHz = (UINT)(json_num(json_get(m, "refreshRate"), 60) + 0.5);
        o->primary = n == 0;
        o->index = n + 1;
        n++;
    }
    json_free(j);
    return n;
}

/* The workspace showing on a display, as its JSON id; 0 when the display isn't found. */
static int shown_workspace(const JVal *mons, const char *name)
{
    for (int i = 0; mons && mons->t == J_ARR && i < mons->n; i++) {
        if (strcmp(json_str(json_get(&mons->kids[i], "name"), ""), name)) continue;
        return (int)json_num(json_get(json_get(&mons->kids[i], "activeWorkspace"), "id"), 0);
    }
    return 0;
}

int hypr_fullscreen_on(const char *monitorName)
{
    JVal *mons = query_json("j/monitors"), *wss = query_json("j/workspaces");
    int id = shown_workspace(mons, monitorName), on = 0;
    for (int i = 0; id && wss && wss->t == J_ARR && i < wss->n; i++) {
        const JVal *w = &wss->kids[i];
        if ((int)json_num(json_get(w, "id"), 0) != id) continue;
        const JVal *v = json_get(w, "hasfullscreen");
        on = v && (v->t == J_BOOL ? v->b : json_num(v, 0) > 0);
    }
    json_free(mons);
    json_free(wss);
    return on;
}

