#include "auto_stub.hpp"
#include "common/ansi_path.hpp"
#include "common/config.hpp"
#include "common/stub.hpp"
#include "common/vfs.hpp"
#include <sys/stat.h>

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

bool TraceBootEnabled() {
    static const bool s_enabled = []() {
        char buf[8]{};
        return GetEnvironmentVariableA("SCARLETT_TRACE_BOOT", buf, sizeof(buf)) > 0 && buf[0] != '0';
    }();
    return s_enabled;
}

void LogCallStack(const char* reason, uint32_t code) {
    char hdr[128];
    std::snprintf(hdr, sizeof(hdr), "code=%u (0x%08X) backtrace:", code, code);
    sc::log::Write(sc::log::Level::Warn, "scarlett_hook", reason, hdr);
    void* frames[12]{};
    USHORT count = CaptureStackBackTrace(1, 12, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE mod = nullptr;
        char line[320];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod) && mod) {
            char path[MAX_PATH]{};
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            name = name ? name + 1 : path;
            auto rva = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod);
            std::snprintf(line, sizeof(line), "  #%02u %s+0x%llX", i, name, static_cast<unsigned long long>(rva));
        } else {
            std::snprintf(line, sizeof(line), "  #%02u 0x%p", i, frames[i]);
        }
        sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "StackFrame", line);
    }
}

void WINAPI HookOutputDebugStringA(LPCSTR lpOutputString) {
    if (lpOutputString && lpOutputString[0] != '\0') {
        std::string s(lpOutputString);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) {
            sc::log::Write(sc::log::Level::Info, "guest_dbg", "OutputDebugStringA", s.c_str());
            if (s.find("WER") != std::string::npos || s.find("crash.log") != std::string::npos) {
                LogCallStack("WER_OutputDebugStringA", 0);
            }
        }
    }
    ::OutputDebugStringA(lpOutputString);
}

void WINAPI HookOutputDebugStringW(LPCWSTR lpOutputString) {
    if (lpOutputString && lpOutputString[0] != L'\0') {
        std::string s = WideToUtf8(lpOutputString);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) {
            sc::log::Write(sc::log::Level::Info, "guest_dbg", "OutputDebugStringW", s.c_str());
        }
    }
    ::OutputDebugStringW(lpOutputString);
}

void WINAPI HookExitProcess(UINT uExitCode) {
    LogCallStack("ExitProcess", uExitCode);
    ::ExitProcess(uExitCode);
}

void __cdecl HookCrtExit(int status) {
    LogCallStack("CRT::exit", static_cast<uint32_t>(status));
    using ExitFn = void(__cdecl*)(int);
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto fn = ucrt ? reinterpret_cast<ExitFn>(GetProcAddress(ucrt, "exit")) : nullptr;
    if (fn) fn(status);
    ::ExitProcess(static_cast<UINT>(status));
}

[[noreturn]] void __cdecl HookCrtAbort() {
    LogCallStack("CRT::abort", 0xC0000409u);
    using AbortFn = void(__cdecl*)();
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto fn = ucrt ? reinterpret_cast<AbortFn>(GetProcAddress(ucrt, "abort")) : nullptr;
    if (fn) fn();
    ::TerminateProcess(::GetCurrentProcess(), 0xC0000409u);
}

[[noreturn]] void __cdecl HookCrtTerminate() {
    LogCallStack("CRT::terminate", 0xC0000409u);
    using TermFn = void(__cdecl*)();
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto fn = ucrt ? reinterpret_cast<TermFn>(GetProcAddress(ucrt, "terminate")) : nullptr;
    if (fn) fn();
    ::TerminateProcess(::GetCurrentProcess(), 0xC0000409u);
}

void __cdecl HookCrtWassert(const wchar_t* expr, const wchar_t* file, unsigned line) {
    char msg[320];
    std::snprintf(msg, sizeof(msg), "-> line=%u", line);
    sc::log::Write(sc::log::Level::Error, "scarlett_hook", "CRT::_wassert", msg);
    LogCallStack("CRT::_wassert", line);
    using WassertFn = void(__cdecl*)(const wchar_t*, const wchar_t*, unsigned);
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto fn = ucrt ? reinterpret_cast<WassertFn>(GetProcAddress(ucrt, "_wassert")) : nullptr;
    if (fn) fn(expr, file, line);
}

