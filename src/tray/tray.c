#include "tray.h"
#include "../app/app.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../../resources/resource.h"
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>

enum {
    IDM_OPEN = 1000, IDM_PAUSE, IDM_NEXT, IDM_AUDIO, IDM_RESTART, IDM_EXIT,
    IDM_WP_BASE = 1100, IDM_WP_MAX = 1120
};

static NOTIFYICONDATAW g_nid;
static HWND g_owner;
static HICON g_icon;
static int g_added;
static wchar_t g_menuIds[IDM_WP_MAX - IDM_WP_BASE][SP_ID_LEN];

static void fill_tip(wchar_t *tip, size_t cap)
{
    int sev = 0;
    const wchar_t *state = app_engine_state_text(&sev);
    WallpaperEntry *e = config_find(&g_app.cfg, app_wallpaper_for(NULL));
    if (e) str_printf(tip, cap, L"Screen+ — %ls\n%.60ls", state, e->name);
    else str_printf(tip, cap, L"Screen+ — %ls", state);
}

BOOL tray_init(HWND owner, HINSTANCE inst)
{
    g_owner = owner;
    if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g_icon)))
        g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_SP_TRAY;
    g_nid.hIcon = g_icon;
    fill_tip(g_nid.szTip, SP_COUNTOF(g_nid.szTip));
    g_added = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (g_added) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        LOGW(L"tray: icon could not be added yet (shell not ready); will retry on TaskbarCreated");
    }
    return g_added;
}

void tray_update(void)
{
    if (!g_owner) return;
    fill_tip(g_nid.szTip, SP_COUNTOF(g_nid.szTip));
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (g_added) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void tray_remove(void)
{
    if (g_added) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = 0;
    if (g_icon) DestroyIcon(g_icon);
    g_icon = NULL;
    g_owner = NULL;
}

void tray_on_taskbar_created(void)
{
    if (!g_owner) return;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.hIcon = g_icon;
    fill_tip(g_nid.szTip, SP_COUNTOF(g_nid.szTip));
    g_added = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (g_added) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
}

static int cmp_recent(const void *a, const void *b)
{
    const WallpaperEntry *x = *(const WallpaperEntry *const *)a, *y = *(const WallpaperEntry *const *)b;
    if (x->lastUsed != y->lastUsed) return x->lastUsed > y->lastUsed ? -1 : 1;
    return x->added > y->added ? -1 : x->added < y->added ? 1 : 0;
}

static void show_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    if (!m || !sub) return;
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Screen+");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_PAUSE, g_app.cfg.s.paused ? L"Resume wallpapers" : L"Pause wallpapers");
    AppendMenuW(m, MF_STRING | (g_app.cfg.count > 1 ? 0 : MF_GRAYED), IDM_NEXT, L"Next wallpaper");

    WallpaperEntry *sorted[IDM_WP_MAX - IDM_WP_BASE];
    int n = 0;
    WallpaperEntry **all = (WallpaperEntry **)malloc(sizeof(WallpaperEntry *) * (size_t)(g_app.cfg.count ? g_app.cfg.count : 1));
    if (all) {
        for (int i = 0; i < g_app.cfg.count; i++) all[i] = &g_app.cfg.items[i];
        qsort(all, (size_t)g_app.cfg.count, sizeof *all, cmp_recent);
        for (int i = 0; i < g_app.cfg.count && n < (int)SP_COUNTOF(sorted); i++) sorted[n++] = all[i];
        free(all);
    }
    const wchar_t *cur = app_wallpaper_for(NULL);
    for (int i = 0; i < n; i++) {
        str_copy(g_menuIds[i], SP_ID_LEN, sorted[i]->id);
        wchar_t label[SP_NAME_LEN + 16];
        str_printf(label, SP_COUNTOF(label), L"%.60ls\t%ls", sorted[i]->name, wallpaper_type_name(sorted[i]->type));
        UINT flags = MF_STRING | (wcscmp(sorted[i]->id, cur) == 0 ? MF_CHECKED : 0) | (sorted[i]->missing ? MF_GRAYED : 0);
        AppendMenuW(sub, flags, IDM_WP_BASE + (UINT)i, label);
    }
    if (!n) AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"Library is empty");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Switch wallpaper");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_app.cfg.s.audio ? MF_CHECKED : 0), IDM_AUDIO, L"Wallpaper audio");
    AppendMenuW(m, MF_STRING, IDM_RESTART, g_app.engineGaveUp ? L"Restart renderer (stopped)" : L"Restart renderer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(g_owner);
    UINT flags = TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    TrackPopupMenuEx(m, flags, x, y, g_owner, NULL);
    PostMessageW(g_owner, WM_NULL, 0, 0);
    DestroyMenu(m);   /* destroys the submenu too */
}

void tray_on_message(WPARAM wp, LPARAM lp)
{
    switch (LOWORD(lp)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        app_show_ui();
        break;
    case WM_CONTEXTMENU:
        show_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        break;
    default:
        break;
    }
}

void tray_on_command(UINT id)
{
    switch (id) {
    case IDM_OPEN:    app_show_ui(); break;
    case IDM_PAUSE:   app_set_paused(!g_app.cfg.s.paused); break;
    case IDM_NEXT:    app_next_wallpaper(); break;
    case IDM_AUDIO:   app_set_audio(!g_app.cfg.s.audio); break;
    case IDM_RESTART: app_restart_renderer(); break;
    case IDM_EXIT:    app_exit(); break;
    default:
        if (id >= IDM_WP_BASE && id < IDM_WP_MAX) app_apply_wallpaper(g_menuIds[id - IDM_WP_BASE], NULL);
        break;
    }
}
