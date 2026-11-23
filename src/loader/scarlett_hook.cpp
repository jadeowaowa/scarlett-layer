#include "auto_stub.hpp"
#include "import_telemetry.hpp"
#include "common/ansi_path.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#define SC_CALLER_PC() _ReturnAddress()
#else
#define SC_CALLER_PC() __builtin_return_address(0)
#endif
#include "common/config.hpp"
#include "common/stub.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
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
using GetProcAddressFn        = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using FreeLibraryFn           = BOOL(WINAPI*)(HMODULE);
using K32EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

struct ModuleIdentity {
    DWORD timeDateStamp{0};
    DWORD sizeOfImage{0};

    bool operator==(const ModuleIdentity& other) const {
        return timeDateStamp == other.timeDateStamp && sizeOfImage == other.sizeOfImage;
    }
};

HMODULE              g_selfModule             = nullptr;
HMODULE              g_exeModule              = nullptr;
LoadLibraryAFn       g_origLoadLibraryA       = nullptr;
LoadLibraryWFn       g_origLoadLibraryW       = nullptr;
LoadLibraryExAFn     g_origLoadLibraryExA     = nullptr;
LoadLibraryExWFn     g_origLoadLibraryExW     = nullptr;
GetModuleHandleAFn   g_origGetModuleHandleA   = nullptr;
GetModuleHandleWFn   g_origGetModuleHandleW   = nullptr;
GetModuleHandleExAFn g_origGetModuleHandleExA = nullptr;
GetModuleHandleExWFn g_origGetModuleHandleExW = nullptr;
GetProcAddressFn     g_origGetProcAddress     = nullptr;
FreeLibraryFn        g_origFreeLibrary        = nullptr;
volatile LONG        g_delayPatchedCount      = 0;
volatile LONG        g_ordinalLookupCount     = 0;

std::wstring g_shimDirLower;
std::wstring g_winDirLower;
std::mutex   g_shimMapMtx;
std::unordered_map<std::wstring, std::wstring> g_shimsWide;

std::mutex g_hookMtx;
std::unordered_map<HMODULE, ModuleIdentity> g_seenModules;

std::string WideToUtf8(const std::wstring& w) {
    return sc::path::WideToUtf8(w);
}

std::wstring AnsiToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

