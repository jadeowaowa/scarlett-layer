#include "pe_patcher.hpp"
#include "common/ansi_path.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sc::loader {
namespace {

struct ScProcessBasicInfo {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

using NtQueryInformationProcessFn = LONG(NTAPI*)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

template <typename T>
bool ReadRemote(HANDLE hProcess, uintptr_t addr, T& out) {
    SIZE_T read = 0;
    return ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr),
                             &out, sizeof(T), &read) != FALSE
        && read == sizeof(T);
}

bool ReadRemoteAnsiString(HANDLE hProcess, uintptr_t addr, std::string& out, size_t maxLen = 260) {
    out.clear();
    uintptr_t cursor = addr;
    while (out.size() < maxLen) {
        size_t pageRemain = 0x1000 - static_cast<size_t>(cursor & 0xFFFULL);
        size_t chunkLen = std::min<size_t>(pageRemain, maxLen - out.size());
        std::vector<char> buf(chunkLen);
        SIZE_T read = 0;
        if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(cursor),
                               buf.data(), chunkLen, &read) || read == 0) {
            return false;
        }
        for (size_t i = 0; i < read; ++i) {
            if (buf[i] == '\0') {
                return true;
            }
            out.push_back(buf[i]);
        }
        cursor += read;
    }
    return true;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::wstring ToLowerW(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

bool WidePathToAnsiChecked(const std::wstring& w, std::string& outAnsi) {
    bool usedShort = false;
    if (sc::path::WidePathToAnsiLossless(w, outAnsi, &usedShort)) {
        if (usedShort) {
            std::string msg = "Used 8.3 short path fallback for non-ACP shim path: " +
                              sc::path::WideToUtf8(w) + " -> " + outAnsi;
            sc::log::Write(sc::log::Level::Info, "loader", "WidePathToAnsi", msg.c_str());
        }
        return true;
    }
    std::string msg = "Shim path cannot be losslessly encoded in CP_ACP (8.3 short path unavailable): " +
                      sc::path::WideToUtf8(w);
    sc::log::Write(sc::log::Level::Error, "loader", "WidePathToAnsi", msg.c_str());
    return false;
}

bool GetRemoteImageBase(HANDLE hProcess, uintptr_t& outImageBase) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return false;
    }
    auto queryFn = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!queryFn) {
        return false;
    }

    ScProcessBasicInfo pbi{};
    LONG status = queryFn(hProcess, 0, &pbi, sizeof(pbi), nullptr);
    if (status < 0 || !pbi.PebBaseAddress) {
        return false;
    }

    uintptr_t pebImageBaseField = reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x10;
    return ReadRemote(hProcess, pebImageBaseField, outImageBase) && outImageBase != 0;
}

uintptr_t AllocateNearImageBase(HANDLE hProcess, uintptr_t imageBase, DWORD sizeOfImage, size_t allocSize) {
    constexpr uintptr_t kGranularity = 0x10000ULL;
    uintptr_t searchStart = (imageBase + static_cast<uintptr_t>(sizeOfImage) + (kGranularity - 1))
                            & ~(kGranularity - 1);
    uintptr_t searchLimit = imageBase + 0xF0000000ULL;

    uintptr_t cursor = searchStart;
    while (cursor < searchLimit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= cursor) {
            break;
        }

        if (mbi.State == MEM_FREE) {
            uintptr_t candidate = (std::max(regionBase, searchStart) + (kGranularity - 1))
                                  & ~(kGranularity - 1);
            if (candidate + allocSize <= regionEnd && candidate < searchLimit) {
                LPVOID mem = VirtualAllocEx(
                    hProcess, reinterpret_cast<LPVOID>(candidate),
                    allocSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (mem) {
                    return reinterpret_cast<uintptr_t>(mem);
                }
            }
        }
        cursor = regionEnd;
    }
    return 0;
}

struct DiscoveredShims {
    std::unordered_map<std::string, std::string> ansiByKey;
    std::unordered_set<std::string>              unrepresentableKeys;
};

