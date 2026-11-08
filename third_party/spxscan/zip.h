/* Read-only zip reader for the scanner: central directory, strict checks, stored and deflate entries.
 * Zip64, encryption and multi-disk archives are refused by the caller, not read. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

typedef struct ZipEntry {
    char    *name;        /* UTF-8 as stored */
    uint64_t csize, usize;
    uint64_t lho;         /* local header offset */
    uint64_t data_off;    /* first byte of the data, from the local header */
    uint32_t crc, ext_attr;
    uint16_t method, flags;
    uint8_t  made_by_os;  /* 3 = Unix: ext_attr >> 16 is st_mode */
    int      local_ok;    /* local header present and its name matches */
} ZipEntry;

typedef struct Zip {
    FILE     *f;
    uint64_t  size;
    uint64_t  cd_off;
    ZipEntry *e;
    int       n;
    int       zip64, multidisk;
} Zip;

FILE *spx_fopen(const char *utf8_path);              /* UTF-8 paths on Windows too */
int   spx_seek(FILE *f, uint64_t off);
int   zip_open(Zip *z, const char *utf8_path, char *err, size_t errcap);
int   zip_read_at(Zip *z, uint64_t off, void *buf, size_t n);  /* raw bytes, 0 ok */
/* Whole entry into a new buffer (caller frees), CRC checked. 0 ok, -1 I/O, -2 corrupt, -3 too big. */
int   zip_extract(Zip *z, const ZipEntry *e, unsigned char **out, size_t max);
void  zip_close(Zip *z);
