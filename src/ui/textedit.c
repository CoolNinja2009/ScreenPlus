/* textedit.c — the single-line text engine every typed field shares.
 *
 * One control edits at a time: ui_pages opens one with te_open (buffer included), the field draws
 * through te_draw (which also records the geometry the mouse uses), and input lands in te_key /
 * te_char. Everything is in DIPs and wchar indices; the engine knows nothing about controls.
 */
#include "ui_internal.h"
#include "../common/util.h"
#include "textedit.h"
#include <wchar.h>

#define TE_SNAP   1024                  /* undo snapshots keep the whole buffer (longest: the link field) */
#define TE_UNDO   32

typedef struct TeSnap { wchar_t s[TE_SNAP]; int caret, anchor; } TeSnap;

typedef struct Te {
    int       active;
    wchar_t  *buf;
    size_t    cap;
    int       len, caret, anchor;
    int     (*accepts)(wchar_t *);
    void    (*notify)(void);
    IDWriteTextFormat *fmt;               /* current text format, recorded by the drawing field */
    D2D1_RECT_F dr;                       /* rect the text is drawn into */
    float     shift;                      /* horizontal scroll of the content, kept between paints */
    int       layoutSet;
    int       pendClicks; float pendX;    /* a click that arrived before the field was ever drawn */
    TeSnap    undo[TE_UNDO], redo[TE_UNDO];
    int       undoN, redoN;
    int       lastOp;                     /* 0 none, 1 insert: typing coalesces into one undo step */
    double    lastAt;
} Te;

static Te te;

static wchar_t te_sbuf[TE_SNAP];          /* clipboard content for paste */
static wchar_t te_fbuf[TE_SNAP];          /* filtered content that actually gets spliced in */

/* ---------------------------------------------------------------- helpers */

static void te_notify(void)
{
    if (te.notify) te.notify();
    ui_invalidate();
}

static void snap_to(TeSnap *d, const wchar_t *s, int len, int caret, int anchor)
{
    int n = (int)SP_MIN(len, TE_SNAP - 1);
    wmemcpy(d->s, s, (size_t)n);
    d->s[n] = 0;
    d->caret = caret;
    d->anchor = anchor;
}

/* Before a mutation: the previous text goes on the undo stack. Typing coalesces into one step. */
static void te_before(int insert)
{
    double now = time_ms();
    if (insert && te.lastOp == 1 && now - te.lastAt < 700.0 && te.undoN > 0) return;
    if (te.undoN < TE_UNDO) {
        snap_to(&te.undo[te.undoN++], te.buf, te.len, te.caret, te.anchor);
        te.redoN = 0;
    }
    te.lastOp = insert;
    te.lastAt = now;
}

static int te_sel_a(void) { return SP_MIN(te.caret, te.anchor); }
static int te_sel_b(void) { return SP_MAX(te.caret, te.anchor); }

/* Removes [a, b); indices move to a. */
static void te_erase(int a, int b)
{
    if (b <= a) return;
    wmemmove(te.buf + a, te.buf + b, (size_t)(te.len - b));
    te.len -= b - a;
    te.buf[te.len] = 0;
    te.caret = te.anchor = a;
}

static void te_get_clip(wchar_t *out, size_t cap);

