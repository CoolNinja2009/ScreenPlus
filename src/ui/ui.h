/* Management window: Win32 + Direct2D/DirectWrite, drawn on demand (no render loop).
 * The window and all its GPU resources are destroyed when closed to the tray. */
#pragma once
#include "../common/common.h"

BOOL ui_show(void);
void ui_destroy(void);
void ui_on_status(void);          /* engine status / app state changed */
void ui_on_config_changed(void);  /* library or settings changed */
void ui_on_customise_changed(void); /* look or wallpaper properties changed elsewhere (CLI) */
BOOL ui_translate(MSG *m);        /* keyboard shortcuts; TRUE if consumed */
