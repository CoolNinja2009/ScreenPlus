/* Web wallpapers' folders on a local web host, what WebView2's virtual host is on Windows. Pages written for it use
 * "/js/x.js", ES modules and fetch, none of which work from file://. Each wallpaper gets its own name,
 * <token>-<id>.localhost:<port>, so "/" is its folder and each is its own origin; the token is new at every start,
 * so nothing but the pages we load can name them. Windows' file names don't care about case: neither does this. */
#include "service.h"
#include "desktop.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../settings/config.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char id[SP_ID_LEN]; char root[MAX_PATH]; } Site;

static Site           *g_sites;
static int             g_siteCount, g_port;
static char            g_token[17];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct { const char *ext, *type; } k_types[] = {
    { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" }, { "js", "text/javascript" }, { "mjs", "text/javascript" },
    { "css", "text/css" }, { "json", "application/json" }, { "svg", "image/svg+xml" }, { "png", "image/png" }, { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "ico", "image/x-icon" },
    { "mp4", "video/mp4" }, { "m4v", "video/mp4" }, { "webm", "video/webm" }, { "ogv", "video/ogg" }, { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" },
    { "opus", "audio/ogg" }, { "wav", "audio/wav" }, { "m4a", "audio/mp4" }, { "wasm", "application/wasm" }, { "ttf", "font/ttf" }, { "otf", "font/otf" },
    { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "glb", "model/gltf-binary" }, { "gltf", "model/gltf+json" }, { "txt", "text/plain; charset=utf-8" },
};

static const char *type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot)
        for (size_t i = 0; i < SP_COUNTOF(k_types); i++)
            if (!strcasecmp(dot + 1, k_types[i].ext)) return k_types[i].type;
    return "application/octet-stream";
}

/* The %XX of a request path, in place. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        unsigned v;
        if (*s == '%' && s[1] && s[2] && sscanf(s + 1, "%2x", &v) == 1) { *o++ = (char)v; s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}

/* root + rel, each folder and file found whatever its case. 0: no such file. */
static int resolve(const char *root, char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s", root);
    char *save = NULL;
    for (char *part = strtok_r(rel, "/\\", &save); part; part = strtok_r(NULL, "/\\", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) return 0;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "/%s", part);
        struct stat st;
        if (!stat(out, &st)) continue;
        out[n] = 0;
        DIR *d = opendir(out);
        struct dirent *e;
        int found = 0;
        while (d && (e = readdir(d)))
            if (!strcasecmp(e->d_name, part)) {
                snprintf(out + n, cap - n, "/%s", e->d_name);
                found = 1;
                break;
            }
        if (d) closedir(d);
        if (!found) return 0;
    }
    return 1;
}

static char *recordings_dir(void)          /* cache/rec, UTF-8; free() */
{
    wchar_t d[SP_MAX_PATH];
    path_join(d, SP_COUNTOF(d), paths_cache(), L"rec");
    return sp_path_utf8(d);
}

static void reply_status(int fd, const char *status)
{
    char h[160];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    if (send(fd, h, (size_t)n, MSG_NOSIGNAL) < 0) {}
}

