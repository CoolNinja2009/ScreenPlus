/* webfetch.h on Linux: each request is a `curl` child whose stdout carries the response headers and then the body, so
 * the service stays plain libc. There's no shared session (http_open_shared is http_open with compression): curl
 * pays for a connection per request, which the Store's worker threads hide. */
#include "../common/webfetch.h"
#include "../common/util.h"
#include "desktop.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>

extern char **environ;

#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

struct HttpGet {
    pid_t   pid;
    int     fd, status, ranges;
    int64_t length;
    wchar_t type[64];
    char    buf[16384];
    size_t  have, at;           /* body bytes already read past the headers */
};

/* The next header line, without its line end. 0 at the end of the stream. */
static int next_line(HttpGet *g, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (g->at == g->have) {
            ssize_t r = read(g->fd, g->buf, sizeof g->buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return 0;
            g->have = (size_t)r;
            g->at = 0;
        }
        char c = g->buf[g->at++];
        if (c == '\n') break;
        if (c != '\r' && n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static HttpGet *open_url(const wchar_t *url, int allowHttp, int compressed, wchar_t *err, size_t cap)
{
    char *u = utf8_from_wide(url, NULL);
    HttpGet *g = (HttpGet *)calloc(1, sizeof *g);
    int fd[2];
    if (!u || !g || pipe2(fd, O_CLOEXEC)) {
        free(u); free(g);
        str_copy(err, cap, L"out of memory");
        return NULL;
    }
    const char *argv[24];
    int n = 0;
    argv[n++] = "curl"; argv[n++] = "-sS"; argv[n++] = "-i"; argv[n++] = "-L"; argv[n++] = "--max-redirs"; argv[n++] = "8";
    argv[n++] = "--connect-timeout"; argv[n++] = "20"; argv[n++] = "--speed-time"; argv[n++] = "60"; argv[n++] = "--speed-limit"; argv[n++] = "1";
    argv[n++] = "-A"; argv[n++] = USER_AGENT;
    argv[n++] = "--proto"; argv[n++] = allowHttp ? "=http,https" : "=https";
    argv[n++] = "--proto-redir"; argv[n++] = allowHttp ? "=http,https" : "=https";
    if (compressed) argv[n++] = "--compressed";
    argv[n++] = "--"; argv[n++] = u; argv[n] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    int rc = posix_spawnp(&g->pid, "curl", &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    free(u);
    if (rc) {
        close(fd[0]);
        free(g);
        str_copy(err, cap, L"curl is not installed");
        return NULL;
    }
    g->fd = fd[0];
    g->length = -1;
    char line[4096];
    for (;;) {                                  /* every hop of a redirect brings its own headers */
        if (!next_line(g, line, sizeof line)) {
            http_close(g);
            str_copy(err, cap, L"no answer from the server");
            return NULL;
        }
        if (strncasecmp(line, "HTTP/", 5)) continue;
        const char *sp = strchr(line, ' ');
        g->status = sp ? atoi(sp + 1) : 0;
        g->length = -1;
        g->ranges = 0;
        g->type[0] = 0;
        int redirect = 0;
        while (next_line(g, line, sizeof line) && line[0]) {
            char *v = strchr(line, ':');
            if (!v) continue;
            *v++ = 0;
            while (*v == ' ') v++;
            if (!strcasecmp(line, "content-length")) g->length = strtoll(v, NULL, 10);
            else if (!strcasecmp(line, "accept-ranges")) g->ranges = !strcasecmp(v, "bytes");
            else if (!strcasecmp(line, "location")) redirect = 1;
            else if (!strcasecmp(line, "content-type")) {
                char *semi = strchr(v, ';');
                if (semi) *semi = 0;
                for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
                wchar_t *w = wide_from_utf8(v, -1);
                if (w) str_copy(g->type, SP_COUNTOF(g->type), w);
                free(w);
                str_trim(g->type);
            }
        }
        if (g->status >= 100 && g->status < 200) continue;
        if (g->status >= 300 && g->status < 400 && redirect) continue;
        if (g->status == 200 && !g->type[0] && g->length < 0 && g->at == g->have) {
            /* a proxy's "200 Connection established" has no fields: the real answer follows */
            ssize_t r = read(g->fd, g->buf, sizeof g->buf);
            if (r > 0) { g->have = (size_t)r; g->at = 0; if (!strncasecmp(g->buf, "HTTP/", 5)) continue; }
        }
        break;
    }
    if (compressed) g->length = -1;             /* curl's own count of the unpacked body isn't known ahead */
    return g;
}

HttpGet *http_open(const wchar_t *url, int allowHttp, wchar_t *err, size_t errCap) { return open_url(url, allowHttp, 0, err, errCap); }
HttpGet *http_open_shared(const wchar_t *url, wchar_t *err, size_t errCap) { return open_url(url, 0, 1, err, errCap); }
int      http_status(const HttpGet *g) { return g->status; }
int      http_ranges(const HttpGet *g) { return g->ranges; }
int64_t  http_length(const HttpGet *g) { return g->length; }
const wchar_t *http_content_type(const HttpGet *g) { return g->type; }

int http_read(HttpGet *g, char *buf, int cap)
{
    int n = 0;
    if (g->at < g->have) {
        n = (int)SP_MIN((size_t)cap, g->have - g->at);
        memcpy(buf, g->buf + g->at, (size_t)n);
        g->at += (size_t)n;
    }
    while (n < cap) {
        ssize_t r = read(g->fd, buf + n, (size_t)(cap - n));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (int)r;
    }
    return n;
}

void http_close(HttpGet *g)
{
    if (!g) return;
    close(g->fd);
    int st;
    kill(g->pid, SIGTERM);
    while (waitpid(g->pid, &st, 0) < 0 && errno == EINTR) {}
    free(g);
}

/* ------------------------------------------------------------------ SHA-256 */

typedef struct { uint32_t h[8]; uint64_t bits; uint8_t b[64]; size_t n; } Sha;

static const uint32_t k_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const uint8_t *p)
{
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k_k[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, sizeof a - sizeof a[0]);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}

static void sha_init(Sha *s)
{
    static const uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memset(s, 0, sizeof *s);
    memcpy(s->h, h, sizeof h);
}

static void sha_add(Sha *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bits += (uint64_t)len * 8;
    while (len) {
        size_t k = SP_MIN(len, 64 - s->n);
        memcpy(s->b + s->n, p, k);
        s->n += k; p += k; len -= k;
        if (s->n == 64) { sha_block(s, s->b); s->n = 0; }
    }
}

static void sha_hex(Sha *s, char hex[65])
{
    uint64_t bits = s->bits;
    uint8_t pad = 0x80, z = 0, len[8];
    sha_add(s, &pad, 1);
    while (s->n != 56) sha_add(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_add(s, len, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", s->h[i]);
}

BOOL file_sha256(const wchar_t *path, char hex[65])
{
    char *p = sp_path_utf8(path);
    FILE *f = p ? fopen(p, "rb") : NULL;
    free(p);
    if (!f) return FALSE;
    Sha s;
    sha_init(&s);
    static __thread char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha_add(&s, buf, n);
    fclose(f);
    sha_hex(&s, hex);
    return TRUE;
}

BOOL http_save(HttpGet *g, const wchar_t *dest, int64_t maxBytes, FetchProgress cb, void *ctx, char *sha256Hex, wchar_t *err, size_t errCap)
{
    char *p = sp_path_utf8(dest);
    FILE *f = p ? fopen(p, "wb") : NULL;
    if (!f) {
        free(p);
        str_copy(err, errCap, L"could not write the file");
        return FALSE;
    }
    Sha s;
    sha_init(&s);
    char *buf = (char *)malloc(1 << 16);
    int64_t done = 0;
    BOOL ok = buf != NULL;
    for (int n; ok && (n = http_read(g, buf, 1 << 16)) > 0; ) {
        done += n;
        if (maxBytes > 0 && done > maxBytes) { ok = FALSE; str_copy(err, errCap, L"the file is too large"); break; }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = FALSE; str_copy(err, errCap, L"the disk is full"); break; }
        sha_add(&s, buf, (size_t)n);
        if (cb && !cb(ctx, done, g->length)) { ok = FALSE; str_copy(err, errCap, L"cancelled"); break; }
    }
    if (ok && g->length >= 0 && done != g->length) { ok = FALSE; str_copy(err, errCap, L"the download was cut off"); }
    free(buf);
    if (fclose(f)) ok = FALSE;
    if (!ok) unlink(p);
    else if (sha256Hex) sha_hex(&s, sha256Hex);
    free(p);
    return ok;
}
