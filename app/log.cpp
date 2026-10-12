// LiDE50 Scan - activity log and crash reporting.
#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifndef LIDE50_VERSION
#define LIDE50_VERSION "1.3.0"
#endif

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
volatile LONG g_crashing = 0;

void raw_write(const char* data, size_t len)
{
    if (g_file == INVALID_HANDLE_VALUE || len == 0) return;
    DWORD written = 0;
    WriteFile(g_file, data, static_cast<DWORD>(len), &written, nullptr);
}

std::string timestamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

// Writes one or more lines, each with timestamp, thread id and level.
void emit(const char* level, const char* text)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    std::string out;
    std::string prefix = timestamp() + " [" + std::to_string(GetCurrentThreadId()) + "] " + level;
    while (prefix.size() < 40) prefix += ' ';
    const char* p = text;
    do {
        const char* nl = std::strchr(p, '\n');
        size_t n = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
        if (n > 0 && p[n - 1] == '\r') --n;
        if (n > 0 || !nl) {
            out += prefix;
            out += ' ';
            out.append(p, n);
            out += "\r\n";
        }
        p = nl ? nl + 1 : nullptr;
    } while (p && *p);

    if (g_crashing) {
        raw_write(out.data(), out.size()); // never block while crashing
        return;
    }
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    raw_write(out.data(), out.size());
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

std::wstring module_name_for(const void* addr, uintptr_t& base)
{
    HMODULE mod = nullptr;
    base = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) || !mod)
        return L"?";
    base = reinterpret_cast<uintptr_t>(mod);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s = path;
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(slash + 1);
}

std::string describe_address(DWORD64 pc)
{
    uintptr_t base = 0;
    std::wstring mod = module_name_for(reinterpret_cast<const void*>(pc), base);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "0x%016llx  %s+0x%llx", static_cast<unsigned long long>(pc),
                  to_utf8(mod).c_str(), static_cast<unsigned long long>(pc - base));
    return buf;
}

// Walks the stack using the x64 unwind tables (works without symbols).
void walk_stack(CONTEXT ctx, const char* level)
{
    for (int frame = 0; frame < 48; ++frame) {
        DWORD64 pc = ctx.Rip;
        if (pc == 0) break;
        char line[320];
        std::snprintf(line, sizeof(line), "  #%02d %s", frame, describe_address(pc).c_str());
        emit(level, line);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if (!fn) {
            // leaf function: return address is on top of the stack
            if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, fn, &ctx, &handler_data, &establisher, nullptr);
        }
    }
}

const char* exception_name(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION (invalid memory access)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLOAT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INTEGER_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INVALID_HANDLE: return "INVALID_HANDLE";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN / fast fail";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0x20474343: return "unhandled C++ exception";
        default: return "unknown exception";
    }
}

void show_crash_box(const std::string& what)
{
    std::wstring msg = L"LiDE50 Scan ran into a problem and has to close.\n\n" +
                       std::wstring(what.begin(), what.end()) +
                       L"\n\nThe details were written to:\n" + g_path +
                       L"\n\nPlease send this log file so the problem can be fixed.";
    MessageBoxW(nullptr, msg.c_str(), L"LiDE50 Scan - error", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_TASKMODAL);
}

void log_exception_record(EXCEPTION_POINTERS* ep, const char* where)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char line[512];
    emit("CRASH", "==================== CRASH ====================");
    std::snprintf(line, sizeof(line), "%s: exception 0x%08lx %s", where,
                  static_cast<unsigned long>(er->ExceptionCode), exception_name(er->ExceptionCode));
    emit("CRASH", line);
    std::snprintf(line, sizeof(line), "at %s", describe_address(reinterpret_cast<DWORD64>(er->ExceptionAddress)).c_str());
    emit("CRASH", line);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0 ? "reading" : (er->ExceptionInformation[0] == 1 ? "writing" : "executing");
        std::snprintf(line, sizeof(line), "while %s address 0x%016llx", op,
                      static_cast<unsigned long long>(er->ExceptionInformation[1]));
        emit("CRASH", line);
    }
    CONTEXT* c = ep->ContextRecord;
    std::snprintf(line, sizeof(line),
                  "RIP=%016llx RSP=%016llx RBP=%016llx RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx",
                  static_cast<unsigned long long>(c->Rip), static_cast<unsigned long long>(c->Rsp),
                  static_cast<unsigned long long>(c->Rbp), static_cast<unsigned long long>(c->Rax),
                  static_cast<unsigned long long>(c->Rbx), static_cast<unsigned long long>(c->Rcx),
                  static_cast<unsigned long long>(c->Rdx));
    emit("CRASH", line);
    std::snprintf(line, sizeof(line), "thread %lu, stack trace (module+offset):",
                  static_cast<unsigned long>(GetCurrentThreadId()));
    emit("CRASH", line);
    walk_stack(*c, "CRASH");
    emit("CRASH", "===============================================");
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&g_crashing, 1) == 0) {
        log_exception_record(ep, "Unhandled exception");
        char what[128];
        std::snprintf(what, sizeof(what), "Error 0x%08lX (%s)",
                      static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                      exception_name(ep->ExceptionRecord->ExceptionCode));
        show_crash_box(what);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI vectored_handler(EXCEPTION_POINTERS* ep)
{
    // A stack overflow may not reach the unhandled filter, so log it here.
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW &&
        InterlockedExchange(&g_crashing, 1) == 0) {
        log_exception_record(ep, "Stack overflow");
        TerminateProcess(GetCurrentProcess(), 0xC00000FD);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_current_stack(const char* level)
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    walk_stack(ctx, level);
}

void terminate_handler()
{
    InterlockedExchange(&g_crashing, 1);
    std::string what = "std::terminate called (unhandled C++ exception)";
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            what = std::string("Unhandled C++ exception: ") + e.what();
        } catch (...) {
            what = "Unhandled C++ exception of unknown type";
        }
    }
    emit("CRASH", "==================== CRASH ====================");
    emit("CRASH", what.c_str());
    emit("CRASH", "stack trace (module+offset):");
    log_current_stack("CRASH");
    emit("CRASH", "===============================================");
    show_crash_box(what);
    TerminateProcess(GetCurrentProcess(), 3);
}