static void serve(int fd)
{
    char req[8192];
    size_t have = 0;
    while (have + 1 < sizeof req) {
        ssize_t n = recv(fd, req + have, sizeof req - 1 - have, 0);
        if (n <= 0) return;
        have += (size_t)n;
        req[have] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    int head = !strncmp(req, "HEAD ", 5);
    if (!head && strncmp(req, "GET ", 4)) { reply_status(fd, "405 Method Not Allowed"); return; }
    char *path = strchr(req, ' ') + 1, *end = strchr(path, ' ');
    if (!end) { reply_status(fd, "400 Bad Request"); return; }
    *end = 0;
    char *rest = end + 1, *host = strcasestr(rest, "\r\nHost:");
    if (!host) { reply_status(fd, "400 Bad Request"); return; }
    host += 7;
    while (*host == ' ') host++;
    /* <token>-<id>.localhost */
    size_t tl = strlen(g_token);
    char root[MAX_PATH] = "";
    if (path[0] == '/' && !strncmp(path + 1, g_token, tl) && !strncmp(path + 1 + tl, "-rec/", 5)) {
        char *dir = recordings_dir();                   /* /<token>-rec/<file>: Customise recordings for the plugin */
        if (dir) snprintf(root, sizeof root, "%s", dir);
        free(dir);
        path += tl + 5;
    } else if (!strncasecmp(host, g_token, tl) && host[tl] == '-') {
        char *id = host + tl + 1;
        size_t n = strcspn(id, ".");
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_siteCount; i++)
            if (strlen(g_sites[i].id) == n && !strncasecmp(g_sites[i].id, id, n)) snprintf(root, sizeof root, "%s", g_sites[i].root);
        pthread_mutex_unlock(&g_lock);
    }
    if (!root[0]) { reply_status(fd, "403 Forbidden"); return; }
    long long from = 0, to = -1;
    int ranged = 0;
    char *range = strcasestr(rest, "\r\nRange: bytes=");
    if (range) {
        char *p = range + 15, *e = NULL;
        from = strtoll(p, &e, 10);
        if (e != p && *e == '-') {
            ranged = 1;
            if (e[1] >= '0' && e[1] <= '9') to = strtoll(e + 1, NULL, 10);
        }
    }
    path[strcspn(path, "?#")] = 0;
    url_decode(path);
    char file[2 * MAX_PATH];
    struct stat st;
    if (!resolve(root, path, file, sizeof file) || stat(file, &st) || !S_ISREG(st.st_mode)) { reply_status(fd, "404 Not Found"); return; }
    int f = open(file, O_RDONLY | O_CLOEXEC);
    if (f < 0) { reply_status(fd, "404 Not Found"); return; }
    long long size = st.st_size;
    if (to < 0 || to >= size) to = size - 1;
    if (ranged && from > to) {
        close(f);
        reply_status(fd, "416 Range Not Satisfiable");
        return;
    }
    long long len = to - from + 1;
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\nConnection: close\r\n",
                     ranged ? "206 Partial Content" : "200 OK", type_of(file), size ? len : 0);
    if (ranged) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, size);
    n += snprintf(h + n, sizeof h - (size_t)n, "\r\n");
    if (send(fd, h, (size_t)n, MSG_NOSIGNAL) == n && !head) {
        off_t at = (off_t)from;
        while (len > 0) {
            ssize_t w = sendfile(fd, f, &at, (size_t)SP_MIN(len, 1 << 20));
            if (w <= 0) break;
            len -= w;
        }
    }
    close(f);
}

static void *connection(void *arg)
{
    int fd = (int)(intptr_t)arg;
    serve(fd);
    close(fd);
    return NULL;
}

static void *listener(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, connection, (void *)(intptr_t)fd)) close(fd);
        else pthread_detach(t);
    }
}

/* Port and token stay the same from run to run: at sign-in Plasma brings the page up by itself, and an unchanged
 * address means it just keeps trying until we answer, with no reload after. */
void webhost_start(void)
{
    uint8_t rnd[8];
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sp_random_bytes(rnd, sizeof rnd);
    for (int i = 0; i < 8; i++) snprintf(g_token + i * 2, 3, "%02x", rnd[i]);
    wchar_t path[SP_MAX_PATH];
    path_join(path, SP_COUNTOF(path), paths_cache(), L"webhost.txt");   /* the engine reads it too */
    dir_create_recursive(paths_cache());
    int was = sp_keep_port(path, g_token, sizeof g_token), reuse = 1;
    a.sin_port = htons((uint16_t)was);
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls >= 0) setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (ls >= 0 && was && bind(ls, (struct sockaddr *)&a, sizeof a)) {
        close(ls);
        ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        was = 0;
        a.sin_port = 0;
    }
    if (ls < 0 || (!was && bind(ls, (struct sockaddr *)&a, sizeof a)) || listen(ls, 64) || getsockname(ls, (struct sockaddr *)&a, &al)) {
        LOGW(L"webhost: no socket; web wallpapers load from their files");
        if (ls >= 0) close(ls);
        return;
    }
    g_port = ntohs(a.sin_port);
    sp_save_port(path, g_port, g_token);
    pthread_t t;
    if (!pthread_create(&t, NULL, listener, (void *)(intptr_t)ls)) pthread_detach(t);
}

