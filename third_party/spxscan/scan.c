/* The .spx pass: container, layout, manifest, every file's type, the pack, web files (docs/design.md §8.1). */
#include "scan_int.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SPX_MAX_ENTRIES 10000
#define SPX_MAX_TOTAL (2ull << 30)
#define SPX_MAX_INFLATE (64u << 20)   /* compressed entries above this must be stored */
#define SPX_BOMB_RATIO 100
#define SPX_SAME_FINDING 3            /* per code and file, so one noisy file doesn't bury the rest */

const char *spx_severity_name(SpxSeverity s)
{
    return s == SPX_BLOCK ? "block" : s == SPX_WARN ? "warn" : "info";
}

void spx_add(SpxReport *r, SpxSeverity sev, const char *code, const char *file, int line, const char *fmt, ...)
{
    int same = 0;
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->items[i].code, code) && !strcmp(r->items[i].file, file ? file : "")) same++;
    if (sev > r->verdict) r->verdict = sev;
    if (same >= SPX_SAME_FINDING) return;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        SpxFinding *f = realloc(r->items, (size_t)cap * sizeof(*f));
        if (!f) return;
        r->items = f;
        r->cap = cap;
    }
    SpxFinding *f = &r->items[r->n++];
    memset(f, 0, sizeof(*f));
    f->sev = sev;
    f->line = line;
    snprintf(f->code, sizeof(f->code), "%s", code);
    snprintf(f->file, sizeof(f->file), "%s", file ? file : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof(f->msg), fmt, ap);
    va_end(ap);
}

const SpxFinding *spx_report_worst(const SpxReport *r)
{
    for (int i = 0; i < r->n; i++) if (r->items[i].sev == r->verdict) return &r->items[i];
    return NULL;
}

void spx_use(SpxReport *r, const char *cap)
{
    for (int i = 0; i < r->nused; i++) if (!strcmp(r->used[i], cap)) return;
    if (r->nused < 48) snprintf(r->used[r->nused++], sizeof(r->used[0]), "%s", cap);
}

void spx_report_free(SpxReport *r)
{
    free(r->items);
    memset(r, 0, sizeof(*r));
}

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* Only these names may appear. Returns 0 if the path is outside the layout. */
static int layout_ok(const char *n, const Manifest *m)
{
    static const char *top[] = { "manifest.json", "project.pck", "preview.jpg", "preview.webm", "LICENSE", "LICENSE.txt",
                                 "README.md", "signature.json" };
    for (size_t i = 0; i < sizeof(top) / sizeof(*top); i++) if (!strcmp(n, top[i])) return 1;
    if (!strncmp(n, "web/", 4)) return 1;
    if (!strncmp(n, "models/", 7)) return ends_with_ci(n, ".onnx") || ends_with_ci(n, ".onnx_data") || ends_with_ci(n, ".json") || ends_with_ci(n, ".txt");
    if (!strncmp(n, "native/windows/", 15) || !strncmp(n, "native/linux/", 13) || !strncmp(n, "native/macos/", 13)) return manifest_has(m, "native") ? 2 : 0;
    return 0;
}

static const char *bad_name(const char *n)
{
    if (!*n) return "empty name";
    if (n[0] == '/' || n[0] == '\\' || (isalpha((unsigned char)n[0]) && n[1] == ':')) return "absolute path";
    if (strchr(n, '\\')) return "backslash in path";
    for (const unsigned char *c = (const unsigned char *)n; *c; c++) if (*c < 0x20 || *c == 0x7f) return "control character in name";
    for (const char *s = n; *s;) {
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if ((len == 2 && !memcmp(s, "..", 2)) || (len == 1 && s[0] == '.')) return "dot segment in path";
        if (len == 0 && e) return "empty segment in path";
        if (!e) break;
        s = e + 1;
    }
    return NULL;
}

