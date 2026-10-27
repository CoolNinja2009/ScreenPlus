#pragma once
#include "../common/common.h"

/* ScreenPlus.scr (asScr) or ScreenPlus.exe --screensaver: /s, /p <hwnd>, /c. Returns the exit code. */
int screensaver_main(HINSTANCE inst, int argc, wchar_t **argv, int asScr);
