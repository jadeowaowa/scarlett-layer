#include "common/stub.hpp"

enum class XSystemDeviceType : uint32_t {
    Unknown              = 0x00,
    Pc                   = 0x01,
    XboxOne              = 0x02,
    XboxOneS             = 0x03,
    XboxOneX             = 0x04,
    XboxOneXDevkit       = 0x05,
    XboxScarlettLockhart = 0x06, // Series S
    XboxScarlettAnaconda = 0x07, // Series X
    XboxScarlettDevkit   = 0x08
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI XGameRuntimeInitialize() {
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> S_OK");
    return S_OK;
}

void WINAPI XGameRuntimeUninitialize() {
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> clean shutdown");
}

XSystemDeviceType WINAPI XSystemGetDeviceType() {
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__,
                   "-> spoofing XboxScarlettAnaconda (0x07)");
    return XSystemDeviceType::XboxScarlettAnaconda;
}

HRESULT WINAPI XUserAddAsync(uint32_t /*options*/, void* /*asyncBlock*/) {
    SC_STUB_ONCE("xgameruntime", E_NOTIMPL);
}

} // extern "C"