static int cmp_lho(const void *a, const void *b)
{
    const ZipEntry *x = *(const ZipEntry *const *)a, *y = *(const ZipEntry *const *)b;
    return x->lho < y->lho ? -1 : x->lho > y->lho;
}

static const ZipEntry *find_entry(const Zip *z, const char *name)
{
    for (int i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}

int spx_scan(const char *path, SpxReport *r)
{
    memset(r, 0, sizeof(*r));
    Zip z;
    char err[128];
    if (zip_open(&z, path, err, sizeof(err)) != 0) {
        int opened = z.f != NULL; /* the file exists but isn't a usable zip: that is a verdict, not an error */
        spx_add(r, SPX_BLOCK, "zip.open", "", 0, "%s", err);
        zip_close(&z);
        return opened ? 0 : -1;
    }
    if (z.zip64 || z.multidisk) {
        spx_add(r, SPX_BLOCK, "zip.format", "", 0, z.zip64 ? "Zip64 isn't accepted (packs are under 2 GB)" : "multi-part zips aren't accepted");
        zip_close(&z);
        return 0;
    }
    if (z.n > SPX_MAX_ENTRIES) spx_add(r, SPX_BLOCK, "zip.count", "", 0, "%d files (at most %d)", z.n, SPX_MAX_ENTRIES);

    /* Container: names, duplicates, methods, links, local headers, sizes, ratios, overlaps. */
    uint64_t total = 0;
    for (int i = 0; i < z.n; i++) {
        ZipEntry *e = &z.e[i];
        const char *why = bad_name(e->name);
        if (why) spx_add(r, SPX_BLOCK, "zip.path", e->name, 0, "%s", why);
        for (int j = 0; j < i; j++)
            if (ci_eq(z.e[j].name, e->name)) spx_add(r, SPX_BLOCK, "zip.duplicate", e->name, 0, "appears twice (letter case ignored)");
        if (e->flags & 1) spx_add(r, SPX_BLOCK, "zip.encrypted", e->name, 0, "encrypted entries can't be checked");
        if (e->method != 0 && e->method != 8) spx_add(r, SPX_BLOCK, "zip.method", e->name, 0, "compression method %u isn't accepted", e->method);
        if (e->made_by_os == 3 && ((e->ext_attr >> 16) & 0170000) == 0120000) spx_add(r, SPX_BLOCK, "zip.symlink", e->name, 0, "symbolic link");
        if (!e->local_ok) spx_add(r, SPX_BLOCK, "zip.local", e->name, 0, "local header missing or names a different file");
        if (e->usize > (10u << 20) && e->csize && e->usize / e->csize > SPX_BOMB_RATIO)
            spx_add(r, SPX_BLOCK, "zip.bomb", e->name, 0, "expands %llux (zip bomb?)", (unsigned long long)(e->usize / e->csize));
        if (e->method == 8 && e->usize > SPX_MAX_INFLATE) spx_add(r, SPX_BLOCK, "zip.big-compressed", e->name, 0, "files over 64 MB must be stored, not compressed");
        total += e->usize;
    }
    if (total > SPX_MAX_TOTAL) spx_add(r, SPX_BLOCK, "zip.size", "", 0, "unpacks to %llu MB (at most 2048)", (unsigned long long)(total >> 20));
    {
        ZipEntry **order = malloc((size_t)(z.n ? z.n : 1) * sizeof(*order));
        if (order) {
            for (int i = 0; i < z.n; i++) order[i] = &z.e[i];
            qsort(order, (size_t)z.n, sizeof(*order), cmp_lho);
            for (int i = 0; i < z.n; i++) {
                uint64_t end = order[i]->data_off + order[i]->csize;
                uint64_t limit = i + 1 < z.n ? order[i + 1]->lho : z.cd_off;
                if (order[i]->local_ok && end > limit) spx_add(r, SPX_BLOCK, "zip.overlap", order[i]->name, 0, "data overlaps another entry");
            }
            free(order);
        }
    }

    /* Manifest first: the rest is checked against it. */
    Manifest m;
    memset(&m, 0, sizeof(m));
    const ZipEntry *me = find_entry(&z, "manifest.json");
    if (!me) spx_add(r, SPX_BLOCK, "manifest.missing", "manifest.json", 0, "missing");
    else {
        unsigned char *buf;
        if (zip_extract(&z, me, &buf, 1u << 20) != 0) spx_add(r, SPX_BLOCK, "manifest.read", "manifest.json", 0, "unreadable or over 1 MB");
        else { manifest_check(r, (const char *)buf, (size_t)me->usize, &m); free(buf); }
    }
    if (!find_entry(&z, "preview.jpg")) spx_add(r, SPX_BLOCK, "layout.preview", "preview.jpg", 0, "missing");
    if (m.ok && !find_entry(&z, m.entry)) spx_add(r, SPX_BLOCK, "layout.entry", m.entry, 0, "the manifest's entry is missing");
    if (m.has_web && !find_entry(&z, "web/index.html")) spx_add(r, SPX_BLOCK, "layout.web", "web/index.html", 0, "missing");
    if (!find_entry(&z, "signature.json")) spx_add(r, SPX_WARN, "sign.missing", "", 0, "not signed: its author can't be confirmed and updates can't be tied to it");
    else spx_add(r, SPX_INFO, "sign.unchecked", "signature.json", 0, "signature checking isn't in this scanner version yet");

    /* Every file. */
    for (int i = 0; i < z.n; i++) {
        ZipEntry *e = &z.e[i];
        size_t nl = strlen(e->name);
        if (nl && e->name[nl - 1] == '/') continue; /* directory */
        r->files++;
        r->bytes += e->usize;
        if (bad_name(e->name)) continue;
        int lay = layout_ok(e->name, &m);
        if (!lay) {
            spx_add(r, SPX_BLOCK, "layout.unknown", e->name, 0, !strncmp(e->name, "native/", 7) ? "native code needs the native capability" : "not part of the .spx layout");
            continue;
        }
        if (!strcmp(e->name, "project.pck")) {
            r->files--; /* counted by its contents */
            pck_scan(r, &z, e, &m);
            continue;
        }
        if (!strcmp(e->name, "manifest.json")) continue;

        unsigned char head[1024];
        size_t hn = e->usize < sizeof(head) ? (size_t)e->usize : sizeof(head);
        unsigned char *buf = NULL;
        int have = 0;
        if (e->method == 0 || e->usize <= SPX_MAX_INFLATE) {
            if (e->method == 0 && e->usize > SPX_MAX_INFLATE) have = zip_read_at(&z, e->data_off, head, hn) == 0 ? 1 : 0;
            else {
                int rc = zip_extract(&z, e, &buf, SPX_MAX_INFLATE);
                if (rc == 0) { memcpy(head, buf, hn); have = 2; }
                else spx_add(r, SPX_BLOCK, "zip.corrupt", e->name, 0, rc == -2 ? "corrupt (checksum or data)" : "can't be read");
            }
        }
        if (have) {
            const char *magic = rules_binary_magic(head, hn);
            if (!strcmp(e->name, "preview.jpg") && !(hn >= 3 && !memcmp(head, "\xff\xd8\xff", 3)))
                spx_add(r, SPX_BLOCK, "layout.preview", e->name, 0, "isn't a JPEG");
            if (lay == 2) {
                if (!magic || (strncmp(magic, "Windows", 7) && strncmp(magic, "Linux", 5) && strncmp(magic, "macOS", 5)))
                    spx_add(r, SPX_BLOCK, "native.type", e->name, 0, "native/ holds only libraries");
                else spx_add(r, SPX_WARN, "native.code", e->name, 0, "native library: %s (runs with the wallpaper's sandbox rights only)", magic);
            } else if (magic && !strcmp(magic, "WebAssembly") && !strncmp(e->name, "web/", 4)) {
                spx_add(r, SPX_WARN, "web.wasm", e->name, 0, "WebAssembly is only accepted from the Screen- web template (template hash check not in this version)");
            } else if (magic) {
                spx_add(r, SPX_BLOCK, "file.binary", e->name, 0, "contains a %s", magic);
            }
        }
        if (!strncmp(e->name, "web/", 4)) {
            static const char *never[] = { ".exe", ".dll", ".bat", ".cmd", ".ps1", ".vbs", ".sh", ".scr", ".msi", ".lnk", ".jar", ".py", ".so", ".dylib" };
            for (size_t k = 0; k < sizeof(never) / sizeof(*never); k++)
                if (ends_with_ci(e->name, never[k])) spx_add(r, SPX_BLOCK, "file.type", e->name, 0, "this file type isn't allowed in web/");
            TextKind k = rules_kind(e->name);
            if (k == TK_WEB && have == 2) rules_scan(r, &m, e->name, buf, (size_t)e->usize, TK_WEB);
        }
        free(buf);
    }
    zip_close(&z);
    return 0;
}

void spx_report_print(const SpxReport *r, FILE *o)
{
    fprintf(o, "%s %s %s (%s)\n", r->id[0] ? r->id : "?", r->version, r->title, r->runtime);
    fprintf(o, "%llu files, %llu bytes\n", r->files, r->bytes);
    if (r->nused) {
        fprintf(o, "uses:");
        for (int i = 0; i < r->nused; i++) fprintf(o, " %s", r->used[i]);
        fprintf(o, "\n");
    }
    for (int s = SPX_BLOCK; s >= SPX_INFO; s--)
        for (int i = 0; i < r->n; i++) {
            const SpxFinding *f = &r->items[i];
            if ((int)f->sev != s) continue;
            fprintf(o, "  %-5s %-22s %s", spx_severity_name(f->sev), f->code, f->file);
            if (f->line) fprintf(o, ":%d", f->line);
            fprintf(o, "%s%s\n", f->file[0] ? ": " : "", f->msg);
        }
    fprintf(o, "verdict: %s\n", r->verdict == SPX_BLOCK ? "BLOCK" : r->verdict == SPX_WARN ? "WARN" : "PASS");
}

static void jstr(FILE *o, const char *s)
{
    fputc('"', o);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') fprintf(o, "\\%c", *c);
        else if (*c < 0x20) fprintf(o, "\\u%04x", *c);
        else fputc(*c, o);
    }
    fputc('"', o);
}

