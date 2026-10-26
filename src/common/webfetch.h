/* HTTP(S) downloads over WinHTTP, and the optional URL-import tools (yt-dlp + QuickJS-NG).
 * Shared by ScreenPlus.exe and ScreenPlus.com (the installer runs `screenplus tools install`),
 * so nothing here logs: failures come back as text. */
#pragma once
#include "common.h"

typedef struct HttpGet HttpGet;

/* Called while a body downloads; total < 0 when the server didn't say. Return 0 to cancel. */
typedef int (*FetchProgress)(void *ctx, int64_t done, int64_t total);

/* Sends a GET for an http(s) URL (https only unless allowHttp) and reads the response headers.
 * Redirects are followed (never https -> http). NULL on failure, with err set. */
HttpGet       *http_open(const wchar_t *url, int allowHttp, wchar_t *err, size_t errCap);
int            http_status(const HttpGet *g);
int            http_ranges(const HttpGet *g);           /* the server reads byte ranges (streamable) */
int64_t        http_length(const HttpGet *g);           /* Content-Length, -1 unknown */
const wchar_t *http_content_type(const HttpGet *g);     /* lower case, without parameters ("" none) */
int            http_read(HttpGet *g, char *buf, int cap); /* up to cap bytes of the body; 0 at the end */
/* Streams the body to dest (overwritten). Fails above maxBytes or when cb cancels. sha256Hex
 * (optional, 65 chars) receives the SHA-256 of what was written. Deletes dest on failure. */
BOOL           http_save(HttpGet *g, const wchar_t *dest, int64_t maxBytes, FetchProgress cb, void *ctx,
                         char *sha256Hex, wchar_t *err, size_t errCap);
void           http_close(HttpGet *g);

BOOL file_sha256(const wchar_t *path, char hex[65]);

/* URL-import tools: official release binaries, pinned by version and SHA-256. They are looked up in
 * <exe dir>\tools (portable build) and then <data>\tools (installer / first use). */
typedef struct UrlTool {
    const wchar_t *file;      /* name in the tools folder */
    const wchar_t *name;      /* name + version, for messages */
    const wchar_t *url;
    const char    *sha256;
    int64_t        size;
} UrlTool;

enum { TOOL_YTDLP = 0, TOOL_QJS = 1, TOOL_COUNT = 2 };
extern const UrlTool k_urlTools[TOOL_COUNT];

BOOL urltools_find(int tool, wchar_t *out, size_t cap);   /* verified copy (size + SHA-256) */
BOOL urltools_present(void);                              /* both files exist (cheap, no hashing) */
/* Downloads whatever is missing or doesn't match its pinned hash into <data>\tools. */
BOOL urltools_install(FetchProgress cb, void *ctx, wchar_t *err, size_t errCap);
void urltools_remove(void);                               /* deletes <data>\tools */