using Il2CppInitFn    = int(__cdecl*)(const char*);
using Il2CppSetDirFn  = void(__cdecl*)(const char*);
Il2CppInitFn   g_realIl2CppInit      = nullptr;
Il2CppSetDirFn g_realIl2CppConfigDir = nullptr;
Il2CppSetDirFn g_realIl2CppDataDir   = nullptr;
Il2CppSetDirFn g_realIl2CppTempDir   = nullptr;

void __cdecl TraceIl2CppSetConfigDir(const char* dir) {
    sc::log::Write(sc::log::Level::Info, "il2cpp_trace", "il2cpp_set_config_dir", dir ? dir : "<null>");
    if (g_realIl2CppConfigDir) g_realIl2CppConfigDir(dir);
}

void __cdecl TraceIl2CppSetDataDir(const char* dir) {
    sc::log::Write(sc::log::Level::Info, "il2cpp_trace", "il2cpp_set_data_dir", dir ? dir : "<null>");
    if (g_realIl2CppDataDir) g_realIl2CppDataDir(dir);
}

void __cdecl TraceIl2CppSetTempDir(const char* dir) {
    sc::log::Write(sc::log::Level::Info, "il2cpp_trace", "il2cpp_set_temp_dir", dir ? dir : "<null>");
    if (g_realIl2CppTempDir) g_realIl2CppTempDir(dir);
}

int __cdecl TraceIl2CppInit(const char* domain) {
    sc::log::Write(sc::log::Level::Info, "il2cpp_trace", "il2cpp_init[enter]", domain ? domain : "<null>");
    int rc = g_realIl2CppInit ? g_realIl2CppInit(domain) : 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "-> returned %d", rc);
    sc::log::Write(sc::log::Level::Info, "il2cpp_trace", "il2cpp_init[exit]", buf);
    return rc;
}

LPCWSTR TranslateVfsPathW(LPCWSTR raw, std::wstring& storage, bool& wasVirtual) {
    wasVirtual = false;
    if (!raw || !raw[0]) return raw;
    LPCWSTR probe = raw;
    bool hadExtPrefix = false;
    if (probe[0] == L'\\' && probe[1] == L'\\' && probe[2] == L'?' && probe[3] == L'\\') {
        probe += 4;
        hadExtPrefix = true;
    }
    if (probe[0] != L'\0' && probe[1] == L':') {
        const wchar_t d = static_cast<wchar_t>(std::towupper(probe[0]));
        if (d == L'G' || d == L'T' || d == L'S') {
            std::filesystem::path mapped = sc::vfs::TranslatePath(probe, &wasVirtual);
            if (wasVirtual) {
                storage = mapped.wstring();
                if (hadExtPrefix && !storage.empty() && storage.rfind(L"\\\\?\\", 0) != 0) {
                    storage = L"\\\\?\\" + storage;
                }
                return storage.c_str();
            }
        }
    }
    return raw;
}

bool TryTranslateVfsPathA(LPCSTR raw, std::wstring& outWide) {
    if (!raw || !raw[0]) return false;
    LPCSTR probe = raw;
    if (probe[0] == '\\' && probe[1] == '\\' && probe[2] == '?' && probe[3] == '\\') {
        probe += 4;
    }
    if (probe[0] == '\0' || probe[1] != ':') return false;
    const char d = static_cast<char>(std::toupper(static_cast<unsigned char>(probe[0])));
    if (d != 'G' && d != 'T' && d != 'S') return false;
    std::wstring wideIn = AnsiToWide(raw);
    bool wasVirt = false;
    TranslateVfsPathW(wideIn.c_str(), outWide, wasVirt);
    return wasVirt;
}

