/* desktop_probe: verifies that a Screen+ wallpaper is attached behind the desktop icons and
 * actually composed by DWM.
 *   1. Lists Screen+ wallpaper windows with parent, z-order neighbour and styles.
 *   2. Shows a small DWM thumbnail of the icon host (Progman) in a temporary topmost,
 *      non-activating window and samples it. The thumbnail contains exactly what DWM composes
 *      for the desktop (system wallpaper, our layered child, icons), even when other windows
 *      cover the desktop.
 *   3. Samples the real screen at points where the desktop is the topmost window (if any).
 * Usage: desktop_probe RRGGBB [tolerance]
 * Exit: 0 expected colour composed, 1 not composed, 3 no wallpaper window. */
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_windows;
static unsigned g_expect;
static int g_tol = 40;

static BOOL CALLBACK child_cb(HWND h, LPARAM lp)
{
    (void)lp;
    wchar_t cls[64] = { 0 }, pcls[64] = L"(none)", acls[64] = L"-", bcls[64] = L"-";
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"ScreenPlus.Wallpaper") != 0) return TRUE;
    HWND p = GetParent(h), above = GetWindow(h, GW_HWNDPREV), below = GetWindow(h, GW_HWNDNEXT);
    if (p) GetClassNameW(p, pcls, 64);
    if (above) GetClassNameW(above, acls, 64);
    if (below) GetClassNameW(below, bcls, 64);
    RECT r;
    GetWindowRect(h, &r);
    printf("wallpaper window %p parent=%ls visible=%d layered=%d rect=(%ld,%ld,%ld,%ld) above=%ls below=%ls\n", (void *)h,
           pcls, IsWindowVisible(h), (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_LAYERED) != 0, r.left, r.top, r.right, r.bottom,
           acls, bcls);
    g_windows++;
    return TRUE;
}

static BOOL CALLBACK top_cb(HWND h, LPARAM lp)
{
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) EnumChildWindows(h, child_cb, lp);
    else if (!wcscmp(cls, L"ScreenPlus.Wallpaper")) child_cb(h, lp);
    return TRUE;
}

static int matches(unsigned px)
{
    int r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
    return abs(r - (int)((g_expect >> 16) & 255)) <= g_tol && abs(g - (int)((g_expect >> 8) & 255)) <= g_tol &&
           abs(b - (int)(g_expect & 255)) <= g_tol;
}

/* Copies a screen rectangle into a top-down 32-bpp buffer (caller frees). */
static unsigned *grab(int x, int y, int w, int h)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits = NULL;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, h, sdc, x, y, SRCCOPY | CAPTUREBLT);
    unsigned *out = (unsigned *)malloc((size_t)w * h * 4);
    if (out) memcpy(out, bits, (size_t)w * h * 4);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    return out;
}

static void pump(int ms)
{
    for (int i = 0; i < ms / 15; i++) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        DwmFlush();
        Sleep(15);
    }
}

static double thumbnail_check(const MONITORINFO *mi)
{
    HWND pm = FindWindowW(L"Progman", NULL);
    RECT pr;
    if (!pm || !GetWindowRect(pm, &pr)) return -1.0;
    int W = 384, H = W * (pr.bottom - pr.top) / (pr.right - pr.left);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SPProbeThumb";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    int x0 = mi->rcWork.left + 8, y0 = mi->rcWork.top + 8;
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, x0, y0, W, H,
                             NULL, NULL, wc.hInstance, NULL);
    HTHUMBNAIL th = NULL;
    double frac = -1.0;
    if (w && SUCCEEDED(DwmRegisterThumbnail(w, pm, &th))) {
        DWM_THUMBNAIL_PROPERTIES p = { 0 };
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
        SetRect(&p.rcDestination, 0, 0, W, H);
        p.fVisible = TRUE;
        p.opacity = 255;
        DwmUpdateThumbnailProperties(th, &p);
        ShowWindow(w, SW_SHOWNOACTIVATE);
        pump(300);
        unsigned *px = grab(x0, y0, W, H);
        int n = 0, m = 0;
        for (int y = H / 12; px && y < H * 11 / 12; y += 2)
            for (int x = W / 4; x < W * 19 / 20; x += 2) {   /* skip the icon column */
                n++;
                m += matches(px[(size_t)y * W + x]);
            }
        if (px) printf("DWM thumbnail of desktop: %d samples, %d match %06X (first %06X)\n", n, m, g_expect,
                       px[(size_t)(H / 2) * W + W / 2] & 0xFFFFFF);
        free(px);
        frac = n ? (double)m / n : 0.0;
        DwmUnregisterThumbnail(th);
    }
    if (w) DestroyWindow(w);
    return frac;
}

static void screen_check(const MONITORINFO *mi)
{
    int W = mi->rcMonitor.right - mi->rcMonitor.left, H = mi->rcMonitor.bottom - mi->rcMonitor.top;
    unsigned *px = grab(mi->rcMonitor.left, mi->rcMonitor.top, W, H);
    int n = 0, m = 0;
    for (int gy = 1; px && gy < 30; gy++)
        for (int gx = 1; gx < 50; gx++) {
            POINT pt = { mi->rcWork.left + (mi->rcWork.right - mi->rcWork.left) * gx / 50,
                         mi->rcWork.top + (mi->rcWork.bottom - mi->rcWork.top) * gy / 30 };
            wchar_t cls[64] = L"";
            GetClassNameW(WindowFromPoint(pt), cls, 64);
            if (wcscmp(cls, L"SysListView32") && wcscmp(cls, L"SHELLDLL_DefView") && wcscmp(cls, L"Progman") &&
                wcscmp(cls, L"WorkerW") && wcscmp(cls, L"ScreenPlus.Wallpaper"))
                continue;
            n++;
            m += matches(px[(size_t)(pt.y - mi->rcMonitor.top) * W + (pt.x - mi->rcMonitor.left)]);
        }
    free(px);
    printf("live screen: %d desktop-visible samples, %d match\n", n, m);
}

int main(int argc, char **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnumWindows(top_cb, 0);
    if (!g_windows) {
        printf("no Screen+ wallpaper window found\n");
        return 3;
    }
    if (argc < 2) return 0;
    g_expect = (unsigned)strtoul(argv[1], NULL, 16);
    if (argc > 2) g_tol = atoi(argv[2]);
    HMONITOR hm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(hm, &mi);
    screen_check(&mi);
    double f = thumbnail_check(&mi);
    printf("result: %s\n", f >= 0.8 ? "COMPOSED" : "NOT COMPOSED");
    return f >= 0.8 ? 0 : 1;
}
