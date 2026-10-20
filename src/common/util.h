#pragma once
#include "common.h"

/* Bounded string helpers (always NUL-terminate, truncate on overflow). */
void   str_copy(wchar_t *dst, size_t cap, const wchar_t *src);
void   str_copyn(wchar_t *dst, size_t cap, const wchar_t *src, size_t n);
void   str_cat(wchar_t *dst, size_t cap, const wchar_t *src);
void   str_printf(wchar_t *dst, size_t cap, const wchar_t *fmt, ...);
int    str_ieq(const wchar_t *a, const wchar_t *b);
int    str_icontains(const wchar_t *hay, const wchar_t *needle);
int    str_istarts(const wchar_t *s, const wchar_t *prefix);
void   str_trim(wchar_t *s);
void   str_sanitize_line(wchar_t *s);      /* replace control chars with spaces */

/* UTF-8 <-> UTF-16 (heap results, free with free()). */
char    *utf8_from_wide(const wchar_t *w, int *outLen);
wchar_t *wide_from_utf8(const char *s, int len);

/* Paths */
const wchar_t *path_filename(const wchar_t *path);
const wchar_t *path_ext(const wchar_t *path);      /* includes the dot, or "" */
void   path_dirname(wchar_t *dst, size_t cap, const wchar_t *path);
void   path_join(wchar_t *dst, size_t cap, const wchar_t *dir, const wchar_t *name);
void   path_stem(wchar_t *dst, size_t cap, const wchar_t *path); /* file name without extension */
int    file_exists(const wchar_t *path);
int    dir_exists(const wchar_t *path);
int    dir_create_recursive(const wchar_t *path);

/* Time */
int64_t time_unix_now(void);
double  time_ms(void);                              /* monotonic, QPC based */
uint64_t filetime_now(void);                        /* precise system time as 100ns ticks */
uint64_t process_creation_filetime(HANDLE process);

/* Misc */
void     random_id(wchar_t out[SP_ID_LEN]);
uint32_t hash_fnv1a_w(const wchar_t *s);
int      url_encode_path(char *dst, size_t cap, const wchar_t *path);   /* percent-encode as UTF-8 */
DWORD    windows_build_number(void);
void     shell_open(const wchar_t *target);   /* ShellExecute "open" */
void     shell_reveal(const wchar_t *path);   /* Explorer with item selected */

/* Growable UTF-8 buffer */
typedef struct StrBuf {
    char  *p;
    size_t len;
    size_t cap;
    int    failed;
} StrBuf;
void sb_append(StrBuf *b, const char *s, size_t n);
void sb_puts(StrBuf *b, const char *s);
void sb_put_wide(StrBuf *b, const wchar_t *w);
void sb_printf(StrBuf *b, const char *fmt, ...);
void sb_free(StrBuf *b);