std::wstring GetModulePathWide(HMODULE mod) {
    if (!mod) {
        return {};
    }
    std::wstring buf(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        const DWORD len = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
    return {};
}

bool ReadModuleIdentity(HMODULE mod, ModuleIdentity& out) {
    if (!mod) {
        return false;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    out.timeDateStamp = nt->FileHeader.TimeDateStamp;
    out.sizeOfImage   = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool FindShimWide(const std::wstring& keyW, std::wstring& outFullWide) {
    if (keyW.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_shimMapMtx);
    auto it = g_shimsWide.find(keyW);
    if (it == g_shimsWide.end()) {
        return false;
    }
    outFullWide = it->second;
    return true;
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
FARPROC WINAPI HookGetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL    WINAPI HookFreeLibrary(HMODULE hLibModule);

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

    auto resolveHook = [](const char* name) -> void* {
        if (std::strcmp(name, "LoadLibraryW") == 0)         return reinterpret_cast<void*>(&HookLoadLibraryW);
        if (std::strcmp(name, "LoadLibraryA") == 0)         return reinterpret_cast<void*>(&HookLoadLibraryA);
        if (std::strcmp(name, "LoadLibraryExW") == 0)       return reinterpret_cast<void*>(&HookLoadLibraryExW);
        if (std::strcmp(name, "LoadLibraryExA") == 0)       return reinterpret_cast<void*>(&HookLoadLibraryExA);
        if (std::strcmp(name, "GetModuleHandleW") == 0)     return reinterpret_cast<void*>(&HookGetModuleHandleW);
        if (std::strcmp(name, "GetModuleHandleA") == 0)     return reinterpret_cast<void*>(&HookGetModuleHandleA);
        if (std::strcmp(name, "GetModuleHandleExW") == 0)   return reinterpret_cast<void*>(&HookGetModuleHandleExW);
        if (std::strcmp(name, "GetModuleHandleExA") == 0)   return reinterpret_cast<void*>(&HookGetModuleHandleExA);
        if (std::strcmp(name, "GetProcAddress") == 0)       return reinterpret_cast<void*>(&HookGetProcAddress);
        if (std::strcmp(name, "FreeLibrary") == 0)          return reinterpret_cast<void*>(&HookFreeLibrary);
        return nullptr;
    };

    auto patchThunkTable = [&](IMAGE_THUNK_DATA64* origThunk, IMAGE_THUNK_DATA64* iatThunk, bool isDelay) -> uint32_t {
        uint32_t count = 0;
        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) {
                continue;
            }
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            void* replacement = resolveHook(ibn->Name);
            if (replacement) {
                DWORD oldProt = 0;
                if (VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR),
                                   PAGE_READWRITE, &oldProt)) {
                    iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                    DWORD restoreProt = 0;
                    VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR),
                                   oldProt, &restoreProt);
                    ++count;
                    if (isDelay) {
                        InterlockedIncrement(&g_delayPatchedCount);
                    }
                }
            }
        }
        return count;
    };

    uint32_t patched = 0;

    // 1. Standard IAT (IMAGE_DIRECTORY_ENTRY_IMPORT)
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress != 0 && importDir.Size != 0) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
        for (; desc->Name != 0 || desc->FirstThunk != 0; ++desc) {
            if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) {
                continue;
            }
            auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
            auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
            patched += patchThunkTable(origThunk, iatThunk, false);
        }
    }

    // 2. Delay-Load IAT (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
    const auto& delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        auto* ddesc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayDir.VirtualAddress);
        for (; ddesc->DllNameRVA != 0 || ddesc->ImportAddressTableRVA != 0; ++ddesc) {
            if (ddesc->ImportNameTableRVA == 0 || ddesc->ImportAddressTableRVA == 0) {
                continue;
            }
            auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + ddesc->ImportNameTableRVA);
            auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + ddesc->ImportAddressTableRVA);
            patched += patchThunkTable(origThunk, iatThunk, true);
        }
    }

    return patched;
}

