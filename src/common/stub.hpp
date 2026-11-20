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
    std::fprintf(stderr, "[%s][T:%05lu][%s] %s %s\n", tag, tid, module, func, detail);
    std::fflush(stderr);
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