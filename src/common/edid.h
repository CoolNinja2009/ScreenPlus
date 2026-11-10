#pragma once
#include "common.h"

struct MonitorInfo;

/* A display's own id from its EDID: "EDID:<model>:<serial>" (model as Windows writes it, ACR0B46), so its wallpaper
 * follows it to any port, dock or computer. 0 when the EDID carries no serial number. */
int  edid_id(const unsigned char *edid, size_t n, wchar_t *out, size_t cap);

#ifndef _WIN32
/* Linux: the EDID of a connector ("DP-1") from /sys/class/drm. */
int  edid_id_for_connector(const char *connector, wchar_t *out, size_t cap);
#endif

/* Two displays reporting the same serial (some EDIDs carry a placeholder) can't be told apart by it: they keep
 * their port-based id (oldId). */
void edid_dedupe(struct MonitorInfo *mons, int n);