uint32_t HookModuleIfEligible(HMODULE mod, bool logSubModule) {
    if (!mod || mod == g_selfModule) {
        return 0;
    }

    const std::wstring modPath = GetModulePathWide(mod);
    if (modPath.empty()) {
        return 0;
    }

    ModuleIdentity id{};
    if (!ReadModuleIdentity(mod, id)) {
        return 0;
    }

    const std::wstring pathLower = ToLowerW(modPath);
    const bool isSystemOrShim =
        StartsWithW(pathLower, g_winDirLower) ||
        StartsWithW(pathLower, g_shimDirLower);

    uint32_t patched = 0;
    {
        std::lock_guard<std::mutex> lock(g_hookMtx);
        auto it = g_seenModules.find(mod);
        if (it != g_seenModules.end() && it->second == id) {
            return 0;
        }
        g_seenModules[mod] = id;
        if (isSystemOrShim) {
            return 0;
        }
        patched = PatchModuleIat(mod);
    }

    if (logSubModule && mod != g_exeModule && patched > 0) {
        std::wstring fileName = modPath;
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

BOOL WINAPI HookFreeLibrary(HMODULE hLibModule) {
    const BOOL ok = g_origFreeLibrary ? g_origFreeLibrary(hLibModule) : FALSE;
    if (ok && hLibModule) {
        const DWORD savedErr = GetLastError();
        HMODULE checkMod = nullptr;
        const BOOL stillLoaded = g_origGetModuleHandleExW
            ? g_origGetModuleHandleExW(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                  reinterpret_cast<LPCWSTR>(hLibModule),
                  &checkMod)
            : FALSE;
        if (!stillLoaded || checkMod != hLibModule) {
            std::lock_guard<std::mutex> lock(g_hookMtx);
            g_seenModules.erase(hLibModule);
        }
        SetLastError(savedErr);
    }
    return ok;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName) {
    std::wstring targetWide;
    if (FindShimWide(NormalizeDllKeyW(lpLibFileName), targetWide)) {
        const std::string reqUtf8 = WideToUtf8(lpLibFileName ? lpLibFileName : L"");
        const std::string dstUtf8 = WideToUtf8(targetWide);
        if (sc::loader::ImportTelemetry::Instance().IsEnabled()) {
            sc::loader::ImportTelemetry::Instance().RecordRedirect(
                "LoadLibraryW", reqUtf8.c_str(), dstUtf8.c_str(), SC_CALLER_PC());
        }
        std::string msg = "[dynamic] " + reqUtf8 + " -> " + dstUtf8;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryW", msg.c_str());
        return g_origLoadLibraryW(targetWide.c_str());
    }
    HMODULE mod = g_origLoadLibraryW(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName) {
    // Route shim redirects directly through g_shimsWide + LoadLibraryW to avoid ANSI path mangling.
    std::wstring targetWide;
    if (g_origLoadLibraryW && FindShimWide(AnsiToWide(NormalizeDllKeyA(lpLibFileName)), targetWide)) {
        const std::string reqStr = lpLibFileName ? lpLibFileName : "";
        const std::string dstUtf8 = WideToUtf8(targetWide);
        if (sc::loader::ImportTelemetry::Instance().IsEnabled()) {
            sc::loader::ImportTelemetry::Instance().RecordRedirect(
                "LoadLibraryA", reqStr.c_str(), dstUtf8.c_str(), SC_CALLER_PC());
        }
        std::string msg = "[dynamic] " + reqStr + " -> " + dstUtf8;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryA", msg.c_str());
        return g_origLoadLibraryW(targetWide.c_str());
    }
    HMODULE mod = g_origLoadLibraryA(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring targetWide;
    if (FindShimWide(NormalizeDllKeyW(lpLibFileName), targetWide)) {
        const std::string reqUtf8 = WideToUtf8(lpLibFileName ? lpLibFileName : L"");
        const std::string dstUtf8 = WideToUtf8(targetWide);
        if (sc::loader::ImportTelemetry::Instance().IsEnabled()) {
            sc::loader::ImportTelemetry::Instance().RecordRedirect(
                "LoadLibraryExW", reqUtf8.c_str(), dstUtf8.c_str(), SC_CALLER_PC());
        }
        std::string msg = "[dynamic] " + reqUtf8 + " -> " + dstUtf8;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExW", msg.c_str());
        return g_origLoadLibraryExW(targetWide.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring targetWide;
    if (g_origLoadLibraryExW && FindShimWide(AnsiToWide(NormalizeDllKeyA(lpLibFileName)), targetWide)) {
        const std::string reqStr = lpLibFileName ? lpLibFileName : "";
        const std::string dstUtf8 = WideToUtf8(targetWide);
        if (sc::loader::ImportTelemetry::Instance().IsEnabled()) {
            sc::loader::ImportTelemetry::Instance().RecordRedirect(
                "LoadLibraryExA", reqStr.c_str(), dstUtf8.c_str(), SC_CALLER_PC());
        }
        std::string msg = "[dynamic] " + reqStr + " -> " + dstUtf8;
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExA", msg.c_str());
        return g_origLoadLibraryExW(targetWide.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    }
    return mod;
}

HMODULE WINAPI HookGetModuleHandleW(LPCWSTR lpModuleName) {
    if (lpModuleName) {
        std::wstring targetWide;
        if (FindShimWide(NormalizeDllKeyW(lpModuleName), targetWide)) {
            return g_origGetModuleHandleW(targetWide.c_str());
        }
    }
    return g_origGetModuleHandleW(lpModuleName);
}

HMODULE WINAPI HookGetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName && g_origGetModuleHandleW) {
        std::wstring targetWide;
        if (FindShimWide(AnsiToWide(NormalizeDllKeyA(lpModuleName)), targetWide)) {
            return g_origGetModuleHandleW(targetWide.c_str());
        }
    }
    return g_origGetModuleHandleA(lpModuleName);
}

BOOL WINAPI HookGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        std::wstring targetWide;
        if (FindShimWide(NormalizeDllKeyW(lpModuleName), targetWide)) {
            return g_origGetModuleHandleExW(dwFlags, targetWide.c_str(), phModule);
        }
    }
    return g_origGetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI HookGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 && g_origGetModuleHandleExW) {
        std::wstring targetWide;
        if (FindShimWide(AnsiToWide(NormalizeDllKeyA(lpModuleName)), targetWide)) {
            return g_origGetModuleHandleExW(dwFlags, targetWide.c_str(), phModule);
        }
    }
    return g_origGetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

sc::loader::AutoStubEngine g_autoStubs;

bool TryGetShimDllName(HMODULE hModule, std::string& outDllLower) {
    if (!hModule || g_shimDirLower.empty()) {
        return false;
    }
    const std::wstring modPath = GetModulePathWide(hModule);
    if (modPath.empty()) {
        return false;
    }
    const std::wstring lowerPath = ToLowerW(modPath);
    if (lowerPath.rfind(g_shimDirLower, 0) != 0) {
        return false;
    }
    const size_t slash = lowerPath.find_last_of(L"\\/");
    const std::wstring fileName = (slash == std::wstring::npos) ? lowerPath : lowerPath.substr(slash + 1);
    outDllLower = WideToUtf8(fileName);
    return !outDllLower.empty();
}

FARPROC WINAPI HookGetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    const bool isOrdinal = lpProcName && (reinterpret_cast<uintptr_t>(lpProcName) >> 16) == 0;
    if (isOrdinal) {
        InterlockedIncrement(&g_ordinalLookupCount);
    }
    FARPROC resolved = g_origGetProcAddress ? g_origGetProcAddress(hModule, lpProcName) : nullptr;
    auto& telemetry = sc::loader::ImportTelemetry::Instance();
    const bool traceOn = telemetry.IsEnabled();

    if (resolved) {
        if (traceOn && hModule && lpProcName) {
            std::string shimDllLower;
            if (TryGetShimDllName(hModule, shimDllLower)) {
                const uint16_t ord = isOrdinal
                    ? static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpProcName) & 0xFFFFu)
                    : 0u;
                telemetry.RecordSymbolLookup(
                    shimDllLower.c_str(),
                    isOrdinal ? nullptr : lpProcName,
                    ord,
                    sc::loader::SymbolStatus::Resolved,
                    0ULL,
                    SC_CALLER_PC());
            }
        }
        return resolved;
    }

    if (!hModule || !lpProcName) {
        return nullptr;
    }

    std::string shimDllLower;
    if (!TryGetShimDllName(hModule, shimDllLower)) {
        return nullptr;
    }

    const uint16_t ord = isOrdinal
        ? static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpProcName) & 0xFFFFu)
        : 0u;
    std::string symName;
    if (isOrdinal) {
        char ordBuf[32];
        std::snprintf(ordBuf, sizeof(ordBuf), "#%u", static_cast<unsigned>(ord));
        symName = ordBuf;
    } else {
        symName = lpProcName;
    }

    FARPROC stubProc = sc::config::IsAutoStubEnabled()
        ? g_autoStubs.GetOrCreate(shimDllLower, symName)
        : nullptr;

    if (traceOn) {
        telemetry.RecordSymbolLookup(
            shimDllLower.c_str(),
            symName.c_str(),
            ord,
            stubProc ? sc::loader::SymbolStatus::AutoStubbed : sc::loader::SymbolStatus::Unresolved,
            stubProc ? sc::loader::AutoStubEngine::DefaultPolicyFor(symName) : 0ULL,
            SC_CALLER_PC());
    }
    return stubProc;
}

