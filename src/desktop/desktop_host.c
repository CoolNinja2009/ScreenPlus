#include "desktop_host.h"
#include "../common/ipc.h"
#include "../common/log.h"
#include "../common/util.h"

#define WM_SPAWN_WORKERW 0x052C

typedef struct FindCtx {
    HWND host;     /* top-level window containing SHELLDLL_DefView */
    HWND defView;
} FindCtx;

static BOOL CALLBACK find_defview(HWND top, LPARAM lp)
{
    FindCtx *ctx = (FindCtx *)lp;
    HWND dv = FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL);
    if (dv) {
        ctx->host = top;
        ctx->defView = dv;
        return FALSE;
    }
    return TRUE;
}

const wchar_t *desktop_host_mode_name(int mode)
{
    switch (mode) {
    case HOSTMODE_PROGMAN:  return L"Progman (24H2+)";
    case HOSTMODE_WORKERW:  return L"WorkerW";
    case HOSTMODE_FALLBACK: return L"Fallback (no shell)";
    default:                return L"None";
    }
}

static BOOL try_layouts(DesktopHost *h)
{
    /* Windows 11 24H2+: DefView and the wallpaper WorkerW are both children of Progman. */
    HWND dv = FindWindowExW(h->progman, NULL, L"SHELLDLL_DefView", NULL);
    HWND ww = FindWindowExW(h->progman, NULL, L"WorkerW", NULL);
    if (dv && ww) {
        h->mode = HOSTMODE_PROGMAN;
        h->defView = dv;
        h->workerw = ww;
        h->parent = h->progman;
        return TRUE;
    }
    /* Legacy: DefView lives in a top-level WorkerW (or Progman); the wallpaper WorkerW is the
     * next top-level sibling behind it. */
    FindCtx ctx = { 0 };
    EnumWindows(find_defview, (LPARAM)&ctx);
    if (ctx.host) {
        HWND behind = FindWindowExW(NULL, ctx.host, L"WorkerW", NULL);
        if (behind) {
            h->mode = HOSTMODE_WORKERW;
            h->defView = ctx.defView;
            h->workerw = behind;
            h->parent = behind;
            return TRUE;
        }
    }
    /* DefView inside Progman but no wallpaper WorkerW: host directly under the icons. */
    if (dv) {
        h->mode = HOSTMODE_PROGMAN;
        h->defView = dv;
        h->workerw = NULL;
        h->parent = h->progman;
        return TRUE;
    }
    return FALSE;
}

BOOL desktop_host_discover(DesktopHost *h)
{
    ZeroMemory(h, sizeof *h);
    h->progman = FindWindowW(L"Progman", NULL);
    if (!h->progman) {
        h->mode = HOSTMODE_FALLBACK;
        LOGW(L"desktop: Progman not found (shell not running?) - using fallback host");
        return FALSE;
    }
    GetWindowThreadProcessId(h->progman, &h->shellPid);
    DWORD_PTR res = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        /* Ask Progman to create the WorkerW behind the icons (no-op if it exists). */
        SendMessageTimeoutW(h->progman, WM_SPAWN_WORKERW, 0xD, 0x1, SMTO_NORMAL, 1000, &res);
        if (attempt == 1) SendMessageTimeoutW(h->progman, WM_SPAWN_WORKERW, 0, 0, SMTO_NORMAL, 1000, &res);
        if (try_layouts(h)) {
            LOGI(L"desktop: host mode %ls (progman=%p defview=%p workerw=%p)", desktop_host_mode_name(h->mode),
                 (void *)h->progman, (void *)h->defView, (void *)h->workerw);
            return TRUE;
        }
        Sleep(60);
    }
    h->mode = HOSTMODE_FALLBACK;
    h->parent = NULL;
    LOGW(L"desktop: could not locate the icon host window - using fallback host");
    return FALSE;
}

BOOL desktop_host_valid(const DesktopHost *h)
{
    if (h->mode == HOSTMODE_FALLBACK) return FindWindowW(L"Progman", NULL) == NULL;
    if (!h->parent || !IsWindow(h->parent)) return FALSE;
    if (h->defView && !IsWindow(h->defView)) return FALSE;
    return TRUE;
}

static void to_parent_coords(const DesktopHost *h, const RECT *screen, RECT *out)
{
    *out = *screen;
    if (h->parent) MapWindowPoints(HWND_DESKTOP, h->parent, (POINT *)out, 2);
}

BOOL desktop_host_attach(const DesktopHost *h, HWND w, const RECT *screenRect, BOOL layered)
{
    LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (layered) ex |= WS_EX_LAYERED;
    else ex &= ~(LONG_PTR)WS_EX_LAYERED;

    if (h->mode == HOSTMODE_FALLBACK || !h->parent) {
        SetWindowLongPtrW(w, GWL_EXSTYLE, ex);
        if (layered) SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
        SetWindowPos(w, HWND_BOTTOM, screenRect->left, screenRect->top, screenRect->right - screenRect->left,
                     screenRect->bottom - screenRect->top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return TRUE;
    }

    style &= ~(LONG_PTR)(WS_POPUP | WS_CAPTION | WS_THICKFRAME);
    style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    SetWindowLongPtrW(w, GWL_STYLE, style);
    SetWindowLongPtrW(w, GWL_EXSTYLE, ex);
    if (layered) SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);

    SetLastError(0);
    if (!SetParent(w, h->parent) && GetLastError() != 0) {
        LOGE(L"desktop: SetParent failed (err %lu)", GetLastError());
        return FALSE;
    }
    RECT r;
    to_parent_coords(h, screenRect, &r);
    HWND after = (h->mode == HOSTMODE_PROGMAN && h->defView) ? h->defView : HWND_TOP;
    SetWindowPos(w, after, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (GetParent(w) != h->parent) {
        LOGE(L"desktop: window %p not parented to host %p after SetParent", (void *)w, (void *)h->parent);
        return FALSE;
    }
    return TRUE;
}

void desktop_host_position(const DesktopHost *h, HWND w, const RECT *screenRect)
{
    if (h->mode == HOSTMODE_FALLBACK || !h->parent) {
        SetWindowPos(w, HWND_BOTTOM, screenRect->left, screenRect->top, screenRect->right - screenRect->left,
                     screenRect->bottom - screenRect->top, SWP_NOACTIVATE);
        return;
    }
    RECT r;
    to_parent_coords(h, screenRect, &r);
    HWND after = (h->mode == HOSTMODE_PROGMAN && h->defView) ? h->defView : HWND_TOP;
    SetWindowPos(w, after, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

void desktop_host_refresh(const DesktopHost *h)
{
    HWND target = h->workerw ? h->workerw : h->parent;
    if (target && IsWindow(target))
        RedrawWindow(target, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