void abort_handler(int)
{
    if (InterlockedExchange(&g_crashing, 1) != 0) return;
    emit("CRASH", "==================== CRASH ====================");
    emit("CRASH", "abort() was called");
    log_current_stack("CRASH");
    show_crash_box("abort() was called");
    TerminateProcess(GetCurrentProcess(), 3);
}

void invalid_parameter_handler(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "C runtime reported an invalid parameter: %ls in %ls (%ls:%u)",
                  expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    emit("ERROR", buf);
    log_current_stack("ERROR");
}

void purecall_handler()
{
    InterlockedExchange(&g_crashing, 1);
    emit("CRASH", "pure virtual function call");
    log_current_stack("CRASH");
    show_crash_box("pure virtual function call");
    TerminateProcess(GetCurrentProcess(), 3);
}

bool open_at(const std::wstring& path)
{
    // keep the log from growing forever: start a new one past 20 MB
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        if (size > 20ull * 1024 * 1024) {
            std::wstring old = path.substr(0, path.size() - 4) + L".old.log";
            MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    g_file = h;
    g_path = path;
    return true;
}

thread_local std::string t_driver_line;

// Receives the scanner driver's output (category "DRV" may arrive in
// fragments; USB/WARN/ERROR lines are complete).
extern "C" void driver_log_sink(const char* category, const char* text)
{
    if (std::strcmp(category, "DRV") != 0) {
        emit(category, text);
        return;
    }
    t_driver_line += text;
    size_t nl;
    while ((nl = t_driver_line.find('\n')) != std::string::npos) {
        std::string line = t_driver_line.substr(0, nl);
        t_driver_line.erase(0, nl + 1);
        if (!line.empty()) emit("DRV", line.c_str());
    }
    if (t_driver_line.size() > 4000) {
        emit("DRV", t_driver_line.c_str());
        t_driver_line.clear();
    }
}

} // namespace

extern "C" void lide50_set_log_sink(void (*sink)(const char*, const char*));

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring log_open(const std::wstring& exe_dir, const std::wstring& fallback_dir)
{
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    bool ok = open_at(exe_dir + L"\\LiDE50Scan.log");
    if (!ok) {
        CreateDirectoryW(fallback_dir.c_str(), nullptr);
        ok = open_at(fallback_dir + L"\\LiDE50Scan.log");
    }
    if (ok) {
        emit("INFO", "");
        emit("INFO", "################################################################");
        emit("INFO", "LiDE50 Scan " LIDE50_VERSION " started (build " __DATE__ " " __TIME__ ")");
        if (g_path.compare(0, exe_dir.size(), exe_dir) != 0)
            emit("WARN", "The program folder is read-only, so the log is kept in the data folder instead.");
    }
    lide50_set_log_sink(driver_log_sink);
    return g_path;
}

std::wstring log_path() { return g_path; }

void logf(const char* level, const char* fmt, ...)
{
    char stack_buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof(stack_buf)) {
        emit(level, stack_buf);
        return;
    }
    std::vector<char> big(static_cast<size_t>(n) + 1);
    va_start(ap, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    emit(level, big.data());
}

void logw(const char* level, const std::wstring& text) { emit(level, to_utf8(text).c_str()); }

void install_crash_handlers()
{
    SetUnhandledExceptionFilter(unhandled_filter);
    AddVectoredExceptionHandler(1, vectored_handler);
    std::set_terminate(terminate_handler);
    std::signal(SIGABRT, abort_handler);
    _set_invalid_parameter_handler(invalid_parameter_handler);
    _set_purecall_handler(purecall_handler);
    prepare_thread_for_crash_logging("main/UI");
}

void prepare_thread_for_crash_logging(const char* thread_name)
{
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    logf("DEBUG", "thread '%s' started", thread_name);
}

void log_stack_trace(const char* reason)
{
    emit("ERROR", reason);
    log_current_stack("ERROR");
}

void log_system_info(const std::wstring& data_dir, const std::wstring& command_line)
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        fn(&vi);
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    wchar_t cwd[MAX_PATH * 2] = {};
    GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                       : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64 (x64 emulation)" : "other";
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    logf("INFO", "Windows %lu.%lu build %lu, %s%s, %lu CPUs, RAM %llu MB total / %llu MB free",
         static_cast<unsigned long>(vi.dwMajorVersion), static_cast<unsigned long>(vi.dwMinorVersion),
         static_cast<unsigned long>(vi.dwBuildNumber), arch, wine ? " (Wine)" : "",
         static_cast<unsigned long>(si.dwNumberOfProcessors),
         static_cast<unsigned long long>(ms.ullTotalPhys / 1048576),
         static_cast<unsigned long long>(ms.ullAvailPhys / 1048576));
    logw("INFO", std::wstring(L"Program: ") + exe);
    logw("INFO", std::wstring(L"Command line: ") + command_line);
    logw("INFO", std::wstring(L"Working folder: ") + cwd);
    logw("INFO", L"Settings/calibration folder: " + data_dir);
    logw("INFO", L"Log file: " + g_path);
    logf("INFO", "Running as administrator: %s", IsUserAnAdmin() ? "yes" : "no");
}
