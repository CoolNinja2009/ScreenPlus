/* The Linux window behind the management UI: SDL3 for the window and input, Cairo (d2d_cairo.c) for the frame.
 * SDL events become the window messages ui_core.c's ui_proc already understands. It runs inside the service, on its
 * thread: the service calls ui_pump() from its loop while the window is open. */
#include "../ui/ui_internal.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include <SDL3/SDL.h>
#include <cairo.h>
#include <pango/pangocairo.h>

LRESULT ui_dispatch(HWND h, UINT msg, WPARAM wp, LPARAM lp);     /* ui_core.c */

#define MAX_TIMERS 8
#define MAX_POSTED 256

typedef struct Timer { UINT_PTR id; UINT ms; double next; } Timer;
typedef struct Posted { UINT msg; WPARAM wp; LPARAM lp; } Posted;

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static int           g_texW, g_texH, g_dirty;
static Timer         g_timers[MAX_TIMERS];
static Posted        g_posted[MAX_POSTED];
static int           g_postedCount;
static SDL_Mutex    *g_lock;
static SDL_Cursor   *g_cursors[4];
static SDL_Cursor   *g_cursorNow;
static SpDrop        g_drop;

/* ------------------------------------------------------------------ USER32 stand-ins */

void InvalidateRect(HWND h, void *r, BOOL erase) { (void)h; (void)r; (void)erase; g_dirty = 1; }

UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn)
{
    (void)h; (void)fn;
    Timer *slot = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].id == id) { slot = &g_timers[i]; break; }
        if (!g_timers[i].id && !slot) slot = &g_timers[i];
    }
    if (!slot) return 0;
    slot->id = id;
    slot->ms = ms;
    slot->next = time_ms() + ms;
    return id;
}

BOOL KillTimer(HWND h, UINT_PTR id)
{
    (void)h;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].id == id) g_timers[i].id = 0;
    return TRUE;
}

short GetKeyState(int vk)
{
    SDL_Keymod m = SDL_GetModState();
    if (vk == VK_CONTROL) return (m & SDL_KMOD_CTRL) ? -1 : 0;
    if (vk == VK_SHIFT) return (m & SDL_KMOD_SHIFT) ? -1 : 0;
    return 0;
}

DWORD GetTickCount(void)       { return (DWORD)SDL_GetTicks(); }
UINT  GetDoubleClickTime(void) { return 500; }
UINT  GetCaretBlinkTime(void)  { return 530; }
HWND  GetForegroundWindow(void) { return g_win && (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) ? (HWND)g_win : NULL; }
BOOL  ClientToScreen(HWND h, POINT *p) { (void)h; (void)p; return TRUE; }   /* menus are drawn in the window, in its pixels */

BOOL GetClientRect(HWND h, RECT *r)
{
    int w = 0, ht = 0;
    SDL_GetWindowSizeInPixels((SDL_Window *)h, &w, &ht);
    r->left = r->top = 0;
    r->right = w;
    r->bottom = ht;
    return TRUE;
}

void SetCursor(LPCWSTR c)
{
    int i = (int)(uintptr_t)c;
    if (i < 0 || i > 3 || !g_cursors[i] || g_cursorNow == g_cursors[i]) return;
    SDL_SetCursor(g_cursors[i]);
    g_cursorNow = g_cursors[i];
}

/* Any thread: the thumbnail worker reports here. */
BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)h;
    BOOL ok = FALSE;
    SDL_LockMutex(g_lock);
    if (g_postedCount < MAX_POSTED) {
        g_posted[g_postedCount++] = (Posted){ msg, wp, lp };
        ok = TRUE;
    }
    SDL_UnlockMutex(g_lock);
    return ok;
}

UINT DragQueryFileW(HDROP d, UINT i, wchar_t *out, UINT cap)
{
    if (i == 0xFFFFFFFF) return (UINT)d->count;
    if (i >= (UINT)d->count) return 0;
    if (out) str_copy(out, cap, d->paths[i]);
    return (UINT)wcslen(d->paths[i]);
}

