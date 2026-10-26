#pragma once
#include "../app/app.h"
#include "ui.h"
#include "textedit.h"
#include "thumbs.h"
#include "dwrite_c.h"
#include "d2d_c.h"
#include "../renderer/html/wallpaper_api.h"

#define UI_MAX_HITS 768

/* shared WM_TIMER ids between ui_core and the pages */
enum UiTimer { TM_ANIM = 1, TM_CARET = 2, TM_STATS = 3, TM_THUMBS = 4 };

enum { PAGE_LIBRARY = 0, PAGE_DISPLAYS = 1, PAGE_CUSTOMIZE = 2, PAGE_SETTINGS = 3, PAGE_COUNT = 4 };

enum HitKind {
    HK_NONE = 0, HK_NAV, HK_IMPORT, HK_IMPORT_FOLDER, HK_SEARCH, HK_SEARCH_CLEAR, HK_FILTER, HK_CARD, HK_TARGET,
    HK_PLAYPAUSE, HK_AUDIO, HK_RESTART_CURRENT, HK_REVEAL, HK_TOGGLE, HK_THEME, HK_OPEN_LOGS, HK_OPEN_DATA,
    HK_RESTART_ENGINE, HK_NOTICE_CLOSE, HK_MONITOR, HK_USE_DEFAULT, HK_CHANGE, HK_GET_WEBVIEW, HK_LICENSES,
    HK_SCROLLBAR, HK_SLIDER, HK_DISC, HK_PCHECK, HK_PSLIDER, HK_PDOWN, HK_PCOLOR, HK_PTEXT, HK_RESET_LOOK,
    HK_RESET_PROPS, HK_LOOKVAL, HK_PVAL, HK_URL, HK_URL_TEXT, HK_URL_MODE, HK_URL_GO, HK_URL_CLOSE,
    HK_NAME,    /* library card name; HK_COOL: data 0 make it, 1 back to the plain image, 2 cancel */
    HK_COOL
};

#define HIT_ID(kind, data) ((int)(((kind) << 16) | ((unsigned)(data) & 0xffff)))
#define HIT_KIND(id)       ((id) >> 16)
#define HIT_DATA(id)       ((int)(short)((id) & 0xffff))

enum { TOG_STARTUP, TOG_TRAY, TOG_FULLSCREEN, TOG_COVERED, TOG_LOCK, TOG_BATTERY, TOG_AUDIO, TOG_RECOVER,
       TOG_HTML_MOUSE, TOG_HTML_SPECTRUM, TOG_LIVELY, TOG_URL, TOG_DEPTH_ALL, TOG_COUNT };

typedef struct Hit {
    int         id;
    D2D1_RECT_F r;
    int         focusable;
    int         cursor;    /* 0 arrow, 1 hand, 2 ibeam */
} Hit;

typedef struct Theme {
    int          dark;
    D2D1_COLOR_F bg, tint, sidebar, navActive, panel, panelHover, border, highlight, text, text2, text3,
                 accent, onAccent, success, warning, danger, chip, field, separator, scrim, track, shadow;
} Theme;

typedef struct Thumb {
    wchar_t           id[SP_ID_LEN];
    int               large;
    int               state;   /* 0 none, 1 requested, 2 ready, 3 unavailable */
    int               w, h;        /* stored bitmap size */
    int               reqW, reqH;  /* pixel size last requested */
    ID2D1Bitmap      *bmp;     /* still image */
    ID2D1BitmapBrush *brush;
    ID2D1Bitmap     **frames;  /* animation (frameCount > 1): one bitmap per frame, drawn through brush */
    uint16_t         *delays;  /* ms per frame */
    int               frameCount, cur;
    double            nextAt;  /* time_ms when the next frame is due (0: not started) */
    uint32_t          used;    /* frame counter of last use */
} Thumb;

typedef struct Ui {
    HWND                   hwnd;
    ID2D1Factory          *d2d;
    ID2D1HwndRenderTarget *rt;
    ID2D1SolidColorBrush  *brush;
    IDWriteFactory        *dw;
    IDWriteTextFormat     *fTitle, *fHeading, *fBody, *fBodyB, *fSmall, *fSmallB, *fCaption, *fBrand, *fIcon, *fIconL,
                          *fIconS, *fCenter, *fCenterS, *fWrap, *fWrapC;
    float                  scale;          /* DIP -> px */
    float                  w, h;           /* client size in DIPs */
    int                    backdrop;       /* system backdrop material active */
    Theme                  t;
    float                  alpha;          /* global alpha multiplier (page transitions) */

    int                    page;
    float                  pageAnim;
    float                  scroll[PAGE_COUNT], scrollTarget[PAGE_COUNT], contentH[PAGE_COUNT], viewTop, viewH;

    Hit                    hits[UI_MAX_HITS];
    int                    hitCount;
    D2D1_RECT_F            pressedRect;    /* geometry of the control being dragged */
    int                    hot, pressed, focus, focusVisible, tracking;
    float                  mx, my;

    wchar_t                search[96];
    int                    searchFocus, caretOn;
    int                    filter;         /* 0 all, 1 image, 2 video, 3 html, 4 recent */
    int                    target;         /* -1 all displays, else index into g_app.mons */
    int                    textFocus;      /* hit id of the Customise field being typed into, 0 none */
    int                   *view;
    int                    viewCount, viewCap, cols;
    float                  gridY, rowH;    /* grid origin (content coordinates) and row pitch */
    float                  tog[TOG_COUNT], togTarget[TOG_COUNT];
    double                 showRequestedAt;

    Thumb                 *thumbs;
    int                    thumbCount, thumbCap;
    int                    thumbTimer;     /* animated thumbnails on screen: frame timer running */
    uint32_t               frame;
    int                    animating;
    double                 lastTick;
    int                    statsTimer;
    const wchar_t         *iconFont;
    HWND                   tip;            /* tooltip for icon-only buttons (pages_tooltip) */
    int                    tipId;          /* hit id the tooltip currently describes */
} Ui;

