#include "shims/pixevt/pixevt.hpp"
#include "shims/xmem/xmem.hpp"

#include <cstdio>
#include <cstdint>
#include <windows.h>

namespace {
using XMemAllocFn = void*(WINAPI*)(size_t, uint32_t);
using XMemFreeFn  = void(WINAPI*)(void*, uint32_t);
using XMemSizeFn  = size_t(WINAPI*)(void*, uint32_t);
} // namespace

extern "C" bool WINAPI PluginRunDiagnostics() {
    // Dynamically loads Xbox shims FROM INSIDE a sub-DLL (never linked to any shim!)
    HMODULE hPix = LoadLibraryW(L"pixevt.dll");
    HMODULE hMem = LoadLibraryA("xmem");
    if (!hPix || !hMem) {
        std::printf("[PLUGIN] FAILED to load shims dynamically from sub-DLL!\n");
        if (hPix) FreeLibrary(hPix);
        if (hMem) FreeLibrary(hMem);
        return false;
    }

    auto pixMark   = reinterpret_cast<PIXSetMarkerFn>(GetProcAddress(hPix, "PIXSetMarker"));
    auto xmemAlloc = reinterpret_cast<XMemAllocFn>(GetProcAddress(hMem, "XMemAlloc"));
    auto xmemFree  = reinterpret_cast<XMemFreeFn>(GetProcAddress(hMem, "XMemFree"));
    auto xmemSize  = reinterpret_cast<XMemSizeFn>(GetProcAddress(hMem, "XMemSize"));

    if (pixMark) {
        pixMark(0xFFFFAA00ULL, "EnginePluginSubModuleHookVerified");
    }

    bool memOk = false;
    if (xmemAlloc && xmemFree && xmemSize) {
        constexpr uint32_t attrs = MakeXAllocAttributes(
            XALLOC_ID_DEFAULT, XALLOC_ALIGNMENT_4K, XALLOC_MEMTYPE_HEAP, true);
        void* p = xmemAlloc(4096, attrs);
        if (p) {
            const bool aligned4k = (reinterpret_cast<uintptr_t>(p) % 4096ULL) == 0;
            const bool exactSz = (xmemSize(p, attrs) == 4096);
            memOk = aligned4k && exactSz;
            xmemFree(p, attrs);
        }
    }

    char pixPath[MAX_PATH]{};
    char memPath[MAX_PATH]{};
    GetModuleFileNameA(hPix, pixPath, MAX_PATH);
    GetModuleFileNameA(hMem, memPath, MAX_PATH);

    std::printf("[PLUGIN] Multi-module hook OK (4KB-xmem=%s):\n", memOk ? "PASS" : "FAIL");
    std::printf("[PLUGIN]   pixevt -> %s\n", pixPath);
    std::printf("[PLUGIN]   xmem   -> %s\n", memPath);

    FreeLibrary(hMem);
    FreeLibrary(hPix);
    return memOk;
}