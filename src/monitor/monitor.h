#pragma once
#include "../common/common.h"

typedef struct MonitorInfo {
    HMONITOR hmon;
    wchar_t  id[SP_MONID_LEN];   /* the display's own: EDID model + serial ("EDID:ACR0B46:1234"), else oldId */
    wchar_t  oldId[SP_MONID_LEN];/* the port's: device interface path (\\?\DISPLAY#...) or GDI fallback; the id before 2.3 */
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
