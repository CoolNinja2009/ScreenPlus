#pragma once
#include <d2d1.h>
#include "dwrite_c.h"

/* Single-line text engine behind every field the user can type into (search box, link, card name,
 * Customise text and value fields). One control is active at a time; the buffered text lives in the
 * caller's buffer, the engine adds a caret, an anchored selection, word navigation, undo/redo,
 * clipboard actions and a x <-> caret position mapping for mouse editing. */

/* accepts may rewrite the character (a decimal comma becomes a dot) and returns 1 to allow it. */
void te_open(wchar_t *buf, size_t cap, int caret, int selectAll, int (*accepts)(wchar_t *), void (*notify)(void));
void te_close(void);                                  /* stop editing; the caller commits its own buffer */
int  te_active(void);

int     te_len(void);
int     te_caret(void);
int     te_has_sel(void);
void    te_clear_sel(void);
void    te_sel_all(void);
wchar_t te_char_at(int i);                            /* watcher/debug aid: the character under caret */

int  te_key(UINT vk, int ctrl, int shift);            /* 1 consumed */
int  te_char(wchar_t ch);                             /* 1 consumed (printable, backspace, Ctrl+V via WM_CHAR) */

void te_set_layout(IDWriteTextFormat *f, D2D1_RECT_F r);      /* geometry of the text being drawn, each paint */
void te_click(int clicks, float x);                   /* 1 caret, 2 word, 3+ all; x is window-relative */
void te_drag(float x);                                /* extend the selection to x */
void te_draw(D2D1_RECT_F r, D2D1_COLOR_F textCol, D2D1_COLOR_F selCol, D2D1_COLOR_F caretCol, int caretOn);
