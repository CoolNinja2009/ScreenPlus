#include "sjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 32

typedef struct P { const char *s, *e; int depth; char *err; size_t errcap; } P;

static int fail(P *p, const char *msg)
{
    if (p->errcap && !p->err[0]) snprintf(p->err, p->errcap, "%s at byte %ld", msg, (long)(p->e - p->s));
    return -1;
}

static void ws(P *p, const char **c)
{
    while (*c < p->e && (**c == ' ' || **c == '\t' || **c == '\n' || **c == '\r')) (*c)++;
}

static int push(SJVal *v, const char *key, SJVal *kid)
{
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 4;
        SJVal *k = realloc(v->kids, (size_t)cap * sizeof(SJVal));
        if (!k) return -1;
        v->kids = k;
        if (v->t == SJ_OBJ) {
            char **ks = realloc(v->keys, (size_t)cap * sizeof(char *));
            if (!ks) return -1;
            v->keys = ks;
        }
        v->cap = cap;
    }
    v->kids[v->n] = *kid;
    if (v->t == SJ_OBJ) v->keys[v->n] = (char *)key;
    v->n++;
    return 0;
}

static void put(char *o, size_t *n, unsigned cp)
{
    if (cp < 0x80) o[(*n)++] = (char)cp;
    else if (cp < 0x800) { o[(*n)++] = (char)(0xC0 | cp >> 6); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o[(*n)++] = (char)(0xE0 | cp >> 12); o[(*n)++] = (char)(0x80 | (cp >> 6 & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else { o[(*n)++] = (char)(0xF0 | cp >> 18); o[(*n)++] = (char)(0x80 | (cp >> 12 & 63)); o[(*n)++] = (char)(0x80 | (cp >> 6 & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
}

static int hex4(const char *c, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char h = c[i];
        *v <<= 4;
        if (h >= '0' && h <= '9') *v |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') *v |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') *v |= (unsigned)(h - 'A' + 10);
        else return -1;
    }
    return 0;
}

static char *str(P *p, const char **c)
{
    (*c)++; /* opening quote */
    const char *a = *c;
    size_t cap = 0;
    while (*c < p->e && **c != '"') { if (**c == '\\') (*c)++; (*c)++; cap++; }
    if (*c >= p->e) { fail(p, "unterminated string"); return NULL; }
    char *o = malloc(cap * 4 + 1);
    if (!o) return NULL;
    size_t n = 0;
    for (const char *s = a; s < *c; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 0x20) { free(o); fail(p, "control character in string"); return NULL; }
        if (ch != '\\') { o[n++] = (char)ch; continue; }
        s++;
        switch (*s) {
        case '"': o[n++] = '"'; break;
        case '\\': o[n++] = '\\'; break;
        case '/': o[n++] = '/'; break;
        case 'b': o[n++] = '\b'; break;
        case 'f': o[n++] = '\f'; break;
        case 'n': o[n++] = '\n'; break;
        case 'r': o[n++] = '\r'; break;
        case 't': o[n++] = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (*c - s < 5 || hex4(s + 1, &cp)) { free(o); fail(p, "bad \\u escape"); return NULL; }
            s += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && *c - s >= 7 && s[1] == '\\' && s[2] == 'u' && !hex4(s + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                s += 6;
            }
            if (cp == 0) { free(o); fail(p, "NUL in string"); return NULL; }
            put(o, &n, cp);
            break;
        }
        default: free(o); fail(p, "bad escape"); return NULL;
        }
    }
    o[n] = 0;
    (*c)++; /* closing quote */
    return o;
}

static void freeval(SJVal *v)
{
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        freeval(&v->kids[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->kids);
    free(v->keys);
}

static int val(P *p, const char **c, SJVal *v)
{
    memset(v, 0, sizeof(*v));
    ws(p, c);
    if (*c >= p->e) return fail(p, "unexpected end");
    char ch = **c;
    if (ch == '{' || ch == '[') {
        if (++p->depth > JSON_MAX_DEPTH) return fail(p, "too deep");
        v->t = ch == '{' ? SJ_OBJ : SJ_ARR;
        char close = ch == '{' ? '}' : ']';
        (*c)++;
        ws(p, c);
        if (*c < p->e && **c == close) { (*c)++; p->depth--; return 0; }
        for (;;) {
            char *key = NULL;
            ws(p, c);
            if (v->t == SJ_OBJ) {
                if (*c >= p->e || **c != '"') return fail(p, "expected a key");
                if (!(key = str(p, c))) return -1;
                ws(p, c);
                if (*c >= p->e || **c != ':') { free(key); return fail(p, "expected ':'"); }
                (*c)++;
            }
            SJVal kid;
            if (val(p, c, &kid) != 0 || push(v, key, &kid) != 0) { free(key); freeval(&kid); return -1; }
            ws(p, c);
            if (*c < p->e && **c == ',') { (*c)++; continue; }
            if (*c < p->e && **c == close) { (*c)++; break; }
            return fail(p, "expected ',' or a closing bracket");
        }
        p->depth--;
        return 0;
    }
    if (ch == '"') { v->t = SJ_STR; return (v->str = str(p, c)) ? 0 : -1; }
    if (p->e - *c >= 4 && !memcmp(*c, "true", 4)) { v->t = SJ_BOOL; v->b = 1; *c += 4; return 0; }
    if (p->e - *c >= 5 && !memcmp(*c, "false", 5)) { v->t = SJ_BOOL; *c += 5; return 0; }
    if (p->e - *c >= 4 && !memcmp(*c, "null", 4)) { *c += 4; return 0; }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char buf[64];
        size_t n = 0;
        while (*c < p->e && n < sizeof(buf) - 1 && strchr("+-.eE0123456789", **c)) buf[n++] = *(*c)++;
        buf[n] = 0;
        char *end;
        v->t = SJ_NUM;
        v->num = strtod(buf, &end);
        return *end ? fail(p, "bad number") : 0;
    }
    return fail(p, "unexpected character");
}

SJVal *sj_parse(const char *text, size_t len, char *err, size_t errcap)
{
    if (errcap) err[0] = 0;
    P p = { text, text + len, 0, err, errcap };
    const char *c = text;
    if (len >= 3 && !memcmp(c, "\xEF\xBB\xBF", 3)) c += 3;
    SJVal *v = malloc(sizeof(SJVal));
    if (!v) return NULL;
    if (val(&p, &c, v) != 0) { freeval(v); free(v); return NULL; }
    ws(&p, &c);
    if (c != p.e) { fail(&p, "trailing data"); sj_free(v); return NULL; }
    return v;
}

void sj_free(SJVal *v)
{
    if (!v) return;
    freeval(v);
    free(v);
}

const SJVal *sj_get(const SJVal *o, const char *key)
{
    if (!o || o->t != SJ_OBJ) return NULL;
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], key)) return &o->kids[i];
    return NULL;
}

int sj_has_dup_keys(const SJVal *o)
{
    if (!o) return 0;
    for (int i = 0; i < o->n; i++) {
        if (o->t == SJ_OBJ)
            for (int j = i + 1; j < o->n; j++)
                if (!strcmp(o->keys[i], o->keys[j])) return 1;
        if (sj_has_dup_keys(&o->kids[i])) return 1;
    }
    return 0;
}
