/* What web wallpapers get from the engine on Windows (audio spectrum, system data, the track that's playing), for
 * pages on Linux. Pages run inside plasmashell or WebKit, so the service can't call them: a small script in the shim
 * (cache/feed.js) opens a WebSocket to 127.0.0.1 with a random token and says what the page listens for, once a
 * second. Nothing is sampled unless a connected page wants it, and nothing at all while paused. */
#include "service.h"
#include "desktop.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include <arpa/inet.h>
#include <complex.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum { CLIENTS = 8, FFT_N = 1024, BANDS = 64, WANT_AUDIO = 1, WANT_SYS = 2, WANT_TRACK = 4 };

typedef struct { int fd, ready, wants; char in[2048]; size_t have; } Client;

static Client  g_c[CLIENTS];
static int     g_listen = -1;
static char    g_token[33];
static volatile int g_paused;
static pid_t   g_rec;
static int     g_recFd = -1;
static float   g_ring[FFT_N];
static size_t  g_ringAt;
static char    g_pending[8];
static size_t  g_pendingN;
static float   g_smooth[BANDS];

static volatile int g_audioOn = 1;

void feed_set_paused(int paused) { g_paused = paused; }
void feed_set_audio(int on) { g_audioOn = on; }

/* ---------------------------------------------------------------- SHA-1 and base64, for the handshake */

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buf[192];
    size_t padded = ((len + 8) / 64 + 1) * 64;
    memset(buf, 0, sizeof buf);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)((uint64_t)len * 8 >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)buf[off+i*4] << 24 | buf[off+i*4+1] << 16 | buf[off+i*4+2] << 8 | buf[off+i*4+3];
        for (int i = 16; i < 80; i++) { uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]; w[i] = x << 1 | x >> 31; }
        for (int i = 0; i < 80; i++) {
            uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) for (int k = 0; k < 4; k++) out[i*4+k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

static void base64(const uint8_t *p, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? p[i+1] << 8 : 0) | (i + 2 < n ? p[i+2] : 0);
        out[o++] = t[v >> 18]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- WebSocket */

static void drop(Client *c)
{
    close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void send_text(Client *c, const char *s, size_t n)
{
    uint8_t h[10];
    size_t hl = 2;
    h[0] = 0x81;
    if (n < 126) h[1] = (uint8_t)n;
    else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    if (send(c->fd, h, hl, MSG_NOSIGNAL) != (ssize_t)hl || send(c->fd, s, n, MSG_NOSIGNAL) != (ssize_t)n) drop(c);
}

static void broadcast(int want, const char *s, size_t n)
{
    for (int i = 0; i < CLIENTS; i++)
        if (g_c[i].fd >= 0 && g_c[i].ready && (g_c[i].wants & want)) send_text(&g_c[i], s, n);
}

static void handshake(Client *c)
{
    char *end = strstr(c->in, "\r\n\r\n"), *key = strcasestr(c->in, "Sec-WebSocket-Key:");
    if (!end) return;
    char want[48];
    snprintf(want, sizeof want, "GET /%s ", g_token);
    if (!key || strncmp(c->in, want, strlen(want))) { drop(c); return; }
    key += 18;
    while (*key == ' ') key++;
    char accept[128], digest64[32], reply[256];
    size_t n = strcspn(key, "\r\n");
    if (n > 60) { drop(c); return; }
    snprintf(accept, sizeof accept, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)n, key);
    uint8_t digest[20];
    sha1((const uint8_t *)accept, strlen(accept), digest);
    base64(digest, 20, digest64);
    int m = snprintf(reply, sizeof reply, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", digest64);
    if (send(c->fd, reply, (size_t)m, MSG_NOSIGNAL) != m) { drop(c); return; }
    c->ready = 1;
    c->have = 0;
}

/* The page's frames: masked text saying what it wants. */
static void read_frames(Client *c)
{
    while (c->have >= 6) {
        uint8_t *p = (uint8_t *)c->in;
        size_t len = p[1] & 127;
        if ((p[0] & 15) == 8 || len > 100) { drop(c); return; }
        if (c->have < 6 + len) return;
        char text[104];
        for (size_t i = 0; i < len; i++) text[i] = (char)(p[6 + i] ^ p[2 + (i & 3)]);
        text[len] = 0;
        if ((p[0] & 15) == 1) c->wants = atoi(text);
        memmove(c->in, c->in + 6 + len, c->have - 6 - len);
        c->have -= 6 + len;
    }
}

static void on_client(Client *c)
{
    ssize_t n = recv(c->fd, c->in + c->have, sizeof c->in - 1 - c->have, 0);
    if (n <= 0) { drop(c); return; }
    c->have += (size_t)n;
    c->in[c->have] = 0;
    if (!c->ready) handshake(c);
    else read_frames(c);
}

/* ---------------------------------------------------------------- audio */

static void audio_stop(void)
{
    if (g_rec <= 0) return;
    kill(g_rec, SIGTERM);
    waitpid(g_rec, NULL, 0);
    close(g_recFd);
    g_rec = 0;
    g_recFd = -1;
    memset(g_smooth, 0, sizeof g_smooth);
}

/* What the system plays, not the microphone: pw-record on the default output's monitor. */
static void audio_start(void)
{
    int fd[2];
    if (g_rec > 0 || pipe2(fd, O_CLOEXEC | O_NONBLOCK)) return;
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);           /* never outlives the service */
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(fd[1], 1); dup2(null, 2);
        fcntl(fd[1], F_SETFL, 0);
        fcntl(1, F_SETFL, 0);
        execlp("pw-record", "pw-record", "-P", "{ stream.capture.sink=true node.name=screenplus-audio }", "--rate", "44100",
               "--channels", "1", "--format", "f32", "-", (char *)NULL);
        execlp("parec", "parec", "-d", "@DEFAULT_MONITOR@", "--format=float32le", "--rate=44100", "--channels=1", "--raw", (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    if (pid < 0) { close(fd[0]); return; }
    g_rec = pid;
    g_recFd = fd[0];
    g_pendingN = 0;
}

static void audio_read(void)
{
    char buf[16384];
    ssize_t n;
    while ((n = read(g_recFd, buf + g_pendingN, sizeof buf - g_pendingN)) > 0) {
        size_t have = g_pendingN + (size_t)n, whole = have / 4 * 4;
        for (size_t i = 0; i < whole; i += 4) {
            float v;
            memcpy(&v, buf + i, 4);
            g_ring[g_ringAt++ % FFT_N] = v;
        }
        g_pendingN = have - whole;
        memmove(buf, buf + whole, g_pendingN);
    }
}

static void fft(float complex *a, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float complex t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float complex w = cexpf(-2.0f * (float)M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            float complex u = 1;
            for (int k = 0; k < len / 2; k++, u *= w) {
                float complex x = a[i + k], y = a[i + k + len / 2] * u;
                a[i + k] = x + y;
                a[i + k + len / 2] = x - y;
            }
        }
    }
}

static void send_audio(void)
{
    static float complex a[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        float hann = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
        a[i] = g_ring[(g_ringAt + i) % FFT_N] * hann;
    }
    fft(a, FFT_N);
    StrBuf b = { 0 };
    sb_puts(&b, "{\"a\":[");
    for (int k = 0; k < BANDS; k++) {           /* log-spaced, as audio_visualizer.c */
        int lo = 1 + (int)pow(FFT_N / 2.0, (double)k / BANDS), hi = 1 + (int)pow(FFT_N / 2.0, (double)(k + 1) / BANDS);
        float m = 0;
        for (int i = lo; i <= hi && i < FFT_N / 2; i++) m = fmaxf(m, cabsf(a[i]));
        float db = 20.0f * log10f(m / (FFT_N / 4.0f) + 1e-6f);
        float v = fminf(1.0f, fmaxf(0.0f, (db + 70.0f) / 70.0f));
        g_smooth[k] = v > g_smooth[k] ? v : g_smooth[k] * 0.8f + v * 0.2f;
        sb_printf(&b, "%s%.4f", k ? "," : "", g_smooth[k]);
    }
    sb_puts(&b, "]}");
    if (!b.failed) broadcast(WANT_AUDIO, b.p, b.len);
    sb_free(&b);
}

/* ---------------------------------------------------------------- system data */

static void first_line_value(const char *path, const char *key, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    char line[512];
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key))) continue;
        char *v = strchr(line, ':');
        if (v) {
            v++;
            while (*v == ' ' || *v == '\t') v++;
            v[strcspn(v, "\n")] = 0;
            snprintf(out, cap, "%s", v);
        }
        break;
    }
    fclose(f);
}

