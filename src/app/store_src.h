/* The Store's sites as store.c drives them (store_sources.c, docs/store/sources.md): per site, the address of a
 * listing or search page for the filters, a parser for its cards and one for its wallpaper page, and where its
 * previews and files are. Parsers run on worker threads over untrusted pages: they bound every copy and never log. */
#pragma once
#include "app.h"

enum { CAT_ALL = 0, CAT_ANIME, CAT_GAMES, CAT_NATURE, CAT_CARS, CAT_SPACE, CAT_ANIMALS, CAT_FANTASY, CAT_CITY,
       CAT_ABSTRACT, CAT_MOVIES, CAT_SPORT, CAT_TECH, CAT_MINIMAL, CAT_COUNT };
enum { SR_ANY = 0, SR_HD, SR_2K, SR_4K, SR_5K, SR_8K, SR_COUNT };           /* resolution: at least */
enum { SQ_BEST = 0, SQ_LATEST, SQ_POPULAR, SQ_DOWNLOADS, SQ_COUNT };        /* sort */

typedef struct StoreQuery {
    wchar_t text[96];
    int     category, res, sort;
} StoreQuery;

typedef struct StoreList {
    StoreItem *items;
    int        count, cap;
    int        more;                    /* the site has another page */
    char       tag[96];                 /* MotionBGS: its search became this tag's listing */
} StoreList;

typedef struct StoreSource {
    const wchar_t *name;
    int            kind;                /* SKIND_LIVE or SKIND_STILL */
    int            maxRes;              /* SR_*: the most it offers; higher filters skip the site */
    int            scans;               /* its search filters its listing's cards: a page without a match isn't the end */
    /* page `page` (1-based) of what the filters ask for; FALSE: the site has nothing for them */
    BOOL (*list_url)(const StoreQuery *q, int page, const char *tag, wchar_t *url, size_t cap);
    void (*parse_list)(const StoreQuery *q, int page, const char *tag, const char *html, int len, StoreList *out);
    BOOL (*detail_url)(const StoreItem *it, wchar_t *url, size_t cap);     /* NULL: the card says all there is */
    int  (*parse_detail)(const char *html, int len, StoreItem *it);        /* 1: it found the files */
    void (*preview_url)(const StoreItem *it, int big, int webp, wchar_t *url, size_t cap);
    /* NULL: the site hands files out only on its own page (a captcha, a download limit, an encrypted link),
     * so Download opens that page instead */
    BOOL (*file_url)(const StoreItem *it, const StoreVariant *v, wchar_t *url, size_t cap);
    void (*page_url)(const StoreItem *it, wchar_t *url, size_t cap);
} StoreSource;

extern const StoreSource k_sources[];
extern const int         k_sourceCount;

typedef struct StoreCategory {
    const wchar_t *name;
    const char    *word;                /* searched for on a site without such a category */
} StoreCategory;
extern const StoreCategory  k_categories[CAT_COUNT];
extern const wchar_t *const k_resNames[SR_COUNT];
extern const int            k_resMin[SR_COUNT][2];
extern const wchar_t *const k_sortNames[SQ_COUNT];

void store_badge_res(const wchar_t *badge, int *w, int *h);              /* "4K" -> 3840 x 2160; 0 unknown */