HANDLE WINAPI HookCreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = ::CreateFileW(effective, dwDesiredAccess, dwShareMode,
                             lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
    if (h == INVALID_HANDLE_VALUE && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "FAILED: " + WideToUtf8(lpFileName ? lpFileName : L"")
                        + (wasVirt ? (" -> " + WideToUtf8(mapped)) : "")
                        + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "file_trace", "CreateFileW", msg.c_str());
        SetLastError(err);
    }
    return h;
}

using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, void*);
HANDLE WINAPI HookCreateFile2(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    DWORD dwCreationDisposition, void* pCreateExParams) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    static CreateFile2Fn s_orig = []() -> CreateFile2Fn {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
        return kb ? reinterpret_cast<CreateFile2Fn>(GetProcAddress(kb, "CreateFile2")) : nullptr;
    }();
    HANDLE h = s_orig ? s_orig(effective, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams)
                      : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "FAILED: " + WideToUtf8(lpFileName ? lpFileName : L"")
                        + (wasVirt ? (" -> " + WideToUtf8(mapped)) : "")
                        + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "file_trace", "CreateFile2", msg.c_str());
        SetLastError(err);
    }
    return h;
}

DWORD WINAPI HookGetFileAttributesW(LPCWSTR lpFileName) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attr = ::GetFileAttributesW(effective);
    if (attr == INVALID_FILE_ATTRIBUTES && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "FAILED: " + WideToUtf8(lpFileName ? lpFileName : L"")
                        + (wasVirt ? (" -> " + WideToUtf8(mapped)) : "")
                        + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "file_trace", "GetFileAttributesW", msg.c_str());
        SetLastError(err);
    }
    return attr;
}

DWORD WINAPI HookGetFileAttributesA(LPCSTR lpFileName) {
    std::wstring mapped;
    if (TryTranslateVfsPathA(lpFileName, mapped)) {
        if (mapped.empty()) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_FILE_ATTRIBUTES;
        }
        return ::GetFileAttributesW(mapped.c_str());
    }
    return ::GetFileAttributesA(lpFileName);
}

BOOL WINAPI HookGetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    BOOL ok = ::GetFileAttributesExW(effective, fInfoLevelId, lpFileInformation);
    if (!ok && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "FAILED: " + WideToUtf8(lpFileName ? lpFileName : L"")
                        + (wasVirt ? (" -> " + WideToUtf8(mapped)) : "")
                        + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "file_trace", "GetFileAttributesExW", msg.c_str());
        SetLastError(err);
    }
    return ok;
}

HANDLE WINAPI HookFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return ::FindFirstFileW(effective, lpFindFileData);
}

BOOL WINAPI HookCreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpPathName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::CreateDirectoryW(effective, lpSecurityAttributes);
}

BOOL WINAPI HookRemoveDirectoryW(LPCWSTR lpPathName) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpPathName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::RemoveDirectoryW(effective);
}

BOOL WINAPI HookDeleteFileW(LPCWSTR lpFileName) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::DeleteFileW(effective);
}

BOOL WINAPI HookSetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpFileName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::SetFileAttributesW(effective, dwFileAttributes);
}

