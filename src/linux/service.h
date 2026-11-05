#pragma once
#include "../common/common.h"

int  service_main(void);                  /* `screenplus --daemon`; returns the exit code */
int  cli_main(int argc, char **argv);     /* everything else */
int  sp_autostart_set(int on);            /* ~/.config/autostart/screenplus.desktop */
int  sp_autostart_get(void);
void url_import_tick(void);               /* service loop: finishes a running download */
