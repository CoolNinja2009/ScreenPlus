/* "Start with Windows" in the app: autostart.c (shared with ScreenPlus.com) registers a Task Scheduler
 * logon task, or the Run key when it can't; this logs it and keeps the setting in step. The task passes
 * --startup, so "Start minimised to the tray" decides whether the window opens. */
#include "app.h"
#include "../common/autostart.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"

BOOL startup_set_enabled(BOOL on)
{
    AutostartKind kind;
    HRESULT hr;
    BOOL ok = autostart_set(on, paths_exe(), &kind, &hr);
    if (on && kind == AUTOSTART_RUN) LOGW(L"startup: logon task not registered (0x%08lX); using the Run key", (unsigned long)hr);
    else if (!ok) LOGW(L"startup: could not turn it %ls (task 0x%08lX)", on ? L"on" : L"off", (unsigned long)hr);
    else LOGI(L"startup: %ls", on ? L"logon task registered" : L"off");
    return ok;
}

/* Once the wallpaper is up (a Task Scheduler call never delays it): moves a Run entry, from an older
 * version or setup's fallback, to the logon task, and follows the executable if it moved. */
void startup_sync(void)
{
    wchar_t exe[SP_MAX_PATH];
    AutostartKind kind = autostart_query(exe, SP_COUNTOF(exe));
    if (kind == AUTOSTART_RUN || (kind == AUTOSTART_TASK && !str_ieq(exe, paths_exe()))) startup_set_enabled(TRUE);
    g_app.cfg.s.startWithWindows = kind != AUTOSTART_OFF;
}