BOOL WINAPI HookGetDiskFreeSpaceExW(
    LPCWSTR lpDirectoryName,
    PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    PULARGE_INTEGER lpTotalNumberOfBytes,
    PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(lpDirectoryName, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::GetDiskFreeSpaceExW(effective, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

BOOL WINAPI HookMoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    std::wstring m1, m2;
    bool v1 = false, v2 = false;
    LPCWSTR e1 = TranslateVfsPathW(lpExistingFileName, m1, v1);
    LPCWSTR e2 = TranslateVfsPathW(lpNewFileName, m2, v2);
    if ((v1 && m1.empty()) || (v2 && m2.empty())) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::MoveFileExW(e1, e2, dwFlags);
}

int __cdecl HookWstat64(const wchar_t* path, struct __stat64* buffer) {
    std::wstring mapped;
    bool wasVirt = false;
    LPCWSTR effective = TranslateVfsPathW(path, mapped, wasVirt);
    if (wasVirt && mapped.empty()) {
        return -1;
    }
    using Wstat64Fn = int(__cdecl*)(const wchar_t*, struct __stat64*);
    static Wstat64Fn s_orig = []() -> Wstat64Fn {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        return ucrt ? reinterpret_cast<Wstat64Fn>(GetProcAddress(ucrt, "_wstat64")) : nullptr;
    }();
    return s_orig ? s_orig(effective, buffer) : ::_wstat64(effective, buffer);
}

int __cdecl HookCrtRemove(const char* path) {
    std::wstring mapped;
    if (TryTranslateVfsPathA(path, mapped)) {
        if (mapped.empty()) return -1;
        using WremoveFn = int(__cdecl*)(const wchar_t*);
        static WremoveFn s_wrem = []() -> WremoveFn {
            HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
            return ucrt ? reinterpret_cast<WremoveFn>(GetProcAddress(ucrt, "_wremove")) : nullptr;
        }();
        return s_wrem ? s_wrem(mapped.c_str()) : ::_wremove(mapped.c_str());
    }
    using RemoveFn = int(__cdecl*)(const char*);
    static RemoveFn s_rem = []() -> RemoveFn {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        return ucrt ? reinterpret_cast<RemoveFn>(GetProcAddress(ucrt, "remove")) : nullptr;
    }();
    return s_rem ? s_rem(path) : std::remove(path);
}

int __cdecl HookPuts(const char* s) {
    if (s && s[0] != '\0' && TraceBootEnabled()) {
        sc::log::Write(sc::log::Level::Info, "guest_dbg", "puts", s);
    }
    using PutsFn = int(__cdecl*)(const char*);
    static PutsFn s_orig = []() -> PutsFn {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        return ucrt ? reinterpret_cast<PutsFn>(GetProcAddress(ucrt, "puts")) : nullptr;
    }();
    return s_orig ? s_orig(s) : 0;
}

volatile LONG g_vehLogCount = 0;

std::string FormatVehAddr(void* addr) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        const auto rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s+0x%llX (0x%llX)", name,
                      static_cast<unsigned long long>(rva),
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr)));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%llX",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr)));
    return buf;
}

static std::mutex g_cpuEmuVehMtx;

