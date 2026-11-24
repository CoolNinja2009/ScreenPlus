/* Standalone Bare window talking to the existing Screen+ CLI on either platform.
 * The service remains the sole owner of config and wallpaper playback. */
#include "screenplus_bare.h"
#include "../common/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct Live {
    SpBareView view;
    SpBareModel model;
    SpBareWallpaper *walls;
    JVal *list_doc;
    SpBareDisplay *displays;
    JVal *status_doc;
    SpBareStoreItem *store_items;
    JVal *store_doc;
    const char *cli;
    char status[256];
    SpBareAction pending;
    char pending_path[4096];
    int has_pending;
} Live;

#ifdef _WIN32
static int append_quoted(wchar_t *dst, size_t cap, const char *utf8) {
    wchar_t word[2048];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, word, 2048);
    if (!n) return 0;
    size_t at = wcslen(dst);
    if (at + (size_t)n * 2 + 4 >= cap) return 0;
    dst[at++] = L'"';
    for (int i = 0; i < n - 1;) {
        int slashes = 0;
        while (word[i] == L'\\') { slashes++; i++; }
        for (int j = 0; j < slashes * (word[i] == L'"' || word[i] == 0 ? 2 : 1); j++) dst[at++] = L'\\';
        if (word[i] == 0) break;
        if (word[i] == L'"') dst[at++] = L'\\';
        dst[at++] = word[i++];
    }
    dst[at++] = L'"'; dst[at] = 0;
    return 1;
}

static char *cli_output(const char *cli, const char *const *args) {
    wchar_t line[8192] = L"";
    if (!append_quoted(line, 8192, cli)) return NULL;
    for (int i = 0; args[i]; i++) {
        size_t at = wcslen(line);
        if (at + 2 >= 8192) return NULL;
        line[at++] = L' '; line[at] = 0;
        if (!append_quoted(line, 8192, args[i])) return NULL;
    }
    if (wcslen(line) + 10 >= 8192) return NULL;
    wcscat(line, L" \"--json\"");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return NULL;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { 0 }; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write_end; si.hStdError = write_end; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };
    BOOL started = CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(write_end);
    if (!started) { CloseHandle(read_end); return NULL; }
    size_t cap = 4096, used = 0;
    char *out = malloc(cap);
    if (out) {
        for (;;) {
            if (used + 4096 + 1 > cap) {
                if (cap >= 16 * 1024 * 1024) break;
                cap *= 2;
                char *bigger = realloc(out, cap);
                if (!bigger) break;
                out = bigger;
            }
            DWORD got = 0;
            if (!ReadFile(read_end, out + used, (DWORD)(cap - used - 1), &got, NULL) || !got) break;
            used += got;
        }
        out[used] = 0;
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return out;
}
#else
static char *cli_output(const char *cli, const char *const *args) {
    int pipes[2];
    if (pipe(pipes)) return NULL;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return NULL; }
    if (!child) {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]); close(pipes[1]);
        char *av[16]; int n = 0;
        av[n++] = (char *)cli;
        for (int i = 0; args[i] && n < 14; i++) av[n++] = (char *)args[i];
        av[n++] = "--json"; av[n] = NULL;
        execvp(cli, av);
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
    if (out) out[used] = 0;
    close(pipes[0]);
    waitpid(child, NULL, 0);
    return out;
}
#endif

static JVal *command(Live *app, const char *const *args) {
    char *out = cli_output(app->cli, args);
    if (!out) return NULL;
    JVal *doc = json_parse(out, strlen(out));
    free(out);
    return doc;
}

static const JVal *data(const JVal *doc) {
    return json_bool(json_get(doc, "ok"), 0) ? json_get(doc, "data") : NULL;
}

static int succeeded(const JVal *doc) {
    return json_bool(json_get(doc, "ok"), 0);
}

static void free_walls(SpBareWallpaper *walls, int count, JVal *doc) {
    for (int i = 0; i < count; i++)
        if (walls[i].thumbnail) bare_image_free(walls[i].thumbnail);
    free(walls);
    json_free(doc);
}

static void clear_walls(Live *app) {
    free_walls(app->walls, app->model.wallpaper_count, app->list_doc);
    app->walls = NULL;
    app->model.wallpapers = NULL; app->model.wallpaper_count = 0;
    app->model.active_wallpaper = -1;
    app->list_doc = NULL;
}

