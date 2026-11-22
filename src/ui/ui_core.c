/* Management window core: window lifecycle, Direct2D/DirectWrite resources, theme,
 * drawing primitives, widgets, hit testing, keyboard focus, scrolling and animation. */
#include "ui_internal.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../app/preview.h"
#include "../app/file_format.h"
#ifdef _WIN32
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifndef _WIN32
#include <stdio.h>
#endif

#define SP_DWMWA_DARK      20
#define SP_DWMWA_CORNER    33
#define SP_DWMWA_BACKDROP  38
#define SP_DWMSBT_ACRYLIC  3

#define RT ((ID2D1RenderTarget *)g_ui->rt)
#define MAX_THUMBS 160

static int g_cardOrigin = -1;   /* library index of the card being reordered, to undo it if it leaves the window */
static float g_cardInit[4];      /* how far the lifted card already stuck out of the grid where it was picked up */

/* How far the lifted card (pressedRect moved with the pointer, 1.05 scale, plus `inflate` DIP) sticks out of the
 * Library's clip (right of the sidebar, below the header) on each side: left, top, right, bottom. */
static void card_overflow(const Ui *u, float mx, float my, float inflate, float out[4])
{
    D2D1_RECT_F r = u->pressedRect;
    float gx = (r.right - r.left) * 0.025f + inflate, gy = (r.bottom - r.top) * 0.025f + inflate;
    float dx = mx - u->pressX, dy = my - u->pressY;
    out[0] = SP_MAX(0.0f, 232.0f - (r.left + dx - gx));             /* SIDEBAR_W */
    out[1] = SP_MAX(0.0f, u->viewTop - (r.top + dy - gy));
    out[2] = SP_MAX(0.0f, r.right + dx + gx - u->w);
    out[3] = SP_MAX(0.0f, r.bottom + dy + gy - u->h);
}

/* the card starts to be clipped: from here only the Shell can draw it */
static int card_leaving(const Ui *u)
{
#ifndef _WIN32
    (void)u;
    return 0;            /* Linux exports through a save dialog once the pointer leaves the window */
#else
    float o[4];
    card_overflow(u, u->mx, u->my, 0.0f, o);
    for (int i = 0; i < 4; i++) if (o[i] > g_cardInit[i] + 1.0f) return 1;
    return 0;
#endif
}

#ifdef _WIN32
/* comfortably back inside (a margin, so a pointer on the edge doesn't flip between the two drags) */
static int card_back(const Ui *u, float mx, float my)
{
    float o[4];
    card_overflow(u, mx, my, 12.0f, o);
    for (int i = 0; i < 4; i++) if (o[i] > g_cardInit[i]) return 0;
    return 1;
}
#endif

static void card_lift(Ui *u)
{
    int vi = HIT_DATA(u->pressed);
    g_cardOrigin = vi >= 0 && vi < u->viewCount ? u->view[vi] : -1;
    card_overflow(u, u->pressX, u->pressY, 0.0f, g_cardInit);
}

#ifdef _WIN32
/* Dragging a card out of the window exports it. The Shell drag starts at once with empty files that give Explorer
 * their names while a worker packs the archives into them. The drag image is the card itself, drawn by the Shell, so
 * it keeps up with the pointer and shows "Copy to <folder>". A release before the archives are ready holds the
 * pointer where the button came up, with a busy cursor, and drops there once they are; Esc cancels. */
typedef struct DragExport {
    HWND owner;
    WallpaperEntry *items;
    wchar_t (*paths)[SP_MAX_PATH];
    wchar_t dir[SP_MAX_PATH];
    int count, ok, reported, held;
    int reorder, back;   /* it left a reorder; it came back inside, so the reorder resumes */
    IDragSourceHelper *helper;   /* the drag image: the card at full size shrinking to its small copy */
    IDataObject *data;
    DWORD *src;                  /* the full-size card, premultiplied BGRA */
    int srcW, srcH, smallW;
    float grabX, grabY;          /* where the pointer holds it, in src pixels */
    double animT0, lastFrame;
    int animDone;
    POINT heldAt;
    volatile LONG ready, refs, cancelled;
} DragExport;
static DragExport *g_drag;

static void drag_free(DragExport *j)
{
    if (!j) return;
    for (int i = 0; i < j->count; i++) if (j->paths && j->paths[i][0]) DeleteFileW(j->paths[i]);
    if (j->dir[0]) RemoveDirectoryW(j->dir);
    free(j->paths); free(j->items); free(j);
}

static void drag_release(DragExport *j)
{ if (InterlockedDecrement(&j->refs) == 0) drag_free(j); }

static DWORD WINAPI drag_cleanup_later(LPVOID p)
{
    Sleep(600000);  /* the drop target may still be copying after SHDoDragDrop returns */
    drag_release((DragExport *)p);
    return 0;
}

/* leftovers of a session that ended before its cleanup ran */
static void drag_sweep(void)
{
    wchar_t root[SP_MAX_PATH], pattern[SP_MAX_PATH];
    path_join(root, SP_COUNTOF(root), paths_cache(), L"format-drag");
    path_join(pattern, SP_COUNTOF(pattern), root, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t dir[SP_MAX_PATH], files[SP_MAX_PATH], f[SP_MAX_PATH];
        path_join(dir, SP_COUNTOF(dir), root, fd.cFileName);
        path_join(files, SP_COUNTOF(files), dir, L"*.spf");
        WIN32_FIND_DATAW ff;
        HANDLE hf = FindFirstFileW(files, &ff);
        if (hf != INVALID_HANDLE_VALUE) {
            do { path_join(f, SP_COUNTOF(f), dir, ff.cFileName); DeleteFileW(f); } while (FindNextFileW(hf, &ff));
            FindClose(hf);
        }
        RemoveDirectoryW(dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void drag_finish(DragExport *j, int dropped)
{
    int uiOwned = g_drag == j;
    KillTimer(j->owner, TM_EXPORT_DRAG);
    if (j->helper) IDragSourceHelper_Release(j->helper);
    if (j->data) IDataObject_Release(j->data);
    free(j->src);
    j->helper = NULL; j->data = NULL; j->src = NULL;
    if (uiOwned) g_drag = NULL;
    if (j->held) ClipCursor(NULL);
    InterlockedExchange(&j->cancelled, 1);
    if (dropped) {
        InterlockedIncrement(&j->refs);
        HANDLE h = CreateThread(NULL, 0, drag_cleanup_later, j, 0, NULL);
        if (h) CloseHandle(h);
        else drag_release(j);
    }
    if (uiOwned) drag_release(j);
}

static void drag_animate(DragExport *j);

typedef struct DragSource {
    IDropSource iface;
    LONG refs;
    DragExport *job;
} DragSource;

static HRESULT STDMETHODCALLTYPE source_query(IDropSource *self, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IDropSource)) return E_NOINTERFACE;
    *out = self;
    IDropSource_AddRef(self);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE source_addref(IDropSource *self)
{ return (ULONG)InterlockedIncrement(&((DragSource *)self)->refs); }
static ULONG STDMETHODCALLTYPE source_release(IDropSource *self)
{ return (ULONG)InterlockedDecrement(&((DragSource *)self)->refs); }

static int drag_over_own_window(const DragExport *j)
{
    POINT pt;
    if (!GetCursorPos(&pt)) return 0;
    HWND target = WindowFromPoint(pt);
    return target && GetAncestor(target, GA_ROOT) == j->owner;
}

static HRESULT STDMETHODCALLTYPE source_continue(IDropSource *self, BOOL escape, DWORD keys)
{
    DragExport *j = ((DragSource *)self)->job;
    drag_animate(j);
    if (escape) return DRAGDROP_S_CANCEL;
    LONG ready = InterlockedCompareExchange(&j->ready, 0, 0);
    if (ready && !j->ok) {
        if (!j->reported++) app_notice(NOTICE_WARN, j->count == 1 ? L"Could not export '%ls'. The log has the reason."
                                                                  : L"Could not export the selected wallpapers.",
                                       j->items[0].name);
        return DRAGDROP_S_CANCEL;
    }
    if (!j->held) {
        if ((keys & MK_LBUTTON) && j->reorder && g_ui && g_ui->hwnd == j->owner) {
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(j->owner, &pt) &&
                card_back(g_ui, pt.x / g_ui->scale, pt.y / g_ui->scale)) {
                j->back = 1;                                     /* the grid takes the card back */
                return DRAGDROP_S_CANCEL;
            }
        }
        if (keys & MK_LBUTTON) return S_OK;
        if (drag_over_own_window(j)) return DRAGDROP_S_CANCEL;   /* dropped back on Screen+: nothing to do */
        if (ready) return DRAGDROP_S_DROP;
        j->held = 1;                                             /* released early: wait here for the archives */
        GetCursorPos(&j->heldAt);
        app_notice(NOTICE_INFO, j->count == 1 ? L"Finishing '%ls'… Esc cancels." : L"Finishing the export… Esc cancels.",
                   j->items[0].name);
    }
    if (ready) return DRAGDROP_S_DROP;
    RECT clip = { j->heldAt.x, j->heldAt.y, j->heldAt.x + 1, j->heldAt.y + 1 };
    ClipCursor(&clip);
    SetCursor(LoadCursorW(NULL, IDC_APPSTARTING));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE source_feedback(IDropSource *self, DWORD effect)
{
    (void)effect;
    if (((DragSource *)self)->job->held) { SetCursor(LoadCursorW(NULL, IDC_APPSTARTING)); return S_OK; }
    return DRAGDROP_S_USEDEFAULTCURSORS;
}
static IDropSourceVtbl k_drag_source = {
    source_query, source_addref, source_release, source_continue, source_feedback
};

/* The drag image at scale k of the full card (bilinear, premultiplied), held at the same point of the card. A
 * running drag only redraws its image when told: the drag window takes DDWM_UPDATEWINDOW (WM_USER + 3), as
 * drag-and-drop libraries do; where that is ignored the card just stays large. */
static void drag_set_image(DragExport *j, float k)
{
    int w = SP_MAX(1, (int)(j->srcW * k + 0.5f)), h = SP_MAX(1, (int)(j->srcH * k + 0.5f));
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    DWORD *out = (DWORD *)bits;
    float fx = (float)j->srcW / w, fy = (float)j->srcH / h;
    for (int y = 0; y < h; y++) {
        float sy = SP_CLAMP((y + 0.5f) * fy - 0.5f, 0.0f, (float)(j->srcH - 1));
        int y0 = (int)sy, y1 = SP_MIN(y0 + 1, j->srcH - 1);
        float ty = sy - y0;
        for (int x = 0; x < w; x++) {
            float sx = SP_CLAMP((x + 0.5f) * fx - 0.5f, 0.0f, (float)(j->srcW - 1));
            int x0 = (int)sx, x1 = SP_MIN(x0 + 1, j->srcW - 1);
            float tx = sx - x0;
            DWORD a = j->src[y0 * j->srcW + x0], b = j->src[y0 * j->srcW + x1],
                  c = j->src[y1 * j->srcW + x0], d = j->src[y1 * j->srcW + x1], v = 0;
            for (int bit = 0; bit < 32; bit += 8) {
                float top = ((a >> bit) & 255) * (1 - tx) + ((b >> bit) & 255) * tx;
                float bot = ((c >> bit) & 255) * (1 - tx) + ((d >> bit) & 255) * tx;
                v |= (DWORD)(top * (1 - ty) + bot * ty + 0.5f) << bit;
            }
            out[y * w + x] = v;
        }
    }
    SHDRAGIMAGE di = { { w, h }, { (LONG)(j->grabX * k), (LONG)(j->grabY * k) }, bmp, CLR_NONE };
    if (FAILED(IDragSourceHelper_InitializeFromBitmap(j->helper, &di, j->data))) { DeleteObject(bmp); return; }
    if (j->animT0 <= 0) return;                    /* the first image: the drag shows it when it starts */
    static UINT cf;
    if (!cf) cf = RegisterClipboardFormatW(L"DragWindow");
    FORMATETC fe = { (CLIPFORMAT)cf, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm = { 0 };
    if (SUCCEEDED(IDataObject_GetData(j->data, &fe, &sm))) {
        DWORD *hw = (DWORD *)GlobalLock(sm.hGlobal);
        if (hw) {
            HWND dw = (HWND)(ULONG_PTR)*hw;
            GlobalUnlock(sm.hGlobal);
            if (IsWindow(dw)) PostMessageW(dw, WM_USER + 3, 0, 0);
        }
        ReleaseStgMedium(&sm);
    }
}

/* the card shrinks from its own size to the small copy over 240 ms after it leaves (TM_EXPORT_DRAG) */
static void drag_animate(DragExport *j)
{
    if (!j || j->animDone || !j->helper) return;
    double now = time_ms();
    if (now - j->lastFrame < 14.0) return;
    j->lastFrame = now;
    float t = (float)SP_CLAMP((now - j->animT0) / 240.0, 0.0, 1.0);
    float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    float end = (float)j->smallW / (float)j->srcW;
    drag_set_image(j, 1.0f + (end - 1.0f) * e);
    if (t >= 1.0f) { j->animDone = 1; KillTimer(j->owner, TM_EXPORT_DRAG); }
}

/* The card as it looks in the Library, captured at full size, with a count badge for a group. */
static void drag_image(Ui *u, DragExport *j, IDataObject *data)
{
    IDragSourceHelper *helper = NULL;
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDragSourceHelper,
                                (void **)&helper)))
        return;
    IDragSourceHelper2 *h2 = NULL;
    if (SUCCEEDED(IDragSourceHelper_QueryInterface(helper, &IID_IDragSourceHelper2, (void **)&h2))) {
        IDragSourceHelper2_SetFlags(h2, DSH_ALLOWDROPDESCRIPTIONTEXT);
        IDragSourceHelper2_Release(h2);
    }
    /* From a reorder the image is the lifted card at full size where it was last drawn, so it carries on over the
     * window's edge without a jump; otherwise a smaller copy of the card in its place. */
    D2D1_RECT_F src = j->reorder ? u->dragDrawn : u->pressedRect;
    RECT r = { (LONG)(src.left * u->scale), (LONG)(src.top * u->scale),
               (LONG)(src.right * u->scale), (LONG)(src.bottom * u->scale) };
    int sw = SP_MAX(1, r.right - r.left), sh = SP_MAX(1, r.bottom - r.top);
    int w = SP_MIN(sw, (int)(520 * u->scale)), hgt = SP_MAX(1, sh * w / sw);
    int smallW = SP_MIN(w, (int)(200 * u->scale));
    float badgeK = (float)w / (float)smallW;       /* the badge is drawn to read right once the card is small */
    float lift = j->reorder ? 1.05f : 1.0f;
    float grabX = (u->pressX - u->pressedRect.left) * lift, grabY = (u->pressY - u->pressedRect.top) * lift;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -hgt, 1, 32, BI_RGB } };
    void *bits = NULL;
    HDC dc = GetDC(u->hwnd), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, 0, 0, w, hgt, dc, r.left, r.top, sw, sh, SRCCOPY);
        if (j->count > 1) {
            int d = (int)(26 * u->scale * badgeK), m = (int)(6 * badgeK);
            RECT badge = { w - d - m, m, w - m, m + d };
            HBRUSH accent = CreateSolidBrush(RGB((BYTE)(u->t.accent.r * 255), (BYTE)(u->t.accent.g * 255),
                                                (BYTE)(u->t.accent.b * 255)));
            HGDIOBJ ob = SelectObject(mem, accent), op = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, badge.left, badge.top, badge.right + 1, badge.bottom + 1);
            SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(accent);
            HFONT font = CreateFontW(-(int)(13 * u->scale * badgeK), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                     CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
            HGDIOBJ of = SelectObject(mem, font);
            wchar_t count[16]; str_printf(count, SP_COUNTOF(count), L"%d", j->count);
            SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, count, -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, of); DeleteObject(font);
        }
        SelectObject(mem, old);
        GdiFlush();
        /* GDI leaves alpha at 0: make the card opaque inside its rounded corners (premultiplied, edges smoothed) */
        DWORD *px = (DWORD *)bits;
        float rad = 14.0f * lift * u->scale * (float)w / (float)sw;
        for (int y = 0; y < hgt; y++)
            for (int x = 0; x < w; x++) {
                float cx = x + 0.5f, cy = y + 0.5f;
                float qx = cx < rad ? rad - cx : cx > w - rad ? cx - (w - rad) : 0.0f;
                float qy = cy < rad ? rad - cy : cy > hgt - rad ? cy - (hgt - rad) : 0.0f;
                float a = qx > 0 && qy > 0 ? SP_CLAMP(rad - sqrtf(qx * qx + qy * qy) + 0.5f, 0.0f, 1.0f) : 1.0f;
                DWORD c = px[y * w + x];
                if (a >= 1.0f) { px[y * w + x] = c | 0xFF000000u; continue; }
                DWORD rr = (DWORD)(((c >> 16) & 255) * a), gg = (DWORD)(((c >> 8) & 255) * a), bb = (DWORD)((c & 255) * a);
                px[y * w + x] = ((DWORD)(a * 255.0f) << 24) | (rr << 16) | (gg << 8) | bb;
            }
        j->src = (DWORD *)malloc((size_t)w * hgt * sizeof(DWORD));
        if (j->src) {
            memcpy(j->src, px, (size_t)w * hgt * sizeof(DWORD));
            j->srcW = w; j->srcH = hgt; j->smallW = smallW;
            j->grabX = SP_CLAMP(grabX * u->scale * w / sw, 0.0f, (float)(w - 1));   /* where it was held */
            j->grabY = SP_CLAMP(grabY * u->scale * hgt / sh, 0.0f, (float)(hgt - 1));
            j->helper = helper; helper = NULL;
            j->data = data; IDataObject_AddRef(data);
            drag_set_image(j, 1.0f);
            j->animT0 = time_ms();
            j->animDone = smallW >= w || u->motion != 0;    /* reduced motion: it stays as it is */
            if (!j->animDone) SetTimer(j->owner, TM_EXPORT_DRAG, 15, NULL);
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(u->hwnd, dc);
    if (helper) IDragSourceHelper_Release(helper);
}

