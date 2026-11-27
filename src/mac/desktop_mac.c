/* macOS: the system facts the service and the engine need, from CoreGraphics, IOKit and CoreAudio, so neither needs
 * AppKit for them. Also the app rules (desktop.h's sp_pid_matches / sp_mic_*), which apprules.c does on Linux:
 *  - Pause: the program in front is the owner of the frontmost on-screen window (CGWindowList: pids and bounds need
 *    no Screen Recording permission, only window titles would).
 *  - Mute: Core Audio's process objects (macOS 14.2+) say which processes are recording; older systems only say the
 *    microphone is in use, which counts for "*". Core Audio calls our listeners on its own thread: they write a byte
 *    to a pipe, the fd the service and the engine already poll for pactl on Linux. */
#include "mac.h"
#include "../linux/desktop.h"
#include "../common/util.h"
#include <ApplicationServices/ApplicationServices.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <net/if.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ displays */

void mac_display_id(uint32_t display, wchar_t *out, size_t cap)
{
    uint32_t v = CGDisplayVendorNumber(display), m = CGDisplayModelNumber(display), s = CGDisplaySerialNumber(display);
    unsigned a = (v >> 10) & 31, b = (v >> 5) & 31, c = v & 31;
    if (a && b && c && a <= 26 && b <= 26 && c <= 26 && s && s != 0x01010101u)
        str_printf(out, cap, L"EDID:%lc%lc%lc%04X:%lu", (wint_t)('@' + a), (wint_t)('@' + b), (wint_t)('@' + c), m & 0xFFFF, (unsigned long)s);
    else
        str_printf(out, cap, L"MAC:%x-%x-%u", v, m, CGDisplayUnitNumber(display));
}

int mac_monitors(MonitorInfo *out, int max)
{
    CGDirectDisplayID ids[SP_MAX_MONITORS];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(SP_MAX_MONITORS, ids, &n) != kCGErrorSuccess) return 0;
    int count = 0;
    for (uint32_t i = 0; i < n && count < max; i++) {
        if (CGDisplayIsInMirrorSet(ids[i]) && CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) continue;
        MonitorInfo *o = &out[count];
        memset(o, 0, sizeof *o);
        mac_display_id(ids[i], o->id, SP_COUNTOF(o->id));
        str_printf(o->oldId, SP_COUNTOF(o->oldId), L"CG:%u", ids[i]);
        CGRect r = CGDisplayBounds(ids[i]);                /* points, origin top left of the main display */
        o->rect.left = (LONG)r.origin.x;
        o->rect.top = (LONG)r.origin.y;
        o->rect.right = o->rect.left + (LONG)r.size.width;
        o->rect.bottom = o->rect.top + (LONG)r.size.height;
        o->work = o->rect;
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
        double hz = 0, scale = 1;
        if (mode) {
            hz = CGDisplayModeGetRefreshRate(mode);
            if (CGDisplayModeGetWidth(mode)) scale = (double)CGDisplayModeGetPixelWidth(mode) / (double)CGDisplayModeGetWidth(mode);
            CGDisplayModeRelease(mode);
        }
        o->dpi = (UINT)(96 * scale + 0.5);
        o->refreshHz = hz > 0 ? (UINT)(hz + 0.5) : 60;    /* built-in panels report 0 */
        o->primary = CGDisplayIsMain(ids[i]) != 0;
        if (CGDisplayIsBuiltin(ids[i])) str_copy(o->name, SP_COUNTOF(o->name), L"Built-in display");
        else str_printf(o->name, SP_COUNTOF(o->name), L"Display %d", count + 1);
        count++;
    }
    for (int i = 1; i < count; i++)                        /* primary first, like Windows */
        if (out[i].primary) {
            MonitorInfo t = out[i];
            memmove(&out[1], &out[0], sizeof *out * (size_t)i);
            out[0] = t;
        }
    for (int i = 0; i < count; i++) out[i].index = i + 1;
    edid_dedupe(out, count);
    return count;
}