static void refresh_look(Live *app);

static void refresh(Live *app) {
    JVal *status = command(app, (const char *[]){ "status", NULL });
    const JVal *st = data(status);
    if (st) {
        app->model.paused = json_bool(json_get(st, "paused"), 0);
        app->model.muted = !json_bool(json_get(st, "audio"), 0);
        snprintf(app->status, sizeof app->status, "%s", json_str(json_get(st, "state"), "Ready"));
        app->model.status = app->status;
        const JVal *displays = json_get(st, "displays");
        if (displays && displays->t == J_ARR) {
            app->model.display_name = displays->n == 1 ? "1 display" : "All displays";
            SpBareDisplay *fresh = calloc((size_t)displays->n, sizeof *fresh);
            if (fresh || !displays->n) {
                for (int i = 0; i < displays->n; i++) {
                    const JVal *d = &displays->kids[i];
                    const JVal *w = json_get(d, "wallpaper");
                    fresh[i].name = json_str(json_get(d, "name"), "Display");
                    fresh[i].wallpaper_title = json_str(json_get(w, "name"), "");
                    fresh[i].index = (int)json_num(json_get(d, "index"), i + 1);
                    fresh[i].width = (int)json_num(json_get(d, "width"), 0);
                    fresh[i].height = (int)json_num(json_get(d, "height"), 0);
                    fresh[i].hz = (int)json_num(json_get(d, "hz"), 0);
                    fresh[i].primary = json_bool(json_get(d, "primary"), 0);
                    fresh[i].connected = true;
                }
                free(app->displays);
                json_free(app->status_doc);
                app->displays = fresh;
                app->status_doc = status;
                app->model.displays = fresh;
                app->model.display_count = displays->n;
                status = NULL;
            }
        }
    } else {
        snprintf(app->status, sizeof app->status, "Screen+ service unavailable");
        app->model.status = app->status;
    }
    json_free(status);
    JVal *settings = command(app, (const char *[]){ "settings", NULL });
    const JVal *s = data(settings);
    if (s) {
#ifdef _WIN32
        app->model.start_with_system = json_bool(json_get(s, "start_with_windows"), 0);
#else
        app->model.start_with_system = json_bool(json_get(s, "start_with_linux"), 0);
#endif
        app->model.start_minimised = json_bool(json_get(s, "launch_to_tray"), 0);
        app->model.still_at_start = json_bool(json_get(s, "still_wallpaper"), 0);
        app->model.auto_update = json_bool(json_get(s, "auto_update"), 0);
        app->model.sample_updates = json_bool(json_get(s, "auto_update_samples"), 0);
        const char *theme = json_str(json_get(s, "theme"), "system");
        const char *motion = json_str(json_get(s, "ui_motion"), "full");
        app->model.appearance = !strcmp(theme, "light") ? 1 : !strcmp(theme, "dark") ? 2 : 0;
        app->model.animation_level = !strcmp(motion, "reduced") ? 1 : !strcmp(motion, "off") ? 2 : 0;
        const char *mode = json_str(json_get(s, "display_mode"), "independent");
        app->model.display_mode = !strcmp(mode, "duplicate") ? 1 : !strcmp(mode, "span") ? 2 : 0;
    }
    json_free(settings);
    JVal *list = command(app, (const char *[]){ "list", NULL });
    const JVal *items = data(list);
    if (!items || items->t != J_ARR) { json_free(list); return; }
    SpBareWallpaper *walls = calloc((size_t)items->n, sizeof *walls);
    if (!walls && items->n) { json_free(list); return; }
    SpBareWallpaper *old_walls = app->walls;
    int old_count = app->model.wallpaper_count;
    JVal *old_doc = app->list_doc;
    app->list_doc = list;
    app->walls = walls;
    app->model.wallpapers = walls;
    app->model.wallpaper_count = items->n;
    for (int i = 0; i < items->n; i++) {
        const JVal *item = &items->kids[i];
        const JVal *w = json_get(item, "wallpaper");
        walls[i].id = json_str(json_get(w, "id"), "");
        walls[i].title = json_str(json_get(w, "name"), "Untitled");
        walls[i].kind = json_str(json_get(w, "type"), "");
        walls[i].selected = json_bool(json_get(item, "onScreen"), 0);
        walls[i].favourite = json_bool(json_get(w, "favourite"), 0);
        walls[i].missing = json_bool(json_get(w, "missing"), 0);
        if (walls[i].selected && app->model.active_wallpaper < 0) app->model.active_wallpaper = i;
        const char *thumb = json_str(json_get(w, "thumbnail"), "");
        if (!*thumb && !strcmp(walls[i].kind, "image")) thumb = json_str(json_get(w, "path"), "");
        walls[i].thumbnail_path = thumb;
        int load_limit = app->view.visible_limit > 90 ? app->view.visible_limit : 90;
        if (*thumb && (i < load_limit || walls[i].selected)) {
            for (int j = 0; j < old_count; j++) {
                if (old_walls[j].thumbnail && !strcmp(walls[i].id, old_walls[j].id) &&
                    !strcmp(thumb, old_walls[j].thumbnail_path ? old_walls[j].thumbnail_path : "")) {
                    walls[i].thumbnail = old_walls[j].thumbnail;
                    old_walls[j].thumbnail = 0;
                    break;
                }
            }
            if (!walls[i].thumbnail) walls[i].thumbnail = bare_image_load(thumb);
        }
    }
    free_walls(old_walls, old_count, old_doc);
    refresh_look(app);
}

