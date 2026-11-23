#include "common/stub.hpp"
#include <atomic>
#include <cstdio>

namespace {
void LogOnce(std::atomic<bool>& flag, const char* fn, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (!flag.exchange(true, std::memory_order_relaxed)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "-> stub (a1=0x%llX a2=0x%llX a3=0x%llX a4=0x%llX)",
                      static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                      static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
        sc::log::Write(sc::log::Level::Info, "xapu", fn, buf);
    }
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xapu", "DllMain", path);
    }
    return TRUE;
}

extern "C" {
HRESULT WINAPI XApuConnect(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    static std::atomic<bool> f{false};
    LogOnce(f, __FUNCTION__, a1, a2, a3, a4);
    return S_OK;
}
HRESULT WINAPI XApuDisconnect(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    static std::atomic<bool> f{false};
    LogOnce(f, __FUNCTION__, a1, a2, a3, a4);
    return S_OK;
}
HRESULT WINAPI XApuEnqueueCommand(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    static std::atomic<bool> f{false};
    LogOnce(f, __FUNCTION__, a1, a2, a3, a4);
    return S_OK;
}
HRESULT WINAPI XApuDequeueResult(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    static std::atomic<bool> f{false};
    LogOnce(f, __FUNCTION__, a1, a2, a3, a4);
    return S_FALSE;
}
uint32_t WINAPI XApuGetResultCount(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    static std::atomic<bool> f{false};
    LogOnce(f, __FUNCTION__, a1, a2, a3, a4);
    return 0u;
}
} // extern "C"
