/* Small strict JSON DOM for manifests: UTF-8, objects keep key order, depth and size bounded. */
#pragma once
#include <stddef.h>

typedef enum SJType { SJ_NULL, SJ_BOOL, SJ_NUM, SJ_STR, SJ_ARR, SJ_OBJ } SJType;

typedef struct SJVal {
    SJType  t;
    int    b;
    double num;
    char  *str;           /* SJ_STR, NUL-terminated UTF-8 (no embedded NULs allowed) */
    struct SJVal *kids;    /* SJ_ARR / SJ_OBJ */
    char **keys;          /* SJ_OBJ, parallel to kids */
    int    n, cap;
} SJVal;

SJVal       *sj_parse(const char *text, size_t len, char *err, size_t errcap);
void        sj_free(SJVal *v);
const SJVal *sj_get(const SJVal *obj, const char *key);
int         sj_has_dup_keys(const SJVal *obj);
