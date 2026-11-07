/* Window messages, virtual keys and the few USER32 calls the management window uses, for Linux. ui_sdl.c turns SDL
 * events into these messages and feeds them to ui_core.c's own ui_proc, so input handling stays one piece of code. */
#pragma once
#include "../common/common.h"

#define CALLBACK
typedef struct POINT { LONG x, y; } POINT;
typedef void *HMENU, *HGLOBAL;
typedef uintptr_t UINT_PTR;
typedef struct PAINTSTRUCT { int unused; } PAINTSTRUCT;
typedef struct MINMAXINFO { POINT ptMinTrackSize; } MINMAXINFO;
typedef struct SpDrop { int count; wchar_t **paths; } SpDrop, *HDROP;

enum { WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_ERASEBKGND = 0x14, WM_SETTINGCHANGE = 0x1A,
       WM_SETCURSOR = 0x20, WM_GETMINMAXINFO = 0x24, WM_KEYDOWN = 0x100, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
       WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203,
       WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20A, WM_DROPFILES = 0x233, WM_DPICHANGED = 0x2E0, WM_MOUSELEAVE = 0x2A3 };
enum { VK_BACK = 8, VK_TAB = 9, VK_RETURN = 13, VK_SHIFT = 16, VK_CONTROL = 17, VK_ESCAPE = 27, VK_SPACE = 32, VK_PRIOR = 33,
       VK_NEXT = 34, VK_END = 35, VK_HOME = 36, VK_LEFT = 37, VK_UP = 38, VK_RIGHT = 39, VK_DOWN = 40, VK_INSERT = 45,
       VK_DELETE = 46, VK_APPS = 93, VK_F2 = 113, VK_F5 = 116, VK_F10 = 121 };
enum { HTCLIENT = 1, WHEEL_DELTA = 120, CF_UNICODETEXT = 13, SW_SHOW = 5, SW_RESTORE = 9, SW_SHOWNORMAL = 1 };
enum { MF_STRING = 0, MF_GRAYED = 1, MF_CHECKED = 8, MF_SEPARATOR = 0x800, TPM_RETURNCMD = 0x100, TPM_LEFTALIGN = 0,
       TPM_TOPALIGN = 0, TPM_RIGHTBUTTON = 2 };
enum { MB_YESNO = 4, MB_ICONQUESTION = 0x20, MB_ICONWARNING = 0x30, MB_DEFBUTTON2 = 0x100, IDYES = 6, IDNO = 7 };

#define LOWORD(l)         ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l)         ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#define MAKELPARAM(a, b)  ((LPARAM)(((uint32_t)(WORD)(a)) | ((uint32_t)(WORD)(b) << 16)))
#define GET_X_LPARAM(l)   ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l)   ((int)(short)HIWORD(l))
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#define RGB(r, g, b)      ((DWORD)((r) | ((g) << 8) | ((b) << 16)))
#define GetRValue(c)      ((BYTE)(c))
#define GetGValue(c)      ((BYTE)((c) >> 8))
#define GetBValue(c)      ((BYTE)((c) >> 16))

#define IDC_ARROW ((LPCWSTR)(uintptr_t)0)
#define IDC_HAND  ((LPCWSTR)(uintptr_t)1)
#define IDC_IBEAM ((LPCWSTR)(uintptr_t)2)

/* Nothing to do on Linux. */
typedef struct TRACKMOUSEEVENT { DWORD size, flags; HWND h; DWORD hover; } TRACKMOUSEEVENT;
#define TME_LEAVE                        2
#define TrackMouseEvent(t)               ((void)(t))
#define SetWindowTheme(h, a, b)          ((void)0)
#define BeginPaint(h, p)                 ((void)(p))
#define EndPaint(h, p)                   ((void)(p))
#define DefWindowProcW(h, m, w, l)       ((LRESULT)0)
#define SetCapture(h)                    ((void)0)
#define ReleaseCapture()                 ((void)0)
#define DragFinish(d)                    ((void)0)
#define DragAcceptFiles(h, b)            ((void)0)
#define SetForegroundWindow(h)           ((void)0)
#define ShowWindow(h, c)                 ((void)0)
#define UpdateWindow(h)                  ((void)0)
#define IsIconic(h)                      0
#define IsWindowVisible(h)               1
#define LoadCursorW(i, c)                (c)
#define SetWindowPos(h, a, x, y, w, ht, f) ((void)0)
#define SystemParametersInfoW(a, b, c, d) ((void)0)
#define SPI_GETCLIENTAREAANIMATION       0
#define SWP_NOZORDER                     0
#define SWP_NOACTIVATE                   0

void     InvalidateRect(HWND h, void *r, BOOL erase);
UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn);
BOOL     KillTimer(HWND h, UINT_PTR id);
short    GetKeyState(int vk);
DWORD    GetTickCount(void);
UINT     GetDoubleClickTime(void);
UINT     GetCaretBlinkTime(void);
HWND     GetForegroundWindow(void);
BOOL     ClientToScreen(HWND h, POINT *p);
BOOL     GetClientRect(HWND h, RECT *r);
void     SetCursor(LPCWSTR c);
BOOL     PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
UINT     DragQueryFileW(HDROP d, UINT i, wchar_t *out, UINT cap);
int      MessageBoxW(HWND h, const wchar_t *text, const wchar_t *title, UINT type);
BOOL     DestroyWindow(HWND h);

HMENU    CreatePopupMenu(void);
BOOL     AppendMenuW(HMENU m, UINT flags, UINT_PTR id, const wchar_t *text);
BOOL     DestroyMenu(HMENU m);
int      TrackPopupMenuEx(HMENU m, UINT flags, int x, int y, HWND h, void *params);

/* The clipboard, as plain text: no OpenClipboard/GlobalLock dance. */
wchar_t *sp_clipboard_get(void);                 /* free() */
void     sp_clipboard_set(const wchar_t *s);

static inline wchar_t *CharUpperW(wchar_t *s)
{
    for (wchar_t *c = s; *c; c++) *c = (wchar_t)towupper((wint_t)*c);
    return s;
}

static inline BOOL UnionRect(RECT *d, const RECT *a, const RECT *b)
{
    RECT r = { SP_MIN(a->left, b->left), SP_MIN(a->top, b->top), SP_MAX(a->right, b->right), SP_MAX(a->bottom, b->bottom) };
    *d = r;
    return TRUE;
}

/* ui_sdl.c */
HWND     sp_window_open(int w, int h, float *scale);
void     sp_shell_open(const wchar_t *target);
int      sp_pick_colour(uint32_t *rgb);             /* modal: kdialog or zenity; 0xRRGGBB */
int      sp_pick_paths(int folder, wchar_t ***paths);   /* modal; returns the count; free each and the array */
