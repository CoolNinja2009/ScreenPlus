/* Identify displays: each display's number (as the Displays page shows it) in its top-left corner
 * for 2.5 s, like Windows display settings. Topmost, click-through, never activated; plain GDI. */
#include "ui_internal.h"
#include "../common/util.h"

#define ID_CLASS L"ScreenPlus.Identify"
#define ID_MS    2500

static LRESULT CALLBACK id_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0x0A, 0x84, 0xFF));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HFONT f = CreateFontW(-(rc.bottom * 7 / 10), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        wchar_t num[8];
        str_printf(num, SP_COUNTOF(num), L"%d", (int)GetWindowLongPtrW(h, GWLP_USERDATA));
        DrawTextW(dc, num, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(h);
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
        SetWindowLongPtrW(h, GWLP_USERDATA, mons[i].index);
        SetLayeredWindowAttributes(h, 0, 235, LWA_ALPHA);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetTimer(h, 1, ID_MS, NULL);
    }
}
