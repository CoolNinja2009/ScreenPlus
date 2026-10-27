/* The Store's sites (store_src.h), mapped in docs/store/sources.md. Each is a handful of functions over its own
 * pages: where a listing or search page is for the filters, how its cards and its wallpaper page read, and where
 * previews and files are. A site without the chosen category searches for the category's word instead (unless
 * something was typed: few sites can combine both, and the typed words matter more); resolution and sort apply
 * where the site supports them, and store.c filters the rest. */
#include "store_src.h"
#include "../common/util.h"
#include "../common/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const StoreCategory k_categories[CAT_COUNT] = {
    { L"All categories", NULL }, { L"Anime", "anime" }, { L"Games", "games" }, { L"Nature", "nature" }, { L"Cars", "cars" },
    { L"Space", "space" }, { L"Animals", "animals" }, { L"Fantasy", "fantasy" }, { L"City", "city" }, { L"Abstract", "abstract" },
    { L"Movies & TV", "movie" }, { L"Sport", "sport" }, { L"Technology", "technology" }, { L"Minimal", "minimal" },
};
const wchar_t *const k_resNames[SR_COUNT] = { L"All resolutions", L"HD 1080p+", L"2K 1440p+", L"4K", L"5K+", L"8K" };
const int k_resMin[SR_COUNT][2] = { { 0, 0 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 5120, 2880 }, { 7680, 4320 } };
const wchar_t *const k_sortNames[SQ_COUNT] = { L"Best match", L"Latest", L"Popular", L"Most downloaded" };

/* ---------------------------------------------------------------- page reading */

static const char *find(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    while (p && end - p >= (ptrdiff_t)n) {
        p = (const char *)memchr(p, needle[0], (size_t)(end - p) - n + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, n) == 0) return p;
        p++;
    }
    return NULL;
}

static const char *after(const char *p, const char *end, const char *needle)
{
    p = find(p, end, needle);
    return p ? p + strlen(needle) : NULL;
}

static const char *until(const char *p, const char *end, char c)
{
    return p && end > p ? (const char *)memchr(p, c, (size_t)(end - p)) : NULL;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_slug(char c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '_'; }

static int number(const char **p, const char *end)
{
    int v = 0;
    while (*p < end && is_digit(**p) && v < 100000000) v = v * 10 + (*(*p)++ - '0');
    return v;
}

/* [a, b) as text: entities decoded, trimmed */
static void text_of(const char *a, const char *b, wchar_t *out, size_t cap)
{
    out[0] = 0;
    wchar_t *w = a && b && b > a ? wide_from_utf8(a, (int)(b - a)) : NULL;
    if (!w) return;
    html_unescape(w);
    str_copy(out, cap, w);
    free(w);
    str_trim(out);
}

static void copy_of(const char *a, const char *b, char *out, size_t cap)
{
    size_t n = a && b && b > a ? (size_t)(b - a) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, a, n);
    out[n] = 0;
}

/* the value of name="…" (or of a bare name=…) in [p, end); *ve: where it ends */
static const char *attr(const char *p, const char *end, const char *name, const char **ve)
{
    const char *v = after(p, end, name), *e;
    if (!v) return NULL;
    if (v < end && *v == '"') {
        v++;
        e = until(v, end, '"');
        *ve = e ? e : end;
        return v;
    }
    for (e = v; e < end && *e != ' ' && *e != '>'; e++) {}
    *ve = e;
    return v;
}

/* "3840x2160" or "3840×2160" at p */
static int res_at(const char *p, const char *end, int *w, int *h)
{
    const char *q = p;
    int a = number(&q, end), b;
    if (q < end && *q == 'x') q++;
    else if (end - q >= 2 && (unsigned char)q[0] == 0xC3 && (unsigned char)q[1] == 0x97) q += 2;
    else return 0;
    b = number(&q, end);
    if (a < 100 || b < 100) return 0;
    *w = a;
    *h = b;
    return 1;
}

/* "23.9Mb", "7.9 MB", "850 KB" in MiB */
static float size_mb(const char *p, const char *end)
{
    if (!p || p >= end) return 0.0f;
    float v = (float)atof(p);
    const char *u = p;
    while (u < end && (is_digit(*u) || *u == '.' || *u == ' ')) u++;
    if (u < end && (*u == 'K' || *u == 'k')) v /= 1024.0f;
    else if (u < end && (*u == 'G' || *u == 'g')) v *= 1024.0f;
    return v;
}

