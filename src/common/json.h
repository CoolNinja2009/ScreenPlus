/* Small, bounded JSON DOM parser for wallpaper metadata (LivelyInfo.json,
 * LivelyProperties.json, Wallpaper Engine project.json) and property values.
 * Strings are UTF-8. Objects keep key order. Depth and size are limited. */
#pragma once
#include "common.h"
#include "util.h"

typedef enum JType { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType   t;
    int     b;
    double  num;
    char   *str;      /* J_STR (UTF-8) */
    JVal   *kids;     /* J_ARR / J_OBJ children */
    char  **keys;     /* J_OBJ keys (UTF-8), parallel to kids */
    int     n, cap;
};

JVal       *json_parse(const char *text, size_t len);
JVal       *json_parse_file(const wchar_t *path);
void        json_free(JVal *v);
const JVal *json_get(const JVal *obj, const char *key);      /* case-sensitive */
const JVal *json_geti(const JVal *obj, const char *key);     /* ASCII case-insensitive */
const char *json_str(const JVal *v, const char *def);
double      json_num(const JVal *v, double def);
int         json_bool(const JVal *v, int def);
void        json_str_w(const JVal *v, wchar_t *dst, size_t cap); /* UTF-8 -> UTF-16, "" if not a string */
void        json_write(StrBuf *b, const JVal *v);              /* compact serialization */
void        json_write_string(StrBuf *b, const char *utf8);    /* quoted + escaped */