static void send_sys(void)
{
    static unsigned long long lastBusy, lastAll, lastRx, lastTx;
    static double lastAt;
    static char cpuName[128];
    unsigned long long u = 0, n = 0, s = 0, id = 0, w = 0, q = 0, sq = 0, st = 0, rx = 0, tx = 0;
    if (!cpuName[0]) first_line_value("/proc/cpuinfo", "model name", cpuName, sizeof cpuName);
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &id, &w, &q, &sq, &st) != 8) u = 0;
        fclose(f);
    }
    unsigned long long busy = u + n + s + q + sq + st, all = busy + id + w;
    double cpu = all > lastAll ? 100.0 * (double)(busy - lastBusy) / (double)(all - lastAll) : 0.0;
    lastBusy = busy; lastAll = all;
    char total[64], avail[64];
    first_line_value("/proc/meminfo", "MemTotal", total, sizeof total);
    first_line_value("/proc/meminfo", "MemAvailable", avail, sizeof avail);
    double totalMb = atof(total) / 1024.0, availMb = atof(avail) / 1024.0;
    f = fopen("/proc/net/dev", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *c = strchr(line, ':');
            unsigned long long r, t, skip;
            if (!c || strstr(line, "lo:")) continue;
            if (sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &r, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &t) == 9) { rx += r; tx += t; }
        }
        fclose(f);
    }
    double now = time_ms(), secs = lastAt ? (now - lastAt) / 1000.0 : 0.0;
    double down = secs > 0 ? (double)(rx - lastRx) / secs : 0, up = secs > 0 ? (double)(tx - lastTx) / secs : 0;
    lastAt = now; lastRx = rx; lastTx = tx;
    StrBuf b = { 0 };
    sb_puts(&b, "{\"s\":{\"NameCpu\":");
    json_write_string(&b, cpuName);
    sb_printf(&b, ",\"CurrentCpu\":%.1f,\"CurrentGpu3D\":0,\"CurrentRamAvail\":%.0f,\"TotalRam\":%.0f,\"CurrentNetDown\":%.0f,\"CurrentNetUp\":%.0f,"
                  "\"MemoryUsage\":%d,\"TotalMemory\":%.0f}}", cpu, availMb, totalMb, down, up,
              totalMb > 0 ? (int)(100.0 - availMb * 100.0 / totalMb) : 0, totalMb * 1048576.0);
    if (!b.failed) broadcast(WANT_SYS, b.p, b.len);
    sb_free(&b);
}