/* room for one more card (the caller counts it once it reads) */
static StoreItem *add(StoreList *l)
{
    if (l->count == l->cap) {
        int nc = l->cap ? l->cap * 2 : 48;
        StoreItem *n = (StoreItem *)realloc(l->items, sizeof(StoreItem) * (size_t)nc);
        if (!n) return NULL;
        l->items = n;
        l->cap = nc;
    }
    StoreItem *it = &l->items[l->count];
    ZeroMemory(it, sizeof *it);
    return it;
}

/* a link to the next page, written as pat (a digit right after it would be another page) */
static int has_next(const char *html, const char *end, const char *pat)
{
    size_t n = strlen(pat);
    for (const char *q = find(html, end, pat); q; q = find(q + 1, end, pat))
        if (q + n >= end || !is_digit(q[n])) return 1;
    return 0;
}

static void encode(const wchar_t *s, wchar_t *out, size_t cap)
{
    static const wchar_t hex[] = L"0123456789ABCDEF";
    char *u8 = utf8_from_wide(s, NULL);
    size_t j = 0;
    for (const unsigned char *c = (const unsigned char *)(u8 ? u8 : ""); *c && j + 4 < cap; c++) {
        if (is_slug((char)*c) || *c == '.' || *c == '~') {
            out[j++] = (wchar_t)*c;
        } else if (*c == ' ') {
            out[j++] = L'+';
        } else {
            out[j++] = L'%';
            out[j++] = hex[*c >> 4];
            out[j++] = hex[*c & 15];
        }
    }
    out[j] = 0;
    free(u8);
}

/* what to search for: the typed words or, on a site without the category, the category's word */
static void words(const StoreQuery *q, const char *native, wchar_t *out, size_t cap)
{
    if (q->text[0] || !q->category || native) str_copy(out, cap, q->text);
    else str_printf(out, cap, L"%hs", k_categories[q->category].word);
}

/* every word of want in [a, b), ignoring case */
static int has_words(const char *a, const char *b, const wchar_t *want)
{
    char *w = utf8_from_wide(want, NULL), *ctx = NULL;
    int ok = 1;
    for (char *t = w ? strtok_s(w, " ", &ctx) : NULL; t && ok; t = strtok_s(NULL, " ", &ctx)) {
        size_t n = strlen(t);
        ok = 0;
        for (const char *p = a; p + n <= b && !ok; p++) ok = _strnicmp(p, t, n) == 0;
    }
    free(w);
    return ok;
}

void store_badge_res(const wchar_t *badge, int *w, int *h)
{
    static const struct { const wchar_t *b; int w, h; } k[] = {
        { L"8K", 7680, 4320 }, { L"5K", 5120, 2880 }, { L"4K", 3840, 2160 }, { L"2K", 2560, 1440 },
        { L"QHD", 2560, 1440 }, { L"FHD", 1920, 1080 }, { L"HD", 1920, 1080 },
    };
    *w = *h = 0;
    for (size_t i = 0; i < SP_COUNTOF(k); i++) {
        if (str_ieq(badge, k[i].b)) {
            *w = k[i].w;
            *h = k[i].h;
            return;
        }
    }
}

/* ---------------------------------------------------------------- MotionBGS (docs/store/motionbgs.md) */

#define MB L"https://motionbgs.com"
static const char *const k_mbCats[CAT_COUNT] = { NULL, "anime", "games", "nature", "car", "space", "animal", "fantasy",
                                                 "city", "abstract", "tv", "sport", "technology", NULL };
enum { MB_LATEST, MB_4K, MB_TAG, MB_SEARCH };

static int mb_mode(const StoreQuery *q, const char *tag)
{
    if (tag[0]) return MB_TAG;
    if (q->text[0] || (q->category && !k_mbCats[q->category])) return MB_SEARCH;
    if (q->category) return MB_TAG;
    return q->res >= SR_4K ? MB_4K : MB_LATEST;
}

static BOOL mb_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    wchar_t w[100], e[400];
    switch (mb_mode(q, tag)) {
    case MB_TAG:
        str_printf(url, cap, page > 1 ? MB L"/tag:%hs/%d/" : MB L"/tag:%hs/", tag[0] ? tag : k_mbCats[q->category], page);
        break;
    case MB_SEARCH:
        words(q, k_mbCats[q->category], w, SP_COUNTOF(w));
        encode(w, e, SP_COUNTOF(e));
        str_printf(url, cap, page > 1 ? MB L"/search?q=%ls&page=%d" : MB L"/search?q=%ls", e, page);
        break;
    case MB_4K:
        str_printf(url, cap, page > 1 ? MB L"/4k/%d/" : MB L"/4k/", page);
        break;
    default:
        str_printf(url, cap, MB L"/hx2/latest/%d/", page);
        break;
    }
    return TRUE;
}

