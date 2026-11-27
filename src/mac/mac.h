/* What the macOS build asks of the system, in plain C (CoreGraphics, IOKit, CoreAudio): desktop_mac.c. The service
 * and the engine (engine_mac.m) both use it. */
#pragma once
#include "../common/common.h"
#include "../monitor/monitor.h"
#include <stdint.h>

int  mac_monitors(MonitorInfo *out, int max);
/* A display's id as the service saves it: "EDID:<mfg><product>:<serial>" like Windows and Linux, else
 * "MAC:<vendor>-<model>-<unit>". */
void mac_display_id(uint32_t display, wchar_t *out, size_t cap);
int  mac_on_battery(void);
int  mac_session_locked(void);
int  mac_front_pid(void);                          /* the app whose window is in front, 0: none */
/* 1: a window of another app covers the display (full screen; with `covered`, also a zoomed or maximised one) */
int  mac_display_covered(uint32_t display, int covered);
void mac_cpu_name(char *out, size_t cap);
/* CPU busy and idle ticks, RAM in MB, bytes received and sent since boot */
void mac_sys_sample(unsigned long long *busy, unsigned long long *idle, double *totalMb, double *availMb,
                    unsigned long long *rx, unsigned long long *tx);