static void te_insert_filtered(const wchar_t *in)   /* replaces the selection after filtering for the field */
{
    size_t n = 0;
    int room = (int)te.cap - 1 - (te.len - (te_sel_b() - te_sel_a()));
    for (size_t i = 0; in[i] && n < (size_t)SP_MAX(room, 0) && n + 1 < SP_COUNTOF(te_fbuf); i++) {
        wchar_t c = in[i];
        if (c < 32) c = L' ';                 /* control chars and line breaks become spaces */
        if (te.accepts && !te.accepts(&c)) continue;
        te_fbuf[n++] = c;
    }
    te_fbuf[n] = 0;
    if (!n) {
        te.caret = te.anchor = te_sel_a();    /* nothing survivable: the selection just collapses */
        te_notify();
        return;
    }
    te_before(0);
    int a = te_sel_a();
    te_erase(a, te_sel_b());
    int nIns = (int)n;
    if (te.len + nIns >= (int)te.cap) nIns = (int)te.cap - 1 - te.len;
    if (nIns > 0) {
        wmemmove(te.buf + a + nIns, te.buf + a, (size_t)(te.len - a));
        wmemcpy(te.buf + a, te_fbuf, (size_t)nIns);
        te.len += nIns;
        te.buf[te.len] = 0;
    }
    te.caret = te.anchor = a + nIns;
    te_notify();
}

static void te_paste(void)
{
    te_get_clip(te_sbuf, SP_COUNTOF(te_sbuf));
    te_insert_filtered(te_sbuf);
}

/* ---------------------------------------------------------------- words */

static int te_is_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; }

static int te_word_back(int i)
{
    while (i > 0 && te_is_space(te.buf[i - 1])) i--;
    while (i > 0 && !te_is_space(te.buf[i - 1])) i--;
    return i;
}

static int te_word_fwd(int i)
{
    while (i < te.len && te_is_space(te.buf[i])) i++;
    while (i < te.len && !te_is_space(te.buf[i])) i++;
    return i;
}

/* ---------------------------------------------------------------- navigation */

/* Moves the caret to the start (0) or end (1) of the text; without Shift a selection collapses
 * at the edge the op moves towards (Windows style: Home -> selection start, End -> its end). */
static void te_home_end(int toEnd, int shift)
{
    if (shift) te.caret = toEnd ? te.len : 0;
    else if (te.caret != te.anchor) te.caret = te.anchor = toEnd ? SP_MAX(te.caret, te.anchor) : SP_MIN(te.caret, te.anchor);
    else te.anchor = te.caret = toEnd ? te.len : 0;
    te.lastOp = 0;
}

static void te_move_caret(int to, int shift)   /* to the given index; Shift extends, else anchored there */
{
    to = SP_CLAMP(to, 0, te.len);
    if (shift) te.caret = to;
    else te.anchor = te.caret = to;
    te.lastOp = 0;
}

static void te_nav(int dir, int word, int shift)   /* dir -1 left, +1 right; word steps a screen word */
{
    int from = SP_MIN(te.caret, te.anchor);
    if (!shift && te.caret != te.anchor) {          /* a caret move without Shift collapses the selection */
        te.caret = te.anchor = dir > 0 ? SP_MAX(te.caret, te.anchor) : from;
        te.lastOp = 0;
        return;
    }
    if (dir < 0) te_move_caret(word ? te_word_back(from) : from - 1, shift);
    else te_move_caret(word ? te_word_fwd(SP_MAX(te.caret, te.anchor)) : SP_MAX(te.caret, te.anchor) + 1, shift);
}

/* ---------------------------------------------------------------- clipboard */

static void te_get_clip(wchar_t *out, size_t cap)
{
    out[0] = 0;
    HWND h = g_ui ? g_ui->hwnd : NULL;
    if (!h || !OpenClipboard(h)) return;
    HANDLE d = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *p = d ? (const wchar_t *)GlobalLock(d) : NULL;
    if (p) {
        str_copy(out, cap, p);
        str_sanitize_line(out);
        GlobalUnlock(d);
    }
    CloseClipboard();
}