static int drag_shell(Ui *u, DragExport *j)
{
    InterlockedIncrement(&j->refs); /* UI may close while the Shell's nested drag loop is running. */
    PIDLIST_ABSOLUTE folder = NULL;
    PIDLIST_ABSOLUTE *items = (PIDLIST_ABSOLUTE *)calloc((size_t)j->count, sizeof *items);
    PCUITEMID_CHILD *children = (PCUITEMID_CHILD *)calloc((size_t)j->count, sizeof *children);
    IDataObject *data = NULL;
    DWORD effect = 0;
    HRESULT hr = items && children ? SHParseDisplayName(j->dir, NULL, &folder, 0, NULL) : E_OUTOFMEMORY;
    for (int i = 0; SUCCEEDED(hr) && i < j->count; i++) {
        hr = SHParseDisplayName(j->paths[i], NULL, &items[i], 0, NULL);
        if (SUCCEEDED(hr)) children[i] = ILFindLastID(items[i]);
    }
    if (SUCCEEDED(hr)) hr = SHCreateDataObject(folder, (UINT)j->count, children, NULL, &IID_IDataObject, (void **)&data);
    DragSource source = { 0 };
    source.iface.lpVtbl = &k_drag_source;
    source.refs = 1;
    source.job = j;
    if (SUCCEEDED(hr) && data) {
        drag_image(u, j, data);
        DragAcceptFiles(j->owner, FALSE);
        hr = SHDoDragDrop(j->owner, data, &source.iface, DROPEFFECT_COPY, &effect);
        if (IsWindow(j->owner)) DragAcceptFiles(j->owner, TRUE);
    }
    if (data) IDataObject_Release(data);
    for (int i = 0; i < j->count; i++) if (items && items[i]) CoTaskMemFree(items[i]);
    if (folder) CoTaskMemFree(folder);
    free(items); free(children);
    int dropped = hr == DRAGDROP_S_DROP;   /* whatever effect a target reports, it may still be reading the files */
    if (dropped && j->count == 1) app_notice(NOTICE_INFO, L"Exported '%ls'.", j->items[0].name);
    else if (dropped) app_notice(NOTICE_INFO, L"Exported %d wallpapers.", j->count);
    int back = j->back;
    drag_finish(j, dropped);
    drag_release(j);
    return back;
}

static int drag_is_internal_path(const wchar_t *path)
{
    wchar_t root[SP_MAX_PATH];
    path_join(root, SP_COUNTOF(root), paths_cache(), L"format-drag");
    size_t len = wcslen(root);
    return !_wcsnicmp(path, root, len) && (path[len] == L'\\' || path[len] == L'/');
}

static DWORD WINAPI drag_worker(LPVOID p)
{
    DragExport *j = (DragExport *)p;
    j->ok = 1;
    for (int i = 0; i < j->count; i++) {
        if (InterlockedCompareExchange(&j->cancelled, 0, 0) || !spf_export(&j->items[i], j->paths[i])) {
            j->ok = 0; break;
        }
    }
    InterlockedExchange(&j->ready, 1);
    PostMessageW(j->owner, WM_NULL, 0, 0);   /* wakes the drag loop to drop a held release */
    drag_release(j);
    return 0;
}

