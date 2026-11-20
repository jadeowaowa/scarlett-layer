#include "pixevt.hpp"
#include "common/stub.hpp"

#include <atomic>
#include <cstdio>

namespace {
std::atomic<uint64_t> g_beginCount{0};
std::atomic<uint64_t> g_endCount{0};
std::atomic<uint64_t> g_markerCount{0};
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "pixevt", "DllMain", path);
    } else if (reason == DLL_PROCESS_DETACH) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "-> summary: begins=%llu, ends=%llu, markers=%llu",
                      static_cast<unsigned long long>(g_beginCount.load()),
                      static_cast<unsigned long long>(g_endCount.load()),
                      static_cast<unsigned long long>(g_markerCount.load()));
        sc::log::Write(sc::log::Level::Info, "pixevt", "DllMain[Unload]", buf);
    }
    return TRUE;
}

extern "C" {

void WINAPI PIXBeginEvent(uint64_t /*color*/, const char* name) {
    uint64_t n = g_beginCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "-> first event '%s' (subsequent frames counted silently)",
                      name ? name : "<null>");
        sc::log::Write(sc::log::Level::Info, "pixevt", __FUNCTION__, buf);
    }
}

void WINAPI PIXEndEvent() {
    g_endCount.fetch_add(1, std::memory_order_relaxed);
}

void WINAPI PIXSetMarker(uint64_t /*color*/, const char* name) {
    uint64_t n = g_markerCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "-> marker #%llu: '%s'",
                      static_cast<unsigned long long>(n),
                      name ? name : "<null>");
        sc::log::Write(sc::log::Level::Info, "pixevt", __FUNCTION__, buf);
    }
}

} // extern "C"