static void te_set_clip(const wchar_t *s, int n)
{
    if (n <= 0) return;
    HWND h = g_ui ? g_ui->hwnd : NULL;
    if (!h || !OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    wchar_t *p = g ? (wchar_t *)GlobalLock(g) : NULL;
    if (g && p) {
        wmemcpy(p, (const void *)s, (size_t)n);
        p[n] = 0;
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    } else if (g) {
        GlobalFree(g);
    }
    CloseClipboard();
}

static void te_copy_sel(void)
{
    if (te.caret == te.anchor) return;
    int a = te_sel_a(), b = te_sel_b();
    wchar_t tmp[TE_SNAP];
    int n = (int)SP_MIN(b - a, SP_COUNTOF(tmp) - 1);
    wmemcpy(tmp, te.buf + a, (size_t)n);
    tmp[n] = 0;
    te_set_clip(tmp, n);
}

static void te_cut_sel(void)
{
    if (te.caret == te.anchor) return;
    te_copy_sel();
    te_before(0);
    te_erase(te_sel_a(), te_sel_b());
    te_notify();
}

static void te_undo_op(void)
{
    if (!te.undoN || !te.buf) return;
    snap_to(&te.redo[te.redoN % TE_UNDO], te.buf, te.len, te.caret, te.anchor);
    if (te.redoN < TE_UNDO) te.redoN++;
    TeSnap *s = &te.undo[--te.undoN];
    te.len = SP_CLAMP((int)wcslen(s->s), 0, (int)te.cap - 1);
    wmemcpy(te.buf, s->s, (size_t)te.len + 1);
    te.caret = SP_CLAMP(s->caret, 0, te.len);
    te.anchor = SP_CLAMP(s->anchor, 0, te.len);
    te.lastOp = 0;
    te_notify();
}

static void te_redo_op(void)
{
    if (!te.redoN || !te.buf) return;
    snap_to(&te.undo[SP_MIN(te.undoN, TE_UNDO - 1)], te.buf, te.len, te.caret, te.anchor);
    te.undoN = SP_MIN(te.undoN + 1, TE_UNDO);
    TeSnap *s = &te.redo[--te.redoN];
    te.len = SP_CLAMP((int)wcslen(s->s), 0, (int)te.cap - 1);
    wmemcpy(te.buf, s->s, (size_t)te.len + 1);
    te.caret = SP_CLAMP(s->caret, 0, te.len);
    te.anchor = SP_CLAMP(s->anchor, 0, te.len);
    te.lastOp = 0;
    te_notify();
}

/* ---------------------------------------------------------------- text layout */

/* Width of the first n characters in the current field's format (a layout per call; fields are short). */
static float te_span(int n)
{
    if (!te.fmt || !g_ui || !g_ui->dw || n <= 0) return 0.0f;
    IDWriteTextLayout *l = NULL;
    if (FAILED(g_ui->dw->lpVtbl->CreateTextLayout(g_ui->dw, te.buf, (UINT32)n, te.fmt, 100000.0f, 100.0f, &l)) || !l)
        return 0.0f;
    DWRITE_TEXT_METRICS m;
    l->lpVtbl->GetMetrics(l, &m);
    l->lpVtbl->Release(l);
    return m.width;
}

void te_set_layout(IDWriteTextFormat *f, D2D1_RECT_F r)
{
    te.fmt = f;
    te.dr = r;
    te.layoutSet = 1;
}

/* x within the drawn text (already scrolled) -> caret index, picking the nearer character edge. */
static int te_index_at(float x)
{
    float rel = x - te.dr.left + te.shift;
    float full = te_span(te.len);
    if (rel <= 0) return 0;
    if (rel >= full) return te.len;
    int lo = 0, hi = te.len;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (te_span(mid) < rel) lo = mid; else hi = mid;
    }
    float wl = te_span(lo), wr = te_span(hi);
    return rel - wl <= wr - rel ? lo : hi;
}

static void te_click_apply(int clicks, float x)
{
    if (clicks >= 3) {
        te.anchor = 0;
        te.caret = te.len;
    } else if (clicks == 2) {
        int i = te_index_at(x);
        if (i < te.len && !te_is_space(te.buf[i])) {
            te.anchor = te_word_back(i + 1);
            te.caret = te_word_fwd(te.anchor);
        } else {
            te.anchor = te.caret = i;
        }
    } else {
        te.anchor = te.caret = te_index_at(x);
    }
}

