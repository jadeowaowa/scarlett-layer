#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <windows.h>

namespace sc::log {

enum class Level { Trace, Info, Warn, Stub, Error };

inline void Write(Level lvl, const char* module, const char* func, const char* detail = "") {
    const char* tag = "INFO";
    switch (lvl) {
        case Level::Trace: tag = "TRCE"; break;
        case Level::Info:  tag = "INFO"; break;
        case Level::Warn:  tag = "WARN"; break;
        case Level::Stub:  tag = "STUB"; break;
        case Level::Error: tag = "ERR "; break;
    }
    DWORD tid = GetCurrentThreadId();
    char line[1024];
    int n = std::snprintf(line, sizeof(line), "[%s][T:%05lu][%s] %s %s\n", tag, tid, module, func, detail);
    if (n > 0) {
        const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
        std::fwrite(line, 1, len, stderr);
        std::fflush(stderr);
        static const char* s_logPath = []() -> const char* {
            static char buf[MAX_PATH]{};
            DWORD got = GetEnvironmentVariableA("SCARLETT_LOG_FILE", buf, MAX_PATH);
            return (got > 0 && got < MAX_PATH) ? buf : nullptr;
        }();
        if (s_logPath) {
            HANDLE hFile = CreateFileA(s_logPath, FILE_APPEND_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
                CloseHandle(hFile);
            }
        }
    }
}

} // namespace sc::log

#define SC_STUB_ONCE(mod, ret_val)                                         \
    do {                                                                   \
        static std::atomic<bool> s_logged{false};                          \
        if (!s_logged.exchange(true, std::memory_order_relaxed)) {         \
            sc::log::Write(sc::log::Level::Stub, mod, __FUNCTION__,        \
                           "-> unimplemented (returning default)");        \
        }                                                                  \
        return (ret_val);                                                  \
    } while (0)