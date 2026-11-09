/* Wallpaper rotation, shared by the Windows shell and the Linux service:
 *  - Next (tray, CLI, slideshow): the next wallpaper of the slideshow's source ("" every wallpaper, "*" favourites,
 *    else a collection), in library order or at random with Shuffle; an empty source falls back to every wallpaper.
 *  - Slideshow: Next every slideshowMin minutes of the same wallpaper (a wallpaper applied by hand restarts the count);
 *    never while paused.
 *  - Schedule: the day wallpaper from dayStart, the night one from nightStart. It applies when the period changes
 *    (and once at start), so a wallpaper picked by hand stays until the next change; the slideshow rests while it is on. */
#include "app.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../ui/ui.h"
#include <stdlib.h>
#include <time.h>

#define ROTATE_TICK_MS 20000

static int64_t  g_since;            /* when the current default wallpaper was first seen */
static wchar_t  g_seen[SP_ID_LEN];
static int      g_period = -1;      /* 0 day, 1 night, -1 not applied yet */
static uint32_t g_rng;

static uint32_t rnd(void)
{
    if (!g_rng) g_rng = (uint32_t)time(NULL) * 2654435761u | 1u;
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int usable(const WallpaperEntry *e, const wchar_t *from)
{
    return !e->missing && config_in_source(e, from);
}

const wchar_t *rotate_next_id(void)
{
    Config *c = &g_app.cfg;
    if (!c->count) return NULL;
    const wchar_t *from = c->s.slideshowFrom;
    int n = 0;
    for (int i = 0; i < c->count; i++) n += usable(&c->items[i], from);
    if (!n) from = L"";                                  /* nothing in the source: every wallpaper */
    int cur = config_index_of(c, app_wallpaper_for(NULL));
    if (c->s.shuffle) {
        n = 0;
        for (int i = 0; i < c->count; i++) n += i != cur && usable(&c->items[i], from);
        if (!n) return NULL;
        int pick = (int)(rnd() % (uint32_t)n);
        for (int i = 0; i < c->count; i++)
            if (i != cur && usable(&c->items[i], from) && pick-- == 0) return c->items[i].id;
        return NULL;
    }
    for (int k = 1; k <= c->count; k++) {
        const WallpaperEntry *e = &c->items[(cur + k + c->count) % c->count];
        if (usable(e, from) && (k < c->count || cur < 0)) return e->id;
    }
    return NULL;
}

void app_next_wallpaper(void)
{
    const wchar_t *id = rotate_next_id();
    if (id) {
        wchar_t keep[SP_ID_LEN];
        str_copy(keep, SP_COUNTOF(keep), id);
        app_apply_wallpaper(keep, NULL);
    }
}

static int period_now(const Settings *s)
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    int h = lt.tm_hour, d = s->dayStart, nn = s->nightStart;
    if (d == nn) return 0;                   /* the same hour for both: day all the time */
    int day = d <= nn ? (h >= d && h < nn) : (h >= d || h < nn);
    return day ? 0 : 1;
}

static void seen_now(void)               /* our own change: not a hand-picked wallpaper restarting the count */
{
    str_copy(g_seen, SP_COUNTOF(g_seen), app_wallpaper_for(NULL));
}

static void tick(void)
{
    Settings *s = &g_app.cfg.s;
    int64_t now = (int64_t)time(NULL);
    const wchar_t *cur = app_wallpaper_for(NULL);
    if (wcscmp(cur, g_seen) != 0) {
        str_copy(g_seen, SP_COUNTOF(g_seen), cur);
        g_since = now;
    }
    if (s->schedule) {
        int p = period_now(s);
        if (p != g_period) {
            const wchar_t *want = p == 0 ? s->dayWallpaper : s->nightWallpaper;
            g_period = p;
            if (want[0] && config_find(&g_app.cfg, want) && wcscmp(want, cur) != 0) {
                wchar_t keep[SP_ID_LEN];
                str_copy(keep, SP_COUNTOF(keep), want);
                LOGI(L"schedule: %ls wallpaper", p == 0 ? L"day" : L"night");
                app_apply_wallpaper(keep, NULL);
                seen_now();
                ui_on_config_changed();
            }
        }
        return;
    }
    g_period = -1;
    int any = 0;                         /* a collection emptied since: wait rather than show everything */
    for (int i = 0; i < g_app.cfg.count && !any; i++) any = usable(&g_app.cfg.items[i], s->slideshowFrom);
    if (s->slideshow && !s->paused && any && now - g_since >= (int64_t)SP_MAX(s->slideshowMin, 1) * 60) {
        LOGI(L"slideshow: next wallpaper");
        g_since = now;
        app_next_wallpaper();
        seen_now();
        ui_on_config_changed();
    }
}

static int needed(void) { return g_app.cfg.s.slideshow || g_app.cfg.s.schedule; }

void rotate_on_timer(void)
{
    tick();
    const Settings *s = &g_app.cfg.s;
    if (!needed()) {                     /* both off: no timer until one goes on (rotate_changed) */
        KillTimer(g_app.shellWnd, T_SHELL_ROTATE);
        return;
    }
    UINT ms = ROTATE_TICK_MS;
    if (s->slideshow && !s->schedule) {  /* wake when the next change is due, not up to a tick late */
        int64_t left = g_since + (int64_t)SP_MAX(s->slideshowMin, 1) * 60 - (int64_t)time(NULL);
        if (left * 1000 < ms) ms = (UINT)SP_MAX(left, 1) * 1000;
    }
    SetTimer(g_app.shellWnd, T_SHELL_ROTATE, ms, NULL);
}

void rotate_init(void)
{
    g_since = (int64_t)time(NULL);
    str_copy(g_seen, SP_COUNTOF(g_seen), app_wallpaper_for(NULL));
    if (needed()) SetTimer(g_app.shellWnd, T_SHELL_ROTATE, 3000, NULL);
}

/* Play a collection ("*": favourites): a slideshow of just those, starting with one of them now. */
void rotate_play(const wchar_t *from, int minutes)
{
    Settings *s = &g_app.cfg.s;
    str_copy(s->slideshowFrom, SP_COUNTOF(s->slideshowFrom), from ? from : L"");
    if (minutes > 0) s->slideshowMin = SP_CLAMP(minutes, 1, 1440);
    s->slideshow = 1;
    s->schedule = 0;                       /* the schedule would hold the slideshow back */
    const WallpaperEntry *cur = config_find(&g_app.cfg, app_wallpaper_for(NULL));
    if (!cur || !config_in_source(cur, s->slideshowFrom)) app_next_wallpaper();
    else app_save_config();
    rotate_changed();
    seen_now();
    LOGI(L"slideshow: playing %ls every %d min", from && from[0] ? from : L"everything", s->slideshowMin);
}

void rotate_changed(void)
{
    g_period = -1;                       /* a schedule change applies at once */
    g_since = (int64_t)time(NULL);
    str_copy(g_seen, SP_COUNTOF(g_seen), app_wallpaper_for(NULL));
    if (needed()) SetTimer(g_app.shellWnd, T_SHELL_ROTATE, 500, NULL);
    else KillTimer(g_app.shellWnd, T_SHELL_ROTATE);
}
