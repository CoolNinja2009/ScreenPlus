/* UI languages: English text in, translated text out.
 *
 * resources/lang/<code>.txt (UTF-8) holds one "English<TAB>translation" pair a line, "@name<TAB>its own name" first,
 * '#' comments; "\n", "\t" and "\\" are escapes. cmake/lang.cmake packs the files into lang_data.c. A language's
 * table is built the first time it is chosen and kept for the life of the process, so tr() needs no lock: switching
 * only swaps the index. Lookups hash the text (the UI asks again every frame, and the cost is a few hundred bytes of
 * hashing). Text that has no translation, including anything already built from a translated format, shows as is. */
#include "i18n.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct SpLangData { const char *code; const char *text; };
extern const struct SpLangData sp_langs[];

#define LANG_MAX 32

typedef struct Pair { const wchar_t *en, *tx; } Pair;
typedef struct Table {
    wchar_t  code[16];
    wchar_t *name;
    Pair    *slots;     /* open addressing, mask + 1 slots */
    uint32_t mask;
    int      built;
} Table;

static Table g_tab[LANG_MAX];
static int   g_count = -1;
static volatile int g_cur = -1;

static void count_langs(void)
{
    if (g_count >= 0) return;
    int n = 0;
    while (n < LANG_MAX && sp_langs[n].code) {
        wchar_t *w = wide_from_utf8(sp_langs[n].code, -1);
        if (w) { str_copy(g_tab[n].code, SP_COUNTOF(g_tab[n].code), w); free(w); }
        n++;
    }
    g_count = n;
}

/* In-place unescape of \n, \t and \\. */
static void unescape(wchar_t *s)
{
    wchar_t *d = s;
    for (; *s; s++) {
        if (*s == L'\\' && s[1]) {
            s++;
            *d++ = *s == L'n' ? L'\n' : *s == L't' ? L'\t' : *s;
        } else *d++ = *s;
    }
    *d = 0;
}

static void put(Table *t, const wchar_t *en, const wchar_t *tx)
{
    uint32_t i = hash_fnv1a_w(en) & t->mask;
    while (t->slots[i].en && wcscmp(t->slots[i].en, en) != 0) i = (i + 1) & t->mask;
    t->slots[i].en = en;
    t->slots[i].tx = tx;
}

static void build(int idx)
{
    Table *t = &g_tab[idx];
    if (t->built) return;
    wchar_t *all = wide_from_utf8(sp_langs[idx].text, -1);   /* kept: the pairs point into it */
    if (!all) return;
    int lines = 1;
    for (wchar_t *p = all; *p; p++) if (*p == L'\n') lines++;
    uint32_t cap = 64;
    while (cap < (uint32_t)lines * 2) cap <<= 1;
    t->slots = (Pair *)calloc(cap, sizeof(Pair));
    if (!t->slots) { free(all); return; }
    t->mask = cap - 1;
    for (wchar_t *line = all; line && *line;) {
        wchar_t *next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        size_t len = wcslen(line);
        if (len && line[len - 1] == L'\r') line[--len] = 0;
        wchar_t *tab = line[0] != L'#' ? wcschr(line, L'\t') : NULL;
        if (tab && tab != line && tab[1]) {
            *tab = 0;
            unescape(line);
            unescape(tab + 1);
            if (wcscmp(line, L"@name") == 0) t->name = tab + 1;
            else put(t, line, tab + 1);
        }
        line = next;
    }
    t->built = 1;
}

static int find_code(const wchar_t *code)
{
    count_langs();
    wchar_t base[16];
    str_copy(base, SP_COUNTOF(base), code);
    for (wchar_t *p = base; *p; p++) if (*p == L'_') *p = L'-';
    for (int i = 0; i < g_count; i++) if (str_ieq(g_tab[i].code, base)) return i;
    wchar_t *dash = wcschr(base, L'-');
    if (dash) *dash = 0;
    for (int i = 0; i < g_count; i++) if (str_ieq(g_tab[i].code, base)) return i;
    return -1;
}

static void system_code(wchar_t *out, size_t cap)
{
    out[0] = 0;
#ifdef _WIN32
    LANGID lang = GetUserDefaultUILanguage();
    if (!LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), out, (int)cap, 0)) out[0] = 0;
#else
    const char *v = getenv("LC_ALL");
    if (!v || !v[0]) v = getenv("LC_MESSAGES");
    if (!v || !v[0]) v = getenv("LANG");
    if (v) {
        wchar_t *w = wide_from_utf8(v, -1);
        if (w) { str_copy(out, cap, w); free(w); }
        wchar_t *dot = wcschr(out, L'.');
        if (dot) *dot = 0;
    }
#endif
}

void i18n_set(const wchar_t *code)
{
    wchar_t sys[96];
    if (!code || !code[0] || str_ieq(code, L"system")) {
        system_code(sys, SP_COUNTOF(sys));
        code = sys;
    }
    int idx = str_ieq(code, L"en") ? -1 : find_code(code);
    if (idx >= 0) build(idx);
    g_cur = idx >= 0 && g_tab[idx].built ? idx : -1;
}

const wchar_t *tr(const wchar_t *en)
{
    int cur = g_cur;
    if (cur < 0 || !en || !en[0]) return en;
    const Table *t = &g_tab[cur];
    uint32_t i = hash_fnv1a_w(en) & t->mask;
    for (; t->slots[i].en; i = (i + 1) & t->mask)
        if (wcscmp(t->slots[i].en, en) == 0) return t->slots[i].tx;
    return en;
}

int i18n_count(void) { count_langs(); return g_count; }
const wchar_t *i18n_code(int i) { count_langs(); return i >= 0 && i < g_count ? g_tab[i].code : L"en"; }
int i18n_current(void) { return g_cur; }

const wchar_t *i18n_name(int i)
{
    count_langs();
    if (i < 0 || i >= g_count) return L"English";
    build(i);
    return g_tab[i].name ? g_tab[i].name : g_tab[i].code;
}
