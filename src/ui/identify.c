/* Identify displays: each display's number (as the Displays page shows it) in its top-left corner
 * for 2.5 s, like Windows display settings. Topmost, click-through, never activated; plain GDI. */
#include "ui_internal.h"
#include "../common/util.h"
#include <stdlib.h>

#define ID_CLASS L"ScreenPlus.Identify"
#define ID_MS    2500

typedef struct IdState { int index, motion; double started; } IdState;

static LRESULT CALLBACK id_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        uint32_t accent = g_app.cfg.s.accentColor & 0xFFFFFF;
        HBRUSH bg = CreateSolidBrush(RGB((accent >> 16) & 255, (accent >> 8) & 255, accent & 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HFONT f = CreateFontW(-(rc.bottom * 7 / 10), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        unsigned luminance = ((accent >> 16) & 255) * 299 + ((accent >> 8) & 255) * 587 + (accent & 255) * 114;
        SetTextColor(dc, luminance > 170000 ? RGB(0, 0, 0) : RGB(255, 255, 255));
        wchar_t num[8];
        IdState *st = (IdState *)GetWindowLongPtrW(h, GWLP_USERDATA);
        str_printf(num, SP_COUNTOF(num), L"%d", st ? st->index : 0);
        DrawTextW(dc, num, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: {
        IdState *st = (IdState *)GetWindowLongPtrW(h, GWLP_USERDATA);
        double elapsed = st ? time_ms() - st->started : ID_MS;
        if (elapsed >= ID_MS) { DestroyWindow(h); return 0; }
        if (st && st->motion == 0) {
            float a = SP_MIN(1.0f, (float)(elapsed / 120.0));
            a = SP_MIN(a, SP_MAX(0.0f, (float)((ID_MS - elapsed) / 160.0)));
            SetLayeredWindowAttributes(h, 0, (BYTE)(235.0f * a), LWA_ALPHA);
        }
        return 0;
    }
    case WM_DESTROY:
        free((void *)GetWindowLongPtrW(h, GWLP_USERDATA));
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void identify_displays(const MonitorInfo *mons, int n)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = id_proc;
        wc.hInstance = inst;
        wc.lpszClassName = ID_CLASS;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        registered = RegisterClassExW(&wc) != 0;
    }
    HWND old;
    for (int k = 0; k < 32 && (old = FindWindowW(ID_CLASS, NULL)) != NULL; k++) DestroyWindow(old);   /* pressed again */
    for (int i = 0; i < n; i++) {
        int size = (int)(150 * mons[i].dpi / 96), margin = (int)(36 * mons[i].dpi / 96);
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, ID_CLASS,
                                 L"", WS_POPUP, mons[i].work.left + margin, mons[i].work.top + margin, size, size, NULL, NULL,
                                 inst, NULL);
        if (!h) continue;
        IdState *st = (IdState *)calloc(1, sizeof *st);
        if (st) { st->index = mons[i].index; st->motion = g_ui ? g_ui->motion : g_app.cfg.s.uiMotion; st->started = time_ms(); }
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        SetLayeredWindowAttributes(h, 0, st && st->motion == 0 ? 0 : 235, LWA_ALPHA);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetTimer(h, 1, st && st->motion == 0 ? 15 : ID_MS, NULL);
    }
}
