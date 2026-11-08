/* Godot .pck inside the .spx (stored, so it is read in place). Format: core/io/file_access_pack.cpp,
 * versions 2-4. Encrypted, sparse and patch packs are refused; every file is checked for executables, and
 * scripts and resources go through the rules. */
#include "scan_int.h"

#include <stdlib.h>
#include <string.h>

#define PCK_MAGIC 0x43504447u
#define PCK_DIR_ENCRYPTED 1u
#define PCK_SPARSE_BUNDLE 4u
#define PCK_FILE_ENCRYPTED 1u
#define PCK_FILE_REMOVAL 2u
#define PCK_FILE_DELTA 4u
#define PCK_MAX_FILES 200000
#define PCK_MAX_TEXT (32u << 20)

typedef struct R { Zip *z; uint64_t base, size, pos; int bad; } R;

static int rd(R *r, void *buf, size_t n)
{
    if (r->bad || r->pos > r->size || n > r->size - r->pos || zip_read_at(r->z, r->base + r->pos, buf, n)) { r->bad = 1; memset(buf, 0, n); return -1; }
    r->pos += n;
    return 0;
}
static uint32_t u32(R *r) { unsigned char b[4]; rd(r, b, 4); return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t u64(R *r) { uint64_t lo = u32(r); return lo | (uint64_t)u32(r) << 32; }

/* "screen-4.7.2-1" -> 4, 7 */
static int runtime_version(const char *rt, unsigned *maj, unsigned *min)
{
    return sscanf(rt, "screen-%u.%u", maj, min) == 2;
}

static int bad_path(const char *p)
{
    if (!*p || strstr(p, "..") || strchr(p, '\\') || strchr(p, ':')) return 1;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) if (*c < 0x20) return 1;
    return 0;
}

