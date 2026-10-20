#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define JSON_MAX_DEPTH 48
#define JSON_MAX_FILE  (4 * 1024 * 1024)

typedef struct Parser {
    const char *p, *end;
    int depth;
    int failed;
} Parser;

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ps->p++;
        else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '/') {          /* tolerate // comments */
            while (ps->p < ps->end && *ps->p != '\n') ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/')) ps->p++;
            ps->p = ps->p + 2 <= ps->end ? ps->p + 2 : ps->end;
        } else break;
    }
}

static int push_child(JVal *parent, char *key, JVal *child)
{
    if (parent->n == parent->cap) {
        int nc = parent->cap ? parent->cap * 2 : 8;
        JVal *k = (JVal *)realloc(parent->kids, sizeof(JVal) * (size_t)nc);
        if (!k) return 0;
        parent->kids = k;
        if (parent->t == J_OBJ) {
            char **ks = (char **)realloc(parent->keys, sizeof(char *) * (size_t)nc);
            if (!ks) return 0;
            parent->keys = ks;
        }
        parent->cap = nc;
    }
    parent->kids[parent->n] = *child;
    if (parent->t == J_OBJ) parent->keys[parent->n] = key;
    parent->n++;
    return 1;
}

static void put_utf8(StrBuf *b, unsigned cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    sb_append(b, t, n);
}

static int hex4(Parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    ps->p += 4;
    *out = v;
    return 1;
}

static char *parse_string(Parser *ps)
{
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    StrBuf b = { 0 };
    sb_append(&b, "", 0);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            sb_append(&b, &c, 1);
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case '"': sb_append(&b, "\"", 1); break;
        case '\\': sb_append(&b, "\\", 1); break;
        case '/': sb_append(&b, "/", 1); break;
        case 'b': sb_append(&b, "\b", 1); break;
        case 'f': sb_append(&b, "\f", 1); break;
        case 'n': sb_append(&b, "\n", 1); break;
        case 'r': sb_append(&b, "\r", 1); break;
        case 't': sb_append(&b, "\t", 1); break;
        case 'u': {
            unsigned cp = 0, lo = 0;
            if (!hex4(ps, &cp)) goto bad;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                ps->p += 2;
                if (!hex4(ps, &lo)) goto bad;
                if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            put_utf8(&b, cp);
            break;
        }
        default: goto bad;
        }
    }
    if (ps->p >= ps->end || b.failed) goto bad;
    ps->p++;   /* closing quote */
    return b.p;
bad:
    sb_free(&b);
    return NULL;
}

static int parse_value(Parser *ps, JVal *out);
static void free_children(JVal *v);

static int parse_container(Parser *ps, JVal *out, int obj)
{
    out->t = obj ? J_OBJ : J_ARR;
    ps->p++;
    if (++ps->depth > JSON_MAX_DEPTH) return 0;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return 1;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (obj) {
            key = parse_string(ps);
            if (!key) return 0;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(key); return 0; }
            ps->p++;
        }
        JVal child;
        memset(&child, 0, sizeof child);
        skip_ws(ps);
        if (!parse_value(ps, &child) || !push_child(out, key, &child)) {
            free(key);
            free_children(&child);   /* child itself lives on the stack */
            return 0;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {   /* tolerate trailing comma */
                ps->p++;
                break;
            }
            continue;
        }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            break;
        }
        return 0;
    }
    ps->depth--;
    return 1;
}

static int parse_value(Parser *ps, JVal *out)
{
    skip_ws(ps);
    if (ps->p >= ps->end) return 0;
    char c = *ps->p;
    if (c == '{') return parse_container(ps, out, 1);
    if (c == '[') return parse_container(ps, out, 0);
    if (c == '"') {
        out->t = J_STR;
        out->str = parse_string(ps);
        return out->str != NULL;
    }
    if (ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) { out->t = J_BOOL; out->b = 1; ps->p += 4; return 1; }
    if (ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) { out->t = J_BOOL; out->b = 0; ps->p += 5; return 1; }
    if (ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) { out->t = J_NULL; ps->p += 4; return 1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", *ps->p)) tmp[n++] = *ps->p++;
        tmp[n] = 0;
        char *endp = NULL;
        out->t = J_NUM;
        out->num = strtod(tmp, &endp);
        return endp && *endp == 0 && isfinite(out->num);
    }
    return 0;
}

