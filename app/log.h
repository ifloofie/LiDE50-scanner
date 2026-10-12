// LiDE50 Scan - activity log and crash reporting.
//
// Everything that happens from startup to the end of a scan is written to
// LiDE50Scan.log next to the executable (created if missing). Lines are written
// straight to the file (no buffering), so the log survives a crash.
#pragma once

#include <string>

// Opens (or creates) the log. Tries next to the exe first; `fallback_dir` is
// used only when the exe folder is read-only. Returns the path in use.
std::wstring log_open(const std::wstring& exe_dir, const std::wstring& fallback_dir);
std::wstring log_path();

// printf-style logging. Levels: "INFO", "WARN", "ERROR", "DEBUG", "USB", "DRV", "UI", "CRASH"
void logf(const char* level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void logw(const char* level, const std::wstring& text);

#define LOG_INFO(...) logf("INFO", __VA_ARGS__)
#define LOG_WARN(...) logf("WARN", __VA_ARGS__)
#define LOG_ERROR(...) logf("ERROR", __VA_ARGS__)
#define LOG_UI(...) logf("UI", __VA_ARGS__)

std::string to_utf8(const std::wstring& w);

// Unhandled exceptions, std::terminate, abort, invalid CRT parameters and
// stack overflows are written to the log with a stack trace before exit.
void install_crash_handlers();

// Call at the start of every thread that runs scanner/driver code, so a stack
// overflow can still be logged.
void prepare_thread_for_crash_logging(const char* thread_name);

// Writes the current call stack to the log (for errors that are caught).
void log_stack_trace(const char* reason);

// Logs a summary of the computer (Windows version, memory, paths).
void log_system_info(const std::wstring& data_dir, const std::wstring& command_line);
