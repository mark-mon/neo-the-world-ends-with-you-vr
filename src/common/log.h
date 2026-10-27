#pragma once
#include <windows.h>
#include <cstdarg>

namespace uxr {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };

void LogInit(const wchar_t* path, int level);
void LogSetLevel(int level);
int  LogGetLevel();
void LogWrite(int level, const char* fmt, ...);
void LogFlush();

} // namespace uxr

#define LOGE(...) ::uxr::LogWrite(::uxr::LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ::uxr::LogWrite(::uxr::LOG_WARN, __VA_ARGS__)
#define LOGI(...) ::uxr::LogWrite(::uxr::LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (::uxr::LogGetLevel() >= ::uxr::LOG_DEBUG) ::uxr::LogWrite(::uxr::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (::uxr::LogGetLevel() >= ::uxr::LOG_TRACE) ::uxr::LogWrite(::uxr::LOG_TRACE, __VA_ARGS__); } while (0)