void DiscoverShimsWin32() {
    wchar_t winBuf[MAX_PATH]{};
    UINT winLen = GetWindowsDirectoryW(winBuf, MAX_PATH);
    if (winLen > 0 && winLen < MAX_PATH) {
        g_winDirLower = EnsureTrailingSlash(ToLowerW(std::wstring(winBuf, winLen)));
    }

    auto shimDirPath = sc::config::GetShimDir();
    if (!shimDirPath) {
        return;
    }

    std::wstring shimDir = shimDirPath->wstring();
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

        std::string fullA;
        if (!sc::path::WidePathToAnsiLossless(fullWide, fullA)) {
            std::string msg = "Shim path is not CP_ACP-representable (ANSI callers redirect via wide LoadLibraryW): " +
                              WideToUtf8(fullWide);
            sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "DiscoverShims", msg.c_str());
        }

        std::lock_guard<std::mutex> lock(g_shimMapMtx);
        g_shimsWide.emplace(std::move(keyW), std::move(fullWide));
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

extern "C" __declspec(dllexport) uint32_t WINAPI ScarlettHookRunGuestAtExit();

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
            g_origGetProcAddress     = reinterpret_cast<GetProcAddressFn>(GetProcAddress(k32, "GetProcAddress"));
            g_origFreeLibrary        = reinterpret_cast<FreeLibraryFn>(GetProcAddress(k32, "FreeLibrary"));
        }

        DiscoverShimsWin32();
        sc::loader::SetAutoStubCallObserver([](const sc::loader::AutoStubMeta& meta, const void* callerPc) noexcept {
            sc::loader::ImportTelemetry::Instance().RecordRuntimeStubCall(
                meta.dllName.c_str(), meta.symbolName.c_str(), meta.defaultReturn, callerPc);
        });
        sc::loader::ImportTelemetry::Instance().InitializeFromEnv(g_exeModule);
        uint32_t patched = HookAllLoadedTitleModules(k32);

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "-> armed %u multi-module LoadLibrary IAT hook(s) across %zu discovered shim(s)",
                      patched, g_shimsWide.size());
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "DllMain", buf);
    } else if (reason == DLL_PROCESS_DETACH) {
        const uint32_t drained = ScarlettHookRunGuestAtExit();
        if (drained > 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "-> drained %u isolated guest atexit handler(s)", drained);
            sc::log::Write(sc::log::Level::Info, "scarlett_hook", "DllMain[Detach]", buf);
        }
        std::string reportPath;
        if (auto envReport = sc::config::GetEnvPath(L"SCARLETT_STUB_REPORT")) {
            reportPath = WideToUtf8(envReport->wstring());
        }
        g_autoStubs.DumpHotlist(reportPath.empty() ? nullptr : reportPath.c_str(), true);
        sc::loader::SetAutoStubCallObserver(nullptr);
        sc::loader::ImportTelemetry::Instance().Shutdown();
    }
    return TRUE;
}

