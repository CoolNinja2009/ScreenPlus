#include "occlusion.h"
#include "../common/util.h"
#include <dwmapi.h>
#include <shellapi.h>

typedef struct Ctx {
    const MonitorInfo *mons;
    int                count;
    DWORD              ownPid;
    HWND               fg;
    OcclusionResult   *out;
} Ctx;

static int ignored_class(const wchar_t *cls)
{
    static const wchar_t *k[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                  L"NotifyIconOverflowWindow", L"Windows.UI.Core.CoreWindow",
                                  L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
                                  L"ScreenPlus.Wallpaper", L"ScreenPlus.Engine", L"ScreenPlus.Shell",
                                  /* our own settings window is see-through (acrylic) and the Customise
                                   * page previews live, so it never pauses the wallpaper */
                                  L"ScreenPlus.Window" };
    for (size_t i = 0; i < SP_COUNTOF(k); i++)
        if (wcscmp(cls, k[i]) == 0) return 1;
    return 0;
}

static int contains(const RECT *outer, const RECT *inner)
{
    return outer->left <= inner->left && outer->top <= inner->top && outer->right >= inner->right && outer->bottom >= inner->bottom;
}

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    Ctx *c = (Ctx *)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->ownPid) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return TRUE;               /* click-through overlays */
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        if (!GetLayeredWindowAttributes(h, &key, &alpha, &flags)) return TRUE;   /* per-pixel alpha */
        if ((flags & LWA_COLORKEY) || ((flags & LWA_ALPHA) && alpha < 250)) return TRUE;
    }
    wchar_t cls[96];
    if (!GetClassNameW(h, cls, SP_COUNTOF(cls)) || ignored_class(cls)) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    if (r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
    BOOL zoomed = IsZoomed(h);
    HMONITOR hm = zoomed ? MonitorFromWindow(h, MONITOR_DEFAULTTONULL) : NULL;
    for (int i = 0; i < c->count; i++) {
        const MonitorInfo *m = &c->mons[i];
        int full = contains(&r, &m->rect);
        if (full || contains(&r, &m->work) || (zoomed && hm == m->hmon)) {
            c->out->covered[i] = 1;
            if (full && h == c->fg && !c->out->fullscreen) {
                c->out->fullscreen = 1;
                c->out->fullscreenWnd = h;
            }
        }
    }
    return TRUE;
}

void occlusion_compute(const MonitorInfo *mons, int count, OcclusionResult *out)
{
    ZeroMemory(out, sizeof *out);
    Ctx c = { mons, SP_MIN(count, SP_MAX_MONITORS), GetCurrentProcessId(), GetForegroundWindow(), out };
    /* Fullscreen = the foreground window covers its whole monitor (this includes exclusive-mode
     * games). SHQueryUserNotificationState is deliberately not used: it was observed reporting
     * QUNS_RUNNING_D3D_FULL_SCREEN while the desktop was visible, which would pause forever. */
    EnumWindows(enum_cb, (LPARAM)&c);
}