/* Every local web wallpaper's folder, from the library as it stands. */
void webhost_sync(const Config *c)
{
    Site *list = (Site *)calloc((size_t)SP_MAX(c->count, 1), sizeof *list);
    int n = 0;
    for (int i = 0; list && i < c->count; i++) {
        const WallpaperEntry *e = &c->items[i];
        if (e->type != WP_HTML || wallpaper_is_url(e->path)) continue;
        wchar_t dir[SP_MAX_PATH];
        path_dirname(dir, SP_COUNTOF(dir), e->path);
        char *id = utf8_from_wide(e->id, NULL), *root = sp_path_utf8(dir);
        if (id && root) {
            snprintf(list[n].id, sizeof list[n].id, "%s", id);
            snprintf(list[n].root, sizeof list[n].root, "%s", root);
            n++;
        }
        free(id);
        free(root);
    }
    pthread_mutex_lock(&g_lock);
    Site *old = g_sites;
    g_sites = list;
    g_siteCount = n;
    pthread_mutex_unlock(&g_lock);
    free(old);
}

/* The page's address, NULL when the host isn't up. free() it. */
char *webhost_url(const WallpaperEntry *e)
{
    if (!g_port || e->type != WP_HTML || wallpaper_is_url(e->path)) return NULL;
    char *id = utf8_from_wide(e->id, NULL), *name = utf8_from_wide(path_filename(e->path), NULL);
    StrBuf b = { 0 };
    if (id && name) {
        sb_printf(&b, "http://%s-%s.localhost:%d/", g_token, id, g_port);
        for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p)) sb_append(&b, (const char *)p, 1);
            else sb_printf(&b, "%%%02X", *p);
        }
    }
    free(id);
    free(name);
    sb_append(&b, "", 1);
    if (b.failed) { sb_free(&b); return NULL; }
    return b.p;
}

/* A Customise recording for the Plasma plugin (Recording.qml fetches it). It can't travel in kde_apply's script:
 * that is one dbus-send argument, which Linux caps at 128 KB, and a recording runs to megabytes. Saved as
 * cache/rec/<id>.json; the address names its content, so a new take loads again. NULL when the host isn't up. */
char *webhost_recording(const wchar_t *wallpaperId, const char *json)
{
    if (!g_port || !wallpaperId || !json) return NULL;
    char *dir = recordings_dir(), *id = utf8_from_wide(wallpaperId, NULL);
    char file[2 * MAX_PATH], tmp[2 * MAX_PATH + 8];
    char *url = NULL;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)json; *p; p++) h = (h ^ *p) * 16777619u;
    if (dir && id) {
        mkdir(dir, 0700);
        snprintf(file, sizeof file, "%s/%s.json", dir, id);
        snprintf(tmp, sizeof tmp, "%s.tmp", file);
        FILE *f = fopen(tmp, "wb");
        size_t len = strlen(json);
        int ok = f && fwrite(json, 1, len, f) == len;
        if (f && fclose(f)) ok = 0;
        if (ok && !rename(tmp, file)) {
            StrBuf b = { 0 };
            sb_printf(&b, "http://127.0.0.1:%d/%s-rec/%s.json?v=%08x", g_port, g_token, id, h);
            if (!b.failed) url = b.p;
        } else {
            unlink(tmp);
        }
    }
    free(dir);
    free(id);
    return url;
}