/* returns 1 when the card was dragged back inside and the reorder goes on */
static int export_card_drag(Ui *u, int reorder)
{
    if (g_drag || u->page != PAGE_LIBRARY) return 0;
    int vi = HIT_DATA(u->pressed);
    if (vi < 0 || vi >= u->viewCount) return 0;
    WallpaperEntry *pressed = &g_app.cfg.items[u->view[vi]];
    int selected = (pressed->uiMark & 1) != 0, count = 0;
    for (int i = 0; i < u->viewCount; i++)
        if (selected && (g_app.cfg.items[u->view[i]].uiMark & 1)) count++;
    if (!count) count = 1;
    static int swept;
    if (!swept++) drag_sweep();
    DragExport *j = (DragExport *)calloc(1, sizeof *j);
    if (!j) return 0;
    j->items = (WallpaperEntry *)calloc((size_t)count, sizeof *j->items);
    j->paths = (wchar_t (*)[SP_MAX_PATH])calloc((size_t)count, sizeof *j->paths);
    if (!j->items || !j->paths) { drag_free(j); return 0; }
    j->owner = u->hwnd; j->count = count; j->refs = 1; j->reorder = reorder;
    int n = 0;
    for (int i = 0; i < u->viewCount; i++) {
        WallpaperEntry *e = &g_app.cfg.items[u->view[i]];
        if (selected && !(e->uiMark & 1)) continue;
        if (!selected && e != pressed) continue;
        if (e->missing && !wallpaper_is_url(e->path)) {
            app_notice(NOTICE_WARN, L"'%ls' can't be exported: its file is missing.", e->name);
            drag_free(j); return 0;
        }
        j->items[n++] = *e;
    }
    wchar_t root[SP_MAX_PATH], id[SP_ID_LEN];
    path_join(root, SP_COUNTOF(root), paths_cache(), L"format-drag");
    random_id(id); path_join(j->dir, SP_COUNTOF(j->dir), root, id);
    if (!dir_create_recursive(j->dir)) { drag_free(j); return 0; }
    for (int i = 0; i < count; i++) {
        wchar_t name[SP_MAX_PATH];
        sp_format_file_name(j->items[i].name, L"spf", name, SP_COUNTOF(name));
        sp_format_unique_path(j->dir, name, j->paths[i], SP_MAX_PATH);   /* the group's own name clashes */
        HANDLE placeholder = CreateFileW(j->paths[i], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (placeholder == INVALID_HANDLE_VALUE) { j->paths[i][0] = 0; drag_free(j); return 0; }
        CloseHandle(placeholder);
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { drag_free(j); return 0; }
    g_drag = j;
    InterlockedIncrement(&j->refs);
    HANDLE h = CreateThread(NULL, 0, drag_worker, j, 0, NULL);
    if (!h) { drag_release(j); drag_finish(j, 0); return 0; }
    CloseHandle(h);
    return drag_shell(u, j);
}
#else
static void export_card_save(Ui *u);
static int export_card_drag(Ui *u, int reorder)   /* Linux: a save dialog; the card goes back to its place */
{
    (void)reorder;
    export_card_save(u);
    return 0;
}
static void export_card_save(Ui *u)
{
    int vi = HIT_DATA(u->pressed);
    if (u->page != PAGE_LIBRARY || vi < 0 || vi >= u->viewCount) return;
    WallpaperEntry e = g_app.cfg.items[u->view[vi]];
    if (e.missing && !wallpaper_is_url(e.path)) {
        app_notice(NOTICE_WARN, L"'%ls' can't be exported: its file is missing.", e.name);
        return;
    }
    int count = 0;
    if (e.uiMark & 1) for (int i = 0; i < u->viewCount; i++)
        count += (g_app.cfg.items[u->view[i]].uiMark & 1) != 0;
    if (count > 1) {
        WallpaperEntry *items = (WallpaperEntry *)calloc((size_t)count, sizeof *items);
        if (!items) return;
        int n = 0;
        for (int i = 0; i < u->viewCount; i++) {
            WallpaperEntry *item = &g_app.cfg.items[u->view[i]];
            if (!(item->uiMark & 1)) continue;
            if (item->missing && !wallpaper_is_url(item->path)) {
                app_notice(NOTICE_WARN, L"'%ls' can't be exported: its file is missing.", item->name);
                free(items); return;
            }
            items[n++] = *item;
        }
        wchar_t **folders = NULL;
        int picked = sp_pick_paths(1, &folders);
        if (picked && folders && folders[0]) {
            if (spf_export_start(items, n, NULL, folders[0])) app_notice(NOTICE_INFO, L"Exporting %d wallpapers…", n);
            else app_notice(NOTICE_WARN, L"Could not start the export.");
        }
        for (int i = 0; i < picked; i++) free(folders[i]);
        free(folders); free(items);
        return;
    }
    wchar_t name[SP_MAX_PATH], file[SP_MAX_PATH];
    sp_format_file_name(e.name, L"spf", name, SP_COUNTOF(name));
    if (sp_format_save_dialog(u->hwnd, name, L"spf", file, SP_COUNTOF(file))) {
        const wchar_t *paths[1] = { file };
        if (spf_export_start(&e, 1, paths, NULL)) app_notice(NOTICE_INFO, L"Exporting '%ls'…", e.name);
        else app_notice(NOTICE_WARN, L"Could not start the export.");
    }
}
#endif
#define MAX_ANIMATED 24   /* animations kept resident; more than a screenful, bounded memory */



Ui *g_ui;
static const wchar_t k_class[] = L"ScreenPlus.Window";
static D2D1_RECT_F g_clip[4];
static int g_clipDepth;

/* ================================================================ colors & geometry */

D2D1_COLOR_F col(uint32_t rgb, float a)
{
    D2D1_COLOR_F c = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a };
    return c;
}

D2D1_COLOR_F col_a(D2D1_COLOR_F c, float a)
{
    c.a = a;
    return c;
}

D2D1_RECT_F rectf(float x, float y, float w, float h)
{
    D2D1_RECT_F r = { x, y, x + w, y + h };
    return r;
}

int in_rect(D2D1_RECT_F r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

/* ================================================================ theme */

#ifndef _WIN32
int ui_plat_dark(void);
static int system_dark(void) { return ui_plat_dark(); }
#else
static int system_dark(void)
{
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return 0;
    return v == 0;
}
#endif

static void theme_build(Ui *u)
{
    int dark = g_app.cfg.s.theme == THEME_DARK || (g_app.cfg.s.theme == THEME_SYSTEM && system_dark());
    Theme built = { 0 }, *t = &built;
    t->dark = dark;
    if (dark) {
        t->bg = col(0x1C1C1E, 1.0f);
        t->tint = col(0x121214, u->backdrop ? 0.60f : 1.0f);
        t->sidebar = col(0xFFFFFF, 0.035f);
        t->navActive = col(0xFFFFFF, 0.09f);
        t->panel = col(0xFFFFFF, 0.055f);
        t->panelHover = col(0xFFFFFF, 0.09f);
        t->border = col(0xFFFFFF, 0.09f);
        t->highlight = col(0xFFFFFF, 0.11f);
        t->text = col(0xF5F5F7, 1.0f);
        t->text2 = col(0xEBEBF5, 0.62f);
        t->text3 = col(0xEBEBF5, 0.36f);
        t->accent = col(g_app.cfg.s.accentColor, 1.0f);
        t->success = col(0x32D74B, 1.0f);
        t->warning = col(0xFF9F0A, 1.0f);
        t->danger = col(0xFF453A, 1.0f);
        t->chip = col(0xFFFFFF, 0.075f);
        t->field = col(0x000000, 0.24f);
        t->separator = col(0xFFFFFF, 0.08f);
        t->scrim = col(0x000000, 0.52f);
        t->track = col(0x787880, 0.38f);
        t->shadow = col(0x000000, 0.30f);
    } else {
        t->bg = col(0xF2F2F7, 1.0f);
        t->tint = col(0xF5F5F7, u->backdrop ? 0.68f : 1.0f);
        t->sidebar = col(0xFFFFFF, 0.32f);
        t->navActive = col(0x000000, 0.06f);
        t->panel = col(0xFFFFFF, 0.64f);
        t->panelHover = col(0xFFFFFF, 0.86f);
        t->border = col(0x000000, 0.08f);
        t->highlight = col(0xFFFFFF, 0.95f);
        t->text = col(0x1D1D1F, 1.0f);
        t->text2 = col(0x3C3C43, 0.70f);
        t->text3 = col(0x3C3C43, 0.44f);
        t->accent = col(g_app.cfg.s.accentColor, 1.0f);
        t->success = col(0x28A745, 1.0f);
        t->warning = col(0xE08600, 1.0f);
        t->danger = col(0xFF3B30, 1.0f);
        t->chip = col(0x000000, 0.055f);
        t->field = col(0x000000, 0.05f);
        t->separator = col(0x000000, 0.08f);
        t->scrim = col(0x000000, 0.46f);
        t->track = col(0x787880, 0.24f);
        t->shadow = col(0x000000, 0.10f);
    }
    uint32_t accent = g_app.cfg.s.accentColor & 0xFFFFFF;
    unsigned luminance = ((accent >> 16) & 255) * 299 + ((accent >> 8) & 255) * 587 + (accent & 255) * 114;
    t->onAccent = col(luminance > 170000 ? 0x000000 : 0xFFFFFF, 1.0f);
#ifdef _WIN32
    BOOL d = dark;
    if (u->hwnd) DwmSetWindowAttribute(u->hwnd, SP_DWMWA_DARK, &d, sizeof d);
#endif
    if (!u->themeReady || u->motion == 2) {
        u->t = built;
        u->themeTarget = built;
        u->themeActive = 0;
        u->themeReady = 1;
    } else {
        int accentOnly = u->t.dark == built.dark;
        u->themeFrom = u->t;
        u->themeTarget = built;
        u->themeT0 = time_ms();
        u->themeDuration = u->motion == 1 ? 120.0f : accentOnly ? 320.0f : 260.0f;
        u->themeActive = 1;
        u->t.dark = built.dark;
        ui_animate();
    }
}

#ifdef _WIN32
static void setup_backdrop(Ui *u)
{
    DWORD corner = 2;   /* DWMWCP_ROUND */
    DwmSetWindowAttribute(u->hwnd, SP_DWMWA_CORNER, &corner, sizeof corner);
    u->backdrop = 0;
    if (windows_build_number() >= 22621) {   /* DWMWA_SYSTEMBACKDROP_TYPE: Windows 11 22H2+ */
        MARGINS m = { -1, -1, -1, -1 };
        DWORD type = SP_DWMSBT_ACRYLIC;
        if (SUCCEEDED(DwmExtendFrameIntoClientArea(u->hwnd, &m)) &&
            SUCCEEDED(DwmSetWindowAttribute(u->hwnd, SP_DWMWA_BACKDROP, &type, sizeof type)))
            u->backdrop = 1;
    }
}
#endif

/* ================================================================ device resources */

static IDWriteTextFormat *mkfmt(Ui *u, const wchar_t *family, float size, DWRITE_FONT_WEIGHT w,
                                DWRITE_TEXT_ALIGNMENT align, int wrap, int trim)
{
    IDWriteTextFormat *f = NULL;
    if (FAILED(u->dw->lpVtbl->CreateTextFormat(u->dw, family, NULL, w, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)) || !f)
        return NULL;
    f->lpVtbl->SetWordWrapping(f, wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    f->lpVtbl->SetParagraphAlignment(f, wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->lpVtbl->SetTextAlignment(f, align);
    if (trim) {
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject *sign = NULL;
        u->dw->lpVtbl->CreateEllipsisTrimmingSign(u->dw, f, &sign);
        f->lpVtbl->SetTrimming(f, &tr, sign);
        if (sign) sign->lpVtbl->Release(sign);
    }
    return f;
}

static BOOL create_text_formats(Ui *u)
{
    int win11 = windows_build_number() >= 22000;
    const wchar_t *disp = win11 ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *text = win11 ? L"Segoe UI Variable Text" : L"Segoe UI";
    u->iconFont = win11 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    u->fTitle   = mkfmt(u, disp, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fHeading = mkfmt(u, disp, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBrand   = mkfmt(u, disp, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBody    = mkfmt(u, text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fBodyB   = mkfmt(u, text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fSmall   = mkfmt(u, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fSmallB  = mkfmt(u, text, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fCaption = mkfmt(u, text, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, 0, 1);
    u->fCenter  = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 1);
    u->fCenterS = mkfmt(u, text, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 1);
    u->fWrap    = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, 1, 0);
    u->fWrapC   = mkfmt(u, text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 1, 0);
    u->fIcon    = mkfmt(u, u->iconFont, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    u->fIconS   = mkfmt(u, u->iconFont, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    u->fIconL   = mkfmt(u, u->iconFont, 36.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, 0, 0);
    return u->fTitle && u->fHeading && u->fBrand && u->fBody && u->fBodyB && u->fSmall && u->fSmallB && u->fCaption &&
           u->fCenter && u->fCenterS && u->fWrap && u->fWrapC && u->fIcon && u->fIconS && u->fIconL;
}

static void release_text_formats(Ui *u)
{
    IDWriteTextFormat **all[] = { &u->fTitle, &u->fHeading, &u->fBrand, &u->fBody, &u->fBodyB, &u->fSmall, &u->fSmallB,
                                  &u->fCaption, &u->fCenter, &u->fCenterS, &u->fWrap, &u->fWrapC, &u->fIcon, &u->fIconS,
                                  &u->fIconL };
    for (size_t i = 0; i < SP_COUNTOF(all); i++) SP_RELEASE(*all[i]);
}

static void thumb_release(Thumb *t)
{
    SP_RELEASE(t->brush);
    SP_RELEASE(t->bmp);
    for (int i = 0; i < t->frameCount; i++) SP_RELEASE(t->frames[i]);
    free(t->frames);
    free(t->delays);
    t->frames = NULL;
    t->delays = NULL;
    t->frameCount = t->cur = 0;
    t->nextAt = 0;
}

static void disc_release(void);

static void discard_device_resources(Ui *u)
{
    disc_release();
    for (int i = 0; i < u->thumbCount; i++) {
        thumb_release(&u->thumbs[i]);
        u->thumbs[i].state = 0;
    }
    SP_RELEASE(u->brush);
    SP_RELEASE(u->rt);
}

static BOOL create_device_resources(Ui *u)
{
    if (u->rt) return TRUE;
    RECT rc;
    GetClientRect(u->hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp;
    ZeroMemory(&rp, sizeof rp);
    rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rp.dpiX = rp.dpiY = 96.0f * u->scale;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp;
    hp.hwnd = u->hwnd;
    hp.pixelSize.width = (UINT32)SP_MAX(rc.right, 1);
    hp.pixelSize.height = (UINT32)SP_MAX(rc.bottom, 1);
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(u->d2d, &rp, &hp, &u->rt);
    if (FAILED(hr) && u->backdrop) {      /* no alpha support: fall back to an opaque window */
        rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        u->backdrop = 0;
        theme_build(u);
        hr = ID2D1Factory_CreateHwndRenderTarget(u->d2d, &rp, &hp, &u->rt);
    }
    if (FAILED(hr)) {
        LOGE(L"ui: CreateHwndRenderTarget failed 0x%08lX", (unsigned long)hr);
        return FALSE;
    }
    ID2D1RenderTarget_SetTextAntialiasMode(RT, u->backdrop ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    D2D1_COLOR_F white = { 1, 1, 1, 1 };
    hr = ID2D1RenderTarget_CreateSolidColorBrush(RT, &white, NULL, &u->brush);
    if (FAILED(hr)) {
        SP_RELEASE(u->rt);
        return FALSE;
    }
    return TRUE;
}

/* ================================================================ primitives */

static void set_color(D2D1_COLOR_F c)
{
    c.a *= g_ui->alpha;
    ID2D1SolidColorBrush_SetColor(g_ui->brush, &c);
}

void ui_fill(D2D1_RECT_F r, D2D1_COLOR_F c)
{
    if (c.a <= 0.0f) return;
    set_color(c);
    ID2D1RenderTarget_FillRectangle(RT, &r, (ID2D1Brush *)g_ui->brush);
}

void ui_rrect(D2D1_RECT_F r, float radius, D2D1_COLOR_F c)
{
    if (c.a <= 0.0f) return;
    D2D1_ROUNDED_RECT rr = { r, radius, radius };
    set_color(c);
    ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, (ID2D1Brush *)g_ui->brush);
}

void ui_rrect_stroke(D2D1_RECT_F r, float radius, float width, D2D1_COLOR_F c)
{
    float h = width * 0.5f;
    D2D1_ROUNDED_RECT rr = { { r.left + h, r.top + h, r.right - h, r.bottom - h }, radius, radius };
    set_color(c);
    ID2D1RenderTarget_DrawRoundedRectangle(RT, &rr, (ID2D1Brush *)g_ui->brush, width, NULL);
}

void ui_line(float x0, float y0, float x1, float y1, float width, D2D1_COLOR_F c)
{
    D2D1_POINT_2F a = { x0, y0 }, b = { x1, y1 };
    set_color(c);
    ID2D1RenderTarget_DrawLine(RT, a, b, (ID2D1Brush *)g_ui->brush, width, NULL);
}

void ui_circle(float cx, float cy, float r, D2D1_COLOR_F c)
{
    D2D1_ELLIPSE e = { { cx, cy }, r, r };
    set_color(c);
    ID2D1RenderTarget_FillEllipse(RT, &e, (ID2D1Brush *)g_ui->brush);
}

void ui_text(const wchar_t *s, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c)
{
    if (!s || !s[0] || !f) return;
    set_color(c);
    ID2D1RenderTarget_DrawText(RT, s, (UINT32)wcslen(s), f, &r, (ID2D1Brush *)g_ui->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                               DWRITE_MEASURING_MODE_NATURAL);
}

float ui_text_width(const wchar_t *s, IDWriteTextFormat *f)
{
    if (!s || !s[0] || !f) return 0.0f;
    IDWriteTextLayout *l = NULL;
    float w = 0.0f;
    if (SUCCEEDED(g_ui->dw->lpVtbl->CreateTextLayout(g_ui->dw, s, (UINT32)wcslen(s), f, 4096.0f, 200.0f, &l)) && l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->lpVtbl->GetMetrics(l, &m))) w = m.widthIncludingTrailingWhitespace;
        l->lpVtbl->Release(l);
    }
    return w;
}

float ui_text_height(const wchar_t *s, IDWriteTextFormat *f, float w)
{
    if (!s || !s[0] || !f) return 0.0f;
    IDWriteTextLayout *l = NULL;
    float h = 0.0f;
    if (SUCCEEDED(g_ui->dw->lpVtbl->CreateTextLayout(g_ui->dw, s, (UINT32)wcslen(s), f, SP_MAX(w, 1.0f), 4096.0f, &l)) && l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->lpVtbl->GetMetrics(l, &m))) h = m.height;
        l->lpVtbl->Release(l);
    }
    return h;
}

void ui_icon(wchar_t glyph, IDWriteTextFormat *f, D2D1_RECT_F r, D2D1_COLOR_F c)
{
    wchar_t s[2] = { glyph, 0 };
    ui_text(s, f, r, c);
}

void ui_glass_amount(D2D1_RECT_F r, float radius, float hover, float elevated)
{
    Theme *t = &g_ui->t;
    if (elevated > 0.01f) {
        for (int i = 0; i < 3; i++) {
            float o = 3.0f + i * 3.0f, g = (float)i * 1.5f;
            D2D1_RECT_F s = { r.left - g, r.top + o - g, r.right + g, r.bottom + o + g };
            ui_rrect(s, radius + g, col_a(t->shadow, t->shadow.a * (0.30f - i * 0.09f) * elevated));
        }
    }
    ui_rrect(r, radius, ui_motion_color(t->panel, t->panelHover, hover));
    ui_rrect_stroke(r, radius, 1.0f, col_a(t->border, t->border.a + hover * 0.05f));
    if (t->dark) {   /* thin specular edge: the "glass" rim */
        D2D1_RECT_F hl = { r.left + radius, r.top + 1.0f, r.right - radius, r.top + 2.0f };
        ui_fill(hl, t->highlight);
    }
}

void ui_glass(D2D1_RECT_F r, float radius, int hover, int elevated)
{
    ui_glass_amount(r, radius, hover ? 1.0f : 0.0f, elevated ? 1.0f : 0.0f);
}

void ui_push_clip(D2D1_RECT_F r)
{
    if (g_clipDepth < 4) g_clip[g_clipDepth] = r;
    g_clipDepth++;
    ID2D1RenderTarget_PushAxisAlignedClip(RT, &r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void ui_pop_clip(void)
{
    if (g_clipDepth > 0) g_clipDepth--;
    ID2D1RenderTarget_PopAxisAlignedClip(RT);
}

void ui_hit(int id, D2D1_RECT_F r, int focusable, int cursor)
{
    Ui *u = g_ui;
    if (g_clipDepth > 0) {
        D2D1_RECT_F c = g_clip[SP_MIN(g_clipDepth, 4) - 1];
        r.left = SP_MAX(r.left, c.left);
        r.top = SP_MAX(r.top, c.top);
        r.right = SP_MIN(r.right, c.right);
        r.bottom = SP_MIN(r.bottom, c.bottom);
        if (r.right <= r.left || r.bottom <= r.top) return;
    }
    if (u->hitCount >= UI_MAX_HITS) return;
    Hit *h = &u->hits[u->hitCount++];
    h->id = id;
    h->r = r;
    h->focusable = focusable;
    h->cursor = cursor;
}

int ui_is_hot(int id) { return g_ui->hot == id; }
int ui_is_down(int id) { return g_ui->pressed == id && g_ui->hot == id; }

float ui_motion_curve(float p, int curve)
{
    p = SP_CLAMP(p, 0.0f, 1.0f);
    if (curve == 1) return p * p * (3.0f - 2.0f * p); /* move */
    if (curve == 2) return p * p;                      /* exit */
    return 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p); /* standard enter */
}

D2D1_COLOR_F ui_motion_color(D2D1_COLOR_F a, D2D1_COLOR_F b, float p)
{
    D2D1_COLOR_F c = { a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p,
                       a.b + (b.b - a.b) * p, a.a + (b.a - a.a) * p };
    return c;
}

float ui_motion_hover(int id, int hot)
{
    Ui *u = g_ui;
    if (u->motion == 2) return hot ? 1.0f : 0.0f;
    if (time_ms() < u->hoverBlockUntil) hot = 0;
    int slot = -1, oldest = 0;
    for (int i = 0; i < (int)SP_COUNTOF(u->hover); i++) {
        if (u->hover[i].id == id) { slot = i; break; }
        if (!u->hover[i].id) oldest = i;
        else if (u->hover[i].used < u->hover[oldest].used) oldest = i;
    }
    if (slot < 0) {
        if (!hot) return 0.0f;
        slot = oldest;
        u->hover[slot].id = id;
        u->hover[slot].value = 0.0f;
    }
    u->hover[slot].used = u->frame;
    u->hover[slot].target = hot;
    if (fabsf(u->hover[slot].value - (float)hot) > 0.01f) ui_animate();
    return u->hover[slot].value;
}

void ui_focus_ring(D2D1_RECT_F r, float radius, int id)
{
    Ui *u = g_ui;
    int current = u->focusVisible && u->focus == id;
    if (current && u->focusId != id) { u->focusId = id; u->focusAmt = 0.0f; ui_animate(); }
    if (!current && u->focusId != id) return;
    float p = u->motion == 2 ? (float)current : u->focusAmt;
    if (p <= 0.01f) return;
    float grow = u->motion == 0 ? 3.0f * p : 3.0f;
    D2D1_RECT_F o = { r.left - grow, r.top - grow, r.right + grow, r.bottom + grow };
    ui_rrect_stroke(o, radius + grow, 2.0f, col_a(u->t.accent, p));
}

void ui_invalidate(void)
{
    if (g_ui && g_ui->hwnd) InvalidateRect(g_ui->hwnd, NULL, FALSE);
}

static int motion_pending(Ui *u)
{
    if (u->themeActive) return 1;
    if (fabsf(u->filterScrollLag) > 0.05f) return 1;
    if (fabsf(u->depthFill - u->depthTarget) > 0.002f) return 1;
    if (u->pageAnim < 1.0f) return 1;
    if (fabsf(u->pressAmt - (u->pressed == u->pressId && u->pressId ? 1.0f : 0.0f)) > 0.01f) return 1;
    if (fabsf(u->focusAmt - (u->focusVisible && u->focus == u->focusId ? 1.0f : 0.0f)) > 0.01f) return 1;
    for (int i = 0; i < (int)SP_COUNTOF(u->hover); i++)
        if (u->hover[i].id && fabsf(u->hover[i].value -
            (u->hover[i].used == u->frame ? (float)u->hover[i].target : 0.0f)) > 0.01f) return 1;
    for (int i = 0; i < (int)SP_COUNTOF(u->cardMotion); i++)
        if (u->cardMotion[i].id[0] && fabsf(u->cardMotion[i].selection - (float)u->cardMotion[i].selected) > 0.01f) return 1;
    for (int i = 0; i < (int)SP_COUNTOF(u->chipMotion); i++)
        if (u->chipMotion[i].id && fabsf(u->chipMotion[i].value - (float)u->chipMotion[i].active) > 0.01f) return 1;
    for (int i = 0; i < (int)SP_COUNTOF(u->storeFill); i++)
        if (u->storeFill[i].id && fabsf(u->storeFill[i].value - u->storeFill[i].target) > 0.002f) return 1;
    for (int p = 0; p < PAGE_COUNT; p++)
        if (fabsf(u->scrollTarget[p] - u->scroll[p]) > 0.5f) return 1;
    for (int i = 0; i < TOG_COUNT; i++)
        if (fabsf(u->togTarget[i] - u->tog[i]) > 0.005f || fabsf(u->togVelocity[i]) > 0.02f) return 1;
    return 0;
}

static void motion_stop(Ui *u)
{
    if (u->animating) KillTimer(u->hwnd, TM_ANIM);
    u->animating = 0;
}

static void motion_snap(Ui *u)
{
    int changed = motion_pending(u);
    if (u->themeActive) { u->t = u->themeTarget; u->themeActive = 0; }
    u->depthFill = u->depthTarget;
    u->filterScrollLag = 0.0f;
    u->pageAnim = 1.0f;
    for (int p = 0; p < PAGE_COUNT; p++) u->scroll[p] = u->scrollTarget[p];
    for (int i = 0; i < TOG_COUNT; i++) { u->tog[i] = u->togTarget[i]; u->togVelocity[i] = 0.0f; }
    for (int i = 0; i < (int)SP_COUNTOF(u->hover); i++)
        u->hover[i].value = (float)u->hover[i].target;
    for (int i = 0; i < (int)SP_COUNTOF(u->cardMotion); i++)
        u->cardMotion[i].selection = (float)u->cardMotion[i].selected;
    for (int i = 0; i < (int)SP_COUNTOF(u->chipMotion); i++)
        u->chipMotion[i].value = (float)u->chipMotion[i].active;
    for (int i = 0; i < (int)SP_COUNTOF(u->storeFill); i++) u->storeFill[i].value = u->storeFill[i].target;
    u->pressAmt = 0.0f;
    u->focusAmt = u->focusVisible ? 1.0f : 0.0f;
    motion_stop(u);
    if (changed) ui_invalidate();
}

#ifndef _WIN32
static int motion_file_enabled(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char line[256];
    int enabled = 1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, n) != 0) continue;
        p += n;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        double factor = strtod(p, &end);
        if (strncmp(p, "false", 5) == 0 || (end != p && factor == 0.0)) enabled = 0;
    }
    fclose(f);
    return enabled;
}

static int linux_motion_enabled(void)
{
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[1024], path[1200];
    if (cfg && *cfg) snprintf(base, sizeof base, "%s", cfg);
    else if (home && *home) snprintf(base, sizeof base, "%s/.config", home);
    else return 1;
    snprintf(path, sizeof path, "%s/gtk-3.0/settings.ini", base);
    if (!motion_file_enabled(path, "gtk-enable-animations")) return 0;
    snprintf(path, sizeof path, "%s/gtk-4.0/settings.ini", base);
    if (!motion_file_enabled(path, "gtk-enable-animations")) return 0;
    snprintf(path, sizeof path, "%s/kdeglobals", base);
    return motion_file_enabled(path, "AnimationDurationFactor");
}
#endif

void ui_refresh_motion(void)
{
    Ui *u = g_ui;
    if (!u) return;
    u->motionSystem = 1;
#ifdef _WIN32
    BOOL client = TRUE;
    ANIMATIONINFO info = { sizeof info };
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &client, 0) && !client) u->motionSystem = 0;
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof info, &info, 0) && !info.iMinAnimate) u->motionSystem = 0;
#else
    u->motionSystem = linux_motion_enabled();
#endif
    u->motion = g_app.cfg.s.uiMotion;
    if (u->motion == 0 && !u->motionSystem) u->motion = 1;
    if (u->motion == 2) motion_snap(u);
    else if (motion_pending(u)) ui_animate();
}

void ui_motion_system_changed(void) { ui_refresh_motion(); }

void ui_animate(void)
{
    Ui *u = g_ui;
    if (!u || !u->hwnd) return;
    if (u->motion == 2) { motion_snap(u); return; }
    if (u->motion == 1) {
        int changed = 0;
        for (int p = 0; p < PAGE_COUNT; p++) {
            if (u->scroll[p] != u->scrollTarget[p]) changed = 1;
            u->scroll[p] = u->scrollTarget[p];
        }
        for (int i = 0; i < TOG_COUNT; i++) {
            if (u->tog[i] != u->togTarget[i]) changed = 1;
            u->tog[i] = u->togTarget[i];
            u->togVelocity[i] = 0.0f;
        }
        if (changed) ui_invalidate();
    }
    if (u->animating || !IsWindowVisible(u->hwnd) || IsIconic(u->hwnd)) return;
    u->animating = 1;
    u->lastTick = time_ms();
    SetTimer(u->hwnd, TM_ANIM, 15, NULL);
}

const wchar_t *ui_type_icon(WallpaperType t)
{
    static wchar_t s[2];
    s[0] = t == WP_VIDEO ? GL_VIDEO : t == WP_HTML ? GL_CODE : GL_PHOTO;
    s[1] = 0;
    return s;
}

/* ================================================================ widgets */

float ui_button_width(const wchar_t *label, wchar_t icon)
{
    float w = label && label[0] ? ui_text_width(label, g_ui->fCenter) + 30.0f : 34.0f;
    if (icon && label && label[0]) w += 22.0f;
    return w;
}

void ui_button(int id, D2D1_RECT_F r, const wchar_t *label, wchar_t icon, int style)
{
    Theme *t = &g_ui->t;
    int hot = ui_is_hot(id);
    float hp = ui_motion_hover(id, hot);
    D2D1_COLOR_F bg, fg = t->text;
    float radius = 9.0f;
    switch (style) {
    case BTN_PRIMARY:
        bg = col_a(t->accent, 1.0f - hp * 0.12f);
        fg = t->onAccent;
        break;
    case BTN_GHOST:
        bg = col_a(t->chip, t->chip.a * hp);
        fg = ui_motion_color(t->text2, t->text, hp);
        break;
    case BTN_DANGER:
        bg = col_a(t->danger, 0.14f + hp * 0.08f);
        fg = t->danger;
        break;
    default:
        bg = ui_motion_color(t->chip, t->panelHover, hp);
        break;
    }
    float press = g_ui->motion == 0 && g_ui->pressId == id ? g_ui->pressAmt : 0.0f;
    float scale = 1.0f - press * 0.015f;
    if (press > 0.001f) {
        float cx = (r.left + r.right) * 0.5f, cy0 = (r.top + r.bottom) * 0.5f;
        D2D1_MATRIX_3X2_F m = { scale, 0, 0, scale, cx * (1.0f - scale), cy0 * (1.0f - scale) };
        ID2D1RenderTarget_SetTransform(RT, &m);
    }
    ui_rrect(r, radius, bg);
    if (g_ui->progressId == id && g_ui->progressFill > 0.0f)
        ui_rrect(rectf(r.left, r.top, (r.right - r.left) * g_ui->progressFill, r.bottom - r.top), radius,
                 col_a(t->accent, 0.22f));
    if (style == BTN_SECONDARY) ui_rrect_stroke(r, radius, 1.0f, col_a(t->border, t->border.a + hp * 0.05f));
    float cy = r.top, h = r.bottom - r.top;
    if (label && label[0]) {
        float tw = ui_text_width(label, g_ui->fCenter);
        float total = tw + (icon ? 22.0f : 0.0f);
        float x = (r.left + r.right) * 0.5f - total * 0.5f;
        if (icon) {
            ui_icon(icon, g_ui->fIconS, rectf(x, cy, 16.0f, h), fg);
            x += 22.0f;
        }
        ui_text(label, g_ui->fCenter, rectf(x - 2.0f, cy, tw + 4.0f, h), fg);
    } else if (icon) {
        ui_icon(icon, g_ui->fIcon, r, fg);
    }
    if (press > 0.001f) {
        D2D1_MATRIX_3X2_F identity = { 1, 0, 0, 1, 0, 0 };
        ID2D1RenderTarget_SetTransform(RT, &identity);
    }
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, radius, id);
}

void ui_chip(int id, D2D1_RECT_F r, const wchar_t *label, int active)
{
    Theme *t = &g_ui->t;
    int hot = ui_is_hot(id);
    float hp = ui_motion_hover(id, hot);
    int slot = -1, oldest = 0;
    for (int i = 0; i < (int)SP_COUNTOF(g_ui->chipMotion); i++) {
        if (g_ui->chipMotion[i].id == id) { slot = i; break; }
        if (!g_ui->chipMotion[i].id) oldest = i;
        else if (g_ui->chipMotion[i].used < g_ui->chipMotion[oldest].used) oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        g_ui->chipMotion[slot].id = id;
        g_ui->chipMotion[slot].value = (float)active;
        g_ui->chipMotion[slot].active = active;
    }
    g_ui->chipMotion[slot].used = g_ui->frame;
    if (g_ui->chipMotion[slot].active != active) {
        g_ui->chipMotion[slot].active = active;
        ui_animate();
    }
    float fill = g_ui->motion == 2 ? (float)active : g_ui->chipMotion[slot].value;
    float radius = (r.bottom - r.top) * 0.5f;
    ui_rrect(r, radius, ui_motion_color(ui_motion_color(t->chip, t->panelHover, hp), t->accent, fill));
    if (fill < 0.99f) ui_rrect_stroke(r, radius, 1.0f, col_a(t->border, t->border.a * 0.8f * (1.0f - fill)));
    ui_text(label, g_ui->fCenter, r, ui_motion_color(ui_motion_color(t->text2, t->text, hp), t->onAccent, fill));
    ui_hit(id, r, 1, 1);
    ui_focus_ring(r, radius, id);
}

void ui_toggle(int id, float x, float y, int index, int on)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    float target = on ? 1.0f : 0.0f;
    if (index >= 0 && index < TOG_COUNT) {
        if (u->tog[index] < 0.0f) u->tog[index] = target;   /* first frame: no animation */
        u->togTarget[index] = target;
        if (u->motion != 0) { u->tog[index] = target; u->togVelocity[index] = 0.0f; }
        if (fabsf(u->tog[index] - target) > 0.001f) ui_animate();
    }
    float p = index >= 0 && index < TOG_COUNT ? u->tog[index] : target;
    float cp = SP_CLAMP(p, 0.0f, 1.0f);
    D2D1_RECT_F track = rectf(x, y, 42.0f, 24.0f);
    D2D1_COLOR_F off = t->track, onc = t->accent;
    D2D1_COLOR_F c = ui_motion_color(off, onc, cp);
    ui_rrect(track, 12.0f, c);
    float kx = x + 12.0f + p * 18.0f;
    ui_circle(kx, y + 12.5f, 10.0f, col(0x000000, 0.12f));
    ui_circle(kx, y + 12.0f, 10.0f, col(0xFFFFFF, 1.0f));
    D2D1_RECT_F hr = { x - 4, y - 4, x + 46, y + 28 };
    ui_hit(id, hr, 1, 1);
    ui_focus_ring(track, 12.0f, id);
}