/* <a title=… href=/<slug>> … <img … src=/i/c/364x205/media/<id>/<poster> …> <span class=ttl>…</span>
 * <span class=frm> 4K </span> </a>; ref: slug, extra: media id, media: the poster's file name */
static int mb_card(const char *p, const char *end, StoreItem *it)
{
    const char *h = after(p, end, " href=/"), *img = after(p, end, " src=/i/c/");
    const char *m = img ? after(img, end, "/media/") : NULL, *tt = after(p, end, "class=ttl>"), *he = h, *ie = m;
    if (!h || !m || !tt) return 0;
    while (he < end && is_slug(*he)) he++;
    if (he == h || (*he != '>' && *he != ' ')) return 0;          /* /mobile/…, /tag:…: not a wallpaper's page */
    while (ie < end && is_digit(*ie)) ie++;
    if (ie == m || ie >= end || *ie != '/') return 0;
    const char *f = ie + 1, *fe = f;
    while (fe < end && *fe != ' ' && *fe != '>' && *fe != '"') fe++;
    copy_of(h, he, it->ref, sizeof it->ref);
    copy_of(m, ie, it->extra, sizeof it->extra);
    copy_of(f, fe, it->media, sizeof it->media);
    text_of(tt, find(tt, end, "</span>"), it->title, SP_COUNTOF(it->title));
    const char *b = after(p, end, "class=frm>");
    if (b) text_of(b, find(b, end, "</span>"), it->badge, SP_COUNTOF(it->badge));
    const char *dot = strrchr(it->media, '.'), *d = dot;     /* newer posters: "<stem>.<W>x<H>.jpg" */
    while (d && d > it->media && d[-1] != '.') d--;
    if (d && d > it->media) res_at(d, dot, &it->w, &it->h);
    it->kind = SKIND_LIVE;
    return it->ref[0] && it->title[0] && it->media[0];
}

static void mb_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    const char *end = html + len, *p = html;
    while ((p = find(p, end, "<a title=")) != NULL) {
        const char *ce = find(p, end, "</a>");
        StoreItem *it = ce ? add(out) : NULL;
        if (!it) break;
        if (mb_card(p, ce, it)) out->count++;
        p = ce + 4;
    }
    int mode = mb_mode(q, tag);
    if (mode == MB_SEARCH && page == 1) {          /* the query named a tag: the site redirected to its listing */
        const char *c = after(html, end, "<link href=https://motionbgs.com/tag:"), *ce = c;
        while (ce && ce < end && is_slug(*ce)) ce++;
        if (c && ce > c && find(ce, SP_MIN(end, ce + 24), "rel=canonical")) copy_of(c, ce, out->tag, sizeof out->tag);
    }
    char pat[160];
    const char *t = out->tag[0] ? out->tag : tag[0] ? tag : q->category && k_mbCats[q->category] ? k_mbCats[q->category] : "";
    if (out->tag[0] || mode == MB_TAG) snprintf(pat, sizeof pat, "/tag:%s/%d/", t, page + 1);
    else if (mode == MB_LATEST) snprintf(pat, sizeof pat, "hx-get=/hx2/latest/%d/", page + 1);
    else if (mode == MB_4K) snprintf(pat, sizeof pat, "/4k/%d/", page + 1);
    else snprintf(pat, sizeof pat, "page=%d", page + 1);
    out->more = has_next(html, end, pat);
}

static BOOL mb_detail_url(const StoreItem *it, wchar_t *url, size_t cap)
{
    str_printf(url, cap, MB L"/%hs", it->ref);
    return TRUE;
}

/* per download: <a href=/dl/<key>/<id> …> … Wallpaper (23.9Mb) … <div class=text-xs>3840x2160 mp4 file</div> … </a> */
static int mb_detail(const char *html, int len, StoreItem *it)
{
    const char *end = html + len;
    it->varCount = 0;
    for (const char *p = after(html, end, "href=/dl/"); p && it->varCount < STORE_VARIANTS; p = after(p, end, "href=/dl/")) {
        const char *k = p, *ae = find(p, end, "</a>"), *res;
        while (k < end && is_slug(*k)) k++;
        if (!ae || k == p || k - p >= (ptrdiff_t)sizeof it->var[0].key || *k != '/') continue;
        StoreVariant v;
        ZeroMemory(&v, sizeof v);
        copy_of(p, k, v.key, sizeof v.key);
        res = after(k, ae, "class=text-xs>");
        if (res) res_at(res, ae, &v.w, &v.h);
        for (const char *q = k; q + 1 < ae; q++) {
            if (*q == '(' && is_digit(q[1])) {
                v.mb = size_mb(q + 1, ae);
                break;
            }
        }
        int dup = 0;
        for (int i = 0; i < it->varCount; i++) dup |= strcmp(it->var[i].key, v.key) == 0;
        if (!dup && v.w > 0) it->var[it->varCount++] = v;
    }
    return it->varCount > 0;
}