DiscoveredShims DiscoverShims(const std::filesystem::path& shimDir) {
    DiscoveredShims result;
    std::error_code ec;
    if (!std::filesystem::exists(shimDir, ec)) {
        return result;
    }
    for (const auto& entry : std::filesystem::directory_iterator(shimDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::wstring extW = ToLowerW(entry.path().extension().wstring());
        if (extW != L".dll") {
            continue;
        }
        std::string keyAnsi;
        if (!sc::path::WideToCodePageLossless(CP_ACP, entry.path().filename().wstring(), keyAnsi)) {
            std::string msg = "Skipping shim with non-ACP filename: " +
                              sc::path::WideToUtf8(entry.path().filename().wstring());
            sc::log::Write(sc::log::Level::Warn, "loader", "DiscoverShims", msg.c_str());
            continue;
        }
        std::string key = ToLowerAscii(std::move(keyAnsi));
        std::string fullAnsi;
        if (WidePathToAnsiChecked(std::filesystem::absolute(entry.path(), ec).wstring(), fullAnsi)) {
            result.ansiByKey.emplace(std::move(key), std::move(fullAnsi));
        } else {
            result.unrepresentableKeys.insert(std::move(key));
        }
    }
    return result;
}

struct PatchTarget {
    uintptr_t rvaFieldAddr;
    std::string originalName;
    std::string fullShimPath;
    bool isDelayLoad;
};

} // namespace

bool RedirectImportsInSuspendedProcess(HANDLE hProcess, const std::filesystem::path& shimDir) {
    auto discovered = DiscoverShims(shimDir);
    auto& shims = discovered.ansiByKey;
    if (shims.empty() && discovered.unrepresentableKeys.empty()) {
        sc::log::Write(sc::log::Level::Warn, "loader", "RedirectImports",
                       "No shim DLLs found in shims directory");
        return true;
    }

    uintptr_t imageBase = 0;
    if (!GetRemoteImageBase(hProcess, imageBase)) {
        sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                       "Failed to read remote ImageBase from PEB");
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!ReadRemote(hProcess, imageBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports", "Invalid DOS header");
        return false;
    }

    uintptr_t ntAddr = imageBase + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadRemote(hProcess, ntAddr, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                       "Target is not a valid 64-bit PE image");
        return false;
    }

    std::vector<PatchTarget> patches;

    const auto& importDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress != 0 && importDir.Size != 0) {
        uintptr_t descAddr = imageBase + importDir.VirtualAddress;
        for (size_t i = 0; i < 512; ++i, descAddr += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR desc{};
            if (!ReadRemote(hProcess, descAddr, desc)) {
                break;
            }
            if (desc.Name == 0 && desc.FirstThunk == 0) {
                break;
            }
            if (desc.Name == 0) {
                continue;
            }
            std::string dllName;
            if (ReadRemoteAnsiString(hProcess, imageBase + desc.Name, dllName)) {
                const std::string lowerName = ToLowerAscii(dllName);
                if (discovered.unrepresentableKeys.count(lowerName) > 0) {
                    std::string msg = "Cannot redirect static import '" + dllName +
                                      "': shim directory path is not representable in CP_ACP";
                    sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports", msg.c_str());
                    return false;
                }
                auto it = shims.find(lowerName);
                if (it != shims.end()) {
                    patches.push_back({
                        descAddr + offsetof(IMAGE_IMPORT_DESCRIPTOR, Name),
                        dllName,
                        it->second,
                        false
                    });
                }
            }
        }
    }

    const auto& delayDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        uintptr_t descAddr = imageBase + delayDir.VirtualAddress;
        for (size_t i = 0; i < 512; ++i, descAddr += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
            IMAGE_DELAYLOAD_DESCRIPTOR desc{};
            if (!ReadRemote(hProcess, descAddr, desc)) {
                break;
            }
            if (desc.DllNameRVA == 0) {
                break;
            }
            if (!desc.Attributes.RvaBased) {
                continue;
            }
            std::string dllName;
            if (ReadRemoteAnsiString(hProcess, imageBase + desc.DllNameRVA, dllName)) {
                const std::string lowerName = ToLowerAscii(dllName);
                if (discovered.unrepresentableKeys.count(lowerName) > 0) {
                    std::string msg = "Cannot redirect delay-load import '" + dllName +
                                      "': shim directory path is not representable in CP_ACP";
                    sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports", msg.c_str());
                    return false;
                }
                auto it = shims.find(lowerName);
                if (it != shims.end()) {
                    patches.push_back({
                        descAddr + offsetof(IMAGE_DELAYLOAD_DESCRIPTOR, DllNameRVA),
                        dllName,
                        it->second,
                        true
                    });
                }
            }
        }
    }

    if (patches.empty()) {
        sc::log::Write(sc::log::Level::Warn, "loader", "RedirectImports",
                       "Target imports no matching shim DLLs");
        return true;
    }

    std::vector<char> stringBlob;
    std::vector<size_t> offsets;
    offsets.reserve(patches.size());
    for (const auto& p : patches) {
        offsets.push_back(stringBlob.size());
        stringBlob.insert(stringBlob.end(), p.fullShimPath.begin(), p.fullShimPath.end());
        stringBlob.push_back('\0');
    }

    size_t allocSize = (stringBlob.size() + 0xFFFULL) & ~0xFFFULL;
    uintptr_t remoteBlob = AllocateNearImageBase(
        hProcess, imageBase, nt.OptionalHeader.SizeOfImage, allocSize);
    if (remoteBlob == 0 || (remoteBlob - imageBase + stringBlob.size()) > 0xFFFFFFFFULL) {
        sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                       "Failed to allocate string page within 4 GB of ImageBase");
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(remoteBlob),
                            stringBlob.data(), stringBlob.size(), &written) ||
        written != stringBlob.size()) {
        sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                       "Failed to write redirected path strings to target process");
        return false;
    }

    const auto& boundDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT];
    if (boundDir.VirtualAddress != 0 || boundDir.Size != 0) {
        uintptr_t boundField = ntAddr +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
            IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
        IMAGE_DATA_DIRECTORY zeroDir{};
        DWORD oldProt = 0;
        if (VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(boundField),
                             sizeof(zeroDir), PAGE_READWRITE, &oldProt)) {
            WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(boundField),
                               &zeroDir, sizeof(zeroDir), &written);
            DWORD restoreProt = 0;
            VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(boundField),
                             sizeof(zeroDir), oldProt, &restoreProt);
        }
    }

    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        DWORD newRva = static_cast<DWORD>((remoteBlob + offsets[i]) - imageBase);

        DWORD oldProt = 0;
        if (!VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(p.rvaFieldAddr),
                              sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
            sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                           "VirtualProtectEx failed on import descriptor");
            return false;
        }

        BOOL ok = WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(p.rvaFieldAddr),
                                     &newRva, sizeof(DWORD), &written);

        DWORD restoreProt = 0;
        VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(p.rvaFieldAddr),
                         sizeof(DWORD), oldProt, &restoreProt);

        if (!ok || written != sizeof(DWORD)) {
            sc::log::Write(sc::log::Level::Error, "loader", "RedirectImports",
                           "WriteProcessMemory failed on import descriptor");
            return false;
        }

        std::string msg = std::string(p.isDelayLoad ? "[delay] " : "[static] ")
                        + p.originalName + " -> " + p.fullShimPath;
        sc::log::Write(sc::log::Level::Info, "loader", "RedirectImport", msg.c_str());
    }

    DWORD oldBlobProt = 0;
    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(remoteBlob),
                     allocSize, PAGE_READONLY, &oldBlobProt);
    return true;
}

