#include "pe_patcher.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
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
        // Clamp each read to the current 4 KB page so we never fault past a section boundary
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

std::string WideToAnsi(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
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
    LONG status = queryFn(hProcess, 0 /*ProcessBasicInformation*/,
                          &pbi, sizeof(pbi), nullptr);
    if (status < 0 || !pbi.PebBaseAddress) {
        return false;
    }

    // On x64 Windows, PEB::ImageBaseAddress sits at offset 0x10
    uintptr_t pebImageBaseField = reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x10;
    return ReadRemote(hProcess, pebImageBaseField, outImageBase) && outImageBase != 0;
}

// Finds and commits a memory region strictly above imageBase within 32-bit RVA range
uintptr_t AllocateNearImageBase(HANDLE hProcess, uintptr_t imageBase, DWORD sizeOfImage, size_t allocSize) {
    constexpr uintptr_t kGranularity = 0x10000ULL; // 64 KB Windows allocation granularity
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

std::unordered_map<std::string, std::string> DiscoverShims(const std::filesystem::path& shimDir) {
    std::unordered_map<std::string, std::string> shims;
    std::error_code ec;
    if (!std::filesystem::exists(shimDir, ec)) {
        return shims;
    }
    for (const auto& entry : std::filesystem::directory_iterator(shimDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = ToLowerAscii(entry.path().extension().string());
        if (ext != ".dll") {
            continue;
        }
        std::string key = ToLowerAscii(entry.path().filename().string());
        std::string fullAnsi = WideToAnsi(std::filesystem::absolute(entry.path(), ec).wstring());
        if (!fullAnsi.empty()) {
            shims.emplace(std::move(key), std::move(fullAnsi));
        }
    }
    return shims;
}

struct PatchTarget {
    uintptr_t rvaFieldAddr;
    std::string originalName;
    std::string fullShimPath;
    bool isDelayLoad;
};

} // namespace

bool RedirectImportsInSuspendedProcess(HANDLE hProcess, const std::filesystem::path& shimDir) {
    auto shims = DiscoverShims(shimDir);
    if (shims.empty()) {
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

    // 1. Scan standard static imports
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
                auto it = shims.find(ToLowerAscii(dllName));
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

    // 2. Scan delay-loaded imports (RVA-based on x64)
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
                auto it = shims.find(ToLowerAscii(dllName));
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

    // 3. Build packed ANSI string table and allocate within 32-bit RVA reach of ImageBase
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

    // Clear bound imports if present so ntdll never skips our redirected DLLs
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

    // 4. Patch each matched import descriptor's Name RVA
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

} // namespace sc::loader