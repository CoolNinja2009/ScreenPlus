/* "Start with Windows", shared by the app and ScreenPlus.com (so it never logs).
 * A per-user Task Scheduler task with a logon trigger starts Screen+ as soon as the user signs in.
 * Programs in the HKCU Run key are held back by Explorer until the desktop has settled: 34 s after
 * Explorer started on a test boot, while Screen+ then needed 0.6 s for its first frame. The Run key
 * stays as the fallback for when the task can't be registered. Both need no administrator rights.
 * Callers initialise COM on their thread first. */
#pragma once
#include "common.h"

typedef enum { AUTOSTART_OFF, AUTOSTART_TASK, AUTOSTART_RUN } AutostartKind;

/* What starts Screen+ at sign-in now, and the executable it starts (exe may be NULL). A disabled task,
 * or a Run entry switched off in Task Manager, counts as off. */
AutostartKind autostart_query(wchar_t *exe, size_t cap);
/* On: the logon task for exe (the Run entry is removed), or the Run entry when the task can't be
 * registered (*taskHr says why). Off: both removed. FALSE when that could not be done. */
BOOL autostart_set(BOOL on, const wchar_t *exe, AutostartKind *kind, HRESULT *taskHr);