static void load_more(Live *app) {
    int limit = app->view.visible_limit;
    if (limit > app->model.wallpaper_count) limit = app->model.wallpaper_count;
    for (int i = 0; i < limit; i++) {
        SpBareWallpaper *w = &app->walls[i];
        if (!w->thumbnail && w->thumbnail_path && *w->thumbnail_path)
            w->thumbnail = bare_image_load(w->thumbnail_path);
    }
}

static void refresh_store(Live *app) {
    JVal *doc = command(app, (const char *[]){ "store", NULL });
    const JVal *result = data(doc);
    const JVal *items = json_get(result, "items");
    if (!items || items->t != J_ARR) {
        json_free(doc);
        app->model.store_error = "Store data unavailable";
        return;
    }
    SpBareStoreItem *fresh = calloc((size_t)items->n, sizeof *fresh);
    if (!fresh && items->n) { json_free(doc); return; }
    for (int i = 0; i < items->n; i++) {
        const JVal *item = &items->kids[i];
        fresh[i].title = json_str(json_get(item, "title"), "Untitled");
        fresh[i].source = json_str(json_get(item, "source"), "");
        fresh[i].kind = json_str(json_get(item, "kind"), "");
        fresh[i].number = (int)json_num(json_get(item, "number"), i + 1);
    }
    free(app->store_items);
    json_free(app->store_doc);
    app->store_items = fresh;
    app->store_doc = doc;
    app->model.store_items = fresh;
    app->model.store_count = items->n;
    app->model.store_loading = json_bool(json_get(result, "loading"), 0);
    app->model.store_error = json_str(json_get(result, "error"), "");
}

static void refresh_look(Live *app) {
    static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
    JVal *doc = command(app, (const char *[]){ "look", NULL });
    const JVal *result = data(doc);
    app->model.look_available = result && result->t == J_OBJ;
    if (app->model.look_available)
        for (int i = 0; i < 5; i++) app->model.look[i] = (int)json_num(json_get(result, keys[i]), 0);
    json_free(doc);
}

