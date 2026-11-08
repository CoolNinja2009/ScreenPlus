#include "zip.h"
#include "third_party/puff.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

FILE *spx_fopen(const char *path)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1024)) return NULL;
    return _wfopen(w, L"rb");
#else
    return fopen(path, "rb");
#endif
}

int spx_seek(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int zip_read_at(Zip *z, uint64_t off, void *buf, size_t n)
{
    if (off > z->size || n > z->size - off) return -1;
    if (spx_seek(z->f, off) != 0) return -1;
    return fread(buf, 1, n, z->f) == n ? 0 : -1;
}

static void seterr(char *err, size_t cap, const char *msg)
{
    if (cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

int zip_open(Zip *z, const char *path, char *err, size_t errcap)
{
    memset(z, 0, sizeof(*z));
    z->f = spx_fopen(path);
    if (!z->f) { seterr(err, errcap, "can't open the file"); return -1; }
#ifdef _WIN32
    _fseeki64(z->f, 0, SEEK_END);
    z->size = (uint64_t)_ftelli64(z->f);
#else
    fseeko(z->f, 0, SEEK_END);
    z->size = (uint64_t)ftello(z->f);
#endif
    /* End of central directory: last 22 bytes plus a comment of up to 65535. */
    size_t tail = z->size < 65557 ? (size_t)z->size : 65557;
    unsigned char *t = malloc(tail ? tail : 1);
    if (!t || tail < 22 || zip_read_at(z, z->size - tail, t, tail) != 0) {
        free(t); seterr(err, errcap, "not a zip file"); return -1;
    }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (le32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); seterr(err, errcap, "not a zip file"); return -1; }
    const unsigned char *e = t + eocd;
    if (eocd >= 20 && le32(e - 20) == 0x07064b50) z->zip64 = 1;
    if (le16(e + 4) != 0 || le16(e + 6) != 0 || le16(e + 8) != le16(e + 10)) z->multidisk = 1;
    uint32_t count = le16(e + 10), cdsize = le32(e + 12), cdoff = le32(e + 16);
    free(t);
    if (z->zip64 || z->multidisk || cdoff == 0xFFFFFFFFu || count == 0xFFFF) return 0; /* caller blocks */
    if ((uint64_t)cdoff + cdsize > z->size) { seterr(err, errcap, "central directory out of range"); return -1; }
    z->cd_off = cdoff;

    unsigned char *cd = malloc(cdsize ? cdsize : 1);
    z->e = calloc(count ? count : 1, sizeof(ZipEntry));
    if (!cd || !z->e || zip_read_at(z, cdoff, cd, cdsize) != 0) {
        free(cd); seterr(err, errcap, "can't read the central directory"); return -1;
    }
    size_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > cdsize || le32(cd + p) != 0x02014b50) { free(cd); seterr(err, errcap, "central directory corrupt"); return -1; }
        const unsigned char *h = cd + p;
        ZipEntry *x = &z->e[z->n];
        x->made_by_os = h[5];
        x->flags = le16(h + 8);
        x->method = le16(h + 10);
        x->crc = le32(h + 16);
        x->csize = le32(h + 20);
        x->usize = le32(h + 24);
        uint16_t nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32);
        x->ext_attr = le32(h + 38);
        x->lho = le32(h + 42);
        if (p + 46 + nl + xl + cl > cdsize) { free(cd); seterr(err, errcap, "central directory corrupt"); return -1; }
        x->name = malloc((size_t)nl + 1);
        if (!x->name) { free(cd); seterr(err, errcap, "out of memory"); return -1; }
        memcpy(x->name, h + 46, nl);
        x->name[nl] = 0;
        z->n++;
        if (x->csize == 0xFFFFFFFFu || x->usize == 0xFFFFFFFFu || x->lho == 0xFFFFFFFFu) z->zip64 = 1;
        p += 46 + (size_t)nl + xl + cl;

        unsigned char lh[30];
        if (zip_read_at(z, x->lho, lh, 30) == 0 && le32(lh) == 0x04034b50) {
            uint16_t lnl = le16(lh + 26), lxl = le16(lh + 28);
            x->data_off = x->lho + 30 + lnl + lxl;
            char *ln = malloc((size_t)lnl + 1);
            if (ln && lnl == nl && zip_read_at(z, x->lho + 30, ln, lnl) == 0 && memcmp(ln, x->name, nl) == 0)
                x->local_ok = 1;
            free(ln);
        }
    }
    free(cd);
    return 0;
}

static uint32_t crc32_buf(const unsigned char *p, size_t n)
{
    static uint32_t tab[256];
    if (!tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int zip_extract(Zip *z, const ZipEntry *e, unsigned char **out, size_t max)
{
    *out = NULL;
    if (!e->local_ok) return -2;
    if (e->usize > max || e->csize > max) return -3;
    unsigned char *dst = malloc(e->usize ? (size_t)e->usize : 1);
    if (!dst) return -3;
    if (e->method == 0) {
        if (e->csize != e->usize || zip_read_at(z, e->data_off, dst, (size_t)e->usize) != 0) { free(dst); return -2; }
    } else if (e->method == 8) {
        unsigned char *src = malloc(e->csize ? (size_t)e->csize : 1);
        if (!src || zip_read_at(z, e->data_off, src, (size_t)e->csize) != 0) { free(src); free(dst); return -1; }
        unsigned long dl = (unsigned long)e->usize, sl = (unsigned long)e->csize;
        int rc = puff(dst, &dl, src, &sl);
        free(src);
        if (rc != 0 || dl != e->usize) { free(dst); return -2; }
    } else {
        free(dst);
        return -2;
    }
    if (crc32_buf(dst, (size_t)e->usize) != e->crc) { free(dst); return -2; }
    *out = dst;
    return 0;
}

void zip_close(Zip *z)
{
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    if (z->f) fclose(z->f);
    memset(z, 0, sizeof(*z));
}