static void mb_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    str_printf(url, cap, MB L"/i/c/%ls/media/%hs/%hs%ls", big ? L"546x308" : L"364x205", it->extra, it->media, webp ? L".webp" : L"");
}

static BOOL mb_file(const StoreItem *it, const StoreVariant *v, wchar_t *url, size_t cap)
{
    str_printf(url, cap, MB L"/dl/%hs/%hs/", v->key, it->extra);
    return TRUE;
}

static void mb_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, MB L"/%hs", it->ref); }

/* ---------------------------------------------------------------- Lively Wallpaper's gallery (livelywallpaper.app) */

#define LW L"https://livelywallpaper.app/live-wallpapers/"
static const char *const k_lwCats[CAT_COUNT] = { NULL, "anime", "games", "nature", "cars", "space", NULL, "art-fantasy",
                                                 NULL, NULL, NULL, NULL, NULL, NULL };

/* a category's pages, the 4K ones, or all of them (the gallery has no search: its cards carry their keywords) */
static const char *lw_cat(const StoreQuery *q)
{
    if (q->text[0]) return NULL;
    if (q->category) return k_lwCats[q->category];
    return q->res >= SR_4K ? "4k" : NULL;
}

static BOOL lw_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    const char *c = lw_cat(q);
    (void)tag;
    if (page > 1) str_printf(url, cap, LW L"%hs%hs%d/", c ? c : "", c ? "/" : "", page);
    else str_printf(url, cap, LW L"%hs%hs", c ? c : "", c ? "/" : "");
    return TRUE;
}

/* <article class="wallpaper-card" … data-title="…" data-search="…" data-resolution="4K" data-size="7.9 MB" …>
 * <a class="wallpaper-card-media" href="./<slug>/" …>; ref: slug */
static void lw_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    const char *end = html + len, *p = html, *c = lw_cat(q);
    wchar_t want[160];
    (void)tag;
    str_copy(want, SP_COUNTOF(want), q->text);
    if (q->category && !c && !q->text[0]) str_printf(want, SP_COUNTOF(want), L"%hs", k_categories[q->category].word);
    while ((p = find(p, end, "<article class=\"wallpaper-card\"")) != NULL) {
        const char *ce = find(p + 8, end, "</article>"), *te = until(p, ce, '>');
        if (!te) break;
        const char *se, *s = attr(p, te, " data-search=", &se), *tv, *t = attr(p, te, " data-title=", &tv);
        const char *rv, *r = attr(p, te, " data-resolution=", &rv), *zv, *z = attr(p, te, " data-size=", &zv);
        const char *h = after(te, ce, "class=\"wallpaper-card-media\" href=\""), *he;
        while (h && h < ce && (*h == '.' || *h == '/')) h++;    /* "./<slug>/" here, "../<slug>/" on a category's pages */
        for (he = h; he && he < ce && is_slug(*he); he++) {}
        StoreItem *it = t && h && he > h && (!want[0] || (s && has_words(s, se, want))) ? add(out) : NULL;
        if (it) {
            copy_of(h, he, it->ref, sizeof it->ref);
            text_of(t, tv, it->title, SP_COUNTOF(it->title));
            if (r) text_of(r, rv, it->badge, SP_COUNTOF(it->badge));
            store_badge_res(it->badge, &it->w, &it->h);
            strcpy_s(it->var[0].key, sizeof it->var[0].key, "mp4");
            it->var[0].w = it->w;
            it->var[0].h = it->h;
            it->var[0].mb = z ? size_mb(z, zv) : 0.0f;
            it->varCount = 1;
            it->kind = SKIND_LIVE;
            if (it->title[0]) out->count++;
        }
        p = ce;
    }
    char pat[96];
    snprintf(pat, sizeof pat, "live-wallpapers/%s%s%d/", c ? c : "", c ? "/" : "", page + 1);
    out->more = has_next(html, end, pat);
}

static void lw_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    (void)webp;                                     /* WebP only */
    str_printf(url, cap, L"https://cdn.livelywallpaper.app/images/wallpapers/library/%hs-%d.webp", it->ref, big ? 640 : 320);
}

static BOOL lw_file(const StoreItem *it, const StoreVariant *v, wchar_t *url, size_t cap)
{
    (void)v;
    str_printf(url, cap, L"https://cdn.livelywallpaper.app/wallpapers/%hs/download.mp4", it->ref);
    return TRUE;
}

static void lw_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, LW L"%hs/", it->ref); }

