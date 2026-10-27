/* Windows screensaver registration, shared by the app and ScreenPlus.com (so it never logs).
 * Windows starts the program named in HKCU\Control Panel\Desktop\SCRNSAVE.EXE with /s after the
 * idle timeout, /p <hwnd> for the preview in Screen Saver Settings and /c for its Settings button.
 * Per user, no administrator rights: ScreenPlus.scr is a copy of ScreenPlus.exe next to it (so the
 * FFmpeg DLLs resolve; main.c sees the .scr name), and the timeout, the switch and "show the
 * sign-in screen on resume" go through SystemParametersInfo so the session picks them up at once. */
#pragma once
#include "common.h"

typedef struct ScrState {
    int     active;       /* a screensaver runs after the timeout */
    int     ours;         /* ...and it is ScreenPlus.scr */
    int     timeoutSec;
    int     secure;       /* sign-in screen on resume */
    int     policy;       /* Group Policy sets the screensaver: Windows ignores ours */
    wchar_t current[SP_MAX_PATH];   /* SCRNSAVE.EXE as stored ("" = none) */
} ScrState;

void scr_path(wchar_t *buf, size_t cap);                 /* <app dir>\ScreenPlus.scr */
void scr_query(ScrState *st);
/* On: refresh ScreenPlus.scr from ScreenPlus.exe and make it the Windows screensaver.
 * Off: only when ScreenPlus.scr is the current one (another screensaver is left alone). */
BOOL scr_enable(BOOL on, wchar_t *err, size_t cap);
BOOL scr_set_timeout(int seconds);
BOOL scr_set_secure(BOOL on);
BOOL scr_sync_binary(void);                              /* copies ScreenPlus.exe over a missing or stale .scr */
BOOL scr_launch(const wchar_t *args);                    /* ScreenPlus.exe --screensaver <args>, not waited for */
