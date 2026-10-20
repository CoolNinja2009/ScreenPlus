#include "sysinfo.h"
#include "../common/json.h"
#include "../common/log.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>

/* Lively-format hardware usage, sampled through PDH only (plus registry and
 * DXGI for the name strings): task-manager style Processor Time from
 * GetSystemTimes, GPU 3D engine load and network interface speed from
 * "GPU Engine" / "Network Interface" counters. No third-party drivers.
 *
 * Engine instances and interfaces come and go, so the counters use wildcards
 * and the query is rebuilt periodically. All sampling state lives here; the
 * engine data tick is the only caller, so no locking is needed. */

#define GPU_REBUILD 8          /* ticks between query rebuilds (~2 s at 250 ms) */

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* ------------------------------------------------------------- names */

static wchar_t s_cpuName[96], s_gpuName[96], s_netName[64];
static int     s_namesRead;

/* Lively passes cpu/gpu/netcard names next to the live numbers; wallpapers
 * print them as-is. Failures just leave a name empty. */
static void read_names(void)
{
    if (s_namesRead) return;
    s_namesRead = 1;

    DWORD n = sizeof s_cpuName;
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      L"ProcessorNameString", RRF_RT_REG_SZ, NULL, s_cpuName, &n))
        str_trim(s_cpuName);

    IDXGIFactory1 *f = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
        IDXGIAdapter1 *a = NULL;
        for (UINT i = 0; f->lpVtbl->EnumAdapters1(f, i, &a) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d = { 0 };
            if (SUCCEEDED(a->lpVtbl->GetDesc1(a, &d)) && d.Description[0]) {
                if (!_wcsnicmp(d.Description, L"Microsoft ", 10)) {   /* skip Basic Render Driver */
                    SP_RELEASE(a);
                    continue;
                }
                str_copy(s_gpuName, SP_COUNTOF(s_gpuName), d.Description);
                SP_RELEASE(a);
                break;
            }
            SP_RELEASE(a);
        }
        SP_RELEASE(f);
    }
}

/* ------------------------------------------------------------- cpu */

/* Total Processor Time percentage over the window since the previous call,
 * the way Task Manager reports it. The first call primes the window. */
static double cpu_usage(void)
{
    static ULONGLONG k0, u0, i0, wall0;
    FILETIME k = { 0, 0 }, u = k, i = k;
    if (!GetSystemTimes(&i, &k, &u)) return 0.0;
    ULONGLONG kk = ft64(k), uu = ft64(u), ii = ft64(i), now = filetime_now();
    double pct = 0.0;
    if (wall0 && now > wall0 && kk >= k0 && uu >= u0 && ii >= i0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        ULONGLONG busy = (kk - k0) + (uu - u0);
        ULONGLONG total = busy + (ii - i0);
        if (total)
            pct = (double)busy * 100.0 / (double)total /
                  (double)SP_MAX(si.dwNumberOfProcessors, 1);
    }
    k0 = kk; u0 = uu; i0 = ii; wall0 = now ? now : 1;
    return SP_CLAMP(pct, 0.0, 100.0);
}

/* ------------------------------------------------------------- pdh helpers */

static PDH_HQUERY   s_query;
static PDH_HCOUNTER s_gpuCount, s_netDown, s_netUp;
static int          s_epoch;          /* ticks since the query was (re)opened */
static double       s_gpuLast, s_downLast, s_upLast;

/* Wildcard counters for the gpu 3D engine and the network interfaces;
 * instances are filtered when formatting. Windows 10 and newer only. */
static void query_add_counters(PDH_HQUERY q)
{
    s_gpuCount = s_netDown = s_netUp = 0;
    PdhAddCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &s_gpuCount);
    PdhAddCounterW(q, L"\\Network Interface(*)\\Bytes Received/Sec", 0, &s_netDown);
    PdhAddCounterW(q, L"\\Network Interface(*)\\Bytes Sent/Sec", 0, &s_netUp);
}

/* PDH wildcard counters report the instances that existed when the query was
 * built; new "GPU Engine" instances (apps launching/leaving) appear only after
 * a rebuild, so rebuild every few seconds and skip one invalid delta. */
static void query_tick(void)
{
    if (++s_epoch >= GPU_REBUILD) {
        if (s_query) { PdhCloseQuery(s_query); s_query = NULL; }
        s_epoch = 0;
    }
    if (!s_query) {
        if (PdhOpenQueryW(NULL, 0, &s_query) != ERROR_SUCCESS) { s_query = NULL; s_epoch = GPU_REBUILD - 1; return; }
        query_add_counters(s_query);
    }
    if (PdhCollectQueryData(s_query) != ERROR_SUCCESS) return;
    static int skipNext = TRUE;        /* first sample after an (re)open has no delta */
    if (skipNext) { skipNext = FALSE; return; }
}

/* Formatted double values of a wildcard counter, filtered by an optional
 * instance-name substring; all matching instances are summed. */
static double counter_sum(PDH_HCOUNTER counter, const wchar_t *instanceFilter, int invert)
{
    if (!counter) return 0.0;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, NULL) != PDH_MORE_DATA ||
        !size || size > 256 * 1024)                     /* cap runaway instance lists */
        return 0.0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!items) return 0.0;
    double sum = 0.0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < count && items[i].szName; i++) {
            int want = instanceFilter ? (wcsstr(items[i].szName, instanceFilter) != NULL) : TRUE;
            int take = invert ? !want : want;
            if (take && items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > 0.0)
                sum += items[i].FmtValue.doubleValue;
        }
    }
    HeapFree(GetProcessHeap(), 0, items);
    return sum;
}

/* ------------------------------------------------------------- gpu + network */

