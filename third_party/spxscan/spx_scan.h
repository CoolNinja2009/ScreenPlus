/* spx scanner: checks a Screen+ project (.spx) before it is installed, exported or uploaded (docs/design.md §8.1).
 * Portable C11 (Windows, Linux, macOS), no dependencies beyond puff (zlib's small inflater).
 * It reports; it never runs or unpacks anything. What it can't prove, the sandbox enforces. */
#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPX_SCANNER_VERSION "0.1.0"

typedef enum SpxSeverity { SPX_PASS = 0, SPX_INFO = 0, SPX_WARN = 1, SPX_BLOCK = 2 } SpxSeverity;

typedef struct SpxFinding {
    SpxSeverity sev;
    char code[32];   /* stable id, e.g. "zip.path", "script.os-exec" */
    char file[256];  /* path inside the .spx; "a.pck:res://x.gd" inside the pack */
    int  line;       /* 1-based, 0 when not a text file */
    char msg[256];
} SpxFinding;

typedef struct SpxReport {
    SpxFinding *items;
    int n, cap;
    SpxSeverity verdict;
    char id[129], title[81], version[32], runtime[48];
    char used[48][32]; /* capabilities the code uses (call_api namespaces), declared or not */
    int  nused;
    unsigned long long files, bytes;
} SpxReport;

/* Returns 0 when the file was scanned (see r->verdict), -1 when it couldn't be opened at all. */
int  spx_scan(const char *utf8_path, SpxReport *r);
void spx_report_free(SpxReport *r);
void spx_report_print(const SpxReport *r, FILE *out);
void spx_report_json(const SpxReport *r, FILE *out);
const char *spx_severity_name(SpxSeverity s);
/* The first finding of the worst severity, for a one-line message; NULL when there are none. */
const SpxFinding *spx_report_worst(const SpxReport *r);

/* Copies one file (e.g. "project.pck") out of a .spx to dest (UTF-8 paths). Only for packs that were scanned and
 * not blocked. 0 ok, -1 with a reason in err. */
int spx_extract_file(const char *spx, const char *name, const char *dest, char *err, size_t errcap);

/* Internal, shared by the scanner's files. */
void spx_add(SpxReport *r, SpxSeverity sev, const char *code, const char *file, int line, const char *fmt, ...);
void spx_use(SpxReport *r, const char *cap);

#ifdef __cplusplus
}
#endif