void spx_report_json(const SpxReport *r, FILE *o)
{
    fprintf(o, "{\"scanner\":\"%s\",\"verdict\":\"%s\",\"id\":", SPX_SCANNER_VERSION,
            r->verdict == SPX_BLOCK ? "block" : r->verdict == SPX_WARN ? "warn" : "pass");
    jstr(o, r->id);
    fprintf(o, ",\"version\":");
    jstr(o, r->version);
    fprintf(o, ",\"title\":");
    jstr(o, r->title);
    fprintf(o, ",\"runtime\":");
    jstr(o, r->runtime);
    fprintf(o, ",\"files\":%llu,\"bytes\":%llu,\"uses\":[", r->files, r->bytes);
    for (int i = 0; i < r->nused; i++) { if (i) fputc(',', o); jstr(o, r->used[i]); }
    fprintf(o, "],\"findings\":[");
    for (int i = 0; i < r->n; i++) {
        const SpxFinding *f = &r->items[i];
        fprintf(o, "%s{\"severity\":\"%s\",\"code\":", i ? "," : "", spx_severity_name(f->sev));
        jstr(o, f->code);
        fprintf(o, ",\"file\":");
        jstr(o, f->file);
        fprintf(o, ",\"line\":%d,\"message\":", f->line);
        jstr(o, f->msg);
        fputc('}', o);
    }
    fprintf(o, "]}\n");
}