/* ---------------------------------------------------------------- MoeWalls (moewalls.com, WordPress) */

#define MW L"https://moewalls.com"
static const char *const k_mwCats[CAT_COUNT] = { NULL, "anime", "games", "landscape", "vehicle", NULL, "animal", "fantasy",
                                                 NULL, "abstract", "movies", NULL, NULL, NULL };

static BOOL mw_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    const char *c = q->category ? k_mwCats[q->category] : NULL;
    wchar_t w[100], e[400];
    (void)tag;
    words(q, c, w, SP_COUNTOF(w));
    if (w[0]) {
        encode(w, e, SP_COUNTOF(e));
        if (page > 1) str_printf(url, cap, MW L"/page/%d/?s=%ls", page, e);
        else str_printf(url, cap, MW L"/?s=%ls", e);
    } else if (c) {
        str_printf(url, cap, page > 1 ? MW L"/category/%hs/page/%d/" : MW L"/category/%hs/", c, page);
    } else {
        str_printf(url, cap, page > 1 ? MW L"/page/%d/" : MW L"/", page);
    }
    return TRUE;
}

/* <article class="entry-tpl-grid … resolutions-1920x1080"> … <a title="<Title> Live Wallpaper" class="g1-frame"
 * href="https://moewalls.com/<category>/<slug>/"> … <img … src="https://moewalls.com/<thumb>" … </article>;
 * ref: <category>/<slug>, media: the thumbnail's path */
static void mw_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    const char *end = html + len, *p = html;
    (void)q;
    (void)tag;
    while ((p = find(p, end, "<article class=\"entry-tpl-grid")) != NULL) {
        const char *ce = find(p + 8, end, "</article>"), *te = until(p, ce, '>');
        if (!te) break;
        const char *t = after(te, ce, "<a title=\""), *tv = until(t, ce, '"');
        const char *h = after(te, ce, "class=\"g1-frame\" href=\"https://moewalls.com/"), *hv = until(h, ce, '"');
        const char *m = after(te, ce, " src=\"https://moewalls.com/"), *mv = until(m, ce, '"');
        StoreItem *it = tv && hv && mv ? add(out) : NULL;
        if (it) {
            while (hv > h && hv[-1] == '/') hv--;
            copy_of(h, hv, it->ref, sizeof it->ref);
            copy_of(m, mv, it->media, sizeof it->media);
            text_of(t, tv, it->title, SP_COUNTOF(it->title));
            size_t n = wcslen(it->title);
            if (n > 15 && str_ieq(it->title + n - 15, L" Live Wallpaper")) it->title[n - 15] = 0;
            const char *r = after(p, te, "resolutions-");
            if (r) res_at(r, te, &it->w, &it->h);
            it->kind = SKIND_LIVE;
            if (it->title[0] && it->ref[0]) out->count++;
        }
        p = ce;
    }
    char pat[32];
    snprintf(pat, sizeof pat, "/page/%d/", page + 1);
    out->more = has_next(html, end, pat);
}

static void mw_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    char m[sizeof it->media];
    strcpy_s(m, sizeof m, it->media);
    char *s = big ? strstr(m, "-364x205.") : NULL;
    if (s) memcpy(s, "-728x410.", 9);
    (void)webp;
    str_printf(url, cap, MW L"/%hs", m);
}

static void mw_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, MW L"/%hs/", it->ref); }

/* ---------------------------------------------------------------- wallsflow (wallsflow.com, DataLife Engine) */

#define WF L"https://wallsflow.com"
static const char *const k_wfCats[CAT_COUNT] = { NULL, "anime", "games", NULL, "cars", NULL, NULL, NULL, NULL, NULL,
                                                 "movies", NULL, NULL, NULL };

static BOOL wf_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    const char *c = q->category ? k_wfCats[q->category] : NULL;
    wchar_t w[100], e[400], base[96];
    (void)tag;
    words(q, c, w, SP_COUNTOF(w));
    if (w[0]) {
        encode(w, e, SP_COUNTOF(e));
        str_printf(url, cap, WF L"/index.php?do=search&subaction=search&story=%ls&search_start=%d&full_search=0", e, page);
        return TRUE;
    }
    str_printf(base, SP_COUNTOF(base), WF L"/live-wallpapers/%hs%hs", c ? c : "", c ? "/" : "");
    if (page > 1) str_printf(url, cap, L"%lspage/%d/", base, page);
    else str_copy(url, cap, base);
    return TRUE;
}

/* <img src="https://cloud.wallsflow.com/posts/<month>/thumbs/<file>.webp" … alt="<Title> live wallpaper for desktop" …>
 * … <a href="https://wallsflow.com/live-wallpapers/<category>/<id>-<slug>.html"; ref: <category>/<id>-<slug> */
