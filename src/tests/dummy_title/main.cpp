#include <cstdio>
#include <cstdint>
#include <windows.h>

extern "C" {
    __declspec(dllimport) HRESULT  WINAPI XGameRuntimeInitialize();
    __declspec(dllimport) void     WINAPI XGameRuntimeUninitialize();
    __declspec(dllimport) uint32_t WINAPI XSystemGetDeviceType();
    __declspec(dllimport) HRESULT  WINAPI XUserAddAsync(uint32_t options, void* asyncBlock);
}

int main() {
    std::printf("[TITLE] Booting synthetic Series X title...\n");

    HRESULT hr = XGameRuntimeInitialize();
    if (FAILED(hr)) {
        std::printf("[TITLE] XGameRuntimeInitialize failed: 0x%08lX\n", hr);
        return 1;
    }

    uint32_t device = XSystemGetDeviceType();
    std::printf("[TITLE] Reported device type: 0x%02X (%s)\n",
                device, device == 0x07 ? "Xbox Series X" : "Unexpected");

    // Call an unimplemented stub twice to verify SC_STUB_ONCE deduplication
    HRESULT userHr1 = XUserAddAsync(0, nullptr);
    HRESULT userHr2 = XUserAddAsync(0, nullptr);
    std::printf("[TITLE] XUserAddAsync calls returned: 0x%08lX, 0x%08lX\n", userHr1, userHr2);

    XGameRuntimeUninitialize();
    std::printf("[TITLE] Shutdown complete.\n");
    return 0;
}