void ui_pill(D2D1_RECT_F r, const wchar_t *label, D2D1_COLOR_F bg, D2D1_COLOR_F fg)
{
    ui_rrect(r, (r.bottom - r.top) * 0.5f, bg);
    ui_text(label, g_ui->fCenterS, r, fg);
}

/* ---------------------------------------------------------------- colour helpers */

D2D1_COLOR_F ui_hsv(float h, float s, float v, float a)
{
    h = h - floorf(h / 360.0f) * 360.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return (D2D1_COLOR_F){ r + m, g + m, b + m, a };
}

void ui_rgb_hsv(D2D1_COLOR_F c, float *hue, float *s, float *v)
{
    float mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    float mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    float d = mx - mn;
    float h = 0;
    if (d > 0) {
        if (mx == c.r) h = 60.0f * fmodf((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = 60.0f * ((c.b - c.r) / d + 2.0f);
        else h = 60.0f * ((c.r - c.g) / d + 4.0f);
    }
    if (h < 0) h += 360.0f;
    *hue = h;
    *s = mx > 0 ? d / mx : 0.0f;
    *v = mx;
}

/* ---------------------------------------------------------------- sliders + colour wheel */

/* Track + filled portion + WinUI-style thumb. While pressed the value follows the pointer via
 * pages_drag (pressed hit rect -> position), committed live and on release. */
void ui_slider(int id, D2D1_RECT_F r, float v)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    v = SP_CLAMP(v, 0.0f, 1.0f);
    int hot = ui_is_hot(id), down = u->pressed == id;
    float hp = ui_motion_hover(id, hot || down);
    float cy = (r.top + r.bottom) * 0.5f, inset = 11.0f;
    D2D1_RECT_F track = rectf(r.left + inset, cy - 2.0f, (r.right - r.left) - inset * 2.0f, 4.0f);
    ui_rrect(track, 2.0f, t->track);
    ui_rrect(rectf(track.left, cy - 2.0f, (track.right - track.left) * v, 4.0f), 2.0f, t->accent);
    float kx = track.left + (track.right - track.left) * v;
    ui_circle(kx, cy + 0.5f, 10.5f, col(0x000000, t->dark ? 0.40f : 0.14f));
    ui_circle(kx, cy, 10.0f, t->dark ? col(0x454545, 1.0f) : col(0xFFFFFF, 1.0f));
    ui_circle(kx, cy, down ? 7.5f : 5.5f + hp, t->accent);
    ui_hit(id, r, 1, 1);
    ui_focus_ring(track, 4.0f, id);
}

static ID2D1Bitmap *g_disc;          /* hue/saturation wheel, rendered at device pixels */
static int          g_discPx;        /* its edge length in pixels: rebuilt when the size or DPI changes */

static void disc_release(void)
{
    if (g_disc) ID2D1Bitmap_Release(g_disc);
    g_disc = NULL;
    g_discPx = 0;
}

/* Hue around the circle (0 = red at 3 o'clock, clockwise like the picking maths), saturation
 * from white at the centre to full colour at the rim. Premultiplied BGRA, anti-aliased rim,
 * fully transparent outside. */
static void disc_build(Ui *u, int px)
{
    disc_release();
    BYTE *bits = (BYTE *)calloc((size_t)px * px, 4);
    if (!bits) return;
    float half = px * 0.5f;
    for (int y = 0; y < px; y++) {
        for (int x = 0; x < px; x++) {
            float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
            float len = sqrtf(dx * dx + dy * dy);
            float a = SP_CLAMP((1.0f - len) * half + 0.5f, 0.0f, 1.0f);   /* ~1 px soft edge */
            if (a <= 0.0f) continue;
            D2D1_COLOR_F c = ui_hsv(atan2f(dy, dx) * 180.0f / 3.14159265f, SP_MIN(len, 1.0f), 1.0f, 1.0f);
            BYTE *p = bits + ((size_t)y * px + x) * 4;
            p[0] = (BYTE)(c.b * a * 255.0f + 0.5f);
            p[1] = (BYTE)(c.g * a * 255.0f + 0.5f);
            p[2] = (BYTE)(c.r * a * 255.0f + 0.5f);
            p[3] = (BYTE)(a * 255.0f + 0.5f);
        }
    }
    D2D1_BITMAP_PROPERTIES pr;
    ZeroMemory(&pr, sizeof pr);
    pr.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    pr.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    pr.dpiX = pr.dpiY = 96.0f * u->scale;
    D2D1_SIZE_U size = { (UINT32)px, (UINT32)px };
    ID2D1Bitmap *bmp = NULL;
    if (SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, size, bits, (UINT32)px * 4, &pr, &bmp))) {
        g_disc = bmp;
        g_discPx = px;
    }
    free(bits);
}