/* ---------------------------------------------------------------- the track that's playing (MPRIS) */

/* In dbus-send's listing: the string after `key`, or for an array its first one. */
static void mpris_value(const char *text, const char *key, char *out, size_t cap)
{
    char q[64];
    out[0] = 0;
    snprintf(q, sizeof q, "\"%s\"", key);
    const char *p = strstr(text, q);
    if (!p || !(p = strstr(p + strlen(q), "string \""))) return;
    p += 8;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

static void send_track(int force)
{
    static char last[1024];
    static char text[32768];
    const char *names[] = { "dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.ListNames", NULL };
    char dest[160] = "";
    if (!sp_run(names, text, sizeof text)) {
        const char *p = strstr(text, "org.mpris.MediaPlayer2.");
        if (p) snprintf(dest, sizeof dest, "--dest=%.*s", (int)strcspn(p, "\""), p);
    }
    StrBuf b = { 0 };
    char title[256] = "", artist[256] = "", album[256] = "", art[512] = "";
    if (dest[0]) {
        const char *get[] = { "dbus-send", "--session", "--print-reply", dest, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties.Get",
                              "string:org.mpris.MediaPlayer2.Player", "string:Metadata", NULL };
        if (!sp_run(get, text, sizeof text)) {
            mpris_value(text, "xesam:title", title, sizeof title);
            mpris_value(text, "xesam:artist", artist, sizeof artist);
            mpris_value(text, "xesam:album", album, sizeof album);
            mpris_value(text, "mpris:artUrl", art, sizeof art);
        }
    }
    char key[1024];
    snprintf(key, sizeof key, "%s|%s|%s", title, artist, album);
    if (!force && !strcmp(key, last)) return;
    snprintf(last, sizeof last, "%s", key);
    if (!title[0]) {
        sb_puts(&b, "{\"t\":null}");
    } else {
        sb_puts(&b, "{\"t\":{\"Title\":");       json_write_string(&b, title);
        sb_puts(&b, ",\"Artist\":");              json_write_string(&b, artist);
        sb_puts(&b, ",\"AlbumTitle\":");          json_write_string(&b, album);
        sb_puts(&b, ",\"AlbumArtist\":");         json_write_string(&b, artist);
        sb_puts(&b, ",\"Subtitle\":\"\",\"TrackNumber\":0,\"AlbumTrackCount\":0,\"Thumbnail\":");
        char *data = NULL;
        if (!strncmp(art, "file://", 7)) {
            FILE *f = fopen(art + 7, "rb");
            if (f) {
                static uint8_t pic[1 << 20];
                size_t n = fread(pic, 1, sizeof pic, f);
                fclose(f);
                if (n && n < sizeof pic && (data = (char *)malloc(n / 3 * 4 + 8))) base64(pic, n, data);
            }
        }
        if (data) { sb_puts(&b, "\""); sb_puts(&b, data); sb_puts(&b, "\""); free(data); }
        else sb_puts(&b, "null");
        sb_puts(&b, "}}");
    }
    if (!b.failed) broadcast(WANT_TRACK, b.p, b.len);
    sb_free(&b);
}

/* ---------------------------------------------------------------- the thread */

static void *worker(void *arg)
{
    (void)arg;
    double nextSys = 0, nextTrack = 0;
    int hadTrack = 0;
    for (;;) {
        struct pollfd p[CLIENTS + 2];
        int n = 0, want = 0, map[CLIENTS + 2];
        p[n].fd = g_listen; p[n].events = POLLIN; map[n++] = -1;
        for (int i = 0; i < CLIENTS; i++) {
            if (g_c[i].fd < 0) continue;
            p[n].fd = g_c[i].fd; p[n].events = POLLIN; map[n++] = i;
            if (g_c[i].ready) want |= g_c[i].wants;
        }
        if (g_paused) want = 0;
        if (!g_audioOn) want &= ~WANT_AUDIO;
        if (want & WANT_AUDIO) audio_start();
        else audio_stop();
        if (g_rec > 0) { p[n].fd = g_recFd; p[n].events = POLLIN; map[n++] = -2; }
        int r = poll(p, (nfds_t)n, want & WANT_AUDIO ? 33 : want ? 250 : 1000);
        if (r > 0) {
            for (int i = 0; i < n; i++) {
                if (!p[i].revents) continue;
                if (map[i] == -1) {
                    int fd = accept4(g_listen, NULL, NULL, SOCK_CLOEXEC);
                    int slot = -1;
                    for (int k = 0; k < CLIENTS; k++) if (g_c[k].fd < 0) slot = k;
                    if (fd >= 0 && slot >= 0) g_c[slot].fd = fd;
                    else if (fd >= 0) close(fd);
                } else if (map[i] == -2) {
                    audio_read();
                } else if (g_c[map[i]].fd >= 0) {
                    on_client(&g_c[map[i]]);
                }
            }
        }
        double now = time_ms();
        if (want & WANT_AUDIO) send_audio();
        if ((want & WANT_SYS) && now >= nextSys) { nextSys = now + 250; send_sys(); }
        if ((want & WANT_TRACK) && now >= nextTrack) {
            nextTrack = now + 2000;
            send_track(!hadTrack);
            hadTrack = 1;
        } else if (!(want & WANT_TRACK)) {
            hadTrack = 0;
        }
    }
    return NULL;
}

void feed_start(void)
{
    uint8_t rnd[16];
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < CLIENTS; i++) g_c[i].fd = -1;
    sp_random_bytes(rnd, sizeof rnd);
    for (int i = 0; i < 16; i++) snprintf(g_token + i * 2, 3, "%02x", rnd[i]);
    wchar_t keep[SP_MAX_PATH];
    path_join(keep, SP_COUNTOF(keep), paths_cache(), L"feed.txt");
    int was = sp_keep_port(keep, g_token, sizeof g_token);
    a.sin_port = htons((uint16_t)was);
    g_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    if (g_listen >= 0) setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (g_listen >= 0 && was && bind(g_listen, (struct sockaddr *)&a, sizeof a)) {
        close(g_listen);                            /* taken: any port will do */
        g_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        was = 0;
        a.sin_port = 0;
    }
    if (g_listen < 0 || (!was && bind(g_listen, (struct sockaddr *)&a, sizeof a)) || listen(g_listen, 8) || getsockname(g_listen, (struct sockaddr *)&a, &al)) {
        LOGW(L"feed: no socket; pages get no audio, system or track data");
        return;
    }
    sp_save_port(keep, ntohs(a.sin_port), g_token);
    wchar_t path[SP_MAX_PATH];
    path_join(path, SP_COUNTOF(path), paths_cache(), L"feed.js");
    dir_create_recursive(paths_cache());
    char *p = sp_path_utf8(path);
    int fd = p ? open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    free(p);
    if (f) {
        fprintf(f, "(function(){function go(){var w;try{w=new WebSocket('ws://127.0.0.1:%d/%s');}catch(e){return;}var t;"
                   "w.onopen=function(){function s(){if(w.readyState==1&&window.__sp)w.send(String(__sp.wants()));}s();t=setInterval(s,1000);};"
                   "w.onmessage=function(e){try{var m=JSON.parse(e.data);if(m.a)__sp.audio(m.a);if(m.s)__sp.sysinfo(m.s);if('t' in m)__sp.track(m.t);}catch(x){}};"
                   "w.onclose=function(){clearInterval(t);setTimeout(go,3000);};}go();})();\n", ntohs(a.sin_port), g_token);
        fclose(f);
    }
    pthread_t t;
    if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t);
}
