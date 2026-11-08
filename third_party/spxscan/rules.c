/* Script rules (docs/design.md §8.1.5). Text matching, not parsing: GDScript, Godot text and binary resources
 * (embedded scripts are plain UTF-8 inside them) and web files. A pack must reach the system through
 * ScreenPlus.call_api / window.screenplus; direct device, network, process and native access is blocked, and
 * code that hides what it does is flagged. Misses are possible by design; the broker and sandbox still hold. */
#include "scan_int.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

int ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

TextKind rules_kind(const char *name)
{
    static const char *godot[] = { ".gd", ".tscn", ".tres", ".scn", ".res", ".godot", ".binary", ".cfg", ".gdshader", ".remap" };
    static const char *web[] = { ".js", ".mjs", ".html", ".htm", ".svg" };
    for (size_t i = 0; i < sizeof(godot) / sizeof(*godot); i++) if (ends_with_ci(name, godot[i])) return TK_GODOT;
    for (size_t i = 0; i < sizeof(web) / sizeof(*web); i++) if (ends_with_ci(name, web[i])) return TK_WEB;
    return TK_NONE;
}

const char *rules_binary_magic(const unsigned char *b, size_t n)
{
    if (n >= 64 && b[0] == 'M' && b[1] == 'Z') {
        size_t pe = (size_t)b[60] | (size_t)b[61] << 8 | (size_t)b[62] << 16 | (size_t)b[63] << 24;
        if (pe + 4 <= n && !memcmp(b + pe, "PE\0\0", 4)) return "Windows executable or DLL";
        return "DOS executable";
    }
    if (n >= 4 && !memcmp(b, "\x7f" "ELF", 4)) return "Linux executable or library";
    if (n >= 4 && (!memcmp(b, "\xfe\xed\xfa\xce", 4) || !memcmp(b, "\xfe\xed\xfa\xcf", 4) || !memcmp(b, "\xce\xfa\xed\xfe", 4) ||
                   !memcmp(b, "\xcf\xfa\xed\xfe", 4) || !memcmp(b, "\xca\xfe\xba\xbe", 4))) return "macOS executable or Java class";
    if (n >= 4 && !memcmp(b, "\0asm", 4)) return "WebAssembly";
    if (n >= 4 && !memcmp(b, "PK\3\4", 4)) return "zip archive";
    if (n >= 6 && !memcmp(b, "7z\xbc\xaf\x27\x1c", 6)) return "7-Zip archive";
    if (n >= 4 && !memcmp(b, "Rar!", 4)) return "RAR archive";
    if (n >= 8 && !memcmp(b, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) return "Windows installer or Office file";
    if (n >= 8 && !memcmp(b, "\x4c\0\0\0\x01\x14\x02\0", 8)) return "Windows shortcut";
    if (n >= 2 && b[0] == '#' && b[1] == '!') return "shell script";
    return NULL;
}

static int ident(int c) { return isalnum(c) || c == '_'; }

typedef struct Rule { const char *pat; SpxSeverity sev; const char *code; const char *msg; } Rule;

static const Rule k_godot[] = {
    { "OS.execute", SPX_BLOCK, "script.process", "starts programs; use the process capability" },
    { "OS.execute_with_pipe", SPX_BLOCK, "script.process", "starts programs; use the process capability" },
    { "OS.create_process", SPX_BLOCK, "script.process", "starts programs; use the process capability" },
    { "OS.create_instance", SPX_BLOCK, "script.process", "starts programs; use the process capability" },
    { "OS.shell_open", SPX_BLOCK, "script.process", "opens files or links outside the wallpaper" },
    { "OS.shell_show_in_file_manager", SPX_BLOCK, "script.process", "opens the file manager" },
    { "OS.kill", SPX_BLOCK, "script.process", "kills processes" },
    { "OS.get_environment", SPX_WARN, "script.env", "reads environment variables" },
    { "OS.set_environment", SPX_BLOCK, "script.env", "changes environment variables" },
    { "OS.open_midi_inputs", SPX_BLOCK, "script.device", "opens MIDI devices directly; use the midi capability" },
    { "HTTPRequest", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "HTTPClient", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "StreamPeerTCP", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "StreamPeerTLS", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "TCPServer", SPX_BLOCK, "script.direct-net", "listens on the network directly; use net.server" },
    { "UDPServer", SPX_BLOCK, "script.direct-net", "listens on the network directly; use net.server" },
    { "PacketPeerUDP", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "PacketPeerDTLS", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "DTLSServer", SPX_BLOCK, "script.direct-net", "listens on the network directly; use net.server" },
    { "WebSocketPeer", SPX_BLOCK, "script.direct-net", "uses the network directly; use ScreenPlus.call_api(\"net.*\")" },
    { "WebSocketMultiplayerPeer", SPX_BLOCK, "script.direct-net", "uses the network directly" },
    { "ENetConnection", SPX_BLOCK, "script.direct-net", "uses the network directly" },
    { "ENetMultiplayerPeer", SPX_BLOCK, "script.direct-net", "uses the network directly" },
    { "WebRTCPeerConnection", SPX_BLOCK, "script.direct-net", "uses the network directly" },
    { "IP.resolve_hostname", SPX_BLOCK, "script.direct-net", "resolves host names directly" },
    { "CameraServer", SPX_BLOCK, "script.device", "opens cameras directly; use the camera capability" },
    { "AudioStreamMicrophone", SPX_BLOCK, "script.device", "records the microphone directly; use the mic capability" },
    { "AudioEffectCapture", SPX_WARN, "script.device", "captures an audio bus (with a microphone input this records the user)" },
    { "DisplayServer.clipboard_get", SPX_BLOCK, "script.device", "reads the clipboard directly; use the clipboard capability" },
    { "DisplayServer.clipboard_set", SPX_BLOCK, "script.device", "writes the clipboard directly; use the clipboard capability" },
    { "GDExtensionManager", SPX_BLOCK, "script.native", "loads native code" },
    { "ProjectSettings.load_resource_pack", SPX_BLOCK, "script.load-pack", "loads more code at run time" },
    { "JavaScriptBridge", SPX_BLOCK, "script.js", "runs JavaScript; web builds reach the page through ScreenPlus" },
    { "bytes_to_var_with_objects", SPX_BLOCK, "script.object-decode", "turns data into objects with scripts (code from data)" },
    { "dict_to_inst", SPX_WARN, "script.object-decode", "turns data into objects" },
    { "str_to_var", SPX_WARN, "script.object-decode", "turns text into values (objects too)" },
    { "Expression", SPX_WARN, "script.eval", "evaluates code built at run time" },
    { "set_script", SPX_WARN, "script.set-script", "swaps scripts at run time" },
    { "GDScript.new", SPX_BLOCK, "script.eval", "compiles code at run time" },
    { "ClassDB.instantiate", SPX_WARN, "script.dynamic", "creates objects by a name chosen at run time" },
    { "Marshalls.base64_to_raw", SPX_WARN, "script.decode", "decodes base64 data" },
    { "Marshalls.base64_to_utf8", SPX_WARN, "script.decode", "decodes base64 text" },
};

static const Rule k_web[] = {
    { "window.chrome.webview", SPX_BLOCK, "web.host", "talks to the WebView2 host directly; use window.screenplus" },
    { "navigator.mediaDevices", SPX_BLOCK, "web.device", "opens cameras or microphones directly; use window.screenplus" },
    { "navigator.clipboard", SPX_BLOCK, "web.device", "uses the clipboard directly; use window.screenplus" },
    { "navigator.geolocation", SPX_BLOCK, "web.device", "reads the location directly; use window.screenplus" },
    { "WebAssembly.instantiate", SPX_WARN, "web.wasm", "runs WebAssembly" },
    { "WebAssembly.compile", SPX_WARN, "web.wasm", "compiles WebAssembly" },
    { "XMLHttpRequest", SPX_INFO, "web.net", "uses the network (the page's CSP allows only declared hosts)" },
    { "fetch", SPX_INFO, "web.net", "uses the network (the page's CSP allows only declared hosts)" },
    { "WebSocket", SPX_INFO, "web.net", "uses the network (the page's CSP allows only declared hosts)" },
    { "atob", SPX_INFO, "web.decode", "decodes base64" },
    { "document.write", SPX_WARN, "web.write", "writes markup at run time" },
};

/* Open-tier namespaces need no declaration. */
static int open_ns(const char *ns) { return !strcmp(ns, "sp") || !strcmp(ns, "gpu") || !strcmp(ns, "quality"); }

typedef struct Ctx {
    SpxReport *r;
    const Manifest *m;
    const char *file;
    const unsigned char *b;
    size_t n;
    int text;          /* line numbers and comments make sense */
    size_t line_pos;   /* line counting cache */
    int line;
} Ctx;

static int line_of(Ctx *c, size_t pos)
{
    if (!c->text) return 0;
    if (pos < c->line_pos) { c->line_pos = 0; c->line = 1; }
    for (; c->line_pos < pos; c->line_pos++) if (c->b[c->line_pos] == '\n') c->line++;
    return c->line;
}

/* A match on a line whose code part starts with a comment marker is skipped. */
static int in_comment(const Ctx *c, size_t pos, TextKind k)
{
    if (!c->text) return 0;
    size_t s = pos;
    while (s > 0 && c->b[s - 1] != '\n') s--;
    while (s < pos && (c->b[s] == ' ' || c->b[s] == '\t')) s++;
    if (k == TK_GODOT) {
        for (size_t i = s; i < pos; i++) if (c->b[i] == '#') return 1;
        return 0;
    }
    return pos - s >= 2 && c->b[s] == '/' && (c->b[s + 1] == '/' || c->b[s + 1] == '*');
}

static const unsigned char *find(const unsigned char *h, size_t hn, const char *needle, size_t from)
{
    size_t nn = strlen(needle);
    for (size_t i = from; i + nn <= hn; i++)
        if (h[i] == (unsigned char)needle[0] && !memcmp(h + i, needle, nn)) return h + i;
    return NULL;
}

/* Visits each whole-word match of pat. */
#define EACH_MATCH(c, pat, posvar) \
    for (const unsigned char *_m = find((c)->b, (c)->n, (pat), 0); _m; _m = find((c)->b, (c)->n, (pat), (size_t)(_m - (c)->b) + 1)) \
        for (size_t posvar = (size_t)(_m - (c)->b), _once = 1; _once; _once = 0) \
            if (!(posvar > 0 && ident((c)->b[posvar - 1]) && ident((unsigned char)(pat)[0])) && \
                !(posvar + strlen(pat) < (c)->n && ident((c)->b[posvar + strlen(pat)]) && ident((unsigned char)(pat)[strlen(pat) - 1])))

/* After "name(" skips blanks; returns the first argument's first byte position or n. */
static size_t arg_start(const Ctx *c, size_t pos)
{
    while (pos < c->n && c->b[pos] != '(' && c->b[pos] != '\n') pos++;
    if (pos >= c->n || c->b[pos] != '(') return c->n;
    pos++;
    while (pos < c->n && (c->b[pos] == ' ' || c->b[pos] == '\t')) pos++;
    return pos;
}

static int lit_prefix(const Ctx *c, size_t pos, const char *prefix)
{
    size_t pn = strlen(prefix);
    return pos < c->n && (c->b[pos] == '"' || c->b[pos] == '\'') && pos + 1 + pn <= c->n && !memcmp(c->b + pos + 1, prefix, pn);
}

static void godot_paths(Ctx *c)
{
    static const char *file_calls[] = { "FileAccess.open", "FileAccess.open_compressed", "FileAccess.open_encrypted",
        "FileAccess.get_file_as_bytes", "FileAccess.get_file_as_string", "DirAccess.open", "DirAccess.make_dir_absolute",
        "DirAccess.make_dir_recursive_absolute", "DirAccess.remove_absolute", "DirAccess.rename_absolute", "DirAccess.copy_absolute",
        "DirAccess.get_files_at", "DirAccess.get_directories_at", "ResourceSaver.save", "Image.load_from_file" };
    for (size_t i = 0; i < sizeof(file_calls) / sizeof(*file_calls); i++) {
        EACH_MATCH(c, file_calls[i], pos) {
            if (in_comment(c, pos, TK_GODOT)) continue;
            size_t a = arg_start(c, pos + strlen(file_calls[i]));
            if (a >= c->n) continue;
            if (lit_prefix(c, a, "res://") || lit_prefix(c, a, "user://")) continue;
            if (c->b[a] == '"' || c->b[a] == '\'')
                spx_add(c->r, SPX_BLOCK, "script.fs-path", c->file, line_of(c, pos), "%s with a path outside res:// and user://; use fs.* capabilities", file_calls[i]);
            else if (c->text)
                spx_add(c->r, SPX_WARN, "script.fs-computed", c->file, line_of(c, pos), "%s with a computed path (only res:// and user:// will open)", file_calls[i]);
        }
    }
    /* load()/ResourceLoader.load(): preload is always a literal; a computed or user:// path can load code the pack wrote. */
    EACH_MATCH(c, "load", pos) {
        if (in_comment(c, pos, TK_GODOT) || !c->text) continue;
        if (pos > 0 && c->b[pos - 1] == '.' && !(pos >= 15 && !memcmp(c->b + pos - 15, "ResourceLoader.", 15))) continue;
        if (pos + 4 >= c->n || c->b[pos + 4] != '(') continue;
        size_t a = arg_start(c, pos + 4);
        if (a >= c->n || lit_prefix(c, a, "res://")) continue;
        spx_add(c->r, lit_prefix(c, a, "user://") ? SPX_WARN : SPX_WARN, "script.load-dynamic", c->file, line_of(c, pos),
                lit_prefix(c, a, "user://") ? "loads a resource from user:// (data the pack wrote can become code)" : "load() with a computed path");
    }
    /* call()/callv() with a computed method name hides what is called. */
    static const char *calls[] = { ".call", ".callv", ".call_deferred" };
    for (size_t i = 0; i < 3; i++) {
        EACH_MATCH(c, calls[i], pos) {
            if (in_comment(c, pos, TK_GODOT) || !c->text) continue;
            size_t a = arg_start(c, pos + strlen(calls[i]));
            if (a < c->n && c->b[a] != '"' && c->b[a] != '\'' && c->b[a] != '&' && c->b[a] != ')')
                spx_add(c->r, SPX_WARN, "script.dynamic", c->file, line_of(c, pos), "%s with a computed method name", calls[i] + 1);
        }
    }
}

/* ScreenPlus.call_api("ns.method") / screenplus.call("ns.method"): which capabilities the code uses. */
static void api_calls(Ctx *c, const char *pat, TextKind k, int *net_like)
{
    EACH_MATCH(c, pat, pos) {
        if (in_comment(c, pos, k)) continue;
        size_t a = arg_start(c, pos + strlen(pat));
        if (a >= c->n) continue;
        if (c->b[a] != '"' && c->b[a] != '\'') {
            if (c->text) spx_add(c->r, SPX_WARN, "script.api-computed", c->file, line_of(c, pos), "%s with a computed method name: its capability can't be checked", pat);
            continue;
        }
        char method[64], ns[32];
        size_t j = 0;
        for (size_t p = a + 1; p < c->n && c->b[p] != c->b[a] && j < sizeof(method) - 1; p++) method[j++] = (char)c->b[p];
        method[j] = 0;
        size_t nl = strcspn(method, ".");
        if (nl == 0 || nl >= sizeof(ns)) continue;
        memcpy(ns, method, nl);
        ns[nl] = 0;
        spx_use(c->r, ns);
        if (!strncmp(method, "net.", 4) || !strncmp(method, "fs.", 3) || !strcmp(ns, "process")) *net_like = 1;
        if (open_ns(ns)) continue;
        /* net.client / fs.read are declared by their full name; others by namespace. */
        char two[64];
        size_t dot2 = strcspn(method + nl + 1, ".");
        snprintf(two, sizeof(two), "%.*s", (int)(nl + 1 + dot2), method);
        if (!manifest_has(c->m, ns) && !manifest_has(c->m, two) &&
            !(!strcmp(ns, "net") && (manifest_has(c->m, "net.client") || manifest_has(c->m, "net.server"))) &&
            !(!strcmp(ns, "fs") && (manifest_has(c->m, "fs.read") || manifest_has(c->m, "fs.write"))))
            spx_add(c->r, SPX_BLOCK, "script.undeclared", c->file, line_of(c, pos), "uses \"%s\" but the manifest doesn't declare it", method);
    }
}

static void urls(Ctx *c)
{
    static const char *schemes[] = { "https://", "http://", "wss://", "ws://" };
    static const char *harmless[] = { "www.w3.org", "w3.org", "godotengine.org", "docs.godotengine.org", "schema.org" };
    char seen[16][128];
    int nseen = 0;
    for (size_t s = 0; s < 4; s++) {
        for (const unsigned char *p = find(c->b, c->n, schemes[s], 0); p; p = find(c->b, c->n, schemes[s], (size_t)(p - c->b) + 1)) {
            size_t pos = (size_t)(p - c->b);
            if (s == 1 && pos > 0 && c->b[pos - 1] == 's') continue; /* "http://" inside "https://" can't happen, but ws inside wss can */
            if (s == 3 && pos > 0 && c->b[pos - 1] == 'w') continue;
            char host[128];
            size_t j = 0;
            for (size_t q = pos + strlen(schemes[s]); q < c->n && j < sizeof(host) - 1; q++) {
                unsigned char ch = c->b[q];
                if (!(isalnum(ch) || ch == '.' || ch == '-')) break;
                host[j++] = (char)tolower(ch);
            }
            host[j] = 0;
            if (!j || in_comment(c, pos, rules_kind(c->file))) continue;
            int skip = 0;
            for (size_t h = 0; h < sizeof(harmless) / sizeof(*harmless); h++) if (!strcmp(host, harmless[h])) skip = 1;
            for (int h = 0; h < nseen; h++) if (!strcmp(seen[h], host)) skip = 1;
            if (skip || manifest_host_ok(c->m, host)) continue;
            if (nseen < 16) strcpy(seen[nseen++], host);
            spx_add(c->r, SPX_WARN, "net.undeclared", c->file, line_of(c, pos), "network address %s isn't declared in net.client", host);
        }
    }
}

/* String literals: long base64 or random-looking text is how payloads and keys hide. */
static void literals(Ctx *c)
{
    int reported = 0;
    for (size_t i = 0; i < c->n && reported < 3; i++) {
        unsigned char q = c->b[i];
        if (q != '"' && q != '\'') continue;
        size_t j = i + 1;
        while (j < c->n && c->b[j] != q && c->b[j] != '\n') { if (c->b[j] == '\\') j++; j++; }
        if (j >= c->n || c->b[j] != q) { i = j; continue; }
        size_t len = j - i - 1;
        if (len >= 64) {
            const unsigned char *s = c->b + i + 1;
            int b64 = 1;
            unsigned cnt[256] = { 0 };
            for (size_t k = 0; k < len; k++) {
                cnt[s[k]]++;
                if (!(isalnum(s[k]) || s[k] == '+' || s[k] == '/' || s[k] == '=' || s[k] == '-' || s[k] == '_')) b64 = 0;
            }
            double h = 0;
            for (int k = 0; k < 256; k++) if (cnt[k]) { double p = (double)cnt[k] / (double)len; h -= p * log2(p); }
            if (b64 && len >= 200) { spx_add(c->r, SPX_WARN, "script.blob", c->file, line_of(c, i), "a %u-character base64 string (hidden data?)", (unsigned)len); reported++; }
            else if (h > 5.2) { spx_add(c->r, SPX_WARN, "script.entropy", c->file, line_of(c, i), "a random-looking %u-character string (packed or encrypted data?)", (unsigned)len); reported++; }
        }
        i = j;
    }
}

void rules_scan(SpxReport *r, const Manifest *m, const char *file, const unsigned char *buf, size_t len, TextKind kind)
{
    Ctx c = { r, m, file, buf, len, 0, 0, 1 };
    /* Binary resources (.scn/.res/.binary) hold embedded sources as UTF-8 among binary data: rules apply, lines don't. */
    c.text = !(ends_with_ci(file, ".scn") || ends_with_ci(file, ".res") || ends_with_ci(file, ".binary"));
    int net_like = 0;
    if (kind == TK_GODOT) {
        for (size_t i = 0; i < sizeof(k_godot) / sizeof(*k_godot); i++)
            EACH_MATCH(&c, k_godot[i].pat, pos) {
                if (in_comment(&c, pos, kind)) continue;
                spx_add(r, k_godot[i].sev, k_godot[i].code, file, line_of(&c, pos), "%s: %s", k_godot[i].pat, k_godot[i].msg);
            }
        godot_paths(&c);
        api_calls(&c, "call_api", kind, &net_like);
        if (net_like && (find(buf, len, "Time.get_date", 0) || find(buf, len, "Time.get_unix_time", 0) || find(buf, len, "Time.get_datetime", 0)))
            spx_add(r, SPX_WARN, "script.time-gate", file, 0, "checks the date in a file that uses the network, files or programs (time bomb?)");
    } else if (kind == TK_WEB) {
        int eval_ok = manifest_has(m, "web.eval");
        static const char *evals[] = { "eval", "Function", "setTimeout(\"", "setInterval(\"" };
        for (size_t i = 0; i < 4; i++)
            EACH_MATCH(&c, evals[i], pos) {
                if (in_comment(&c, pos, kind)) continue;
                size_t after = pos + strlen(evals[i]);
                if (i < 2 && (after >= len || buf[after] != '(')) continue;
                if (!eval_ok) spx_add(r, SPX_BLOCK, "web.eval", file, line_of(&c, pos), "%s runs text as code; declare web.eval", evals[i]);
            }
        EACH_MATCH(&c, "import", pos) {
            size_t a = pos + 6;
            if (a >= len || buf[a] != '(' || in_comment(&c, pos, kind)) continue;
            a = arg_start(&c, a);
            if (a < len && (lit_prefix(&c, a, "http") || lit_prefix(&c, a, "//")))
                spx_add(r, SPX_BLOCK, "web.remote-code", file, line_of(&c, pos), "imports code from the internet");
            else if (a < len && buf[a] != '"' && buf[a] != '\'' && buf[a] != '`')
                spx_add(r, SPX_WARN, "web.remote-code", file, line_of(&c, pos), "import() with a computed address");
        }
        for (const unsigned char *p = find(buf, len, "<script", 0); p; p = find(buf, len, "<script", (size_t)(p - buf) + 1)) {
            size_t pos = (size_t)(p - buf), end = pos;
            while (end < len && buf[end] != '>' && end - pos < 400) end++;
            for (size_t q = pos; q + 4 < end; q++)
                if (!memcmp(buf + q, "src=", 4)) {
                    size_t v = q + 4 + (buf[q + 4] == '"' || buf[q + 4] == '\'');
                    if (v + 4 <= end && (!memcmp(buf + v, "http", 4) || !memcmp(buf + v, "//", 2)))
                        spx_add(r, SPX_BLOCK, "web.remote-code", file, line_of(&c, pos), "loads a script from the internet");
                }
        }
        for (size_t i = 0; i < sizeof(k_web) / sizeof(*k_web); i++)
            EACH_MATCH(&c, k_web[i].pat, pos) {
                if (in_comment(&c, pos, kind)) continue;
                spx_add(r, k_web[i].sev, k_web[i].code, file, line_of(&c, pos), "%s: %s", k_web[i].pat, k_web[i].msg);
            }
        api_calls(&c, "screenplus.call", kind, &net_like);
    }
    if (kind != TK_NONE) {
        urls(&c);
        if (c.text) literals(&c);
    }
}