void ui_disc(int id, float cx, float cy, float radius, float hue, float sat)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    int px = (int)ceilf(radius * 2.0f * u->scale);
    if (!g_disc || g_discPx != px) disc_build(u, px);
    ui_circle(cx, cy, radius + 1.5f, t->border);                 /* hairline rim */
    D2D1_RECT_F dr = rectf(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    if (g_disc) ID2D1RenderTarget_DrawBitmap(RT, g_disc, &dr, u->alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    /* marker: hue angle + saturation distance from the centre, filled with the picked colour */
    float a = hue * 3.14159265f / 180.0f, dist = SP_CLAMP(sat, 0.0f, 1.0f) * radius;
    float mx = cx + cosf(a) * dist, my = cy + sinf(a) * dist;
    float grow = ui_is_down(id) ? 1.5f : 0.0f;
    ui_circle(mx, my + 0.5f, 9.0f + grow, col(0x000000, 0.30f));
    ui_circle(mx, my, 8.0f + grow, col(0xFFFFFF, 1.0f));
    ui_circle(mx, my, 5.5f + grow, ui_hsv(hue, sat, 1.0f, 1.0f));
    ui_hit(id, dr, 1, 1);
    ui_focus_ring(rectf(cx - radius - 2, cy - radius - 2, radius * 2 + 4, radius * 2 + 4), radius + 2, id);
}

/* ================================================================ thumbnails */

static Thumb *thumb_get(const wchar_t *id, int large)
{
    Ui *u = g_ui;
    for (int i = 0; i < u->thumbCount; i++)
        if (u->thumbs[i].large == large && wcscmp(u->thumbs[i].id, id) == 0) return &u->thumbs[i];
    if (u->thumbCount >= MAX_THUMBS) {          /* evict the least recently drawn */
        int old = -1;
        for (int i = 0; i < u->thumbCount; i++)
            if (u->thumbs[i].used != u->frame && (old < 0 || u->thumbs[i].used < u->thumbs[old].used)) old = i;
        if (old < 0) return NULL;
        thumb_release(&u->thumbs[old]);
        u->thumbs[old] = u->thumbs[--u->thumbCount];
    }
    if (u->thumbCount == u->thumbCap) {
        int nc = u->thumbCap ? u->thumbCap * 2 : 32;
        Thumb *p = (Thumb *)realloc(u->thumbs, sizeof(Thumb) * (size_t)nc);
        if (!p) return NULL;
        u->thumbs = p;
        u->thumbCap = nc;
    }
    Thumb *t = &u->thumbs[u->thumbCount++];
    ZeroMemory(t, sizeof *t);
    str_copy(t->id, SP_ID_LEN, id);
    t->large = large;
    return t;
}

/* Like ui_draw_thumb's large one, but asked for in the picture's own shape (a thumbnail is a cover crop of the size
 * asked for) and drawn stretched over r. */
BOOL ui_draw_thumb_stretch(const WallpaperEntry *e, D2D1_RECT_F r, float aspect, float opacity)
{
    Ui *u = g_ui;
    float rw = r.right - r.left, rh = r.bottom - r.top;
    Thumb *th = thumb_get(e->id, 1);
    if (!th || aspect <= 0.0f || rw < 1.0f || rh < 1.0f) return FALSE;
    th->used = u->frame;
    float big = SP_MAX(rw, rh) * u->scale;
    int pw = ((int)(aspect >= 1.0f ? big : big * aspect) + 15) & ~15, ph = ((int)(aspect >= 1.0f ? big / aspect : big) + 15) & ~15;
    pw = SP_CLAMP(pw, 16, 1024);
    ph = SP_CLAMP(ph, 16, 1024);
    if (th->state == 2 && (pw > th->reqW * 5 / 4 || ph > th->reqH * 5 / 4 ||
                           fabsf((float)th->reqW / SP_MAX(th->reqH, 1) - (float)pw / ph) > 0.03f * pw / ph)) th->state = 0;
    if (th->state == 1 && (th->reqW <= 0 || th->reqH <= 0)) th->state = 0;   /* a request that was never sent */
    if (th->state == 0) {
        th->state = 1;
        th->reqW = pw;
        th->reqH = ph;
        thumbs_request(e, pw, ph, 1);
    }
    if (th->state != 2 || !th->brush) return FALSE;
    if (th->frameCount > 1) {
        ID2D1BitmapBrush_SetBitmap(th->brush, th->frames[th->cur]);
        if (u->motion == 0 && !u->thumbTimer) {
            u->thumbTimer = 1;
            SetTimer(u->hwnd, TM_THUMBS, 33, NULL);
        }
    }
    D2D1_MATRIX_3X2_F m = { rw / th->w, 0, 0, rh / th->h, r.left, r.top };
    ID2D1BitmapBrush_SetTransform(th->brush, &m);
    ID2D1BitmapBrush_SetOpacity(th->brush, u->alpha * opacity);
    ID2D1RenderTarget_FillRectangle(RT, &r, (ID2D1Brush *)th->brush);
    return TRUE;
}

void ui_draw_thumb(const WallpaperEntry *e, D2D1_RECT_F r, float radius, int large)
{
    Ui *u = g_ui;
    Theme *t = &u->t;
    float rw = r.right - r.left, rh = r.bottom - r.top;
    Thumb *th = thumb_get(e->id, large);
    if (th) {
        th->used = u->frame;
        int pw = ((int)(rw * u->scale) + 15) & ~15, ph = ((int)(rh * u->scale) + 15) & ~15;
        if (th->state == 2 && (pw > th->reqW * 5 / 4 || ph > th->reqH * 5 / 4)) th->state = 0;   /* grew: reload sharper */
        if (th->state == 0 && pw > 0 && ph > 0) {    /* an empty rect asks for nothing (it would never be answered) */
            th->state = 1;
            th->reqW = pw;      /* animations are stored smaller than requested: compare with the request */
            th->reqH = ph;
            thumbs_request(e, pw, ph, large);
        }
    }
    if (th && th->state == 2 && th->brush) {
        if (th->frameCount > 1) {
            ID2D1BitmapBrush_SetBitmap(th->brush, th->frames[th->cur]);
            if (u->motion == 0 && !u->thumbTimer) {
                u->thumbTimer = 1;
                SetTimer(u->hwnd, TM_THUMBS, 33, NULL);   /* ~30 Hz: due frames advance together */
            }
        }
        float s = SP_MAX(rw / th->w, rh / th->h);
        D2D1_MATRIX_3X2_F m = { s, 0, 0, s, r.left + (rw - th->w * s) * 0.5f, r.top + (rh - th->h * s) * 0.5f };
        ID2D1BitmapBrush_SetTransform(th->brush, &m);
        float fade = 1.0f;
        if (u->motion != 2 && th->readyAt > 0) {
            float p = (float)((time_ms() - th->readyAt) / 180.0);
            fade = ui_motion_curve(p, 0);
            if (p < 1.0f) ui_animate();
        }
        ID2D1BitmapBrush_SetOpacity(th->brush, u->alpha * fade);
        D2D1_ROUNDED_RECT rr = { r, radius, radius };
        ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, (ID2D1Brush *)th->brush);
        return;
    }
    ui_rrect(r, radius, t->chip);
    wchar_t g = e->type == WP_VIDEO ? GL_VIDEO : e->type == WP_HTML ? GL_CODE : GL_PHOTO;
    ui_icon(g, rh > 120 ? u->fIconL : u->fIcon, r, t->text3);
}

