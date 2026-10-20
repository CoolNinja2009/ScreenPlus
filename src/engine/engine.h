/* Wallpaper engine process ("ScreenPlus.exe --engine"): owns the desktop-hosted wallpaper
 * windows and all renderers, runs the adaptive state machine and per-monitor recovery.
 * It is supervised by the shell process, which restarts it with backoff if it crashes. */
#pragma once
#include "../common/common.h"

int engine_main(HINSTANCE inst, HWND shell, int safeMode);