/* ------------------------------------------------------------------ power, lock */

int mac_on_battery(void)
{
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return 0;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    int on = type && CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    CFRelease(info);
    return on;
}

int mac_session_locked(void)
{
    CFDictionaryRef d = CGSessionCopyCurrentDictionary();
    if (!d) return 0;
    CFBooleanRef v = (CFBooleanRef)CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
    int locked = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    CFRelease(d);
    return locked;
}

/* ------------------------------------------------------------------ windows */

static int num(CFDictionaryRef d, CFStringRef key)
{
    CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(d, key);
    int v = 0;
    if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

/* Ordinary app windows on screen, front to back (layer 0: no menu bar, Dock, desktop or our own windows). */
static CFArrayRef app_windows(void)
{
    return CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
}

int mac_front_pid(void)
{
    CFArrayRef list = app_windows();
    int pid = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && !pid; i++) {
        CFDictionaryRef w = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        if (num(w, kCGWindowLayer) == 0) pid = num(w, kCGWindowOwnerPID);
    }
    if (list) CFRelease(list);
    return pid;
}

int mac_display_covered(uint32_t display, int covered)
{
    CGRect screen = CGDisplayBounds(display);
    CFArrayRef list = app_windows();
    int on = 0, self = getpid();
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && !on; i++) {
        CFDictionaryRef w = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CGRect r;
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(w, kCGWindowBounds);
        if (num(w, kCGWindowLayer) != 0 || num(w, kCGWindowOwnerPID) == self || !b || !CGRectMakeWithDictionaryRepresentation(b, &r)) continue;
        CGRect in = CGRectIntersection(r, screen);
        if (CGRectIsNull(in)) continue;
        if (CGRectEqualToRect(in, screen)) on = 1;                          /* full screen */
        else if (covered && in.size.width >= screen.size.width - 2 && in.size.height >= screen.size.height - 80) on = 1;   /* zoomed: all but menu bar and Dock */
    }
    if (list) CFRelease(list);
    return on;
}

/* ------------------------------------------------------------------ system data for pages */

void mac_cpu_name(char *out, size_t cap)
{
    size_t n = cap;
    if (sysctlbyname("machdep.cpu.brand_string", out, &n, NULL, 0)) snprintf(out, cap, "Apple silicon");
}

void mac_sys_sample(unsigned long long *busy, unsigned long long *idle, double *totalMb, double *availMb,
                    unsigned long long *rx, unsigned long long *tx)
{
    host_cpu_load_info_data_t cpu;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &n) == KERN_SUCCESS) {
        *busy = (unsigned long long)cpu.cpu_ticks[CPU_STATE_USER] + cpu.cpu_ticks[CPU_STATE_SYSTEM] + cpu.cpu_ticks[CPU_STATE_NICE];
        *idle = cpu.cpu_ticks[CPU_STATE_IDLE];
    }
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    *totalMb = (double)mem / 1048576.0;
    vm_statistics64_data_t vm;
    n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n) == KERN_SUCCESS)
        *availMb = (double)(vm.free_count + vm.inactive_count + vm.purgeable_count) * (double)vm_page_size / 1048576.0;
    struct ifaddrs *ifs = NULL;
    *rx = *tx = 0;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_LINK || (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_data) continue;
            const struct if_data *d = (const struct if_data *)i->ifa_data;
            *rx += d->ifi_ibytes;
            *tx += d->ifi_obytes;
        }
        freeifaddrs(ifs);
    }
}

/* ------------------------------------------------------------------ app rules */

static int has_utf8(const wchar_t *rules, const char *name)
{
    if (!name || !name[0]) return 0;
    wchar_t *w = wide_from_utf8(name, -1);
    int hit = w && config_rule_has(rules, w);
    free(w);
    return hit;
}