/* Replaces a thumbnail's still with its animation; on any failure the still stays. */
static void set_animation(Ui *u, Thumb *t, ThumbResult *res)
{
    if (!u->rt) return;
    ID2D1Bitmap **frames = (ID2D1Bitmap **)calloc((size_t)res->frames, sizeof *frames);
    ID2D1BitmapBrush *brush = NULL;
    D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.0f, 96.0f };
    D2D1_SIZE_U sz = { (UINT32)res->w, (UINT32)res->h };
    size_t fsz = (size_t)res->w * res->h * 4;
    int n = 0;
    while (frames && n < res->frames &&
           SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, sz, res->bgra + fsz * (size_t)n, (UINT32)res->w * 4, &bp, &frames[n])))
        n++;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
    if (n == 0 || n != res->frames || FAILED(ID2D1RenderTarget_CreateBitmapBrush(RT, frames[0], &bbp, NULL, &brush))) {
        for (int i = 0; i < n; i++) SP_RELEASE(frames[i]);
        free(frames);
        return;
    }
    /* Over budget: the least recently drawn other animation goes (it reloads when seen again). */
    int animated = 0, lru = -1;
    for (int i = 0; i < u->thumbCount; i++) {
        Thumb *o = &u->thumbs[i];
        if (o == t || o->frameCount < 2) continue;
        animated++;
        if (o->used != u->frame && (lru < 0 || o->used < u->thumbs[lru].used)) lru = i;
    }
    if (animated >= MAX_ANIMATED && lru >= 0) {
        thumb_release(&u->thumbs[lru]);
        u->thumbs[lru].state = 0;
    }
    thumb_release(t);
    t->frames = frames;
    t->frameCount = n;
    t->delays = res->delays;
    res->delays = NULL;
    t->brush = brush;
    t->w = res->w;
    t->h = res->h;
    t->state = 2;
    if (!t->readyAt) t->readyAt = time_ms();
    ui_invalidate();
}

/* Advances the animated thumbnails drawn in the last frame; stops when none are on screen. */
static void thumbs_tick(Ui *u)
{
    double now = time_ms();
    int onScreen = 0, changed = 0;
    if (IsWindowVisible(u->hwnd) && !IsIconic(u->hwnd)) {
        for (int i = 0; i < u->thumbCount; i++) {
            Thumb *t = &u->thumbs[i];
            if (t->frameCount < 2 || t->state != 2 || t->used != u->frame) continue;
            onScreen = 1;
            if (!t->nextAt) t->nextAt = now + t->delays[t->cur];
            if (now < t->nextAt) continue;
            t->cur = (t->cur + 1) % t->frameCount;
            double due = t->nextAt + t->delays[t->cur];
            t->nextAt = due < now ? now + t->delays[t->cur] : due;   /* fell behind (e.g. hidden): resync */
            changed = 1;
        }
    }
    if (changed) ui_invalidate();
    if (!onScreen) {
        KillTimer(u->hwnd, TM_THUMBS);
        u->thumbTimer = 0;
    }
}

static void on_thumb(Ui *u, ThumbResult *res)
{
    for (int i = 0; i < u->thumbCount; i++) {
        Thumb *t = &u->thumbs[i];
        if (t->large != res->large || wcscmp(t->id, res->id) != 0) continue;
        if (res->frames > 1) {
            if (res->bgra && res->delays) set_animation(u, t, res);
            break;
        }
        thumb_release(t);
        t->state = 3;
        if (res->bgra && u->rt) {
            D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.0f, 96.0f };
            D2D1_SIZE_U sz = { (UINT32)res->w, (UINT32)res->h };
            if (SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, sz, res->bgra, (UINT32)res->w * 4, &bp, &t->bmp))) {
                D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
                if (SUCCEEDED(ID2D1RenderTarget_CreateBitmapBrush(RT, t->bmp, &bbp, NULL, &t->brush))) {
                    t->w = res->w;
                    t->h = res->h;
                    t->state = 2;
                    if (!t->readyAt) t->readyAt = time_ms();
                }
            }
        }
        ui_invalidate();
        break;
    }
    thumbs_free(res);
}

/* ================================================================ painting */

static void paint(Ui *u)
{
    double paintAt = time_ms();
    if (!create_device_resources(u)) return;
    u->frame++;
    u->hitCount = 0;
    g_clipDepth = 0;
    ID2D1RenderTarget_BeginDraw(RT);
    D2D1_MATRIX_3X2_F identity = { 1, 0, 0, 1, 0, 0 };
    ID2D1RenderTarget_SetTransform(RT, &identity);
    D2D1_COLOR_F clear = u->backdrop ? col(0, 0.0f) : u->t.bg;
    ID2D1RenderTarget_Clear(RT, &clear);
    u->alpha = 1.0f;
    if (u->backdrop) ui_fill(rectf(0, 0, u->w, u->h), u->t.tint);
    pages_draw();
    HRESULT hr = ID2D1RenderTarget_EndDraw(RT, NULL, NULL);
    if (u->trace)
        LOGI(L"ui trace: paint %.2f ms; page=%d scroll=%d theme=%d flow=%d ghost=%d notice=%d timer=%d",
             time_ms() - paintAt, u->pageAnim < 1.0f, motion_pending(u), u->themeActive,
             u->flowT0 > 0, u->ghost.active, u->noticeT0 > 0, u->animating);
    if (hr == D2DERR_RECREATE_TARGET) {
        LOGW(L"ui: render target lost; recreating");
        discard_device_resources(u);
        ui_invalidate();
    }
    if (!g_app.uiReadyMs) {
        g_app.uiReadyMs = (double)(filetime_now() - g_app.processStart) / 10000.0;
        LOGI(L"ui: first frame %.1f ms after process start (window open took %.1f ms)", g_app.uiReadyMs,
             time_ms() - u->showRequestedAt);
    }
}

static void anim_step(Ui *u)
{
    if (!IsWindowVisible(u->hwnd) || IsIconic(u->hwnd)) { motion_stop(u); return; }
    double now = time_ms();
    float dt = (float)SP_MIN(now - u->lastTick, 50.0) / 1000.0f;
    u->lastTick = now;
    int active = 0;
    if (fabsf(u->depthFill - u->depthTarget) > 0.002f) active = 1;
    if (u->flowT0 > 0 && now - u->flowT0 < 220.0) active = 1;
    if (u->ghost.active && now - u->ghost.t0 < 320.0) active = 1;
    if (u->heroFadeAt > 0 && now - u->heroFadeAt < 180.0) active = 1;
    if (u->brandAt > 0 && now - u->brandAt < 580.0) active = 1;
    if (u->emptyPulseAt > 0 && now - u->emptyPulseAt < 2400.0 && u->motion == 0) active = 1;
    if (u->presetAt > 0 && now - u->presetAt < (u->motion == 0 ? 640.0 : 120.0) && u->motion != 2) active = 1;
    if (u->statusAt > 0 && now - u->statusAt < 8000.0 && u->motion == 0) active = 1;
    if (u->navT0 > 0 && now - u->navT0 < 220.0) active = 1;
    if (fabsf(u->filterScrollLag) > 0.05f) {
        u->filterScrollLag *= 1.0f - SP_MIN(1.0f, dt * 18.0f);
        active = 1;
    } else u->filterScrollLag = 0.0f;
    if (u->filterPillAt > 0 && now - u->filterPillAt < 200.0) active = 1;
    if (u->noticeT0 > 0 && now - u->noticeT0 < 220.0) active = 1;
    if (u->urlT0 > 0 && now - u->urlT0 < 220.0) active = 1;
    if (u->depthListT0 > 0 && now - u->depthListT0 < 220.0) active = 1;
    if (u->page == PAGE_STORE && u->storeSkeletonAt > 0 && now - u->storeSkeletonAt < 8000.0 && u->motion == 0) active = 1;
    if (g_app.noticeKind == NOTICE_INFO && !g_app.noticeHover && u->motion != 2 &&
        now - g_app.noticeAt < g_app.noticeRemaining) active = 1;
    for (int i = 0; i < (int)SP_COUNTOF(u->segment); i++)
        if (u->segment[i].t0 > 0 && now - u->segment[i].t0 < 200.0) active = 1;
    if (u->themeActive) {
        float p = (float)((now - u->themeT0) / u->themeDuration);
        float eased = ui_motion_curve(p, 0);
        D2D1_COLOR_F *dst = &u->t.bg;
        const D2D1_COLOR_F *a = &u->themeFrom.bg, *b = &u->themeTarget.bg;
        for (size_t i = 0; i < (sizeof(Theme) - offsetof(Theme, bg)) / sizeof(D2D1_COLOR_F); i++)
            dst[i] = ui_motion_color(a[i], b[i], eased);
        u->t.dark = u->themeTarget.dark;
        if (p >= 1.0f) { u->t = u->themeTarget; u->themeActive = 0; }
        else active = 1;
    }
    float pressTarget = u->pressId && u->pressed == u->pressId ? 1.0f : 0.0f;
    if (fabsf(u->pressAmt - pressTarget) > 0.01f) {
        u->pressAmt += (pressTarget - u->pressAmt) * SP_MIN(1.0f, dt * (pressTarget ? 20.0f : 15.0f));
        active = 1;
    } else {
        u->pressAmt = pressTarget;
        if (!pressTarget) u->pressId = 0;
    }
    float focusTarget = u->focusId && u->focusVisible && u->focus == u->focusId ? 1.0f : 0.0f;
    if (fabsf(u->focusAmt - focusTarget) > 0.01f) {
        u->focusAmt += (focusTarget - u->focusAmt) * SP_MIN(1.0f, dt * (focusTarget ? 18.0f : 12.0f));
        active = 1;
    } else {
        u->focusAmt = focusTarget;
        if (!focusTarget) u->focusId = 0;
    }
    for (int i = 0; i < (int)SP_COUNTOF(u->hover); i++) {
        if (!u->hover[i].id) continue;
        if (u->hover[i].used != u->frame) u->hover[i].target = 0;
        float target = u->hover[i].target && now >= u->hoverBlockUntil ? 1.0f : 0.0f;
        float d = target - u->hover[i].value;
        if (fabsf(d) > 0.01f) {
            u->hover[i].value += d * SP_MIN(1.0f, dt * (now < u->hoverBlockUntil ? 18.0f : 12.0f));
            active = 1;
        } else {
            u->hover[i].value = target;
            if (!target && u->hover[i].used + 5 < u->frame) u->hover[i].id = 0;
        }
    }
    for (int i = 0; i < (int)SP_COUNTOF(u->cardMotion); i++) {
        if (!u->cardMotion[i].id[0]) continue;
        float target = (float)u->cardMotion[i].selected;
        float d = target - u->cardMotion[i].selection;
        if (fabsf(d) > 0.01f) {
            u->cardMotion[i].selection += d * SP_MIN(1.0f, dt * 30.0f);
            active = 1;
        } else u->cardMotion[i].selection = target;
    }
    for (int i = 0; i < (int)SP_COUNTOF(u->chipMotion); i++) {
        if (!u->chipMotion[i].id) continue;
        float target = (float)u->chipMotion[i].active;
        float d = target - u->chipMotion[i].value;
        if (fabsf(d) > 0.01f) {
            u->chipMotion[i].value += d * SP_MIN(1.0f, dt * 18.0f);
            active = 1;
        } else u->chipMotion[i].value = target;
    }
    for (int i = 0; i < (int)SP_COUNTOF(u->storeFill); i++)
        if (u->storeFill[i].id && fabsf(u->storeFill[i].value - u->storeFill[i].target) > 0.002f) active = 1;
    if (u->pageAnim < 1.0f) {
        u->pageAnim = SP_MIN(1.0f, u->pageAnim + dt / (u->motion == 1 ? 0.12f : 0.30f));
        active = 1;
    }
    for (int p = 0; p < PAGE_COUNT; p++) {
        float d = u->scrollTarget[p] - u->scroll[p];
        if (u->motion == 1) { u->scroll[p] = u->scrollTarget[p]; continue; }
        if (fabsf(d) > 0.5f) {
            u->scroll[p] += d * SP_MIN(1.0f, dt * 14.0f);
            active = 1;
        } else {
            u->scroll[p] = u->scrollTarget[p];
        }
    }
    for (int i = 0; i < TOG_COUNT; i++) {
        float d = u->togTarget[i] - u->tog[i];
        if (u->motion == 1) { u->tog[i] = u->togTarget[i]; u->togVelocity[i] = 0.0f; continue; }
        if (fabsf(d) > 0.005f || fabsf(u->togVelocity[i]) > 0.02f) {
            u->togVelocity[i] += (d * 850.0f - u->togVelocity[i] * 42.0f) * dt;
            u->tog[i] = SP_CLAMP(u->tog[i] + u->togVelocity[i] * dt, -0.03f, 1.03f);
            active = 1;
        } else {
            u->tog[i] = u->togTarget[i];
            u->togVelocity[i] = 0.0f;
        }
    }
    if (!active) motion_stop(u);
    ui_invalidate();
}