static void wf_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    static const wchar_t suffix[] = L" live wallpaper for desktop";
    const char *end = html + len, *p = html;
    (void)tag;
    while ((p = after(p, end, "<img src=\"https://cloud.wallsflow.com/")) != NULL) {
        const char *me = until(p, end, '"'), *te = until(p, end, '>'), *lim = SP_MIN(end, p + 4000);
        const char *h = after(p, lim, "href=\"https://wallsflow.com/live-wallpapers/"), *he = h ? find(h, lim, ".html\"") : NULL;
        if (!me || !te || !he || !find(p, me, "/thumbs/")) continue;
        StoreItem *it = add(out);
        if (!it) break;
        const char *av, *a = attr(p, te, " alt=", &av);
        copy_of(p, me, it->media, sizeof it->media);
        copy_of(h, he, it->ref, sizeof it->ref);
        if (a) text_of(a, av, it->title, SP_COUNTOF(it->title));
        size_t n = wcslen(it->title), sn = SP_COUNTOF(suffix) - 1;
        if (n > sn && str_ieq(it->title + n - sn, suffix)) it->title[n - sn] = 0;
        it->kind = SKIND_LIVE;
        if (it->title[0] && it->ref[0]) out->count++;
    }
    char pat[48];
    if (q->text[0] || (q->category && !k_wfCats[q->category])) snprintf(pat, sizeof pat, "list_submit(%d)", page + 1);
    else snprintf(pat, sizeof pat, "/page/%d/", page + 1);
    out->more = has_next(html, end, pat);
}

static BOOL wf_detail_url(const StoreItem *it, wchar_t *url, size_t cap)
{
    str_printf(url, cap, WF L"/live-wallpapers/%hs.html", it->ref);
    return TRUE;
}

/* its resolution ("3840×2160") and size ("97.11 Mb"), for the card and for choosing among copies */
static int wf_detail(const char *html, int len, StoreItem *it)
{
    const char *end = html + len;
    StoreVariant v;
    ZeroMemory(&v, sizeof v);
    strcpy_s(v.key, sizeof v.key, "page");
    for (const char *x = find(html, end, "\xC3\x97"); x && !v.w; x = find(x + 2, end, "\xC3\x97")) {
        const char *d = x;
        while (d > html && is_digit(d[-1])) d--;
        if (x - d >= 3) res_at(d, end, &v.w, &v.h);
    }
    for (const char *m = find(html, end, " Mb"); m && !v.mb; m = find(m + 3, end, " Mb")) {
        const char *d = m;
        while (d > html && (is_digit(d[-1]) || d[-1] == '.')) d--;
        if (d < m) v.mb = (float)atof(d);
    }
    it->varCount = 0;
    if (v.w) it->var[it->varCount++] = v;
    return v.w > 0;
}

static void wf_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    (void)big;
    (void)webp;
    str_printf(url, cap, L"https://cloud.wallsflow.com/%hs", it->media);
}

static void wf_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, WF L"/live-wallpapers/%hs.html", it->ref); }

/* ---------------------------------------------------------------- backiee (backiee.com): still pictures to 8K */

#define BK L"https://backiee.com"
static const char *const k_bkCats[CAT_COUNT] = { NULL, "anime", NULL, "nature", "car", "space", "animals", "fantasy", "city",
                                                 "abstract", NULL, "sport", "technics", NULL };
static const char *const k_bkRes[SR_COUNT] = { NULL, "hd", "hd", "4k", "5k", "8k" };
static const char *const k_bkSort[SQ_COUNT] = { NULL, "date", "rating", "downloads" };

static BOOL bk_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    const char *c = q->category ? k_bkCats[q->category] : NULL, *r = k_bkRes[q->res], *s = k_bkSort[q->sort];
    wchar_t w[100], e[400], args[96];
    (void)tag;
    str_printf(args, SP_COUNTOF(args), L"?page=%d%hs%hs%hs%hs", page, r ? "&resolution=" : "", r ? r : "",
               s ? "&sort=" : "", s ? s : "");
    words(q, c, w, SP_COUNTOF(w));
    if (w[0]) {
        encode(w, e, SP_COUNTOF(e));
        str_printf(url, cap, BK L"/search/%ls%ls%hs%hs", e, args, c && q->text[0] ? "&category=" : "", c && q->text[0] ? c : "");
    } else if (c) {
        str_printf(url, cap, BK L"/categories/%hs%ls", c, args);
    } else {
        str_printf(url, cap, BK L"/%hs%ls", q->sort == SQ_POPULAR ? "popular-wallpapers" : "latest", args);
    }
    return TRUE;
}