int MessageBoxW(HWND h, const wchar_t *text, const wchar_t *title, UINT type)
{
    (void)h;
    char *t = utf8_from_wide(text, NULL), *c = utf8_from_wide(title, NULL);
    SDL_MessageBoxButtonData b[2] = { { 0, IDNO, "No" }, { 0, IDYES, "Yes" } };
    SDL_MessageBoxData d = { SDL_MESSAGEBOX_WARNING, g_win, c, t, (type & MB_YESNO) ? 2 : 1, (type & MB_YESNO) ? b : b + 1, NULL };
    int hit = IDNO;
    SDL_ShowMessageBox(&d, &hit);
    free(t);
    free(c);
    return hit;
}

wchar_t *sp_clipboard_get(void)
{
    char *u = SDL_GetClipboardText();
    wchar_t *w = u ? wide_from_utf8(u, -1) : NULL;
    SDL_free(u);
    return w;
}

void sp_clipboard_set(const wchar_t *s)
{
    char *u = utf8_from_wide(s, NULL);
    if (u) SDL_SetClipboardText(u);
    free(u);
}

void sp_shell_open(const wchar_t *target)
{
    char *u = wallpaper_is_url(target) ? utf8_from_wide(target, NULL) : sp_path_utf8(target);
    if (!u) return;
    if (u[0] == '/') {
        char *uri = NULL;
        if (SDL_asprintf(&uri, "file://%s", u) > 0) SDL_OpenURL(uri);
        SDL_free(uri);
    } else {
        SDL_OpenURL(u);
    }
    free(u);
}

void shell_open(const wchar_t *target) { sp_shell_open(target); }

void shell_reveal(const wchar_t *path)
{
    wchar_t dir[SP_MAX_PATH];
    path_dirname(dir, SP_COUNTOF(dir), path);
    sp_shell_open(dir);
}

/* ------------------------------------------------------------------ popup menus */

typedef struct MenuItem { UINT flags; UINT_PTR id; char *text; } MenuItem;
typedef struct Menu { MenuItem *items; int count; } Menu;

HMENU CreatePopupMenu(void) { return calloc(1, sizeof(Menu)); }

BOOL AppendMenuW(HMENU h, UINT flags, UINT_PTR id, const wchar_t *text)
{
    Menu *m = (Menu *)h;
    MenuItem *p = (MenuItem *)realloc(m->items, sizeof(MenuItem) * (size_t)(m->count + 1));
    if (!p) return FALSE;
    m->items = p;
    p[m->count++] = (MenuItem){ flags, id, text ? utf8_from_wide(text, NULL) : NULL };
    return TRUE;
}

BOOL DestroyMenu(HMENU h)
{
    Menu *m = (Menu *)h;
    for (int i = 0; m && i < m->count; i++) free(m->items[i].text);
    if (m) free(m->items);
    free(m);
    return TRUE;
}

static void present(void);

#define MENU_ROW 30.0
#define MENU_SEP 9.0
#define MENU_PAD 6.0

static double menu_item_y(const Menu *m, int i)
{
    double y = MENU_PAD;
    for (int k = 0; k < i; k++) y += (m->items[k].flags & MF_SEPARATOR) ? MENU_SEP : MENU_ROW;
    return y;
}

