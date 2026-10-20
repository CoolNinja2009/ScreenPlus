/* "Start with Windows": per-user Run key (no elevation). Task Manager's StartupApproved
 * state is honoured when reading and reset when the user re-enables it from Screen+. */
#include "app.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/paths.h"

#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define VALUE_NAME   L"ScreenPlus"

BOOL startup_is_enabled(void)
{
    wchar_t v[SP_MAX_PATH + 32];
    DWORD size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME, RRF_RT_REG_SZ, NULL, v, &size) != ERROR_SUCCESS) return FALSE;
    BYTE approved[12];
    size = sizeof approved;
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, VALUE_NAME, RRF_RT_REG_BINARY, NULL, approved, &size) == ERROR_SUCCESS &&
        size >= 1 && (approved[0] & 1))
        return FALSE;   /* disabled in Task Manager / Settings */
    return TRUE;
}

BOOL startup_set_enabled(BOOL on)
{
    LSTATUS st;
    if (on) {
        wchar_t cmd[SP_MAX_PATH + 32];
        str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --tray", paths_exe());
        st = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME, REG_SZ, cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, VALUE_NAME);
    } else {
        st = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    if (st != ERROR_SUCCESS) LOGW(L"startup: registry update failed (%ld)", (long)st);
    return st == ERROR_SUCCESS;
}
