/* Copies one file out of a .spx: what the host needs to run a scanned pack (project.pck, preview.jpg, manifest).
 * Stored entries stream in 1 MB pieces (packs can be large); deflated ones go through zip_extract's limit.
 * Writes to <dest>.part and renames, so a half-written file never looks finished. */
#include "scan_int.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define EXTRACT_CHUNK (1u << 20)
#define EXTRACT_MAX_INFLATE (64u << 20)

static FILE *open_w(const char *path)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1024)) return NULL;
    return _wfopen(w, L"wb");
#else
    return fopen(path, "wb");
#endif
}

static int replace(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t a[1024], b[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, from, -1, a, 1024) || !MultiByteToWideChar(CP_UTF8, 0, to, -1, b, 1024)) return -1;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static void remove_u8(const char *path)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) DeleteFileW(w);
#else
    remove(path);
#endif
}

int spx_extract_file(const char *spx, const char *name, const char *dest, char *err, size_t errcap)
{
    Zip z;
    if (errcap) err[0] = 0;
    if (zip_open(&z, spx, err, errcap) != 0 || z.zip64 || z.multidisk) {
        if (errcap && !err[0]) snprintf(err, errcap, "not a usable .spx");
        zip_close(&z);
        return -1;
    }
    const ZipEntry *e = NULL;
    for (int i = 0; i < z.n; i++) if (!strcmp(z.e[i].name, name)) e = &z.e[i];
    if (!e || !e->local_ok) {
        snprintf(err, errcap, "%s isn't in the pack", name);
        zip_close(&z);
        return -1;
    }
    char part[1100];
    snprintf(part, sizeof(part), "%s.part", dest);
    FILE *o = open_w(part);
    if (!o) {
        snprintf(err, errcap, "can't write %s", part);
        zip_close(&z);
        return -1;
    }
    int ok = 1;
    if (e->method == 0) {
        unsigned char *buf = malloc(EXTRACT_CHUNK);
        uint64_t left = e->usize, at = e->data_off;
        ok = buf != NULL && e->csize == e->usize;
        while (ok && left) {
            size_t n = left < EXTRACT_CHUNK ? (size_t)left : EXTRACT_CHUNK;
            ok = zip_read_at(&z, at, buf, n) == 0 && fwrite(buf, 1, n, o) == n;
            at += n;
            left -= n;
        }
        free(buf);
    } else {
        unsigned char *buf;
        ok = zip_extract(&z, e, &buf, EXTRACT_MAX_INFLATE) == 0;
        if (ok) {
            ok = fwrite(buf, 1, (size_t)e->usize, o) == e->usize;
            free(buf);
        }
    }
    ok = (fclose(o) == 0) && ok;
    zip_close(&z);
    if (!ok || replace(part, dest) != 0) {
        remove_u8(part);
        snprintf(err, errcap, "couldn't copy %s out of the pack", name);
        return -1;
    }
    return 0;
}