extern Ui *g_ui;

/* ui_core.c — drawing primitives (all coordinates in DIPs) */
D2D1_COLOR_F col(uint32_t rgb, float a);
D2D1_COLOR_F col_a(D2D1_COLOR_F c, float a);
D2D1_RECT_F  rectf(float x, float y, float w, float h);
int          in_rect(D2D1_RECT_F r, float x, float y);
void         ui_fill(D2D1_RECT_F r, D2D1_COLOR_F c);
void         ui_rrect(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
void         ui_rrect_stroke(D2D1_RECT_F r, float radius, float width, D2D1_COLOR_F c);
void         ui_line(float x0, float y0, float x1, float y1, float width, D2D1_COLOR_F c);
void         ui_circle(float cx, float cy, float r, D2D1_COLOR_F c);
void         ui_text(const wchar_t *s, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c);
float        ui_text_width(const wchar_t *s, IDWriteTextFormat *f);
void         ui_icon(wchar_t glyph, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c);
void         ui_glass(D2D1_RECT_F r, float radius, int hover, int elevated);
void         ui_hit(int id, D2D1_RECT_F r, int focusable, int cursor);
int          ui_is_hot(int id);
int          ui_is_down(int id);
void         ui_focus_ring(D2D1_RECT_F r, float radius, int id);
void         ui_push_clip(D2D1_RECT_F r);
void         ui_pop_clip(void);
void         ui_invalidate(void);
void         ui_animate(void);
void         ui_refresh_theme(void);
void         ui_draw_thumb(const WallpaperEntry *e, D2D1_RECT_F r, float radius, int large);
const wchar_t *ui_type_icon(WallpaperType t);

/* widgets */
enum { BTN_SECONDARY = 0, BTN_PRIMARY = 1, BTN_GHOST = 2, BTN_DANGER = 3 };
float        ui_button_width(const wchar_t *label, wchar_t icon);
void         ui_button(int id, D2D1_RECT_F r, const wchar_t *label, wchar_t icon, int style);
void         ui_chip(int id, D2D1_RECT_F r, const wchar_t *label, int active);
void         ui_toggle(int id, float x, float y, int index, int on);        /* index < 0: no animation */
void         ui_pill(D2D1_RECT_F r, const wchar_t *label, D2D1_COLOR_F bg, D2D1_COLOR_F fg);
void         ui_slider(int id, D2D1_RECT_F r, float v);                     /* v 0..1; drag via pages_drag */
void         ui_disc(int id, float cx, float cy, float radius, float hue, float sat);  /* tint colour wheel */
D2D1_COLOR_F ui_hsv(float h, float s, float v, float a);                    /* h in degrees */
void         ui_rgb_hsv(D2D1_COLOR_F c, float *hue, float *s, float *v);

/* ui_pages.c */
void pages_draw(void);
void pages_activate(int id, int dblclick);
void pages_context(int id, int sx, int sy);
int  pages_key(UINT vk, int ctrl, int shift);
void pages_rebuild_view(void);
void pages_scroll_into_view(int id);
void pages_drag(int id, float x, float y, int commit);      /* sliders + colour wheel tracking */
void pages_customise_reload(void);                         /* Customise values changed elsewhere */
void pages_search_open(void);                              /* focus + caret in the search box */
int  pages_text_char(wchar_t ch);                           /* 1 consumed: the Customise text field */
int  pages_text_key(UINT vk, int ctrl, int shift);
void pages_text_end(int commit);                            /* focus left the field (0: cancel) */
const wchar_t *pages_tooltip(int id);                       /* tooltip text for a hit id, or NULL */

/* glyphs (Segoe Fluent Icons / Segoe MDL2 Assets) */
#define GL_LIBRARY  0xE8F1
#define GL_DISPLAY  0xE7F4
#define GL_SETTINGS 0xE713
#define GL_ADD      0xE710
#define GL_FOLDER   0xE8B7
#define GL_LINK     0xE71B
#define GL_PLAY     0xE768
#define GL_PAUSE    0xE769
#define GL_VOLUME   0xE767
#define GL_MUTE     0xE74F
#define GL_REFRESH  0xE72C
#define GL_SEARCH   0xE721
#define GL_CLOSE    0xE711
#define GL_CHECK    0xE73E
#define GL_PHOTO    0xEB9F
#define GL_VIDEO    0xE714
#define GL_CODE     0xE943
#define GL_WARNING  0xE7BA
#define GL_INFO     0xE946
#define GL_ERROR    0xEA39
#define GL_OPEN     0xE8A7
#define GL_DOC      0xE8A5
#define GL_SPEED    0xEC4A
#define GL_COLOR    0xE2B1     /* palette glyph for the Customise page */
