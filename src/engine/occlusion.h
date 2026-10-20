#pragma once
#include "../common/common.h"
#include "../monitor/monitor.h"

typedef struct OcclusionResult {
    int  covered[SP_MAX_MONITORS];  /* monitor fully hidden by a single opaque window */
    int  fullscreen;                /* a fullscreen application is in the foreground */
    HWND fullscreenWnd;
} OcclusionResult;

/* One pass over the top-level window list (cheap; event-driven + low-rate safety timer). */
void occlusion_compute(const MonitorInfo *mons, int count, OcclusionResult *out);