/* ================================================================ input */

static int hit_at(Ui *u, float x, float y)
{
    for (int i = u->hitCount - 1; i >= 0; i--)
        if (in_rect(u->hits[i].r, x, y)) return u->hits[i].id;
    return 0;
}

static const Hit *hit_find(Ui *u, int id)
{
    for (int i = 0; i < u->hitCount; i++)
        if (u->hits[i].id == id) return &u->hits[i];
    return NULL;
}

/* Every field that supports typing goes through the text engine (textedit.c). */
static int is_text_kind(int kind)
{
    return kind == HK_SEARCH || kind == HK_URL_TEXT || kind == HK_NAME || kind == HK_STORE_TEXT || kind == HK_COLL_TEXT ||
           kind == HK_PTEXT || kind == HK_PVAL || kind == HK_LOOKVAL || kind == HK_REC_REPS;
}

/* Click streak on the same spot: 1 caret, 2 word, 3+ the whole text. */
static int text_clicks(Ui *u, int id, float x, float y)
{
    static int lastId = 0, streak = 1;
    static DWORD lastAt = 0;
    static float lastX = 0.0f, lastY = 0.0f;
    DWORD now = GetTickCount();
    float lim = 4.0f * u->scale;
    if (id == lastId && now - lastAt < GetDoubleClickTime() && fabsf(x - lastX) <= lim && fabsf(y - lastY) <= lim)
        streak += id == lastId ? 1 : 0;
    else streak = 1;
    lastId = id;
    lastAt = now;
    lastX = x;
    lastY = y;
    return streak;
}

static void text_press(Ui *u, int id, float x, float y)
{
    pages_activate(id, 0);              /* opens (or keeps open) the field */
    te_click(text_clicks(u, id, x, y), x);
    ui_invalidate();
}

static void move_focus(Ui *u, int dir)
{
    int n = u->hitCount, cur = -1;
    for (int i = 0; i < n; i++)
        if (u->hits[i].id == u->focus) cur = i;
    for (int step = 1; step <= n; step++) {
        int i = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * step + n * 4) % n;
        if (u->hits[i].focusable) {
            u->focus = u->hits[i].id;
            u->focusVisible = 1;
            if (HIT_KIND(u->focus) == HK_SEARCH) pages_search_open();
            else if (u->textFocus && HIT_KIND(u->textFocus) == HK_SEARCH) pages_text_end(1);
            u->searchFocus = HIT_KIND(u->focus) == HK_SEARCH;
            pages_scroll_into_view(u->focus);
            ui_invalidate();
            return;
        }
    }
}

static void set_search_focus(Ui *u, int on)
{
    if (on) {
        pages_search_open();
        return;
    }
    u->searchFocus = 0;
    if (u->textFocus && HIT_KIND(u->textFocus) == HK_SEARCH) pages_text_end(0);
    else KillTimer(u->hwnd, TM_CARET);
    ui_invalidate();
}

static void on_key(Ui *u, WPARAM vk)
{
    if (pages_preview_key((UINT)vk)) return;
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && vk == 'F') {
        if (u->page == PAGE_STORE) {
            pages_store_search_open();
            return;
        }
        if (u->page != PAGE_SETTINGS) u->page = PAGE_LIBRARY;
        u->focus = HIT_ID(HK_SEARCH, u->page == PAGE_SETTINGS);
        set_search_focus(u, 1);
        return;
    }
    if (ctrl && vk == 'O') {
        library_import_dialog(u->hwnd, shift);
        return;
    }
    if (ctrl && vk >= '1' && vk < '1' + PAGE_COUNT) {
        pages_activate(HIT_ID(HK_NAV, (int)(vk - '1')), 0);
        return;
    }
    if (u->textFocus && pages_text_key((UINT)vk, ctrl, shift)) return;
    switch (vk) {
    case VK_TAB:
        move_focus(u, shift ? -1 : 1);
        return;
    case VK_ESCAPE:
        if (pages_escape()) return;
        if (u->searchFocus) set_search_focus(u, 0);   /* the engine cleared the text; an empty box closes */
        return;
    case VK_RETURN:
    case VK_SPACE:
        if (u->searchFocus && vk == VK_SPACE) return;   /* typed via WM_CHAR */
        if (u->focus && !u->searchFocus) {
            u->focusVisible = 1;
            pages_activate(u->focus, vk == VK_RETURN);
        }
        return;
    case VK_APPS:
        if (u->focus) {
            const Hit *h = hit_find(u, u->focus);
            if (h) {
                POINT p = { (LONG)(h->r.left * u->scale), (LONG)(h->r.bottom * u->scale) };
                ClientToScreen(u->hwnd, &p);
                pages_context(u->focus, p.x, p.y);
            }
        }
        return;
    case VK_F5:
        app_restart_renderer();
        return;
    default:
        if (!u->searchFocus) pages_key((UINT)vk, ctrl, shift);
        return;
    }
}

static void on_char(Ui *u, wchar_t ch)
{
    pages_text_char(ch);
}

