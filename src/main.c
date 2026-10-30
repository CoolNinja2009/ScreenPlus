/* Screen+ entry point. One executable, two roles:
 *   ScreenPlus.exe [--tray|--startup|--show|--pause|--resume|--toggle-pause|--next|--exit]   shell (UI, tray, supervisor)
 *   ScreenPlus.exe --engine --shell=<hwnd> [--safe-mode]                          wallpaper engine (spawned by shell)
 *   ScreenPlus.exe --capture-preview <wallpaper id>                               records an HTML preview (spawned by shell)
 *   ScreenPlus.exe --depth <image> <dir> [--cpu | --bench | --crashed] [--png]    depth map of one image (spawned by shell)
 *   ScreenPlus.scr /s | /p <hwnd> | /c,  ScreenPlus.exe --screensaver /s           screensaver (started by Windows / Preview) */
#include "common/common.h"
#include "common/paths.h"
#include "common/util.h"
#include "app/app.h"
#include "engine/engine.h"
#include "app/preview.h"
#include "screensaver/screensaver.h"
#include <shellapi.h>
#include <stdlib.h>

uint64_t g_mainEntered;     /* app.c logs how long something held the process before it ran */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev; (void)cmdLine; (void)show;
    g_mainEntered = filetime_now();
    /* Hardening: DLLs only from the application directory and System32 (no current directory). */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    paths_init();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    int engine = 0, safeMode = 0, depthMode = DEPTH_AUTO, wantPng = 0;
    int scr = str_ieq(path_ext(paths_exe()), L".scr");    /* 1: started as the .scr, 2: --screensaver */
    HWND shell = NULL;
    const wchar_t *capture = NULL, *depthImage = NULL, *depthDir = NULL;
    for (int i = 1; i < argc; i++) {
        if (str_ieq(argv[i], L"--engine")) engine = 1;
        else if (str_ieq(argv[i], L"--safe-mode")) safeMode = 1;
        else if (str_istarts(argv[i], L"--shell=")) shell = (HWND)(uintptr_t)wcstoull(argv[i] + 8, NULL, 16);
        else if (str_ieq(argv[i], L"--capture-preview") && i + 1 < argc) capture = argv[++i];
        else if (str_ieq(argv[i], L"--depth") && i + 2 < argc) { depthImage = argv[i + 1]; depthDir = argv[i + 2]; i += 2; }
        else if (str_ieq(argv[i], L"--cpu")) depthMode = DEPTH_CPU;
        else if (str_ieq(argv[i], L"--bench")) depthMode = DEPTH_BENCH;
        else if (str_ieq(argv[i], L"--crashed")) depthMode = DEPTH_CRASHED;
        else if (str_ieq(argv[i], L"--png")) wantPng = 1;
        else if (str_ieq(argv[i], L"--screensaver") && !scr) scr = 2;
    }
    int rc = scr ? screensaver_main(inst, argc, argv, scr == 1) : depthImage ? depth_infer_main(depthImage, depthDir, depthMode, wantPng)
           : capture ? preview_capture_main(capture) : engine ? engine_main(inst, shell, safeMode) : app_main(inst, argc, argv);
    LocalFree(argv);
    return rc;
}