void te_click(int clicks, float x)
{
    if (!te.active || !te.layoutSet) {        /* before the field has been painted: remember it */
        te.pendClicks = clicks;
        te.pendX = x;
        return;
    }
    te_click_apply(clicks, x);
    te.lastOp = 0;
    ui_invalidate();
}

void te_drag(float x)
{
    if (!te.active || !te.layoutSet) return;
    int i = te_index_at(x);
    if (i != te.caret) {
        te.caret = SP_CLAMP(i, 0, te.len);
        te.lastOp = 0;
        ui_invalidate();
    }
}

/* ---------------------------------------------------------------- drawing */

void te_draw(D2D1_RECT_F r, D2D1_COLOR_F textCol, D2D1_COLOR_F selCol, D2D1_COLOR_F caretCol, int caretOn)
{
    if (!te.active || !te.fmt) return;
    te.dr = r;
    te.layoutSet = 1;
    if (te.pendClicks) {                      /* a click that waited for this geometry */
        te_click_apply(te.pendClicks, te.pendX);
        te.pendClicks = 0;
    }
    float avail = SP_MAX(r.right - r.left, 1.0f);
    float full = te_span(te.len);
    float a = te_span(te_sel_a()), b = te_span(te_sel_b());
    float caretX = te_span(te.caret);

    if (full > avail) te.shift = SP_CLAMP(SP_MAX(full + 2.0f - avail, 0.0f), 0.0f, full);  /* keep the end in view */
    else te.shift = 0.0f;
    if (caretX - te.shift < 0) te.shift = caretX;
    else if (caretX - te.shift > avail - 1.0f) te.shift = caretX - (avail - 1.0f);
    te.shift = SP_CLAMP(te.shift, 0.0f, SP_MAX(full - avail, 0.0f));

    ui_push_clip(rectf(r.left, r.top - 1.0f, r.right - r.left, (r.bottom - r.top) + 2.0f));
    if (te.caret != te.anchor)
        ui_rrect(rectf(r.left + SP_MIN(a, b) - te.shift, r.top, SP_MAX(b - a, 1.0f) + 2.0f,
                       r.bottom - r.top), 3, selCol);
    ui_text(te.buf, te.fmt, rectf(r.left - te.shift, r.top - 1.0f, full + 4.0f, (r.bottom - r.top) + 2.0f), textCol);
    ui_pop_clip();
    if (te.caret == te.anchor && caretOn) {
        float cx = r.left + caretX - te.shift + 1.0f;
        ui_line(cx, r.top + 2.0f, cx, r.bottom - 2.0f, 1.2f, caretCol);
    }
}

/* ---------------------------------------------------------------- input */

void te_open(wchar_t *buf, size_t cap, int caret, int selectAll, int (*accepts)(wchar_t *), void (*notify)(void))
{
    te.buf = buf;
    te.cap = cap && buf ? cap : 1;
    te.len = buf ? (int)SP_CLAMP(wcslen(buf), 0, (int)cap - 1) : 0;
    te.accepts = accepts;
    te.notify = notify;
    te.caret = selectAll ? te.len : SP_CLAMP(caret, 0, te.len);
    te.anchor = selectAll ? 0 : te.caret;
    te.fmt = NULL;                            /* the drawing field records its own format */
    te.dr = rectf(0, 0, 0, 0);
    te.shift = 0.0f;
    te.layoutSet = 0;
    te.pendClicks = 0;
    te.undoN = te.redoN = 0;
    te.lastOp = 0;
    te.lastAt = 0;
    te.active = buf ? 1 : 0;
}

void te_close(void)
{
    te.active = 0;
    te.buf = NULL;
    te.notify = NULL;
}