/* <article class="wall-card …"> <a class="wall-link" href="https://backiee.com/wallpaper/<slug>/<id>"> …
 * …/static/wallpapers/456x257/<image>.webp … <h3>Title</h3> … </article>; ref: <slug>/<id>, media: the image id.
 * Live wallpapers (/live-wallpaper/) are left out: the site hands those to its app, not to a download. */
static void bk_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    const char *end = html + len, *p = html;
    (void)q;
    (void)tag;
    while ((p = find(p, end, "<article class=\"wall-card")) != NULL) {
        const char *ce = find(p + 8, end, "</article>");
        if (!ce) break;
        const char *h = after(p, ce, "href=\"https://backiee.com/wallpaper/"), *hv = until(h, ce, '"');
        const char *m = after(p, ce, "/static/wallpapers/456x257/"), *mv = m;
        const char *t = after(p, ce, "<h3>"), *tv = t ? find(t, ce, "</h3>") : NULL;
        while (mv && mv < ce && is_digit(*mv)) mv++;
        StoreItem *it = hv && mv && mv > m && tv ? add(out) : NULL;
        if (it) {
            copy_of(h, hv, it->ref, sizeof it->ref);
            copy_of(m, mv, it->media, sizeof it->media);
            text_of(t, tv, it->title, SP_COUNTOF(it->title));
            it->kind = SKIND_STILL;
            if (it->title[0]) out->count++;
        }
        p = ce;
    }
    char pat[24];
    snprintf(pat, sizeof pat, "page=%d", page + 1);
    out->more = has_next(html, end, pat);
}

static BOOL bk_detail_url(const StoreItem *it, wchar_t *url, size_t cap)
{
    str_printf(url, cap, BK L"/wallpaper/%hs", it->ref);
    return TRUE;
}

/* The sizes its page lists (…/static/wallpapers/<W>x<H>/<image>.jpg, 1280 wide and up) that exist: every page
 * lists the same 1080p to 8K set, but only those up to the original (its JSON-LD "width"/"height") are files.
 * Without that, the size its own Download button fetches (data-file-url). */
static int bk_detail(const char *html, int len, StoreItem *it)
{
    const char *end = html + len, *ow = after(html, end, "\"width\":"), *oh = ow ? after(ow, SP_MIN(end, ow + 24), "\"height\":") : NULL;
    const char *fu = after(html, end, "data-file-url=\"https://backiee.com/static/wallpapers/");
    int maxW = 0, maxH = 0;
    if (oh) {
        maxW = number(&ow, end);
        maxH = number(&oh, end);
    } else if (fu) {
        res_at(fu, end, &maxW, &maxH);
    }
    char tail[48];
    snprintf(tail, sizeof tail, "/%s.jpg", it->media);
    size_t tn = strlen(tail);
    it->varCount = 0;
    for (const char *p = after(html, end, "/static/wallpapers/"); p && it->varCount < STORE_VARIANTS; p = after(p, end, "/static/wallpapers/")) {
        int w = 0, h = 0, dup = 0;
        const char *slash = until(p, SP_MIN(end, p + 12), '/');
        if (!slash || end - slash < (ptrdiff_t)tn || memcmp(slash, tail, tn) || !res_at(p, slash, &w, &h) || w < 1280) continue;
        if (!maxW || w > maxW || h > maxH) continue;
        for (int i = 0; i < it->varCount; i++) dup |= it->var[i].w == w && it->var[i].h == h;
        if (dup) continue;
        StoreVariant *v = &it->var[it->varCount++];
        ZeroMemory(v, sizeof *v);
        snprintf(v->key, sizeof v->key, "%dx%d", w, h);
        v->w = w;
        v->h = h;
    }
    return it->varCount > 0;
}

static void bk_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    (void)big;
    str_printf(url, cap, BK L"/static/wallpapers/456x257/%hs%ls", it->media, webp ? L".webp" : L".jpg");
}

static BOOL bk_file(const StoreItem *it, const StoreVariant *v, wchar_t *url, size_t cap)
{
    str_printf(url, cap, BK L"/static/wallpapers/%hs/%hs.jpg", v->key, it->media);
    return TRUE;
}

static void bk_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, BK L"/wallpaper/%hs", it->ref); }

/* ---------------------------------------------------------------- Wallhaven (wallhaven.cc/help/api): safe for work only */

#define WH L"https://wallhaven.cc"
static const char *const k_whSort[SQ_COUNT] = { NULL, "date_added", "favorites", "views" };

