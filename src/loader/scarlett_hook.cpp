#include "common/stub.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using LoadLibraryAFn   = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFn   = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleAFn   = HMODULE(WINAPI*)(LPCSTR);
using GetModuleHandleWFn   = HMODULE(WINAPI*)(LPCWSTR);
using GetModuleHandleExAFn = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);
using GetModuleHandleExWFn = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using K32EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

HMODULE          g_selfModule         = nullptr;
HMODULE          g_exeModule          = nullptr;
LoadLibraryAFn   g_origLoadLibraryA   = nullptr;
LoadLibraryWFn   g_origLoadLibraryW   = nullptr;
LoadLibraryExAFn g_origLoadLibraryExA = nullptr;
LoadLibraryExWFn     g_origLoadLibraryExW     = nullptr;
GetModuleHandleAFn   g_origGetModuleHandleA   = nullptr;
GetModuleHandleWFn   g_origGetModuleHandleW   = nullptr;
GetModuleHandleExAFn g_origGetModuleHandleExA = nullptr;
GetModuleHandleExWFn g_origGetModuleHandleExW = nullptr;

std::wstring g_shimDirLower;
std::wstring g_winDirLower;
std::unordered_map<std::wstring, std::wstring> g_shimsWide;
std::unordered_map<std::string, std::string>   g_shimsAnsi;

std::mutex g_hookMtx;
std::unordered_set<HMODULE> g_seenModules;

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string WideToAnsi(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ToLowerW(std::wstring s) {
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring EnsureTrailingSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/') {
        s.push_back(L'\\');
    }
    return s;
}

bool StartsWithW(const std::wstring& value, const std::wstring& prefix) {
    return !prefix.empty() && value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

std::wstring NormalizeDllKeyW(const wchar_t* raw) {
    if (!raw || !*raw) {
        return {};
    }
    std::wstring s(raw);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        s = s.substr(slash + 1);
    }
    s = ToLowerW(std::move(s));
    if (s.find(L'.') == std::wstring::npos) {
        s += L".dll";
    }
    return s;
}

std::string NormalizeDllKeyA(const char* raw) {
    if (!raw || !*raw) {
        return {};
    }
    std::string s(raw);
    size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (s.find('.') == std::string::npos) {
        s += ".dll";
    }
    return s;
}

DWORD SanitizeFlagsForAbsoluteShim(DWORD flags) {
    constexpr DWORD kRestrictiveMask =
        LOAD_LIBRARY_SEARCH_SYSTEM32 |
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
        LOAD_LIBRARY_SEARCH_USER_DIRS |
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    return flags & ~kRestrictiveMask;
}

bool IsDataOnlyLoad(DWORD flags) {
    constexpr DWORD kDataFlags =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    return (flags & kDataFlags) != 0;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName);
HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE WINAPI HookGetModuleHandleW(LPCWSTR lpModuleName);
HMODULE WINAPI HookGetModuleHandleA(LPCSTR lpModuleName);
BOOL    WINAPI HookGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
BOOL    WINAPI HookGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule);

uint32_t PatchModuleIat(HMODULE mod) {
    if (!mod) {
        return 0;
    }
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        return 0;
    }

    uint32_t patched = 0;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name != 0 || desc->FirstThunk != 0; ++desc) {
        if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) {
            continue;
        }
        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) {
                continue;
            }
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            void* replacement = nullptr;

            if (std::strcmp(ibn->Name, "LoadLibraryW") == 0) {
                replacement = reinterpret_cast<void*>(&HookLoadLibraryW);
            } else if (std::strcmp(ibn->Name, "LoadLibraryA") == 0) {
                replacement = reinterpret_cast<void*>(&HookLoadLibraryA);
            } else if (std::strcmp(ibn->Name, "LoadLibraryExW") == 0) {
                replacement = reinterpret_cast<void*>(&HookLoadLibraryExW);
            } else if (std::strcmp(ibn->Name, "LoadLibraryExA") == 0) {
                replacement = reinterpret_cast<void*>(&HookLoadLibraryExA);
            } else if (std::strcmp(ibn->Name, "GetModuleHandleW") == 0) {
                replacement = reinterpret_cast<void*>(&HookGetModuleHandleW);
            } else if (std::strcmp(ibn->Name, "GetModuleHandleA") == 0) {
                replacement = reinterpret_cast<void*>(&HookGetModuleHandleA);
            } else if (std::strcmp(ibn->Name, "GetModuleHandleExW") == 0) {
                replacement = reinterpret_cast<void*>(&HookGetModuleHandleExW);
            } else if (std::strcmp(ibn->Name, "GetModuleHandleExA") == 0) {
                replacement = reinterpret_cast<void*>(&HookGetModuleHandleExA);
            }

            if (replacement) {
                DWORD oldProt = 0;
                if (VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR),
                                   PAGE_READWRITE, &oldProt)) {
                    iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                    DWORD restoreProt = 0;
                    VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR),
                                   oldProt, &restoreProt);
                    ++patched;
                }
            }
        }
    }
    return patched;
}

