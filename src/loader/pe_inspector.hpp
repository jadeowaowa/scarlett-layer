#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace sc::loader {

struct PeImportAuditResult {
    bool                     validPe64{false};
    std::string              error;
    std::vector<std::string> shimmedImports;
    std::vector<std::string> systemImports;
    std::vector<std::string> missingImports;
    size_t                   totalImportedSymbols{0};
};

namespace detail {

inline std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

inline bool IsHostSystemDll(const std::string& lowerName) {
    if (lowerName.rfind("api-ms-win-", 0) == 0 ||
        lowerName.rfind("ext-ms-win-", 0) == 0) {
        return true;
    }
    static const std::unordered_set<std::string> kHostDlls = {
        "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
        "ole32.dll", "oleaut32.dll", "ntdll.dll", "ws2_32.dll", "bcrypt.dll",
        "crypt32.dll", "winmm.dll", "d3d12.dll", "dxgi.dll", "d3dcompiler_47.dll",
        "xinput1_4.dll", "xinput9_1_0.dll", "ucrtbase.dll", "ucrtbased.dll",
        "vcruntime140.dll", "vcruntime140d.dll", "vcruntime140_1.dll", "vcruntime140_1d.dll",
        "msvcp140.dll", "msvcp140d.dll", "concrt140.dll", "concrt140d.dll", "dbghelp.dll",
        "shlwapi.dll", "version.dll", "secur32.dll", "iphlpapi.dll", "rpcrt4.dll",
        "combase.dll", "msvcrt.dll"
    };
    return kHostDlls.contains(lowerName);
}

inline bool RvaToFileOffset(
    uint32_t rva,
    const IMAGE_SECTION_HEADER* sections,
    uint16_t numSections,
    size_t fileSize,
    size_t& outOffset) {
    for (uint16_t i = 0; i < numSections; ++i) {
        const uint32_t va = sections[i].VirtualAddress;
        const uint32_t span = (std::max)(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
        if (rva >= va && rva < va + span) {
            const size_t off = static_cast<size_t>(sections[i].PointerToRawData) + (rva - va);
            if (off < fileSize) {
                outOffset = off;
                return true;
            }
        }
    }
    return false;
}

} // namespace detail

inline PeImportAuditResult InspectPeImports(
    const std::filesystem::path& pePath,
    const std::filesystem::path& shimDir) {
    PeImportAuditResult res{};

    std::ifstream f(pePath, std::ios::binary | std::ios::ate);
    if (!f) {
        res.error = "Failed to open PE file: " + pePath.string();
        return res;
    }
    const auto fileSize = static_cast<size_t>(f.tellg());
    if (fileSize < sizeof(IMAGE_DOS_HEADER)) {
        res.error = "File too small to be a PE image";
        return res;
    }
    std::vector<uint8_t> buf(fileSize);
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fileSize));

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > fileSize) {
        res.error = "Invalid DOS/NT PE64 header";
        return res;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        res.error = "Not a valid PE32+ (x64) executable";
        return res;
    }

    res.validPe64 = true;

    // Index available shims in shimDir (case-insensitive)
    std::unordered_set<std::string> shims;
    std::error_code ec;
    if (std::filesystem::exists(shimDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(shimDir, ec)) {
            if (entry.is_regular_file() &&
                detail::ToLowerAscii(entry.path().extension().string()) == ".dll") {
                shims.insert(detail::ToLowerAscii(entry.path().filename().string()));
            }
        }
    }

    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0 || impDir.Size == 0) {
        return res;
    }

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const uint16_t numSections = nt->FileHeader.NumberOfSections;

    size_t descOff = 0;
    if (!detail::RvaToFileOffset(impDir.VirtualAddress, sections, numSections, fileSize, descOff)) {
        res.error = "Import directory RVA out of bounds";
        return res;
    }

    while (descOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= fileSize) {
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(buf.data() + descOff);
        if (desc->Name == 0 && desc->FirstThunk == 0) {
            break;
        }

        size_t nameOff = 0;
        if (detail::RvaToFileOffset(desc->Name, sections, numSections, fileSize, nameOff)) {
            const char* rawName = reinterpret_cast<const char*>(buf.data() + nameOff);
            const size_t maxLen = fileSize - nameOff;
            const size_t len = strnlen(rawName, maxLen);
            std::string dllName(rawName, len);
            std::string lower = detail::ToLowerAscii(dllName);

            if (shims.contains(lower)) {
                res.shimmedImports.push_back(dllName);
            } else if (detail::IsHostSystemDll(lower)) {
                res.systemImports.push_back(dllName);
            } else {
                res.missingImports.push_back(dllName);
            }
        }

        const uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        size_t thunkOff = 0;
        if (thunkRva != 0 && detail::RvaToFileOffset(thunkRva, sections, numSections, fileSize, thunkOff)) {
            while (thunkOff + sizeof(uint64_t) <= fileSize) {
                uint64_t val = 0;
                std::memcpy(&val, buf.data() + thunkOff, sizeof(val));
                if (val == 0) break;
                ++res.totalImportedSymbols;
                thunkOff += sizeof(uint64_t);
            }
        }

        descOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    return res;
}

} // namespace sc::loader