/* Modal, like the Win32 call: draws over the current frame until something is chosen. */
int TrackPopupMenuEx(HMENU h, UINT flags, int x, int y, HWND hwnd, void *params)
{
    (void)flags; (void)hwnd; (void)params;
    Menu *m = (Menu *)h;
    Ui *u = g_ui;
    if (!m || !m->count || !u) return 0;
    int fw, fh, stride;
    const void *frame = d2d_frame((ID2D1RenderTarget *)u->rt, &fw, &fh, &stride);
    cairo_surface_t *back = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
    memcpy(cairo_image_surface_get_data(back), frame, (size_t)stride * fh);
    cairo_surface_mark_dirty(back);

    double s = u->scale, menuH = menu_item_y(m, m->count) + MENU_PAD, menuW = 180;
    cairo_t *probe = cairo_create(back);
    PangoLayout *l = pango_cairo_create_layout(probe);
    PangoFontDescription *fd = pango_font_description_from_string("Inter,Noto Sans,sans-serif");
    pango_font_description_set_absolute_size(fd, 13.5 * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    for (int i = 0; i < m->count; i++) {
        if (!m->items[i].text) continue;
        int w, ht;
        pango_layout_set_text(l, m->items[i].text, -1);
        pango_layout_get_pixel_size(l, &w, &ht);
        if (w + 56 > menuW) menuW = w + 56;
    }
    double px = x / s, py = y / s;
    if (px + menuW > fw / s) px = fw / s - menuW - 6;
    if (py + menuH > fh / s) py = fh / s - menuH - 6;
    if (px < 6) px = 6;
    if (py < 6) py = 6;

    int hot = -1, chosen = 0, open = 1, redraw = 1;
    while (open && g_win) {
        if (redraw) {
            cairo_t *cr = cairo_create(cairo_get_target(((cairo_t *)probe)));
            cairo_destroy(cr);
            cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
            cr = cairo_create(out);
            cairo_set_source_surface(cr, back, 0, 0);
            cairo_paint(cr);
            cairo_scale(cr, s, s);
            const Theme *t = &u->t;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
            cairo_rectangle(cr, px + 2, py + 3, menuW, menuH);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, t->dark ? 0.17 : 0.98, t->dark ? 0.17 : 0.98, t->dark ? 0.18 : 0.99, 1);
            cairo_rectangle(cr, px, py, menuW, menuH);
            cairo_fill(cr);
            for (int i = 0; i < m->count; i++) {
                MenuItem *it = &m->items[i];
                double iy = py + menu_item_y(m, i);
                if (it->flags & MF_SEPARATOR) {
                    cairo_set_source_rgba(cr, t->separator.r, t->separator.g, t->separator.b, t->separator.a);
                    cairo_rectangle(cr, px + 8, iy + 4, menuW - 16, 1);
                    cairo_fill(cr);
                    continue;
                }
                if (i == hot && !(it->flags & MF_GRAYED)) {
                    cairo_set_source_rgba(cr, t->accent.r, t->accent.g, t->accent.b, 0.22);
                    cairo_rectangle(cr, px + 4, iy, menuW - 8, MENU_ROW);
                    cairo_fill(cr);
                }
                const D2D1_COLOR_F c = (it->flags & MF_GRAYED) ? t->text3 : t->text;
                cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
                PangoLayout *tl = pango_cairo_create_layout(cr);
                pango_layout_set_font_description(tl, fd);
                pango_layout_set_text(tl, it->text ? it->text : "", -1);
                cairo_move_to(cr, px + 30, iy + 6);
                pango_cairo_show_layout(cr, tl);
                g_object_unref(tl);
                if (it->flags & MF_CHECKED) {
                    cairo_set_line_width(cr, 1.6);
                    cairo_move_to(cr, px + 12, iy + 15);
                    cairo_line_to(cr, px + 16, iy + 19);
                    cairo_line_to(cr, px + 23, iy + 10);
                    cairo_stroke(cr);
                }
            }
            cairo_destroy(cr);
            cairo_surface_flush(out);
            SDL_UpdateTexture(g_tex, NULL, cairo_image_surface_get_data(out), cairo_image_surface_get_stride(out));
            SDL_RenderClear(g_ren);
            SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
            SDL_RenderPresent(g_ren);
            cairo_surface_destroy(out);
            redraw = 0;
        }
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            double mx = e.motion.x * SDL_GetWindowPixelDensity(g_win) / s - px, my = e.motion.y * SDL_GetWindowPixelDensity(g_win) / s - py;
            int was = hot;
            hot = -1;
            for (int i = 0; mx >= 0 && mx < menuW && i < m->count; i++) {
                double iy = menu_item_y(m, i), ih = (m->items[i].flags & MF_SEPARATOR) ? MENU_SEP : MENU_ROW;
                if (my >= iy && my < iy + ih && !(m->items[i].flags & MF_SEPARATOR)) hot = i;
            }
            redraw = hot != was;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) {
                if (hot >= 0 && !(m->items[hot].flags & MF_GRAYED)) chosen = (int)m->items[hot].id;
                if (hot >= 0 || e.button.button == SDL_BUTTON_LEFT) open = 0;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) open = 0;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            open = 0;
            break;
        default:
            break;
        }
    }
    pango_font_description_free(fd);
    g_object_unref(l);
    cairo_destroy(probe);
    cairo_surface_destroy(back);
    g_dirty = 1;
    return chosen;
}

/* ------------------------------------------------------------------ file dialogs */

typedef struct Pick { int done, count; wchar_t **paths; } Pick;