static void run_action(Live *app, SpBareAction a) {
    const char *args[5] = { NULL };
    switch (a.kind) {
    case SP_BARE_SELECT_WALLPAPER:
        if (a.index >= 0 && a.index < app->model.wallpaper_count && !app->walls[a.index].missing) {
            args[0] = "apply"; args[1] = app->walls[a.index].id;
            if (a.value > 0) {
                static char display_number[16];
                snprintf(display_number, sizeof display_number, "%d", a.value);
                args[2] = "--display"; args[3] = display_number;
            }
        }
        break;
    case SP_BARE_IMPORT: if (a.path && *a.path) { args[0] = "import"; args[1] = a.path; } break;
    case SP_BARE_PAUSE: args[0] = a.value ? "pause" : "resume"; break;
    case SP_BARE_MUTE: args[0] = "audio"; args[1] = a.value ? "off" : "on"; break;
    case SP_BARE_RESTART: args[0] = "restart"; break;
    case SP_BARE_SETTING: {
        static const char *keys[] = {
#ifdef _WIN32
            "start_with_windows",
#else
            "start_with_linux",
#endif
            "launch_to_tray", "still_wallpaper", "auto_update", "auto_update_samples", "theme", "ui_motion"
        };
        static const char *themes[] = { "system", "light", "dark" };
        static const char *motions[] = { "full", "reduced", "off" };
        if (a.index < 0 || a.index >= 7) break;
        args[0] = "settings"; args[1] = "set"; args[2] = keys[a.index];
        args[3] = a.index == 5 ? themes[a.value >= 0 && a.value <= 2 ? a.value : 0] :
                  a.index == 6 ? motions[a.value >= 0 && a.value <= 2 ? a.value : 0] : a.value ? "on" : "off";
        break;
    }
    case SP_BARE_REFRESH: refresh(app); return;
    case SP_BARE_LOAD_MORE: load_more(app); return;
    case SP_BARE_NAVIGATE:
        if (a.index == SP_BARE_STORE) refresh_store(app);
        if (a.index == SP_BARE_CUSTOMISE) refresh_look(app);
        return;
    case SP_BARE_STORE_SEARCH:
        if (a.path && *a.path) { args[0] = "store"; args[1] = "search"; args[2] = a.path; }
        else { args[0] = "store"; args[1] = "search"; args[2] = ""; }
        break;
    case SP_BARE_STORE_MORE: args[0] = "store"; args[1] = "more"; break;
    case SP_BARE_STORE_GET: {
        static char number[16];
        snprintf(number, sizeof number, "%d", a.index);
        args[0] = "store"; args[1] = "get"; args[2] = number;
        break;
    }
    case SP_BARE_LOOK_SET: {
        static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
        static char value[16];
        if (a.index < 0 || a.index >= 5) break;
        snprintf(value, sizeof value, "%d", a.value);
        args[0] = "look"; args[1] = "set"; args[2] = keys[a.index]; args[3] = value;
        break;
    }
    case SP_BARE_LOOK_RESET: args[0] = "look"; args[1] = "reset"; break;
    case SP_BARE_DISPLAY_MODE: {
        static const char *modes[] = { "independent", "duplicate", "span" };
        if (a.index < 0 || a.index >= 3) break;
        args[0] = "mode"; args[1] = modes[a.index];
        break;
    }
    default: return;
    }
    if (!args[0]) return;
    JVal *reply = command(app, args);
    if (!succeeded(reply)) {
        snprintf(app->status, sizeof app->status, "%.250s", json_str(json_get(reply, "text"), "Action failed"));
        app->model.status = app->status;
    } else {
        if (a.kind == SP_BARE_STORE_SEARCH || a.kind == SP_BARE_STORE_MORE || a.kind == SP_BARE_STORE_GET)
            refresh_store(app);
        else if (a.kind == SP_BARE_LOOK_SET || a.kind == SP_BARE_LOOK_RESET) refresh_look(app);
        else refresh(app);
    }
    json_free(reply);
}

static void queue_action(void *user, SpBareAction a) {
    Live *app = user;
    app->pending = a;
    if (a.path) {
        size_t n = strlen(a.path);
        if (n >= sizeof app->pending_path) return;
        memcpy(app->pending_path, a.path, n + 1);
        app->pending.path = app->pending_path;
    }
    app->has_pending = 1;
}

int main(int argc, char **argv) {
    Live app = { 0 };
#ifdef _WIN32
    app.cli = argc > 1 ? argv[1] : "ScreenPlus.com";
#else
    app.cli = argc > 1 ? argv[1] : "screenplus";
#endif
    app.model.active_wallpaper = -1;
    app.model.status = "Connecting to Screen+";
    app.model.transparent_window = true;
    if (!bare_window_transparent(1500, 960, "Screen+")) return 1;
    bare_background(BARE_CLEAR);
    refresh(&app);
    while (bare_frame()) {
        if (app.has_pending) {
            SpBareAction next = app.pending;
            app.has_pending = 0;
            run_action(&app, next);
        }
        sp_bare_draw(&app.view, &app.model, queue_action, &app);
    }
    clear_walls(&app);
    free(app.displays);
    json_free(app.status_doc);
    free(app.store_items);
    json_free(app.store_doc);
    bare_quit();
    return 0;
}