int te_active(void) { return te.active; }
int te_len(void) { return te.len; }
int te_caret(void) { return te.caret; }
int te_has_sel(void) { return te.active && te.caret != te.anchor; }
wchar_t te_char_at(int i) { return te.active && i >= 0 && i < te.len ? te.buf[i] : 0; }

void te_clear_sel(void)
{
    if (!te.active || te.caret == te.anchor) return;
    te.caret = te.anchor = SP_MIN(te.caret, te.anchor);
    te.lastOp = 0;
    ui_invalidate();
}

void te_sel_all(void)
{
    if (!te.active) return;
    te.anchor = 0;
    te.caret = te.len;
    ui_invalidate();
}

int te_key(UINT vk, int ctrl, int shift)
{
    if (!te.active) return 0;
    switch (vk) {
    case VK_LEFT:  te_nav(-1, ctrl, shift); ui_invalidate(); return 1;
    case VK_RIGHT: te_nav(1, ctrl, shift); ui_invalidate(); return 1;
    case VK_HOME:  te_home_end(0, shift); ui_invalidate(); return 1;
    case VK_END:   te_home_end(1, shift); ui_invalidate(); return 1;
    case VK_DELETE: {
        int changed = 0;
        if (shift && !ctrl) { te_cut_sel(); return 1; }
        if (te.caret != te.anchor) { te_before(0); te_erase(te_sel_a(), te_sel_b()); changed = 1; }
        else if (te.caret < te.len) {
            te_before(0);
            te_erase(te.caret, ctrl ? te_word_fwd(te.caret) : te.caret + 1);
            changed = 1;
        }
        if (changed) te_notify();
        return 1;
    }
    case VK_INSERT:
        if (ctrl) te_copy_sel();
        else if (shift) te_paste();
        else return 0;
        return 1;
    case 'A':
        if (!ctrl) return 0;
        te_sel_all();
        return 1;
    case 'C':
        if (ctrl) te_copy_sel();
        else return 0;
        return 1;
    case 'X':
        if (ctrl) te_cut_sel();
        else return 0;
        return 1;
    case 'V':
        if (!ctrl) return 0;
        te_paste();
        return 1;
    case 'Z':
        if (!ctrl) return 0;
        if (shift) te_redo_op(); else te_undo_op();
        return 1;
    case 'Y':
        if (ctrl) te_redo_op();
        else return 0;
        return 1;
    default:
        return 0;
    }
}

int te_char(wchar_t ch)
{
    if (!te.active) return 0;
    switch (ch) {
    case 8:                                   /* backspace */
        if (te.caret != te.anchor) { te_before(0); te_erase(te_sel_a(), te_sel_b()); }
        else if (te.caret > 0) { te_before(0); te_erase(te.caret - 1, te.caret); }
        else return 1;
        te_notify();
        return 1;
    case 127:                                 /* Ctrl+Backspace: the previous word */
        if (te.caret != te.anchor) { te_before(0); te_erase(te_sel_a(), te_sel_b()); }
        else if (te.caret > 0) { te_before(0); te_erase(te_word_back(te.caret), te.caret); }
        else return 1;
        te_notify();
        return 1;
    case 3: te_copy_sel(); return 1;          /* Ctrl+C through WM_CHAR, when the key wasn't handled */
    case 24: te_cut_sel(); return 1;
    case 22:
        te_paste();
        return 1;
    default:
        if (ch < 32) return 0;
        {
            wchar_t c = ch;
            if (te.accepts && !te.accepts(&c)) return 1;
            if (te.cap - 1 - (te.len - (te_sel_b() - te_sel_a())) <= 0) return 1;
            te_before(1);
            int a = te_sel_a();
            te_erase(a, te_sel_b());
            wmemmove(te.buf + a + 1, te.buf + a, (size_t)(te.len - a));
            te.buf[a] = c;
            te.len++;
            te.buf[te.len] = 0;
            te.caret = te.anchor = a + 1;
            te_notify();
        }
        return 1;
    }
}
