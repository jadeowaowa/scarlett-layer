#include "common/stub.hpp"
#include <filesystem>
#include <mutex>

namespace {
HMODULE g_selfMod = nullptr;
std::mutex g_dsMtx;
using DStorageGetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
DStorageGetFactoryFn g_dsGetFactory = nullptr;

DStorageGetFactoryFn ResolveDStorageGetFactory() {
    std::lock_guard<std::mutex> lock(g_dsMtx);
    if (g_dsGetFactory) return g_dsGetFactory;

    HMODULE hDs = GetModuleHandleW(L"dstorage_xs.dll");
    if (!hDs && g_selfMod) {
        wchar_t selfPath[MAX_PATH]{};
        if (GetModuleFileNameW(g_selfMod, selfPath, MAX_PATH) > 0) {
            auto sib = std::filesystem::path(selfPath).parent_path() / L"dstorage_xs.dll";
            hDs = LoadLibraryW(sib.c_str());
        }
    }
    if (!hDs) {
        hDs = LoadLibraryW(L"dstorage_xs.dll");
    }
    if (hDs) {
        g_dsGetFactory = reinterpret_cast<DStorageGetFactoryFn>(GetProcAddress(hDs, "DStorageGetFactory"));
    }
    return g_dsGetFactory;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_selfMod = hModule;
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xcrdapi", "DllMain", path);
    }
    return TRUE;
}

extern "C" {
HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    auto fn = ResolveDStorageGetFactory();
    if (!fn) {
        sc::log::Write(sc::log::Level::Error, "xcrdapi", __FUNCTION__, "-> failed to load sibling dstorage_xs.dll");
        if (ppv) *ppv = nullptr;
        return E_FAIL;
    }
    sc::log::Write(sc::log::Level::Info, "xcrdapi", __FUNCTION__, "-> forwarding to dstorage_xs!DStorageGetFactory");
    return fn(riid, ppv);
}
} // extern "C"
