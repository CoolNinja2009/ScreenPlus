#include "paths.h"
#include "util.h"
#include <shlobj.h>

static wchar_t g_root[SP_MAX_PATH], g_config[SP_MAX_PATH], g_backup[SP_MAX_PATH], g_logs[SP_MAX_PATH],
               g_crash[SP_MAX_PATH], g_cache[SP_MAX_PATH], g_webview[SP_MAX_PATH], g_exe[SP_MAX_PATH],
               g_exeDir[SP_MAX_PATH], g_downloads[SP_MAX_PATH];

void paths_init(void)
{
    if (g_root[0]) return;
    PWSTR local = NULL;
    wchar_t over[SP_MAX_PATH];
    DWORD on = GetEnvironmentVariableW(L"SCREENPLUS_DATA_DIR", over, SP_COUNTOF(over));   /* portable / test override */
    if (on > 0 && on < SP_COUNTOF(over)) {
        str_copy(g_root, SP_COUNTOF(g_root), over);
    } else if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &local)) && local) {
        path_join(g_root, SP_COUNTOF(g_root), local, SP_APP_ID);
    } else {
        GetTempPathW(SP_COUNTOF(g_root), g_root);
        str_cat(g_root, SP_COUNTOF(g_root), SP_APP_ID);
    }
    CoTaskMemFree(local);
    path_join(g_config, SP_COUNTOF(g_config), g_root, L"config.ini");
    path_join(g_backup, SP_COUNTOF(g_backup), g_root, L"config.bak");
    path_join(g_logs, SP_COUNTOF(g_logs), g_root, L"logs");
    path_join(g_crash, SP_COUNTOF(g_crash), g_logs, L"crash");
    path_join(g_cache, SP_COUNTOF(g_cache), g_root, L"cache");
    path_join(g_webview, SP_COUNTOF(g_webview), g_root, L"WebView2");
    path_join(g_downloads, SP_COUNTOF(g_downloads), g_root, L"downloads");
    dir_create_recursive(g_logs);
    dir_create_recursive(g_cache);

    DWORD n = GetModuleFileNameW(NULL, g_exe, SP_COUNTOF(g_exe));
    if (n == 0 || n >= SP_COUNTOF(g_exe)) g_exe[0] = 0;
    path_dirname(g_exeDir, SP_COUNTOF(g_exeDir), g_exe);
}

const wchar_t *paths_root(void)          { return g_root; }
const wchar_t *paths_config(void)        { return g_config; }
const wchar_t *paths_config_backup(void) { return g_backup; }
const wchar_t *paths_logs(void)          { return g_logs; }
const wchar_t *paths_crash(void)         { return g_crash; }
const wchar_t *paths_cache(void)         { return g_cache; }
const wchar_t *paths_webview(void)       { return g_webview; }
const wchar_t *paths_downloads(void)     { return g_downloads; }
const wchar_t *paths_exe(void)           { return g_exe; }
const wchar_t *paths_exe_dir(void)       { return g_exeDir; }