void pck_scan(SpxReport *rep, Zip *z, const ZipEntry *e, const Manifest *m)
{
    char where[300];
    if (e->method != 0) {
        spx_add(rep, SPX_BLOCK, "pck.compressed", e->name, 0, "must be stored, not compressed, in the .spx");
        return;
    }
    R r = { z, e->data_off, e->usize, 0, 0 };
    if (u32(&r) != PCK_MAGIC) { spx_add(rep, SPX_BLOCK, "pck.magic", e->name, 0, "not a Godot pack"); return; }
    uint32_t version = u32(&r), maj = u32(&r), min = u32(&r);
    u32(&r); /* patch */
    if (version < 2 || version > 4) { spx_add(rep, SPX_BLOCK, "pck.version", e->name, 0, "pack format %u is not supported", version); return; }
    unsigned rmaj, rmin;
    if (runtime_version(rep->runtime, &rmaj, &rmin) && (maj != rmaj || min != rmin))
        spx_add(rep, SPX_BLOCK, "pck.engine", e->name, 0, "made with Godot %u.%u but the manifest says runtime %s", maj, min, rep->runtime);
    uint32_t flags = u32(&r);
    if (flags & PCK_DIR_ENCRYPTED) { spx_add(rep, SPX_BLOCK, "pck.encrypted", e->name, 0, "encrypted packs can't be checked"); return; }
    if (flags & PCK_SPARSE_BUNDLE) { spx_add(rep, SPX_BLOCK, "pck.sparse", e->name, 0, "sparse bundles aren't supported"); return; }
    /* Offsets are from the pack's start (V3+, or V2 with the relative flag); a standalone .pck starts at 0 anyway. */
    uint64_t file_base = u64(&r);
    if (version >= 3) {
        uint64_t dir = u64(&r);
        r.pos = dir;
    } else {
        for (int i = 0; i < 16; i++) u32(&r);
    }
    uint32_t count = u32(&r);
    if (r.bad || count > PCK_MAX_FILES) { spx_add(rep, SPX_BLOCK, "pck.corrupt", e->name, 0, "directory is corrupt"); return; }

    typedef struct F { char *path; uint64_t off, size; } F;
    F *files = calloc(count ? count : 1, sizeof(F));
    if (!files) return;
    uint32_t nf = 0;
    for (uint32_t i = 0; i < count && !r.bad; i++) {
        uint32_t sl = u32(&r);
        if (sl == 0 || sl > 4096) { r.bad = 1; break; }
        char *path = malloc((size_t)sl + 1);
        if (!path) break;
        rd(&r, path, sl);
        path[sl] = 0;
        path[strnlen(path, sl)] = 0;
        uint64_t off = u64(&r), size = u64(&r);
        unsigned char md5[16];
        rd(&r, md5, 16);
        uint32_t ff = u32(&r);
        const char *rel = !strncmp(path, "res://", 6) ? path + 6 : path;
        snprintf(where, sizeof(where), "%s:res://%s", e->name, rel);
        if (bad_path(rel)) spx_add(rep, SPX_BLOCK, "pck.path", where, 0, "bad path in the pack");
        if (ff & PCK_FILE_ENCRYPTED) spx_add(rep, SPX_BLOCK, "pck.encrypted", where, 0, "encrypted file can't be checked");
        if (ff & (PCK_FILE_REMOVAL | PCK_FILE_DELTA)) spx_add(rep, SPX_BLOCK, "pck.patch", where, 0, "patch packs aren't supported");
        if (file_base + off > e->usize || size > e->usize - (file_base + off)) { spx_add(rep, SPX_BLOCK, "pck.range", where, 0, "file lies outside the pack"); free(path); continue; }
        files[nf].path = path;
        files[nf].off = file_base + off;
        files[nf].size = size;
        nf++;
    }
    if (r.bad) spx_add(rep, SPX_BLOCK, "pck.corrupt", e->name, 0, "directory is corrupt");

    for (uint32_t i = 0; i < nf; i++) {
        const char *rel = !strncmp(files[i].path, "res://", 6) ? files[i].path + 6 : files[i].path;
        snprintf(where, sizeof(where), "%s:res://%s", e->name, rel);
        rep->files++;
        rep->bytes += files[i].size;
        if (ends_with_ci(rel, ".gdextension") || !strcmp(rel, ".godot/extension_list.cfg"))
            spx_add(rep, SPX_BLOCK, "pck.native", where, 0, "native extensions can't be loaded from a pack");
        else if (ends_with_ci(rel, ".cs") || ends_with_ci(rel, ".csproj") || !strncmp(rel, ".godot/mono/", 12))
            spx_add(rep, SPX_BLOCK, "pck.csharp", where, 0, "C# isn't supported by the Screen- runtime");
        else if (ends_with_ci(rel, ".gdc"))
            spx_add(rep, SPX_BLOCK, "pck.tokens", where, 0, "scripts exported as binary tokens can't be checked; export scripts as text");
        else if (!strcmp(rel, "override.cfg"))
            spx_add(rep, SPX_BLOCK, "pck.override", where, 0, "override.cfg changes engine settings");
        else if (ends_with_ci(rel, ".dll") || ends_with_ci(rel, ".so") || ends_with_ci(rel, ".dylib"))
            spx_add(rep, SPX_BLOCK, "pck.native", where, 0, "native library in the pack");

        unsigned char head[1024];
        size_t hn = files[i].size < sizeof(head) ? (size_t)files[i].size : sizeof(head);
        if (hn && zip_read_at(z, e->data_off + files[i].off, head, hn) == 0) {
            const char *magic = rules_binary_magic(head, hn);
            if (magic) spx_add(rep, SPX_BLOCK, "pck.binary", where, 0, "contains a %s", magic);
        }
        TextKind k = rules_kind(rel);
        if (k == TK_NONE) continue;
        if (files[i].size > PCK_MAX_TEXT) { spx_add(rep, SPX_WARN, "pck.big-text", where, 0, "too big to check"); continue; }
        unsigned char *buf = malloc(files[i].size ? (size_t)files[i].size : 1);
        if (buf && zip_read_at(z, e->data_off + files[i].off, buf, (size_t)files[i].size) == 0)
            rules_scan(rep, m, where, buf, (size_t)files[i].size, k);
        free(buf);
    }
    for (uint32_t i = 0; i < nf; i++) free(files[i].path);
    free(files);
}
