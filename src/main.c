/* Screen+ entry point. One executable, two roles:
 *   ScreenPlus.exe [--tray|--show|--pause|--resume|--toggle-pause|--next|--exit]   shell (UI, tray, supervisor)
 *   ScreenPlus.exe --engine --shell=<hwnd> [--safe-mode]                          wallpaper engine (spawned by shell)
 *   ScreenPlus.exe --capture-preview <wallpaper id>                               records an HTML preview (spawned by shell) */
#include "common/common.h"
#include "common/paths.h"
#include "common/util.h"
#include "app/app.h"
#include "engine/engine.h"
#include "app/preview.h"
#include <shellapi.h>
#include <stdlib.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev; (void)cmdLine; (void)show;
    /* Hardening: DLLs only from the application directory and System32 (no current directory). */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    paths_init();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    int engine = 0, safeMode = 0;
    HWND shell = NULL;
    const wchar_t *capture = NULL;
    for (int i = 1; i < argc; i++) {
        if (str_ieq(argv[i], L"--engine")) engine = 1;
        else if (str_ieq(argv[i], L"--safe-mode")) safeMode = 1;
        else if (str_istarts(argv[i], L"--shell=")) shell = (HWND)(uintptr_t)wcstoull(argv[i] + 8, NULL, 16);
        else if (str_ieq(argv[i], L"--capture-preview") && i + 1 < argc) capture = argv[++i];
    }
    int rc = capture ? preview_capture_main(capture) : engine ? engine_main(inst, shell, safeMode) : app_main(inst, argc, argv);
    LocalFree(argv);
    return rc;
}
