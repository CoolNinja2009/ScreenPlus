#pragma once
#include "../common/common.h"

BOOL tray_init(HWND owner, HINSTANCE inst);
void tray_update(void);
void tray_remove(void);
void tray_on_taskbar_created(void);
void tray_on_message(WPARAM wp, LPARAM lp);
void tray_on_command(UINT id);
