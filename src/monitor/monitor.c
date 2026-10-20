#include "monitor.h"
#include "../common/util.h"
#include <shellscalingapi.h>
#include <stdlib.h>

typedef struct EnumCtx {
    MonitorInfo *out;
    int max;
    int count;
} EnumCtx;

static BOOL CALLBACK enum_proc(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hdc; (void)rc;
    EnumCtx *ctx = (EnumCtx *)lp;
    if (ctx->count >= ctx->max) return FALSE;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;
    MonitorInfo *m = &ctx->out[ctx->count];
    ZeroMemory(m, sizeof *m);
    m->hmon = hmon;
    m->rect = mi.rcMonitor;
    m->work = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    str_copy(m->gdiName, SP_COUNTOF(m->gdiName), mi.szDevice);

    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    m->dpi = dx;

    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        m->refreshHz = dm.dmDisplayFrequency;
    else
        m->refreshHz = 60;

    /* Stable id: monitor device interface path. */
    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof dd);
    dd.cb = sizeof dd;
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) {
        str_copy(m->id, SP_COUNTOF(m->id), dd.DeviceID);
        str_copy(m->name, SP_COUNTOF(m->name), dd.DeviceString);
    } else {
        str_printf(m->id, SP_COUNTOF(m->id), L"GDI:%ls", mi.szDevice);
    }
    ctx->count++;
    return TRUE;
}

/* Friendly names from the DisplayConfig API ("DELL U2720Q" instead of "Generic PnP Monitor"). */
static void apply_friendly_names(MonitorInfo *list, int count)
{
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm, sizeof *modes);
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            ZeroMemory(&src, sizeof src);
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof src;
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
            ZeroMemory(&tgt, sizeof tgt);
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof tgt;
            tgt.header.adapterId = paths[i].targetInfo.adapterId;
            tgt.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
            for (int m = 0; m < count; m++) {
                if (!str_ieq(list[m].gdiName, src.viewGdiDeviceName)) continue;
                if (tgt.monitorFriendlyDeviceName[0])
                    str_copy(list[m].name, SP_COUNTOF(list[m].name), tgt.monitorFriendlyDeviceName);
                else if (tgt.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                         tgt.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED)
                    str_copy(list[m].name, SP_COUNTOF(list[m].name), L"Built-in display");
                if (tgt.monitorDevicePath[0] && str_istarts(list[m].id, L"GDI:"))
                    str_copy(list[m].id, SP_COUNTOF(list[m].id), tgt.monitorDevicePath);
            }
        }
    }
    free(paths);
    free(modes);
}

static int cmp_monitor(const void *a, const void *b)
{
    const MonitorInfo *x = (const MonitorInfo *)a, *y = (const MonitorInfo *)b;
    if (x->primary != y->primary) return y->primary - x->primary;
    if (x->rect.left != y->rect.left) return x->rect.left < y->rect.left ? -1 : 1;
    if (x->rect.top != y->rect.top) return x->rect.top < y->rect.top ? -1 : 1;
    return 0;
}

int monitor_enumerate(MonitorInfo *out, int max)
{
    EnumCtx ctx = { out, max, 0 };
    EnumDisplayMonitors(NULL, NULL, enum_proc, (LPARAM)&ctx);
    apply_friendly_names(out, ctx.count);
    qsort(out, (size_t)ctx.count, sizeof *out, cmp_monitor);
    for (int i = 0; i < ctx.count; i++) {
        out[i].index = i + 1;
        if (!out[i].name[0] || str_ieq(out[i].name, L"Generic PnP Monitor"))
            str_printf(out[i].name, SP_COUNTOF(out[i].name), L"Display %d", i + 1);
    }
    return ctx.count;
}

const MonitorInfo *monitor_find(const MonitorInfo *list, int count, const wchar_t *id)
{
    for (int i = 0; i < count; i++)
        if (str_ieq(list[i].id, id)) return &list[i];
    return NULL;
}
