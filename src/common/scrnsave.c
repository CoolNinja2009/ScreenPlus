#include "scrnsave.h"
#include "util.h"
#include "paths.h"

#define DESKTOP_KEY L"Control Panel\\Desktop"
#define POLICY_KEY  L"Software\\Policies\\Microsoft\\Windows\\Control Panel\\Desktop"

static void app_file(wchar_t *buf, size_t cap, const wchar_t *name)
{
    path_join(buf, cap, paths_exe_dir(), name);
}

void scr_path(wchar_t *buf, size_t cap) { app_file(buf, cap, L"ScreenPlus.scr"); }

static int reg_value_exists(const wchar_t *key, const wchar_t *name)
{
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_ANY, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static int same_file(const wchar_t *a, const wchar_t *b)
{
    wchar_t la[SP_MAX_PATH], lb[SP_MAX_PATH];
    if (!GetLongPathNameW(a, la, SP_COUNTOF(la))) str_copy(la, SP_COUNTOF(la), a);
    if (!GetLongPathNameW(b, lb, SP_COUNTOF(lb))) str_copy(lb, SP_COUNTOF(lb), b);
    return str_ieq(la, lb);
}

void scr_query(ScrState *st)
{
    ZeroMemory(st, sizeof *st);
    DWORD cb = sizeof st->current;
    if (RegGetValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"SCRNSAVE.EXE", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL,
                     st->current, &cb) != ERROR_SUCCESS)
        st->current[0] = 0;
    BOOL b = FALSE;
    UINT t = 0;
    SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &b, 0);
    st->active = b && st->current[0];    /* "(None)" in Screen Saver Settings removes SCRNSAVE.EXE */
    if (SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT, 0, &t, 0)) st->timeoutSec = (int)t;
    if (st->timeoutSec <= 0) {
        wchar_t buf[32] = L"";
        DWORD sz = sizeof buf;
        if (RegGetValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"ScreenSaveTimeOut", RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS)
            st->timeoutSec = _wtoi(buf);
    }
    b = FALSE;
    SystemParametersInfoW(SPI_GETSCREENSAVESECURE, 0, &b, 0);
    st->secure = b != 0;
    if (!st->secure) {
        wchar_t buf[32] = L"";
        DWORD sz = sizeof buf;
        if (RegGetValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"ScreenSaverIsSecure", RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS)
            st->secure = _wtoi(buf) != 0;
    }
    wchar_t ours[SP_MAX_PATH];
    scr_path(ours, SP_COUNTOF(ours));
    st->ours = st->current[0] && same_file(st->current, ours);
    st->policy = reg_value_exists(POLICY_KEY, L"SCRNSAVE.EXE") || reg_value_exists(POLICY_KEY, L"ScreenSaveActive");
}

BOOL scr_sync_binary(void)
{
    wchar_t exe[SP_MAX_PATH], scr[SP_MAX_PATH];
    app_file(exe, SP_COUNTOF(exe), L"ScreenPlus.exe");
    scr_path(scr, SP_COUNTOF(scr));
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(exe, GetFileExInfoStandard, &a)) return file_exists(scr);
    if (GetFileAttributesExW(scr, GetFileExInfoStandard, &b) && a.nFileSizeLow == b.nFileSizeLow &&
        a.nFileSizeHigh == b.nFileSizeHigh && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0)
        return TRUE;                     /* CopyFile keeps the write time: same build */
    return CopyFileW(exe, scr, FALSE) || file_exists(scr);   /* a running copy stays usable */
}

BOOL scr_enable(BOOL on, wchar_t *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    ScrState st;
    scr_query(&st);
    if (!on) {
        if (!st.ours) return TRUE;
        RegDeleteKeyValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"SCRNSAVE.EXE");
        SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        return TRUE;
    }
    wchar_t scr[SP_MAX_PATH], shortPath[SP_MAX_PATH];
    scr_path(scr, SP_COUNTOF(scr));
    if (!scr_sync_binary()) {
        if (err) str_printf(err, cap, L"Could not create %ls (error %lu).", scr, GetLastError());
        return FALSE;
    }
    /* Windows has launched screensavers by their 8.3 name since NT; a path with spaces may not start. */
    DWORD n = GetShortPathNameW(scr, shortPath, SP_COUNTOF(shortPath));
    const wchar_t *value = n && n < SP_COUNTOF(shortPath) ? shortPath : scr;
    LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"SCRNSAVE.EXE", REG_SZ, value,
                                 (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) {
        if (err) str_printf(err, cap, L"Could not register the screensaver (error %ld).", (long)rc);
        return FALSE;
    }
    if (st.timeoutSec <= 0) scr_set_timeout(300);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    if (st.policy && err) str_copy(err, cap, L"Group Policy manages the screensaver on this PC, so Windows may keep its own.");
    return TRUE;
}

BOOL scr_set_timeout(int seconds)
{
    int s = SP_CLAMP(seconds, 60, 86400);
    wchar_t val[16];
    str_printf(val, SP_COUNTOF(val), L"%d", s);
    RegSetKeyValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"ScreenSaveTimeOut", REG_SZ, val,
                    (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    return SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, (UINT)s, NULL,
                                 SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

BOOL scr_set_secure(BOOL on)
{
    const wchar_t *val = on ? L"1" : L"0";
    RegSetKeyValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"ScreenSaverIsSecure", REG_SZ, val,
                    (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    return SystemParametersInfoW(SPI_SETSCREENSAVESECURE, on ? TRUE : FALSE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

BOOL scr_launch(const wchar_t *args)
{
    scr_sync_binary();
    wchar_t exe[SP_MAX_PATH], cmd[SP_MAX_PATH + 64];
    app_file(exe, SP_COUNTOF(exe), L"ScreenPlus.exe");
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --screensaver %ls", exe, args ? args : L"/s");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}