bool InjectHookDllInSuspendedProcess(
    HANDLE hProcess,
    HANDLE hThread,
    const std::filesystem::path& hookDllPath) {
    std::error_code ec;
    if (!std::filesystem::exists(hookDllPath, ec)) {
        sc::log::Write(sc::log::Level::Warn, "loader", "InjectHook",
                       "scarlett_hook.dll not found; skipping dynamic LoadLibrary hooks");
        return true;
    }

    const std::wstring absPath = std::filesystem::absolute(hookDllPath, ec).wstring();
    const size_t pathBytes = (absPath.size() + 1) * sizeof(wchar_t);

    LPVOID remotePath = VirtualAllocEx(
        hProcess, nullptr, pathBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) {
        sc::log::Write(sc::log::Level::Error, "loader", "InjectHook", "VirtualAllocEx failed");
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remotePath, absPath.c_str(), pathBytes, &written) ||
        written != pathBytes) {
        sc::log::Write(sc::log::Level::Error, "loader", "InjectHook", "WriteProcessMemory failed");
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibW = k32 ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
    if (!loadLibW) {
        sc::log::Write(sc::log::Level::Error, "loader", "InjectHook",
                       "Could not resolve kernel32!LoadLibraryW");
        return false;
    }

    // Runs inside ntdll!LdrInitializeThunk via NtTestAlert right before target's entry point
    if (QueueUserAPC(reinterpret_cast<PAPCFUNC>(loadLibW), hThread,
                     reinterpret_cast<ULONG_PTR>(remotePath)) == 0) {
        sc::log::Write(sc::log::Level::Error, "loader", "InjectHook", "QueueUserAPC failed");
        return false;
    }

    std::string msg = "Queued pre-entrypoint APC -> " + sc::path::WideToUtf8(hookDllPath.wstring());
    sc::log::Write(sc::log::Level::Info, "loader", "InjectHook", msg.c_str());
    return true;
}

} // namespace sc::loader