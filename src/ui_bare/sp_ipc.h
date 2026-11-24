#pragma once
#include <stdbool.h>
#include "../common/json.h"

/* Sends argv (UTF-8, NULL-terminated) to the Screen+ service as ScreenPlus.com does and
 * returns its answer, {"ok":bool,"text":"...","data":...}; NULL if Screen+ isn't running
 * or didn't answer. Call from one thread only. */
JVal *sp_request(const char *const *argv);
bool  sp_service_running(void);
#ifndef _WIN32
void  sp_set_cli(const char *path);
#endif
