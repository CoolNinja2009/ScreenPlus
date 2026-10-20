#pragma once
#include "../common/common.h"

typedef struct MonitorInfo {
    HMONITOR hmon;
    wchar_t  id[SP_MONID_LEN];   /* stable device interface path (\\?\DISPLAY#...) or GDI fallback */
    wchar_t  name[64];           /* friendly name ("DELL U2720Q") */
    wchar_t  gdiName[32];        /* \\.\DISPLAY1 */
    RECT     rect;               /* physical pixels, virtual-screen coordinates */
    RECT     work;
    UINT     dpi;
    UINT     refreshHz;
    int      primary;
    int      index;              /* 1-based order shown in the UI */
} MonitorInfo;

/* Enumerates active monitors (process must be per-monitor DPI aware). Sorted: primary first,
 * then left-to-right / top-to-bottom. Returns the count. */
int monitor_enumerate(MonitorInfo *out, int max);
const MonitorInfo *monitor_find(const MonitorInfo *list, int count, const wchar_t *id);