uint32_t HookModuleIfEligible(HMODULE mod, bool logSubModule) {
    if (!mod || mod == g_selfModule) {
        return 0;
    }

    wchar_t pathBuf[MAX_PATH]{};
    DWORD pathLen = GetModuleFileNameW(mod, pathBuf, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) {
        return 0;
    }

    std::wstring pathLower = ToLowerW(std::wstring(pathBuf, pathLen));
    const bool isSystemOrShim =
        StartsWithW(pathLower, g_winDirLower) ||
        StartsWithW(pathLower, g_shimDirLower);

    uint32_t patched = 0;
    {
        std::lock_guard<std::mutex> lock(g_hookMtx);
        if (!g_seenModules.insert(mod).second) {
            return 0;
        }
        if (isSystemOrShim) {
            return 0;
        }
        patched = PatchModuleIat(mod);
    }

    if (logSubModule && mod != g_exeModule && patched > 0) {
        std::wstring fileName(pathBuf, pathLen);
        size_t slash = fileName.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            fileName = fileName.substr(slash + 1);
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "-> armed %u runtime LoadLibrary IAT hook(s) in sub-module '%s'",
                      patched, WideToUtf8(fileName).c_str());
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "PatchSubModule", buf);
    }
    return patched;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName) {
    std::wstring key = NormalizeDllKeyW(lpLibFileName);
    auto it = g_shimsWide.find(key);
    if (it != g_shimsWide.end()) {
        std::string msg = "[dynamic] " + WideToUtf8(lpLibFileName ? lpLibFileName : L"")
                        + " -> " + WideToUtf8(it->second);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryW", msg.c_str());
        return g_origLoadLibraryW(it->second.c_str());
    }
    HMODULE mod = g_origLoadLibraryW(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName) {
    std::string key = NormalizeDllKeyA(lpLibFileName);
    auto it = g_shimsAnsi.find(key);
    if (it != g_shimsAnsi.end()) {
        std::string msg = "[dynamic] " + std::string(lpLibFileName ? lpLibFileName : "")
                        + " -> " + it->second;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryA", msg.c_str());
        return g_origLoadLibraryA(it->second.c_str());
    }
    HMODULE mod = g_origLoadLibraryA(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring key = NormalizeDllKeyW(lpLibFileName);
    auto it = g_shimsWide.find(key);
    if (it != g_shimsWide.end()) {
        std::string msg = "[dynamic] " + WideToUtf8(lpLibFileName ? lpLibFileName : L"")
                        + " -> " + WideToUtf8(it->second);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExW", msg.c_str());
        return g_origLoadLibraryExW(it->second.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::string key = NormalizeDllKeyA(lpLibFileName);
    auto it = g_shimsAnsi.find(key);
    if (it != g_shimsAnsi.end()) {
        std::string msg = "[dynamic] " + std::string(lpLibFileName ? lpLibFileName : "")
                        + " -> " + it->second;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExA", msg.c_str());
        return g_origLoadLibraryExA(it->second.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookGetModuleHandleW(LPCWSTR lpModuleName) {
    if (lpModuleName) {
        std::wstring key = NormalizeDllKeyW(lpModuleName);
        auto it = g_shimsWide.find(key);
        if (it != g_shimsWide.end()) {
            return g_origGetModuleHandleW(it->second.c_str());
        }
    }
    return g_origGetModuleHandleW(lpModuleName);
}

HMODULE WINAPI HookGetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName) {
        std::string key = NormalizeDllKeyA(lpModuleName);
        auto it = g_shimsAnsi.find(key);
        if (it != g_shimsAnsi.end()) {
            return g_origGetModuleHandleA(it->second.c_str());
        }
    }
    return g_origGetModuleHandleA(lpModuleName);
}

BOOL WINAPI HookGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        std::wstring key = NormalizeDllKeyW(lpModuleName);
        auto it = g_shimsWide.find(key);
        if (it != g_shimsWide.end()) {
            return g_origGetModuleHandleExW(dwFlags, it->second.c_str(), phModule);
        }
    }
    return g_origGetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI HookGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        std::string key = NormalizeDllKeyA(lpModuleName);
        auto it = g_shimsAnsi.find(key);
        if (it != g_shimsAnsi.end()) {
            return g_origGetModuleHandleExA(dwFlags, it->second.c_str(), phModule);
        }
    }
    return g_origGetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

void DiscoverShimsWin32() {
    wchar_t winBuf[MAX_PATH]{};
    UINT winLen = GetWindowsDirectoryW(winBuf, MAX_PATH);
    if (winLen > 0 && winLen < MAX_PATH) {
        g_winDirLower = EnsureTrailingSlash(ToLowerW(std::wstring(winBuf, winLen)));
    }

    wchar_t dirBuf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", dirBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    std::wstring shimDir(dirBuf, len);
    g_shimDirLower = EnsureTrailingSlash(ToLowerW(shimDir));

    std::wstring pattern = shimDir + L"\\*.dll";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::wstring fileName(fd.cFileName);
        std::wstring fullWide = shimDir + L"\\" + fileName;
        std::wstring keyW = NormalizeDllKeyW(fileName.c_str());
        std::string keyA = WideToAnsi(keyW);
        std::string fullA = WideToAnsi(fullWide);

        g_shimsWide.emplace(std::move(keyW), std::move(fullWide));
        g_shimsAnsi.emplace(std::move(keyA), std::move(fullA));
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

uint32_t HookAllLoadedTitleModules(HMODULE k32) {
    uint32_t totalPatched = HookModuleIfEligible(g_exeModule, false);

    auto enumMods = k32
        ? reinterpret_cast<K32EnumProcessModulesFn>(GetProcAddress(k32, "K32EnumProcessModules"))
        : nullptr;
    if (!enumMods) {
        return totalPatched;
    }

    std::vector<HMODULE> mods(256);
    DWORD needed = 0;
    if (enumMods(GetCurrentProcess(), mods.data(),
                 static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
        size_t count = needed / sizeof(HMODULE);
        if (count > mods.size()) {
            mods.resize(count);
            enumMods(GetCurrentProcess(), mods.data(),
                     static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed);
            count = needed / sizeof(HMODULE);
        }
        for (size_t i = 0; i < count; ++i) {
            totalPatched += HookModuleIfEligible(mods[i], true);
        }
    }
    return totalPatched;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        g_exeModule = GetModuleHandleW(nullptr);
        DisableThreadLibraryCalls(hModule);

        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            g_origLoadLibraryA   = reinterpret_cast<LoadLibraryAFn>(GetProcAddress(k32, "LoadLibraryA"));
            g_origLoadLibraryW   = reinterpret_cast<LoadLibraryWFn>(GetProcAddress(k32, "LoadLibraryW"));
            g_origLoadLibraryExA = reinterpret_cast<LoadLibraryExAFn>(GetProcAddress(k32, "LoadLibraryExA"));
            g_origLoadLibraryExW     = reinterpret_cast<LoadLibraryExWFn>(GetProcAddress(k32, "LoadLibraryExW"));
            g_origGetModuleHandleA   = reinterpret_cast<GetModuleHandleAFn>(GetProcAddress(k32, "GetModuleHandleA"));
            g_origGetModuleHandleW   = reinterpret_cast<GetModuleHandleWFn>(GetProcAddress(k32, "GetModuleHandleW"));
            g_origGetModuleHandleExA = reinterpret_cast<GetModuleHandleExAFn>(GetProcAddress(k32, "GetModuleHandleExA"));
            g_origGetModuleHandleExW = reinterpret_cast<GetModuleHandleExWFn>(GetProcAddress(k32, "GetModuleHandleExW"));
        }

        DiscoverShimsWin32();
        uint32_t patched = HookAllLoadedTitleModules(k32);

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "-> armed %u multi-module LoadLibrary IAT hook(s) across %zu discovered shim(s)",
                      patched, g_shimsWide.size());
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "DllMain", buf);
    }
    return TRUE;
}