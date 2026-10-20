#pragma once
#include "common.h"

typedef enum LogLevel { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

/* Opens %LOCALAPPDATA%\ScreenPlus\logs\<component>.log (rotated at 1 MB, 2 files kept). */
void log_init(const wchar_t *component);
void log_shutdown(void);
void log_write(LogLevel level, const wchar_t *fmt, ...);
void log_path(wchar_t *dst, size_t cap);

#define LOGD(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_write(LOG_ERROR, __VA_ARGS__)