/* One tooltip, moved onto whichever icon-only button is hovered (texts: pages_tooltip). */
static void tip_update(Ui *u)
{
#ifndef _WIN32
    (void)u;                               /* no tooltips on Linux yet */
#else
    const wchar_t *text = u->hot && !u->pressed ? pages_tooltip(u->hot) : NULL;
    const Hit *h = text ? hit_find(u, u->hot) : NULL;
    int id = h ? u->hot : 0;
    if (id == u->tipId) return;
    u->tipId = id;
    if (!u->tip && h) {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        u->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, u->hwnd, NULL, g_app.inst, NULL);
        TOOLINFOW add = { sizeof add, TTF_SUBCLASS, u->hwnd, 1 };
        add.lpszText = L"";
        if (u->tip) {
            SendMessageW(u->tip, TTM_ADDTOOLW, 0, (LPARAM)&add);
            SetWindowTheme(u->tip, u->t.dark ? L"DarkMode_Explorer" : NULL, NULL);
        }
    }
    if (!u->tip) return;
    TOOLINFOW ti = { sizeof ti, TTF_SUBCLASS, u->hwnd, 1 };
    ti.lpszText = L"";                             /* empty rect and text: nothing to show */
    if (h) {
        ti.rect.left = (LONG)(h->r.left * u->scale);
        ti.rect.top = (LONG)(h->r.top * u->scale);
        ti.rect.right = (LONG)(h->r.right * u->scale);
        ti.rect.bottom = (LONG)(h->r.bottom * u->scale);
        ti.lpszText = (LPWSTR)text;
    }
    SendMessageW(u->tip, TTM_POP, 0, 0);
    SendMessageW(u->tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    SendMessageW(u->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
#endif
}

static void on_wheel(Ui *u, int delta)
{
    if (pages_preview_wheel(delta)) return;
    int p = u->page;
    u->hoverBlockUntil = time_ms() + 90.0;
    float maxScroll = SP_MAX(0.0f, u->contentH[p] - u->viewH);
    u->scrollTarget[p] = SP_CLAMP(u->scrollTarget[p] - (float)delta / WHEEL_DELTA * 96.0f, 0.0f, maxScroll);
    ui_animate();
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Ui *u = g_ui;
    if (!u || (u->hwnd && u->hwnd != h)) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_DPICHANGED: {
        u->scale = HIWORD(wp) / 96.0f;
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (u->rt) ID2D1RenderTarget_SetDpi(RT, 96.0f * u->scale, 96.0f * u->scale);
        ui_invalidate();
        return 0;
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp), hh = HIWORD(lp);
        u->w = w / u->scale;
        u->h = hh / u->scale;
        if (u->rt) {
            D2D1_SIZE_U sz = { w, hh };
            ID2D1HwndRenderTarget_Resize(u->rt, &sz);
        }
        if (IsIconic(h)) motion_stop(u);
        else if (motion_pending(u)) ui_animate();
        ui_invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = (LONG)(940 * u->scale);
        mm->ptMinTrackSize.y = (LONG)(640 * u->scale);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint(u);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        u->mx = GET_X_LPARAM(lp) / u->scale;
        u->my = GET_Y_LPARAM(lp) / u->scale;
        if (!u->tracking) {
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            u->tracking = 1;
        }
        if (pages_marquee_active()) {                   /* drag to select */
            pages_marquee_move(u->mx, u->my);
            return 0;
        }
        if (HIT_KIND(u->pressed) == HK_CARD &&
            ((u->cardDrag && card_leaving(u)) || u->mx < 0 || u->mx >= u->w || u->my < 0 || u->my >= u->h)) {
            int reorder = u->cardDrag != 0;
            ReleaseCapture();
            if (u->cardDrag && g_cardOrigin >= 0) {     /* leaving the window was not a reorder: undo it */
                int vi = HIT_DATA(u->pressed);
                if (vi >= 0 && vi < u->viewCount && u->view[vi] != g_cardOrigin &&
                    config_move(&g_app.cfg, u->view[vi], g_cardOrigin)) {
                    pages_rebuild_view();
                    for (int i = 0; i < u->viewCount; i++)
                        if (u->view[i] == g_cardOrigin) u->pressed = HIT_ID(HK_CARD, i);
                }
            }
            g_cardOrigin = -1;
            if (reorder) u->cardAway = 1;   /* the Shell draws the card from here; its gap stays in the grid */
            else {                         /* the image is the card in its place */
                ui_invalidate();
                UpdateWindow(h);
            }
            int back = export_card_drag(u, reorder);
            if (g_ui != u) return 0; /* the Shell drag loop can process a window close */
            u->cardAway = 0;
#ifdef _WIN32
            if (back && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {   /* dragged back in: carry on reordering */
                POINT pt;
                GetCursorPos(&pt); ScreenToClient(h, &pt);
                u->mx = pt.x / u->scale; u->my = pt.y / u->scale;
                SetCapture(h);
                card_lift(u);
                pages_card_drag(u->mx, u->my);
                ui_invalidate();
                return 0;
            }
#else
            (void)back;
#endif
            u->cardDrag = 0;
            u->pressed = u->hot = 0;
            ui_invalidate();
            return 0;
        }
        if (HIT_KIND(u->pressed) == HK_CARD) {          /* a card dragged past a few pixels moves in the library */
            if (!u->cardDrag && fabsf(u->mx - u->pressX) + fabsf(u->my - u->pressY) > 8.0f) {
                card_lift(u);
                u->cardDrag = pages_card_drag(u->mx, u->my);
            }
            else if (u->cardDrag) pages_card_drag(u->mx, u->my);
            if (u->cardDrag) return 0;
        }
        int hot = hit_at(u, u->mx, u->my);
        if (hot != u->hot) {                            /* keep the drag target while the button is down */
            u->hot = pages_is_drag(HIT_KIND(u->pressed))
                     ? u->pressed : hot;
            ui_invalidate();
            tip_update(u);
        }
        if (u->pressed) {                               /* continuous controls: slider + colour wheel */
            int k = HIT_KIND(u->pressed);
            if (pages_is_drag(k)) pages_drag(u->pressed, u->mx, u->my, 0);
        }
        u->hot = is_text_kind(HIT_KIND(u->pressed)) && u->textFocus == u->pressed ? u->hot : hot;
        if (u->pressed == u->textFocus && te_active()) te_drag(u->mx);   /* drag selection in a text field */
        return 0;
    }
    case WM_MOUSELEAVE:
        u->tracking = 0;
        if (u->hot) {
            u->hot = 0;
            ui_invalidate();
            tip_update(u);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const Hit *hit = u->hot ? hit_find(u, u->hot) : NULL;
            static const LPCWSTR k_cursors[] = { IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_CROSS, IDC_SIZEALL, IDC_SIZENWSE,
                                                 IDC_SIZENESW, IDC_SIZEWE, IDC_SIZENS };
            LPCWSTR c = hit && hit->cursor > 0 && hit->cursor < (int)SP_COUNTOF(k_cursors) ? k_cursors[hit->cursor] : IDC_ARROW;
            SetCursor(LoadCursorW(NULL, c));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        u->pressed = u->hot;
        u->pressId = u->pressed;
        if (u->pressId) ui_animate();
        u->pressedRect = rectf(0, 0, 0, 0);
        u->pressX = u->mx;
        u->pressY = u->my;
        u->cardDrag = 0;
        if (u->hot) {
            const Hit *hit = hit_find(u, u->hot);
            if (hit) {
                u->pressedRect = hit->r;
                if (hit->focusable) u->focus = u->hot;
            }
        }
        if (u->textFocus && u->hot != u->textFocus &&
            !(HIT_KIND(u->textFocus) == HK_COLL_TEXT &&
              (HIT_KIND(u->hot) == HK_COLL_SAVE || HIT_KIND(u->hot) == HK_COLL_CANCEL ||
               HIT_KIND(u->hot) == HK_COLL_PANEL)))
            pages_text_end(1);   /* commit when the click lands elsewhere */
        {
            int k = HIT_KIND(u->pressed);   /* continuous controls jump to the click right away */
            if (pages_is_drag(k)) pages_drag(u->pressed, u->mx, u->my, 0);
        }
        set_search_focus(u, HIT_KIND(u->hot) == HK_SEARCH);
        if (is_text_kind(HIT_KIND(u->hot))) text_press(u, u->hot, u->mx, u->my);   /* caret to the click */
        if (!u->hot) pages_marquee_begin(u->mx, u->my);       /* empty space: drag to select */
        ui_invalidate();
        return 0;
    case WM_LBUTTONUP: {
        int id = u->pressed, dragged = u->cardDrag;
        u->pressed = 0;
        if (u->pressId) ui_animate();
        u->cardDrag = 0;
        ReleaseCapture();
        if (pages_marquee_active()) {
            pages_marquee_end();
            return 0;
        }
        if (dragged) pages_card_drop();
        else if (id) {
            int k = HIT_KIND(id);
            if (pages_is_drag(k)) pages_drag(id, u->mx, u->my, 1);
            if (id == u->hot && k != HK_SEARCH && !is_text_kind(k)) pages_activate(id, 0);
        }
        ui_invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (u->hot) {
            int k = HIT_KIND(u->hot);
            if (k == HK_CARD) pages_activate(u->hot, 1);
            else if (k == HK_REC_PRESET) pages_activate(u->hot, 0);   /* quick - / + clicks: every one counts */
            else if (is_text_kind(k)) text_press(u, u->hot, u->mx, u->my);   /* 2nd click: word selection */
        }
        return 0;
    case WM_RBUTTONUP:
        if (u->hot) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &p);
            pages_context(u->hot, p.x, p.y);
        }
        return 0;
    case WM_MOUSEWHEEL:
        on_wheel(u, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp != VK_F10) break;
        if (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0) wp = VK_APPS;
        on_key(u, wp);
        return 0;
    case WM_CHAR:
        on_char(u, (wchar_t)wp);
        return 0;
    case WM_TIMER:
        if (wp == TM_ANIM) anim_step(u);
        else if (wp == TM_THUMBS) thumbs_tick(u);
#ifdef _WIN32
        else if (wp == TM_EXPORT_DRAG) drag_animate(g_drag);
#endif
        else if (wp == TM_STORE) pages_store_timer();
        else if (wp == TM_CARET) {
            u->caretOn = !u->caretOn;
            ui_invalidate();
        } else if (wp == TM_STATS) {
            if (!IsIconic(h)) {
                supervisor_sample_stats();
                ui_invalidate();
            }
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        wchar_t **paths = (wchar_t **)calloc(n ? n : 1, sizeof(wchar_t *));
        int got = 0;
        for (UINT i = 0; paths && i < n; i++) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            wchar_t *p = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
            if (p && DragQueryFileW(drop, i, p, len + 1)
#ifdef _WIN32
                && !drag_is_internal_path(p)
#endif
            ) paths[got++] = p;
            else free(p);
        }
        DragFinish(drop);
        for (int i = 0; i < got; i++)            /* a Screen+ archive opens its preview; the rest are added */
            if (str_ieq(path_ext(paths[i]), L".spf") || str_ieq(path_ext(paths[i]), L".spp")) {
                pages_preview_open(paths[i]);
                free(paths[i]);
                paths[i--] = paths[--got];
            }
        if (got) library_import((const wchar_t *const *)paths, got, TRUE);
        for (int i = 0; i < got; i++) free(paths[i]);
        free(paths);
        return 0;
    }
    case WM_SETTINGCHANGE:
        ui_refresh_motion();
        if (lp && wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0) {
            theme_build(u);
            ui_invalidate();
        }
        return 0;
    case WM_ACTIVATE:
        if (motion_pending(u)) ui_animate();
        ui_invalidate();
        break;
    case WM_SP_PREVIEW: {        /* a recorded preview arrived: reload that wallpaper's thumbnails */
        wchar_t *id = (wchar_t *)lp;
        for (int i = 0; id && i < u->thumbCount; i++) {
            if (wcscmp(u->thumbs[i].id, id) == 0) {
                thumb_release(&u->thumbs[i]);
                u->thumbs[i].state = 0;
            }
        }
        free(id);
        ui_invalidate();
        return 0;
    }
    case WM_SP_THUMB:
        on_thumb(u, (ThumbResult *)lp);
        return 0;
    case WM_CLOSE:
        LOGI(L"ui: window closed (Screen+ keeps running in the notification area)");
        ui_destroy();
        update_on_ui_closed();       /* a downloaded update was waiting for the window */
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================================================================ public */

#ifndef _WIN32
LRESULT ui_dispatch(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return ui_proc(h, msg, wp, lp); }   /* ui_sdl.c */
#endif

BOOL ui_show_screensaver(void)
{
    if (!ui_show() || !g_ui) return FALSE;
    g_ui->page = PAGE_SETTINGS;
    g_ui->pageAnim = 0.0f;
    g_ui->scroll[PAGE_SETTINGS] = g_ui->scrollTarget[PAGE_SETTINGS] = 0.0f;
    ui_animate();
    return TRUE;
}

void ui_preview_file(const wchar_t *path)
{
    if (ui_show()) pages_preview_open(path);
}

BOOL ui_show(void)
{
    if (g_ui) {
        if (IsIconic(g_ui->hwnd)) ShowWindow(g_ui->hwnd, SW_RESTORE);
        else ShowWindow(g_ui->hwnd, SW_SHOW);
        SetForegroundWindow(g_ui->hwnd);
        return TRUE;
    }
    Ui *u = (Ui *)calloc(1, sizeof *u);
    if (!u) return FALSE;
    g_ui = u;
    u->showRequestedAt = time_ms();
    u->trace = getenv("SCREENPLUS_UI_TRACE") && strcmp(getenv("SCREENPLUS_UI_TRACE"), "1") == 0;
    u->target = -1;
    u->textFocus = 0;    /* no Customise field is being typed into */
    u->alpha = 1.0f;
    u->pageAnim = g_app.uiReadyMs ? 1.0f : 0.0f;
    if (!g_app.uiReadyMs) u->brandAt = time_ms();
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &SP_IID_ID2D1Factory, NULL, (void **)&u->d2d);
    if (SUCCEEDED(hr)) hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &SP_IID_IDWriteFactory, (IUnknown **)&u->dw);
    if (FAILED(hr) || !create_text_formats(u)) {
        LOGE(L"ui: Direct2D/DirectWrite initialization failed 0x%08lX", (unsigned long)hr);
        ui_destroy();
        return FALSE;
    }

#ifndef _WIN32
    u->hwnd = sp_window_open(1180, 760, &u->scale);
    if (!u->hwnd) {
        ui_destroy();
        return FALSE;
    }
    RECT rc;
    GetClientRect(u->hwnd, &rc);
    u->w = rc.right / u->scale;
    u->h = rc.bottom / u->scale;
#else
    static int registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = ui_proc;
        wc.hInstance = g_app.inst;
        wc.hIcon = LoadIconW(g_app.inst, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), 0);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = k_class;
        registered = RegisterClassExW(&wc) != 0;
    }

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    u->scale = dpi / 96.0f;
    int ww = (int)(1180 * u->scale), wh = (int)(760 * u->scale);
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    ww = SP_MIN(ww, aw * 94 / 100);
    wh = SP_MIN(wh, ah * 94 / 100);
    int x = mi.rcWork.left + (aw - ww) / 2, y = mi.rcWork.top + (ah - wh) / 2;

    u->hwnd = CreateWindowExW(0, k_class, L"Screen+", WS_OVERLAPPEDWINDOW, x, y, ww, wh, NULL, NULL, g_app.inst, NULL);
    if (!u->hwnd) {
        LOGE(L"ui: CreateWindow failed (err %lu)", GetLastError());
        ui_destroy();
        return FALSE;
    }
    u->scale = GetDpiForWindow(u->hwnd) / 96.0f;
    RECT rc;
    GetClientRect(u->hwnd, &rc);
    u->w = rc.right / u->scale;
    u->h = rc.bottom / u->scale;
    setup_backdrop(u);
#endif
    theme_build(u);
    ui_refresh_motion();
    for (int i = 0; i < TOG_COUNT; i++) u->tog[i] = u->togTarget[i] = -1.0f;   /* snap on first frame */
    DragAcceptFiles(u->hwnd, TRUE);
    thumbs_start(u->hwnd);
    pages_rebuild_view();
    SetTimer(u->hwnd, TM_STATS, 1500, NULL);
    supervisor_sample_stats();
    ShowWindow(u->hwnd, SW_SHOWNORMAL);
    if (u->pageAnim < 1.0f) ui_animate();
    SetForegroundWindow(u->hwnd);
    UpdateWindow(u->hwnd);
    return TRUE;
}

BOOL ui_is_open(void) { return g_ui != NULL; }

void ui_destroy(void)
{
    Ui *u = g_ui;
    if (!u) return;
#ifdef _WIN32
    if (g_drag) {
        DragExport *j = g_drag;
        g_drag = NULL;
        InterlockedExchange(&j->cancelled, 1);
        drag_release(j);
    }
#endif
    g_ui = NULL;
    thumbs_stop();
    if (u->hwnd) {
        KillTimer(u->hwnd, TM_ANIM);
        KillTimer(u->hwnd, TM_CARET);
        KillTimer(u->hwnd, TM_STATS);
        KillTimer(u->hwnd, TM_THUMBS);
        DestroyWindow(u->hwnd);
    }
    for (int i = 0; i < u->thumbCount; i++) thumb_release(&u->thumbs[i]);
    free(u->thumbs);
    free(u->view);
    disc_release();
    SP_RELEASE(u->brush);
    SP_RELEASE(u->rt);
    release_text_formats(u);
    SP_RELEASE(u->dw);
    SP_RELEASE(u->d2d);
    free(u);
}

void ui_on_status(void)
{
    ui_invalidate();
}

void ui_refresh_theme(void)
{
    if (!g_ui) return;
    theme_build(g_ui);
    if (g_ui->tip) SetWindowTheme(g_ui->tip, g_ui->t.dark ? L"DarkMode_Explorer" : NULL, NULL);
    ui_invalidate();
}

void ui_on_config_changed(void)
{
    if (!g_ui) return;
    ui_refresh_motion();
    pages_rebuild_view();
    theme_build(g_ui);   /* the theme setting may have changed (CLI) */
    ui_invalidate();
}

void ui_on_customise_changed(void)
{
    pages_customise_reload();   /* the page keeps its cache while closed too */
    if (g_ui) ui_invalidate();
}

BOOL ui_translate(MSG *m)
{
    (void)m;
    return FALSE;
}
