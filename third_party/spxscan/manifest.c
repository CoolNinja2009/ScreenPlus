/* manifest.json against spx/manifest.schema.json, written out by hand so the scanner needs no schema engine.
 * Keep the two in step. */
#include "scan_int.h"
#include "sjson.h"

#include <ctype.h>
#include <string.h>

#define M "manifest.json"

/* Capability name, kind: 0 bool, 1 hosts, 2 folders, 3 net.server object. */
static const struct { const char *name; int kind; } k_caps[] = {
    { "audio", 0 }, { "mic", 0 }, { "camera", 0 }, { "ai", 0 }, { "system", 0 },
    { "net.client", 1 }, { "net.server", 3 }, { "osc", 0 }, { "midi", 0 }, { "serial", 0 }, { "hid", 0 },
    { "gamepad", 0 }, { "bluetooth.le", 0 }, { "fs.read", 2 }, { "fs.write", 2 }, { "desktop", 0 },
    { "screen", 0 }, { "clipboard", 0 }, { "location", 0 }, { "bus", 0 }, { "web.eval", 0 },
    { "process", 0 }, { "native", 0 },
};
static const char *k_folders[] = { "pictures", "music", "videos", "documents", "downloads", "picked" };
static const char *k_proptypes[] = { "slider", "color", "checkbox", "dropdown", "textbox", "file" };

static int in_list(const char *s, const char **list, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

/* ^[a-z0-9]+(\.[a-z0-9-]+){1,5}$ */
static int valid_id(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 128 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) return 0;
    int dots = 0, seg = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '.') { if (!seg) return 0; dots++; seg = 0; continue; }
        if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || (*c == '-' && dots))) return 0;
        seg++;
    }
    return seg && dots >= 1 && dots <= 5;
}

static int valid_semver(const char *s)
{
    int parts = 0, digits = 0;
    for (const char *c = s;; c++) {
        if (isdigit((unsigned char)*c)) { digits++; continue; }
        if (!digits) return 0;
        parts++;
        digits = 0;
        if (!*c) break;
        if (*c != '.') return 0;
    }
    return parts == 3;
}

/* web | screen-<n>.<n>[.<n>]-<n> */
static int valid_runtime(const char *s)
{
    if (!strcmp(s, "web")) return 1;
    if (strncmp(s, "screen-", 7)) return 0;
    const char *dash = strrchr(s, '-');
    if (dash == s + 6 || !dash[1]) return 0;
    for (const char *c = dash + 1; *c; c++) if (!isdigit((unsigned char)*c)) return 0;
    char ver[32];
    size_t n = (size_t)(dash - (s + 7));
    if (n == 0 || n >= sizeof(ver)) return 0;
    memcpy(ver, s + 7, n);
    ver[n] = 0;
    int dots = 0;
    for (const char *c = ver; *c; c++) {
        if (*c == '.') { if (c == ver || c[1] == '.' || !c[1]) return 0; dots++; }
        else if (!isdigit((unsigned char)*c)) return 0;
    }
    return dots == 1 || dots == 2;
}

/* ^(\*|(\*\.)?[a-z0-9-]+(\.[a-z0-9-]+)*(:[0-9]{1,5})?)$ */
static int valid_host(const char *s)
{
    if (!strcmp(s, "*")) return 1;
    if (!strncmp(s, "*.", 2)) s += 2;
    const char *colon = strchr(s, ':');
    size_t n = colon ? (size_t)(colon - s) : strlen(s);
    if (n == 0 || n > 120) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') { if (i == 0 || i == n - 1 || s[i + 1] == '.') return 0; continue; }
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) return 0;
    }
    if (colon) {
        size_t d = strlen(colon + 1);
        if (d < 1 || d > 5) return 0;
        for (const char *c = colon + 1; *c; c++) if (!isdigit((unsigned char)*c)) return 0;
    }
    return 1;
}