static size_t DecodeClwbOrClflushoptLength(const uint8_t* ip, bool& isClwb) {
    isClwb = false;
    size_t i = 0;
    bool has66 = false;
    while (i < 6) {
        uint8_t b = ip[i];
        if (b == 0x66) { has66 = true; ++i; continue; }
        if (b == 0x67 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            ++i; continue;
        }
        break;
    }
    if (!has66) return 0;
    if ((ip[i] & 0xF0) == 0x40) ++i; // optional REX
    if (ip[i] != 0x0F || ip[i + 1] != 0xAE) return 0;
    i += 2;
    uint8_t modrm = ip[i++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;
    if (mod == 3 || (reg != 6 && reg != 7)) return 0; // /6 = CLWB, /7 = CLFLUSHOPT
    isClwb = (reg == 6);
    if (rm == 4) {
        uint8_t sib = ip[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    } else if (mod == 0 && rm == 5) {
        i += 4;
    }
    if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    return i;
}

static LONG CALLBACK XboxCpuEmulationVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != STATUS_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    auto* ip = reinterpret_cast<uint8_t*>(ep->ContextRecord->Rip);
    if (!ip) return EXCEPTION_CONTINUE_SEARCH;

    // Emulate Zen 2 CLZERO (0F 01 FC): zeroes the 64-byte cache line at [RAX & ~63]
    if (ip[0] == 0x0F && ip[1] == 0x01 && ip[2] == 0xFC) {
        void* line = reinterpret_cast<void*>(ep->ContextRecord->Rax & ~63ULL);
        std::memset(line, 0, 64);
        ep->ContextRecord->Rip += 3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    std::lock_guard<std::mutex> lock(g_cpuEmuVehMtx);
    // If another thread already NOP-patched this exact site, just re-execute the NOPs.
    if (ip[0] == 0x90) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool isClwb = false;
    const size_t len = DecodeClwbOrClflushoptLength(ip, isClwb);
    if (len > 0) {
        DWORD oldProt = 0;
        bool patched = false;
        if (VirtualProtect(ip, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memset(ip, 0x90, len);
            DWORD tmp = 0;
            VirtualProtect(ip, len, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), ip, len);
            patched = true;
        }
        static std::atomic<uint32_t> s_patchLogs{0};
        if (s_patchLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "-> %s (%zuB) at %s (%s)",
                          isClwb ? "CLWB" : "CLFLUSHOPT", len,
                          FormatVehAddr(ip).c_str(),
                          patched ? "NOP-patched" : "skipped");
            sc::log::Write(sc::log::Level::Info, "scarlett_hook", "CpuEmuVeh", msg);
        }
        ep->ContextRecord->Rip += len;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK BootTraceVeh(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && ep->ContextRecord) {
        const auto* ip = reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rip);
        if (ip && ip[0] == 0xCCu) {
            static std::atomic<uint32_t> s_bpLogs{0};
            if (s_bpLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                char bmsg[160];
                std::snprintf(bmsg, sizeof(bmsg), "Continued past int3 at %s",
                              FormatVehAddr(ep->ExceptionRecord->ExceptionAddress).c_str());
                sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "VEH", bmsg);
            }
            ep->ContextRecord->Rip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && ep->ContextRecord) {
        const auto* ip = reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rip);
        if (ip && ip[0] == 0x0Fu && ip[1] == 0x01u && (ip[2] == 0xFAu || ip[2] == 0xFBu)) {
            const bool isMwaitx = (ip[2] == 0xFBu);
            static std::atomic<uint32_t> s_monxLogs{0};
            if (s_monxLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
                char mmsg[128];
                std::snprintf(mmsg, sizeof(mmsg), "Emulated %s at %s",
                              isMwaitx ? "MWAITX" : "MONITORX",
                              FormatVehAddr(ep->ExceptionRecord->ExceptionAddress).c_str());
                sc::log::Write(sc::log::Level::Info, "scarlett_hook", "VEH", mmsg);
            }
            ep->ContextRecord->Rip += 3;
            if (isMwaitx) {
                ::SwitchToThread();
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    const bool isFatalClass = (code >= 0x80000000u && code != 0xE06D7363u);
    if (!isFatalClass) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_vehLogCount) > 24) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto& ctx = *ep->ContextRecord;
    char msg[512];
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const auto op = ep->ExceptionRecord->ExceptionInformation[0];
        const char* opStr = (op == 0) ? "READ" : (op == 1) ? "WRITE" : (op == 8) ? "EXEC" : "OP";
        std::snprintf(msg, sizeof(msg),
                      "code=0x%08lX (%s addr=0x%llX) RIP=%s",
                      code, opStr,
                      static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]),
                      FormatVehAddr(ep->ExceptionRecord->ExceptionAddress).c_str());
    } else {
        std::snprintf(msg, sizeof(msg),
                      "code=0x%08lX RIP=%s",
                      code,
                      FormatVehAddr(ep->ExceptionRecord->ExceptionAddress).c_str());
    }
    sc::log::Write(sc::log::Level::Error, "scarlett_hook", "VEH", msg);

    char regs[384];
    std::snprintf(regs, sizeof(regs),
                  "RAX=0x%llX RBX=0x%llX RCX=0x%llX RDX=0x%llX R8=0x%llX R9=0x%llX RSP=0x%llX RBP=0x%llX",
                  static_cast<unsigned long long>(ctx.Rax),
                  static_cast<unsigned long long>(ctx.Rbx),
                  static_cast<unsigned long long>(ctx.Rcx),
                  static_cast<unsigned long long>(ctx.Rdx),
                  static_cast<unsigned long long>(ctx.R8),
                  static_cast<unsigned long long>(ctx.R9),
                  static_cast<unsigned long long>(ctx.Rsp),
                  static_cast<unsigned long long>(ctx.Rbp));
    sc::log::Write(sc::log::Level::Error, "scarlett_hook", "VEH[regs]", regs);

    LogCallStack("VEH[bt]", code);
    return EXCEPTION_CONTINUE_SEARCH;
}

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
        if (TraceBootEnabled()) {
            if (std::strcmp(name, "OutputDebugStringA") == 0)   return reinterpret_cast<void*>(&HookOutputDebugStringA);
            if (std::strcmp(name, "OutputDebugStringW") == 0)   return reinterpret_cast<void*>(&HookOutputDebugStringW);
            if (std::strcmp(name, "ExitProcess") == 0)          return reinterpret_cast<void*>(&HookExitProcess);
            if (std::strcmp(name, "exit") == 0)                 return reinterpret_cast<void*>(&HookCrtExit);
            if (std::strcmp(name, "abort") == 0)                return reinterpret_cast<void*>(&HookCrtAbort);
            if (std::strcmp(name, "terminate") == 0)            return reinterpret_cast<void*>(&HookCrtTerminate);
            if (std::strcmp(name, "_wassert") == 0)             return reinterpret_cast<void*>(&HookCrtWassert);
            if (std::strcmp(name, "CreateFileW") == 0)          return reinterpret_cast<void*>(&HookCreateFileW);
            if (std::strcmp(name, "CreateFile2") == 0)          return reinterpret_cast<void*>(&HookCreateFile2);
            if (std::strcmp(name, "GetFileAttributesW") == 0)   return reinterpret_cast<void*>(&HookGetFileAttributesW);
            if (std::strcmp(name, "GetFileAttributesExW") == 0) return reinterpret_cast<void*>(&HookGetFileAttributesExW);
            if (std::strcmp(name, "puts") == 0)                 return reinterpret_cast<void*>(&HookPuts);
        }
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
        std::string msg = "[dynamic] " + WideToUtf8(lpLibFileName ? lpLibFileName : L"")
                        + " -> " + WideToUtf8(targetWide);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryW", msg.c_str());
        return g_origLoadLibraryW(targetWide.c_str());
    }
    HMODULE mod = g_origLoadLibraryW(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    } else if (TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "LoadLibrary FAILED: " + WideToUtf8(lpLibFileName ? lpLibFileName : L"") + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "LoadLibraryW", msg.c_str());
        SetLastError(err);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName) {
    // Route shim redirects directly through g_shimsWide + LoadLibraryW to avoid ANSI path mangling.
    std::wstring targetWide;
    if (g_origLoadLibraryW && FindShimWide(AnsiToWide(NormalizeDllKeyA(lpLibFileName)), targetWide)) {
        std::string msg = "[dynamic] " + std::string(lpLibFileName ? lpLibFileName : "")
                        + " -> " + WideToUtf8(targetWide);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryA", msg.c_str());
        return g_origLoadLibraryW(targetWide.c_str());
    }
    HMODULE mod = g_origLoadLibraryA(lpLibFileName);
    if (mod) {
        HookModuleIfEligible(mod, true);
    } else if (TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "LoadLibrary FAILED: " + std::string(lpLibFileName ? lpLibFileName : "") + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "LoadLibraryA", msg.c_str());
        SetLastError(err);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring targetWide;
    if (FindShimWide(NormalizeDllKeyW(lpLibFileName), targetWide)) {
        std::string msg = "[dynamic] " + WideToUtf8(lpLibFileName ? lpLibFileName : L"")
                        + " -> " + WideToUtf8(targetWide);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExW", msg.c_str());
        return g_origLoadLibraryExW(targetWide.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    } else if (!mod && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "LoadLibrary FAILED: " + WideToUtf8(lpLibFileName ? lpLibFileName : L"") + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "LoadLibraryExW", msg.c_str());
        SetLastError(err);
    }
    return mod;
}

HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring targetWide;
    if (g_origLoadLibraryExW && FindShimWide(AnsiToWide(NormalizeDllKeyA(lpLibFileName)), targetWide)) {
        std::string msg = "[dynamic] " + std::string(lpLibFileName ? lpLibFileName : "")
                        + " -> " + WideToUtf8(targetWide);
        sc::log::Write(sc::log::Level::Info, "scarlett_hook", "LoadLibraryExA", msg.c_str());
        return g_origLoadLibraryExW(targetWide.c_str(), hFile, SanitizeFlagsForAbsoluteShim(dwFlags));
    }
    HMODULE mod = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (mod && !IsDataOnlyLoad(dwFlags)) {
        HookModuleIfEligible(mod, true);
    } else if (!mod && TraceBootEnabled()) {
        DWORD err = GetLastError();
        std::string msg = "LoadLibrary FAILED: " + std::string(lpLibFileName ? lpLibFileName : "") + " (err=" + std::to_string(err) + ")";
        sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "LoadLibraryExA", msg.c_str());
        SetLastError(err);
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
    if (TraceBootEnabled() && !isOrdinal && lpProcName) {
        if (resolved) {
            if (std::strcmp(lpProcName, "il2cpp_init") == 0) {
                g_realIl2CppInit = reinterpret_cast<Il2CppInitFn>(resolved);
                return reinterpret_cast<FARPROC>(&TraceIl2CppInit);
            }
            if (std::strcmp(lpProcName, "il2cpp_set_config_dir") == 0) {
                g_realIl2CppConfigDir = reinterpret_cast<Il2CppSetDirFn>(resolved);
                return reinterpret_cast<FARPROC>(&TraceIl2CppSetConfigDir);
            }
            if (std::strcmp(lpProcName, "il2cpp_set_data_dir") == 0) {
                g_realIl2CppDataDir = reinterpret_cast<Il2CppSetDirFn>(resolved);
                return reinterpret_cast<FARPROC>(&TraceIl2CppSetDataDir);
            }
            if (std::strcmp(lpProcName, "il2cpp_set_temp_dir") == 0) {
                g_realIl2CppTempDir = reinterpret_cast<Il2CppSetDirFn>(resolved);
                return reinterpret_cast<FARPROC>(&TraceIl2CppSetTempDir);
            }
        } else {
            DWORD err = GetLastError();
            char modPath[MAX_PATH]{};
            if (hModule) GetModuleFileNameA(hModule, modPath, MAX_PATH);
            const char* modName = std::strrchr(modPath, '\\');
            modName = modName ? modName + 1 : modPath;
            std::string msg = std::string(modName) + "!" + lpProcName + " -> nullptr (err=" + std::to_string(err) + ")";
            sc::log::Write(sc::log::Level::Warn, "scarlett_hook", "GetProcAddress[miss]", msg.c_str());
            SetLastError(err);
        }
    }
    if (resolved || !hModule || !lpProcName || !sc::config::IsAutoStubEnabled()) {
        return resolved;
    }

    std::string shimDllLower;
    if (!TryGetShimDllName(hModule, shimDllLower)) {
        return nullptr;
    }

    std::string symName;
    if (isOrdinal) {
        char ordBuf[32];
        std::snprintf(ordBuf, sizeof(ordBuf), "#%u",
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(lpProcName) & 0xFFFFu));
        symName = ordBuf;
    } else {
        symName = lpProcName;
    }
    return g_autoStubs.GetOrCreate(shimDllLower, symName);
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

    // Xbox One GDK -> Scarlett shim aliases
    static constexpr std::pair<const wchar_t*, const wchar_t*> kShimAliasesW[] = {
        {L"d3d12_x.dll", L"d3d12_xs.dll"},
        {L"xg_x.dll",    L"xg_xs.dll"},
    };
    std::lock_guard<std::mutex> lock(g_shimMapMtx);
    for (const auto& [aliasKey, targetKey] : kShimAliasesW) {
        if (g_shimsWide.find(aliasKey) == g_shimsWide.end()) {
            auto it = g_shimsWide.find(targetKey);
            if (it != g_shimsWide.end()) {
                g_shimsWide.emplace(aliasKey, it->second);
            }
        }
    }
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
        uint32_t patched = HookAllLoadedTitleModules(k32);

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "-> armed %u multi-module LoadLibrary IAT hook(s) across %zu discovered shim(s)",
                      patched, g_shimsWide.size());
    if (TraceBootEnabled()) {
        AddVectoredExceptionHandler(1, BootTraceVeh);
    }
    AddVectoredExceptionHandler(1, XboxCpuEmulationVeh);
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