static void free_children(JVal *v)
{
    for (int i = 0; i < v->n; i++) {
        free_children(&v->kids[i]);
        if (v->t == J_OBJ && v->keys) free(v->keys[i]);
    }
    free(v->kids);
    free(v->keys);
    free(v->str);
    v->kids = NULL;
    v->keys = NULL;
    v->str = NULL;
    v->n = v->cap = 0;
}

void json_free(JVal *v)   /* for roots returned by json_parse / json_parse_file */
{
    if (!v) return;
    free_children(v);
    free(v);
}

JVal *json_parse(const char *text, size_t len)
{
    if (!text) return NULL;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }
    Parser ps = { text, text + len, 0, 0 };
    JVal *root = (JVal *)calloc(1, sizeof *root);
    if (!root) return NULL;
    if (!parse_value(&ps, root)) {
        free_children(root);
        free(root);
        return NULL;
    }
    return root;
}

JVal *json_parse_file(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    JVal *v = NULL;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < JSON_MAX_FILE) {
        char *buf = (char *)malloc((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, NULL)) v = json_parse(buf, rd);
        free(buf);
    }
    CloseHandle(f);
    return v;
}

const JVal *json_get(const JVal *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (obj->keys[i] && !strcmp(obj->keys[i], key)) return &obj->kids[i];
    return NULL;
}

const JVal *json_geti(const JVal *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (obj->keys[i] && !_stricmp(obj->keys[i], key)) return &obj->kids[i];
    return NULL;
}

const char *json_str(const JVal *v, const char *def) { return v && v->t == J_STR ? v->str : def; }

double json_num(const JVal *v, double def)
{
    if (!v) return def;
    if (v->t == J_NUM) return v->num;
    if (v->t == J_BOOL) return v->b;
    if (v->t == J_STR && v->str) {
        char *e = NULL;
        double d = strtod(v->str, &e);
        if (e && e != v->str) return d;
    }
    return def;
}

int json_bool(const JVal *v, int def)
{
    if (!v) return def;
    if (v->t == J_BOOL) return v->b;
    if (v->t == J_NUM) return v->num != 0.0;
    if (v->t == J_STR && v->str) return !_stricmp(v->str, "true") || !strcmp(v->str, "1");
    return def;
}

void json_str_w(const JVal *v, wchar_t *dst, size_t cap)
{
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!v || v->t != J_STR || !v->str) return;
    wchar_t *w = wide_from_utf8(v->str, -1);
    if (w) {
        str_copy(dst, cap, w);
        free(w);
    }
}

void json_write_string(StrBuf *b, const char *s)
{
    sb_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[8];
        switch (*p) {
        case '"':  sb_append(b, "\\\"", 2); break;
        case '\\': sb_append(b, "\\\\", 2); break;
        case '\n': sb_append(b, "\\n", 2); break;
        case '\r': sb_append(b, "\\r", 2); break;
        case '\t': sb_append(b, "\\t", 2); break;
        default:
            if (*p < 0x20 || *p == '<' || *p == '>' || *p == '&') {   /* safe inside <script> too */
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                sb_append(b, esc, 6);
            } else {
                sb_append(b, (const char *)p, 1);
            }
        }
    }
    sb_append(b, "\"", 1);
}

void json_write(StrBuf *b, const JVal *v)
{
    if (!v) { sb_puts(b, "null"); return; }
    switch (v->t) {
    case J_NULL: sb_puts(b, "null"); break;
    case J_BOOL: sb_puts(b, v->b ? "true" : "false"); break;
    case J_NUM: {
        char t[40];
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) snprintf(t, sizeof t, "%.0f", v->num);
        else snprintf(t, sizeof t, "%.17g", v->num);
        sb_puts(b, t);
        break;
    }
    case J_STR: json_write_string(b, v->str); break;
    case J_ARR:
        sb_append(b, "[", 1);
        for (int i = 0; i < v->n; i++) {
            if (i) sb_append(b, ",", 1);
            json_write(b, &v->kids[i]);
        }
        sb_append(b, "]", 1);
        break;
    case J_OBJ:
        sb_append(b, "{", 1);
        for (int i = 0; i < v->n; i++) {
            if (i) sb_append(b, ",", 1);
            json_write_string(b, v->keys[i]);
            sb_append(b, ":", 1);
            json_write(b, &v->kids[i]);
        }
        sb_append(b, "}", 1);
        break;
    }
}