static void SDLCALL pick_done(void *data, const char *const *list, int filter)
{
    (void)filter;
    Pick *p = (Pick *)data;
    int n = 0;
    while (list && list[n]) n++;
    p->paths = n ? (wchar_t **)calloc((size_t)n, sizeof(wchar_t *)) : NULL;
    for (int i = 0; p->paths && i < n; i++) p->paths[p->count++] = wide_from_utf8(list[i], -1);
    p->done = 1;
}

int sp_pick_paths(int folder, wchar_t ***paths)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Wallpapers", "mp4;webm;mkv;mov;avi;gif;jpg;jpeg;png;webp;bmp;html;htm" },
        { "All files", "*" }
    };
    Pick p = { 0 };
    if (folder) SDL_ShowOpenFolderDialog(pick_done, &p, g_win, NULL, false);
    else SDL_ShowOpenFileDialog(pick_done, &p, g_win, filters, 2, NULL, true);
    while (!p.done) {                   /* the dialog is another process: keep the window alive */
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_WINDOW_EXPOSED) g_dirty = 1;
    }
    *paths = p.paths;
    return p.count;
}

void library_import_dialog(HWND owner, BOOL folder)
{
    (void)owner;
    wchar_t **paths = NULL;
    int n = sp_pick_paths(folder, &paths);
    if (n) library_import((const wchar_t *const *)paths, n, TRUE);
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

/* ------------------------------------------------------------------ window */

int ui_plat_dark(void) { return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT; }

HWND sp_window_open(int w, int h, float *scale)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
        SDL_SetHint(SDL_HINT_APP_ID, "org.screenplus.ScreenPlus");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            LOGE(L"ui: SDL_Init failed: %s", SDL_GetError());
            return NULL;
        }
        g_lock = SDL_CreateMutex();
        char *dir = sp_path_utf8(paths_exe_dir()), font[MAX_PATH];
        snprintf(font, sizeof font, "%s/fonts/FluentSystemIcons-Regular.ttf", dir ? dir : ".");
        free(dir);
        d2d_set_icon_font(font);
        g_cursors[0] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        g_cursors[1] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        g_cursors[2] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        g_cursors[3] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    }
    if (!SDL_CreateWindowAndRenderer("Screen+", w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &g_win, &g_ren)) {
        LOGE(L"ui: no window: %s", SDL_GetError());
        return NULL;
    }
    SDL_SetWindowMinimumSize(g_win, 940, 640);
    SDL_StartTextInput(g_win);
    *scale = SDL_GetWindowDisplayScale(g_win);
    if (*scale <= 0.0f) *scale = 1.0f;
    return g_win;
}

BOOL DestroyWindow(HWND h)
{
    (void)h;
    memset(g_timers, 0, sizeof g_timers);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_tex = NULL;
    g_ren = NULL;
    g_win = NULL;
    g_cursorNow = NULL;
    return TRUE;
}

static void present(void)
{
    int w, h, stride;
    if (!g_ui || !g_ui->rt) return;
    const void *px = d2d_frame((ID2D1RenderTarget *)g_ui->rt, &w, &h, &stride);
    if (!g_tex || w != g_texW || h != g_texH) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        g_texW = w;
        g_texH = h;
    }
    SDL_UpdateTexture(g_tex, NULL, px, stride);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

/* ------------------------------------------------------------------ events */

static WPARAM vk_of(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z) return 'A' + (k - SDLK_A);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    switch (k) {
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB:       return VK_TAB;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return VK_RETURN;
    case SDLK_ESCAPE:    return VK_ESCAPE;
    case SDLK_SPACE:     return VK_SPACE;
    case SDLK_PAGEUP:    return VK_PRIOR;
    case SDLK_PAGEDOWN:  return VK_NEXT;
    case SDLK_END:       return VK_END;
    case SDLK_HOME:      return VK_HOME;
    case SDLK_LEFT:      return VK_LEFT;
    case SDLK_UP:        return VK_UP;
    case SDLK_RIGHT:     return VK_RIGHT;
    case SDLK_DOWN:      return VK_DOWN;
    case SDLK_INSERT:    return VK_INSERT;
    case SDLK_DELETE:    return VK_DELETE;
    case SDLK_MENU:      return VK_APPS;
    case SDLK_F2:        return VK_F2;
    case SDLK_F5:        return VK_F5;
    case SDLK_F10:       return VK_F10;
    default:             return 0;
    }
}