/* Sums the engtype_3D engine utilization, i.e. the 3D engine load Task
 * Manager shows (0-100 across all processes, can exceed 100 when summed so it
 * is clamped). */
static double gpu_usage(void)
{
    double v = counter_sum(s_gpuCount, L"engtype_3D", FALSE);
    if (s_gpuCount && v > 0.0) s_gpuLast = SP_CLAMP(v, 0.0, 100.0);
    return s_gpuLast;
}

/* Summed network speed of connected real interfaces, bytes per second. PDH
 * already reports the rate; all we do is subtract the pseudo-interfaces. */
static void net_usage(double *down, double *up)
{
    *down = counter_sum(s_netDown, NULL, FALSE);
    *up   = counter_sum(s_netUp,   NULL, FALSE);
    /* Instances include loopback, isatap tunnels, WAN miniports and other
     * pseudo-interfaces; subtract those to stay close to Task Manager's view. */
    double junkD = counter_sum(s_netDown, L"loopback", FALSE) +
                   counter_sum(s_netDown, L"isatap", FALSE) +
                   counter_sum(s_netDown, L"teredo", FALSE) +
                   counter_sum(s_netDown, L"Miniport", FALSE);
    double junkU = counter_sum(s_netUp, L"loopback", FALSE) +
                   counter_sum(s_netUp, L"isatap", FALSE) +
                   counter_sum(s_netUp, L"teredo", FALSE) +
                   counter_sum(s_netUp, L"Miniport", FALSE);
    *down = SP_MAX(*down - junkD, 0.0);
    *up   = SP_MAX(*up   - junkU, 0.0);

    /* Lively shows the netcard device name; take it from the instance with the
     * most traffic so a machine with wired + wifi names its active adapter. */
    if (s_netName[0] && *up + *down < 1.0) return;   /* keep the name while idle */
    DWORD size = 0, count = 0;
    if (!s_netDown ||
        PdhGetFormattedCounterArrayW(s_netDown, PDH_FMT_DOUBLE, &size, &count, NULL) != PDH_MORE_DATA ||
        !size || size > 256 * 1024)
        return;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!items) return;
    if (PdhGetFormattedCounterArrayW(s_netDown, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
        wchar_t best[64] = L"";
        double bestVal = 0.0;
        for (DWORD i = 0; i < count && items[i].szName; i++) {
            if (wcsstr(items[i].szName, L"[loopback") || str_icontains(items[i].szName, L"miniport")) continue;
            double v = 0.0;
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) v = items[i].FmtValue.doubleValue;
            if (!best[0] || v > bestVal) {   /* first real interface; better only when active */
                if (!best[0] || v > 1.0) { bestVal = v; str_copy(best, SP_COUNTOF(best), items[i].szName); }
            }
        }
        if (best[0] && _wcsicmp(best, s_netName)) {
            /* Instances may carry a "[#N]" suffix when two adapters share a name. */
            wchar_t *h = wcsrchr(best, L'[');
            if (h && h[1] == L'#') *h = 0;
            str_copy(s_netName, SP_COUNTOF(s_netName), best);
        }
    }
    HeapFree(GetProcessHeap(), 0, items);
}

/* ------------------------------------------------------------- json */

static void put_name(StrBuf *b, const wchar_t *name)
{
    char *u = utf8_from_wide(name, NULL);
    json_write_string(b, u ? u : "");
    free(u);
}

void sysinfo_sample(char *out, size_t cap)
{
    read_names();

    double cpu = cpu_usage();
    query_tick();
    double gpu = gpu_usage(), down = 0.0, up = 0.0;
    net_usage(&down, &up);

    MEMORYSTATUSEX ms = { sizeof ms };
    if (!GlobalMemoryStatusEx(&ms)) ZeroMemory(&ms, sizeof ms);
    SYSTEM_POWER_STATUS ps = { 0 };
    GetSystemPowerStatus(&ps);

    StrBuf b = { 0 };
    sb_puts(&b, "{\"NameCpu\":");     put_name(&b, s_cpuName);
    sb_puts(&b, ",\"NameGpu\":");     put_name(&b, s_gpuName);
    sb_puts(&b, ",\"NameNetCard\":"); put_name(&b, s_netName);
    sb_printf(&b, ",\"CurrentCpu\":%.1f", cpu);
    sb_printf(&b, ",\"CurrentGpu3D\":%.1f", gpu);
    sb_printf(&b, ",\"CurrentRamAvail\":%.0f", (double)(ms.ullAvailPhys >> 20));
    sb_printf(&b, ",\"TotalRam\":%.0f", (double)(ms.ullTotalPhys >> 20));
    sb_printf(&b, ",\"CurrentNetDown\":%.0f", down);
    sb_printf(&b, ",\"CurrentNetUp\":%.0f", up);
    /* Screen+ extras (no Lively equivalent): memory pressure and battery. */
    sb_printf(&b, ",\"MemoryUsage\":%lu", ms.dwMemoryLoad);
    sb_printf(&b, ",\"TotalMemory\":%llu", (unsigned long long)ms.ullTotalPhys);
    if (ps.BatteryLifePercent <= 100) sb_printf(&b, ",\"BatteryPercent\":%u", ps.BatteryLifePercent);
    else sb_puts(&b, ",\"BatteryPercent\":null");
    sb_puts(&b, ps.ACLineStatus == 0 ? ",\"IsSystemOnBatteryPower\":true}"
                                     : ",\"IsSystemOnBatteryPower\":false}");

    size_t n = b.failed ? 0 : SP_MIN(cap - 1, b.len);
    if (n && b.p) memcpy(out, b.p, n);
    out[n] = 0;
    sb_free(&b);
}