static const char *req_str(SpxReport *r, const SJVal *root, const char *key, size_t maxlen)
{
    const SJVal *v = sj_get(root, key);
    if (!v || v->t != SJ_STR || !v->str[0]) { spx_add(r, SPX_BLOCK, "manifest.field", M, 0, "\"%s\" is missing or not a string", key); return NULL; }
    if (strlen(v->str) > maxlen) { spx_add(r, SPX_BLOCK, "manifest.field", M, 0, "\"%s\" is longer than %d", key, (int)maxlen); return NULL; }
    return v->str;
}

static void copy(char *dst, size_t cap, const char *s)
{
    if (!s) return;
    strncpy(dst, s, cap - 1);
    dst[cap - 1] = 0;
}

int manifest_check(SpxReport *r, const char *text, size_t len, Manifest *m)
{
    memset(m, 0, sizeof(*m));
    char err[128];
    SJVal *root = sj_parse(text, len, err, sizeof(err));
    if (!root) { spx_add(r, SPX_BLOCK, "manifest.json", M, 0, "not valid JSON: %s", err); return -1; }
    if (root->t != SJ_OBJ) { spx_add(r, SPX_BLOCK, "manifest.json", M, 0, "must be an object"); sj_free(root); return -1; }
    if (sj_has_dup_keys(root))
        spx_add(r, SPX_BLOCK, "manifest.duplicate", M, 0, "a key appears twice (readers could disagree on its value)");

    static const char *known[] = { "spx", "id", "title", "author", "description", "version", "runtime", "entry", "web", "capabilities", "properties" };
    for (int i = 0; i < root->n; i++)
        if (!in_list(root->keys[i], known, sizeof(known) / sizeof(*known)))
            spx_add(r, SPX_BLOCK, "manifest.unknown", M, 0, "unknown key \"%s\"", root->keys[i]);

    const SJVal *spx = sj_get(root, "spx");
    if (!spx || spx->t != SJ_NUM || spx->num != 1) spx_add(r, SPX_BLOCK, "manifest.version", M, 0, "\"spx\" must be 1");

    const char *id = req_str(r, root, "id", 128), *title = req_str(r, root, "title", 80);
    const char *version = req_str(r, root, "version", 31), *runtime = req_str(r, root, "runtime", 47);
    const char *entry = req_str(r, root, "entry", 31);
    req_str(r, root, "author", 80);
    if (id && !valid_id(id)) spx_add(r, SPX_BLOCK, "manifest.id", M, 0, "id \"%s\" must look like com.author.name", id);
    if (version && !valid_semver(version)) spx_add(r, SPX_BLOCK, "manifest.semver", M, 0, "version must be x.y.z");
    if (runtime && !valid_runtime(runtime)) spx_add(r, SPX_BLOCK, "manifest.runtime", M, 0, "unknown runtime \"%s\"", runtime);
    if (entry && strcmp(entry, "project.pck") && strcmp(entry, "web/index.html"))
        spx_add(r, SPX_BLOCK, "manifest.entry", M, 0, "entry must be project.pck or web/index.html");
    if (entry && runtime && !strcmp(runtime, "web") != !strcmp(entry, "web/index.html"))
        spx_add(r, SPX_BLOCK, "manifest.entry", M, 0, "runtime \"web\" goes with entry web/index.html, and only it");
    copy(r->id, sizeof(r->id), id);
    copy(r->title, sizeof(r->title), title);
    copy(r->version, sizeof(r->version), version);
    copy(r->runtime, sizeof(r->runtime), runtime);
    copy(m->entry, sizeof(m->entry), entry);
    const SJVal *web = sj_get(root, "web");
    if (web && (web->t != SJ_STR || strcmp(web->str, "web/index.html")))
        spx_add(r, SPX_BLOCK, "manifest.web", M, 0, "\"web\" must be \"web/index.html\"");
    m->has_web = (entry && !strcmp(entry, "web/index.html")) || web != NULL;

    const SJVal *caps = sj_get(root, "capabilities");
    if (caps && caps->t != SJ_OBJ) spx_add(r, SPX_BLOCK, "manifest.capabilities", M, 0, "capabilities must be an object");
    for (int i = 0; caps && caps->t == SJ_OBJ && i < caps->n; i++) {
        const char *name = caps->keys[i];
        const SJVal *v = &caps->kids[i];
        int kind = -1;
        for (size_t k = 0; k < sizeof(k_caps) / sizeof(*k_caps); k++) if (!strcmp(name, k_caps[k].name)) kind = k_caps[k].kind;
        if (kind < 0) { spx_add(r, SPX_BLOCK, "manifest.capability", M, 0, "unknown capability \"%s\"", name); continue; }
        int granted = 1;
        if (kind == 0) {
            if (v->t != SJ_BOOL) spx_add(r, SPX_BLOCK, "manifest.capability", M, 0, "\"%s\" must be true or false", name);
            granted = v->t == SJ_BOOL && v->b;
        } else if (kind == 1 || kind == 2) {
            if (v->t != SJ_ARR || v->n == 0) { spx_add(r, SPX_BLOCK, "manifest.capability", M, 0, "\"%s\" must be a non-empty list", name); continue; }
            for (int j = 0; j < v->n; j++) {
                const SJVal *s = &v->kids[j];
                if (s->t != SJ_STR || (kind == 1 ? !valid_host(s->str) : !in_list(s->str, k_folders, sizeof(k_folders) / sizeof(*k_folders)))) {
                    spx_add(r, SPX_BLOCK, "manifest.capability", M, 0, "\"%s\" has a bad entry \"%s\"", name, s->t == SJ_STR ? s->str : "?");
                    continue;
                }
                if (kind == 1) {
                    if (!strcmp(s->str, "*")) m->any_host = 1;
                    else if (m->nhosts < SPX_MAX_HOSTS) copy(m->hosts[m->nhosts++], sizeof(m->hosts[0]), s->str);
                }
            }
        } else if (v->t != SJ_OBJ) {
            spx_add(r, SPX_BLOCK, "manifest.capability", M, 0, "\"%s\" must be an object", name);
        }
        if (granted && m->ncaps < 64) copy(m->caps[m->ncaps++], sizeof(m->caps[0]), name);
    }
    if (manifest_has(m, "native") || manifest_has(m, "process"))
        spx_add(r, SPX_WARN, "manifest.unrestricted", M, 0, "asks for unrestricted capabilities (%s%s%s): full access to the computer",
                manifest_has(m, "native") ? "native" : "", manifest_has(m, "native") && manifest_has(m, "process") ? ", " : "",
                manifest_has(m, "process") ? "process" : "");
    if (m->any_host) spx_add(r, SPX_INFO, "manifest.net-any", M, 0, "may connect to any internet address");

    const SJVal *props = sj_get(root, "properties");
    if (props && (props->t != SJ_OBJ || props->n > 64)) spx_add(r, SPX_BLOCK, "manifest.properties", M, 0, "properties must be an object of at most 64");
    for (int i = 0; props && props->t == SJ_OBJ && i < props->n; i++) {
        const SJVal *t = sj_get(&props->kids[i], "type"), *l = sj_get(&props->kids[i], "label");
        if (!t || t->t != SJ_STR || !in_list(t->str, k_proptypes, sizeof(k_proptypes) / sizeof(*k_proptypes)) || !l || l->t != SJ_STR)
            spx_add(r, SPX_BLOCK, "manifest.property", M, 0, "property \"%s\" needs a known type and a label", props->keys[i]);
    }
    sj_free(root);
    m->ok = id && version && runtime && entry;
    return 0;
}

int manifest_has(const Manifest *m, const char *cap)
{
    for (int i = 0; i < m->ncaps; i++) if (!strcmp(m->caps[i], cap)) return 1;
    return 0;
}

int manifest_host_ok(const Manifest *m, const char *host)
{
    if (m->any_host) return 1;
    for (int i = 0; i < m->nhosts; i++) {
        const char *h = m->hosts[i];
        char base[128];
        copy(base, sizeof(base), h);
        char *colon = strchr(base, ':');
        if (colon) *colon = 0; /* ports are the broker's business */
        if (!strncmp(base, "*.", 2)) {
            size_t hl = strlen(host), bl = strlen(base + 1); /* ".example.org" */
            if (hl > bl && !strcmp(host + hl - bl, base + 1)) return 1;
        } else if (!strcmp(base, host)) {
            return 1;
        }
    }
    return 0;
}