static LPARAM mouse_lp(float x, float y)
{
    float d = SDL_GetWindowPixelDensity(g_win);
    return MAKELPARAM((int)(x * d), (int)(y * d));
}

static void drop_clear(void)
{
    for (int i = 0; i < g_drop.count; i++) free(g_drop.paths[i]);
    free(g_drop.paths);
    memset(&g_drop, 0, sizeof g_drop);
}

static void on_event(const SDL_Event *e)
{
    HWND h = g_win;
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        ui_dispatch(h, WM_CLOSE, 0, 0);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        ui_dispatch(h, WM_SIZE, 0, MAKELPARAM(e->window.data1, e->window.data2));
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_dirty = 1;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        ui_dispatch(h, WM_MOUSELEAVE, 0, 0);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        ui_dispatch(h, WM_MOUSEMOVE, 0, mouse_lp(e->motion.x, e->motion.y));
        ui_dispatch(h, WM_SETCURSOR, 0, HTCLIENT);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT) break;
        ui_dispatch(h, e->button.clicks == 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, 0, mouse_lp(e->button.x, e->button.y));
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) ui_dispatch(h, WM_LBUTTONUP, 0, mouse_lp(e->button.x, e->button.y));
        else if (e->button.button == SDL_BUTTON_RIGHT) ui_dispatch(h, WM_RBUTTONUP, 0, mouse_lp(e->button.x, e->button.y));
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        ui_dispatch(h, WM_MOUSEWHEEL, (WPARAM)((uint32_t)(WORD)(short)(e->wheel.y * WHEEL_DELTA) << 16), 0);
        break;
    case SDL_EVENT_KEY_DOWN: {
        WPARAM vk = vk_of(e->key.key);
        if (vk) ui_dispatch(h, WM_KEYDOWN, vk, 0);
        /* Windows also sends these as characters; the text engine reads Backspace and Ctrl+letter from WM_CHAR */
        if (vk == VK_BACK || vk == VK_RETURN || vk == VK_ESCAPE) ui_dispatch(h, WM_CHAR, vk, 0);
        else if ((e->key.mod & SDL_KMOD_CTRL) && vk >= 'A' && vk <= 'Z') ui_dispatch(h, WM_CHAR, vk - 'A' + 1, 0);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        wchar_t *w = wide_from_utf8(e->text.text, -1);
        for (wchar_t *c = w; c && *c; c++) ui_dispatch(h, WM_CHAR, (WPARAM)*c, 0);
        free(w);
        break;
    }
    case SDL_EVENT_DROP_FILE: {
        wchar_t **p = (wchar_t **)realloc(g_drop.paths, sizeof(wchar_t *) * (size_t)(g_drop.count + 1));
        if (p) {
            g_drop.paths = p;
            g_drop.paths[g_drop.count++] = wide_from_utf8(e->drop.data, -1);
        }
        break;
    }
    case SDL_EVENT_DROP_COMPLETE:
        if (g_drop.count) ui_dispatch(h, WM_DROPFILES, (WPARAM)&g_drop, 0);
        drop_clear();
        break;
    default:
        break;
    }
}

/* Service loop, about every 15 ms while the window is open. */
void ui_pump(void)
{
    SDL_Event e;
    while (g_win && SDL_PollEvent(&e)) on_event(&e);

    Posted q[MAX_POSTED];
    int n = 0;
    if (g_lock) {
        SDL_LockMutex(g_lock);
        n = g_postedCount;
        memcpy(q, g_posted, sizeof(Posted) * (size_t)n);
        g_postedCount = 0;
        SDL_UnlockMutex(g_lock);
    }
    for (int i = 0; i < n; i++) {
        if (g_win) ui_dispatch(g_win, q[i].msg, q[i].wp, q[i].lp);
        else if (q[i].msg == WM_SP_THUMB) thumbs_free((ThumbResult *)q[i].lp);
    }

    double now = time_ms();
    for (int i = 0; g_win && i < MAX_TIMERS; i++) {
        if (!g_timers[i].id || now < g_timers[i].next) continue;
        g_timers[i].next = now + g_timers[i].ms;
        ui_dispatch(g_win, WM_TIMER, g_timers[i].id, 0);
    }
    if (g_win && g_dirty) {
        g_dirty = 0;
        ui_dispatch(g_win, WM_PAINT, 0, 0);
        present();
    }
}