namespace {

std::mutex g_guestAtExitMtx;
std::vector<void(__cdecl*)()> g_guestAtExitHandlers;

bool InspectLoadedModuleTlsImpl(
    HMODULE mod,
    uint32_t* outRawSize,
    uint32_t* outZeroFill,
    uint32_t* outCallbackCount) {
    if (!mod) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
    const uint32_t rawSize = (tls->EndAddressOfRawData >= tls->StartAddressOfRawData)
        ? static_cast<uint32_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData)
        : 0u;
    uint32_t cbCount = 0;
    if (tls->AddressOfCallBacks != 0) {
        const auto* cbs = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        while (cbCount < 256 && cbs[cbCount] != nullptr) {
            ++cbCount;
        }
    }
    if (outRawSize)       *outRawSize       = rawSize;
    if (outZeroFill)      *outZeroFill      = tls->SizeOfZeroFill;
    if (outCallbackCount) *outCallbackCount = cbCount;
    return true;
}

} // namespace

extern "C" __declspec(dllexport) uint32_t WINAPI ScarlettHookPatchModuleForTest(HMODULE mod) {
    return PatchModuleIat(mod);
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookGetRuntimeStats(
    uint32_t* outDelayPatched,
    uint32_t* outOrdinalLookups) {
    if (outDelayPatched)   *outDelayPatched   = static_cast<uint32_t>(g_delayPatchedCount);
    if (outOrdinalLookups) *outOrdinalLookups = static_cast<uint32_t>(g_ordinalLookupCount);
}

extern "C" __declspec(dllexport) int WINAPI ScarlettHookRegisterGuestAtExit(void(__cdecl* fn)()) {
    if (!fn) return -1;
    std::lock_guard<std::mutex> lock(g_guestAtExitMtx);
    g_guestAtExitHandlers.push_back(fn);
    return 0;
}

extern "C" __declspec(dllexport) uint32_t WINAPI ScarlettHookRunGuestAtExit() {
    std::vector<void(__cdecl*)()> pending;
    {
        std::lock_guard<std::mutex> lock(g_guestAtExitMtx);
        pending.swap(g_guestAtExitHandlers);
    }
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        if (*it) {
            (*it)();
        }
    }
    return static_cast<uint32_t>(pending.size());
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookInspectModuleTls(
    HMODULE mod,
    uint32_t* outRawSize,
    uint32_t* outZeroFill,
    uint32_t* outCallbackCount) {
    return InspectLoadedModuleTlsImpl(mod, outRawSize, outZeroFill, outCallbackCount) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookInitImplicitTls(
    const IMAGE_TLS_DIRECTORY64* tlsDir,
    HMODULE moduleHandle,
    void** outTlsBlock,
    uint32_t* outTlsIndex,
    uint32_t* outCallbacksRun) {
    if (!tlsDir || !outTlsBlock || !outTlsIndex) return FALSE;
    *outTlsBlock = nullptr;
    *outTlsIndex = TLS_OUT_OF_INDEXES;
    if (outCallbacksRun) *outCallbacksRun = 0;

    if (tlsDir->EndAddressOfRawData < tlsDir->StartAddressOfRawData) return FALSE;
    const size_t rawSize = static_cast<size_t>(tlsDir->EndAddressOfRawData - tlsDir->StartAddressOfRawData);
    const size_t zeroSize = tlsDir->SizeOfZeroFill;
    const size_t totalSize = (rawSize + zeroSize) > 0 ? (rawSize + zeroSize) : 16;

    const DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) return FALSE;

    void* block = std::calloc(1, totalSize);
    if (!block) {
        TlsFree(slot);
        return FALSE;
    }
    if (rawSize > 0 && tlsDir->StartAddressOfRawData != 0) {
        std::memcpy(block, reinterpret_cast<const void*>(tlsDir->StartAddressOfRawData), rawSize);
    }
    if (!TlsSetValue(slot, block)) {
        std::free(block);
        TlsFree(slot);
        return FALSE;
    }
    if (tlsDir->AddressOfIndex != 0) {
        *reinterpret_cast<uint32_t*>(tlsDir->AddressOfIndex) = slot;
    }

    uint32_t ran = 0;
    if (tlsDir->AddressOfCallBacks != 0) {
        const auto* cbs = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tlsDir->AddressOfCallBacks);
        while (ran < 256 && cbs[ran] != nullptr) {
            cbs[ran](moduleHandle, DLL_PROCESS_ATTACH, nullptr);
            ++ran;
        }
    }

    *outTlsBlock = block;
    *outTlsIndex = slot;
    if (outCallbacksRun) *outCallbacksRun = ran;
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookFreeImplicitTls(
    const IMAGE_TLS_DIRECTORY64* tlsDir,
    HMODULE moduleHandle,
    void* tlsBlock,
    uint32_t tlsIndex) {
    if (tlsDir && tlsDir->AddressOfCallBacks != 0) {
        const auto* cbs = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tlsDir->AddressOfCallBacks);
        for (uint32_t i = 0; i < 256 && cbs[i] != nullptr; ++i) {
            cbs[i](moduleHandle, DLL_PROCESS_DETACH, nullptr);
        }
    }
    if (tlsIndex != TLS_OUT_OF_INDEXES) {
        TlsSetValue(tlsIndex, nullptr);
        TlsFree(tlsIndex);
    }
    if (tlsBlock) {
        std::free(tlsBlock);
    }
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookGetAutoStubStats(
    uint32_t* outUniqueStubs,
    uint64_t* outTotalCalls) {
    g_autoStubs.GetStats(outUniqueStubs, outTotalCalls);
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookSetAutoStubPolicy(
    const char* symbolOrKey,
    uint64_t returnValue) {
    if (symbolOrKey && symbolOrKey[0] != '\0') {
        g_autoStubs.SetPolicyOverride(symbolOrKey, returnValue);
    }
}

extern "C" __declspec(dllexport) uint32_t WINAPI ScarlettHookDumpAutoStubReport(
    const char* reportPathUtf8) {
    return g_autoStubs.DumpHotlist(reportPathUtf8, false);
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookIsModuleTracked(HMODULE mod) {
    std::lock_guard<std::mutex> lock(g_hookMtx);
    return g_seenModules.find(mod) != g_seenModules.end() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) uint32_t WINAPI ScarlettHookTrackAndPatchForTest(HMODULE mod) {
    ModuleIdentity id{};
    if (!ReadModuleIdentity(mod, id)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_hookMtx);
    auto it = g_seenModules.find(mod);
    if (it != g_seenModules.end() && it->second == id) {
        return 0;
    }
    g_seenModules[mod] = id;
    return PatchModuleIat(mod);
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookUntrackModuleForTest(HMODULE mod) {
    std::lock_guard<std::mutex> lock(g_hookMtx);
    g_seenModules.erase(mod);
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookRegisterTestShim(
    const wchar_t* dllName,
    const wchar_t* fullWidePath) {
    if (!dllName || !*dllName) {
        return FALSE;
    }
    const std::wstring keyW = NormalizeDllKeyW(dllName);
    if (keyW.empty()) {
        return FALSE;
    }
    std::lock_guard<std::mutex> lock(g_shimMapMtx);
    if (!fullWidePath || !*fullWidePath) {
        g_shimsWide.erase(keyW);
        return TRUE;
    }
    g_shimsWide[keyW] = fullWidePath;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookTelemetryEnable(
    const wchar_t* outputPathW,
    BOOL installVeh) {
    return sc::loader::ImportTelemetry::Instance().Enable(
        outputPathW, g_exeModule, installVeh != FALSE) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookTelemetryDisable() {
    sc::loader::ImportTelemetry::Instance().Disable();
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookTelemetryResetForTest() {
    sc::loader::ImportTelemetry::Instance().ResetForTests();
}

extern "C" __declspec(dllexport) BOOL WINAPI ScarlettHookTelemetryFlush(
    const char* reasonUtf8) {
    return sc::loader::ImportTelemetry::Instance().Flush(reasonUtf8) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI ScarlettHookTelemetryGetStats(
    uint32_t* outUniqueSymbols,
    uint64_t* outTotalStubCalls,
    uint32_t* outRedirects) {
    sc::loader::ImportTelemetry::Instance().GetStats(
        outUniqueSymbols, outTotalStubCalls, outRedirects);
}
