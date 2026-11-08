/* Scanner internals shared by scan.c, manifest.c, pck.c and rules.c. */
#pragma once
#include "spx_scan.h"
#include "zip.h"

#define SPX_MAX_HOSTS 64

typedef struct Manifest {
    int  ok;                  /* parsed and valid enough to scan against */
    char entry[32];           /* project.pck or web/index.html */
    int  has_web;             /* web/index.html shipped (entry or "web") */
    int  any_host;            /* net.client contains "*" */
    char hosts[SPX_MAX_HOSTS][128];
    int  nhosts;
    char caps[64][32];        /* declared capability names */
    int  ncaps;
} Manifest;

int  manifest_check(SpxReport *r, const char *text, size_t len, Manifest *m);
int  manifest_has(const Manifest *m, const char *cap);
int  manifest_host_ok(const Manifest *m, const char *host);

/* What kind of text a file is, for the rules. */
typedef enum TextKind { TK_NONE, TK_GODOT, TK_WEB } TextKind;
TextKind rules_kind(const char *name);
void rules_scan(SpxReport *r, const Manifest *m, const char *file, const unsigned char *buf, size_t len, TextKind kind);
const char *rules_binary_magic(const unsigned char *buf, size_t len); /* "PE executable", ... or NULL */

void pck_scan(SpxReport *r, Zip *z, const ZipEntry *e, const Manifest *m);

int  ends_with_ci(const char *s, const char *suffix);