/* The program's file, and for an app its bundle's name ("Safari" for .../Safari.app/Contents/MacOS/Safari). */
int sp_pid_matches(int pid, const wchar_t *rules)
{
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (pid <= 0 || !rules[0] || proc_pidpath(pid, path, sizeof path) <= 0) return 0;
    if (has_utf8(rules, path)) return 1;
    char *app = strstr(path, ".app/Contents/MacOS/");
    if (!app) return 0;
    *app = 0;
    char *name = strrchr(path, '/');
    return has_utf8(rules, name ? name + 1 : path);
}

/* Core Audio's process objects (macOS 14.2): spelled out, so older SDKs build too. */
#define SP_PROCESS_LIST  0x70727323u    /* 'prs#' kAudioHardwarePropertyProcessObjectList */
#define SP_PROCESS_PID   0x70706964u    /* 'ppid' kAudioProcessPropertyPID */
#define SP_PROCESS_INPUT 0x70697269u    /* 'piri' kAudioProcessPropertyIsRunningInput */

static AudioObjectID default_input(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
    return dev;
}

int sp_mic_busy(const wchar_t *rules)
{
    if (!rules[0]) return 0;
    int any = config_rule_has(rules, L"*");
    AudioObjectPropertyAddress list = { SP_PROCESS_LIST, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list, 0, NULL, &size) == noErr && size) {
        AudioObjectID procs[256];
        if (size > sizeof procs) size = sizeof procs;
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &list, 0, NULL, &size, procs) != noErr) return 0;
        for (UInt32 i = 0; i < size / sizeof *procs; i++) {
            AudioObjectPropertyAddress in = { SP_PROCESS_INPUT, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                       pa = { SP_PROCESS_PID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            UInt32 running = 0, s = sizeof running;
            pid_t pid = 0;
            if (AudioObjectGetPropertyData(procs[i], &in, 0, NULL, &s, &running) != noErr || !running) continue;
            s = sizeof pid;
            if (any || (AudioObjectGetPropertyData(procs[i], &pa, 0, NULL, &s, &pid) == noErr && sp_pid_matches(pid, rules))) return 1;
        }
        return 0;
    }
    AudioObjectID dev = default_input();                   /* before 14.2: in use, but not by whom */
    AudioObjectPropertyAddress run = { kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 running = 0, s = sizeof running;
    return any && dev != kAudioObjectUnknown && AudioObjectGetPropertyData(dev, &run, 0, NULL, &s, &running) == noErr && running;
}

static int g_pipe[2] = { -1, -1 };
static AudioObjectID g_watchedDev = kAudioObjectUnknown;

static OSStatus on_audio(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *data)
{
    (void)obj; (void)n; (void)a; (void)data;
    char b = 1;
    if (g_pipe[1] >= 0 && write(g_pipe[1], &b, 1) < 0) {}
    return noErr;
}

static const AudioObjectPropertyAddress k_procs = { SP_PROCESS_LIST, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                        k_run = { kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

int sp_mic_watch_start(void)
{
    if (g_pipe[0] >= 0) return g_pipe[0];
    if (pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK)) return -1;
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &k_procs, on_audio, NULL);
    g_watchedDev = default_input();
    if (g_watchedDev != kAudioObjectUnknown) AudioObjectAddPropertyListener(g_watchedDev, &k_run, on_audio, NULL);
    return g_pipe[0];
}

int sp_mic_watch_read(void)
{
    char buf[256];
    int hit = 0;
    ssize_t n;
    while ((n = read(g_pipe[0], buf, sizeof buf)) > 0) hit = 1;
    return hit;
}

void sp_mic_watch_stop(void)
{
    if (g_pipe[0] < 0) return;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &k_procs, on_audio, NULL);
    if (g_watchedDev != kAudioObjectUnknown) AudioObjectRemovePropertyListener(g_watchedDev, &k_run, on_audio, NULL);
    g_watchedDev = kAudioObjectUnknown;
    close(g_pipe[0]);
    close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
}