static BOOL wh_list(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap)
{
    wchar_t w[100], e[400], least[40] = L"";
    const char *sort = k_whSort[q->sort] ? k_whSort[q->sort] : q->text[0] ? "relevance" : "toplist";
    const char *cats = q->category == CAT_ANIME ? "010" : q->category ? "100" : "110";   /* general / anime, no people */
    (void)tag;
    words(q, q->category == CAT_ANIME ? "anime" : NULL, w, SP_COUNTOF(w));
    encode(w, e, SP_COUNTOF(e));
    if (q->res) str_printf(least, SP_COUNTOF(least), L"&atleast=%dx%d", k_resMin[q->res][0], k_resMin[q->res][1]);
    str_printf(url, cap, WH L"/api/v1/search?q=%ls&categories=%hs&purity=100&sorting=%hs&order=desc&ratios=landscape%ls&page=%d",
               e, cats, sort, least, page);
    return TRUE;
}

/* {"data":[{"id","purity","dimension_x","dimension_y","file_size","file_type","thumbs":{"original"}}…],
 *  "meta":{"current_page","last_page"}}; ref: id, media: the thumbnail's address, extra: jpg / png */
static void wh_parse(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out)
{
    JVal *root = json_parse(html, (size_t)len);
    const JVal *data = json_get(root, "data"), *meta = json_get(root, "meta");
    (void)q;
    (void)page;
    (void)tag;
    for (int i = 0; data && data->t == J_ARR && i < data->n; i++) {
        const JVal *d = &data->kids[i];
        const char *id = json_str(json_get(d, "id"), ""), *thumb = json_str(json_get(json_get(d, "thumbs"), "original"), "");
        if (!id[0] || strlen(id) > 16 || strncmp(thumb, "https://", 8) || strcmp(json_str(json_get(d, "purity"), ""), "sfw")) continue;
        StoreItem *it = add(out);
        if (!it) break;
        strcpy_s(it->ref, sizeof it->ref, id);
        copy_of(thumb, thumb + strlen(thumb), it->media, sizeof it->media);
        strcpy_s(it->extra, sizeof it->extra, strstr(json_str(json_get(d, "file_type"), ""), "png") ? "png" : "jpg");
        it->w = (int)json_num(json_get(d, "dimension_x"), 0);
        it->h = (int)json_num(json_get(d, "dimension_y"), 0);
        str_printf(it->title, SP_COUNTOF(it->title), L"Wallhaven %hs", id);
        strcpy_s(it->var[0].key, sizeof it->var[0].key, "full");
        it->var[0].w = it->w;
        it->var[0].h = it->h;
        it->var[0].mb = (float)(json_num(json_get(d, "file_size"), 0) / 1048576.0);
        it->varCount = 1;
        it->kind = SKIND_STILL;
        out->count++;
    }
    out->more = meta && json_num(json_get(meta, "current_page"), 0) < json_num(json_get(meta, "last_page"), 0);
    json_free(root);
}

static void wh_preview(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap)
{
    (void)big;
    (void)webp;
    str_printf(url, cap, L"%hs", it->media);
}

static BOOL wh_file(const StoreItem *it, const StoreVariant *v, wchar_t *url, size_t cap)
{
    (void)v;
    str_printf(url, cap, L"https://w.wallhaven.cc/full/%.2hs/wallhaven-%hs.%hs", it->ref, it->ref, it->extra);
    return TRUE;
}

static void wh_page(const StoreItem *it, wchar_t *url, size_t cap) { str_printf(url, cap, WH L"/w/%hs", it->ref); }

/* ---------------------------------------------------------------- the table (feed order: one card from each in turn) */

const StoreSource k_sources[] = {
    { L"MotionBGS", SKIND_LIVE, SR_4K, 0, mb_list, mb_parse, mb_detail_url, mb_detail, mb_preview, mb_file, mb_page },
    { L"Lively", SKIND_LIVE, SR_4K, 1, lw_list, lw_parse, NULL, NULL, lw_preview, lw_file, lw_page },
    { L"MoeWalls", SKIND_LIVE, SR_4K, 0, mw_list, mw_parse, NULL, NULL, mw_preview, NULL, mw_page },
    { L"wallsflow", SKIND_LIVE, SR_4K, 0, wf_list, wf_parse, wf_detail_url, wf_detail, wf_preview, NULL, wf_page },
    { L"backiee", SKIND_STILL, SR_8K, 0, bk_list, bk_parse, bk_detail_url, bk_detail, bk_preview, bk_file, bk_page },
    { L"Wallhaven", SKIND_STILL, SR_8K, 0, wh_list, wh_parse, NULL, NULL, wh_preview, wh_file, wh_page },
};
const int k_sourceCount = (int)SP_COUNTOF(k_sources);
