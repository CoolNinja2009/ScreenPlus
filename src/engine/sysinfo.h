#pragma once
#include "../common/common.h"

/* Real-time system data for livelySystemInformation, in the schema Lively web
 * wallpapers parse: name strings for cpu/gpu/netcard plus CurrentCpu and
 * CurrentGpu3D (percent), CurrentRamAvail and TotalRam (MB), CurrentNetDown and
 * CurrentNetUp (bytes/sec). Screen+'s extra keys (memory load, battery) are
 * appended so wallpapers written against the old feed keep working.
 *
 * Every call advances the sampling windows (cpu time, network octets), so call
 * it from a single thread at a steady pace; out receives at most cap-1 bytes. */
void sysinfo_sample(char *out, size_t cap);
