#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sc::loader {

struct PeImportAuditResult {
    bool                     validPe64{false};
    std::string              error;
    std::vector<std::string> shimmedImports;
    std::vector<std::string> systemImports;
    std::vector<std::string> missingImports;
    std::vector<std::string> delayLoadImports;
    size_t                   totalImportedSymbols{0};
    size_t                   ordinalImportCount{0};
    bool                     hasTlsDirectory{false};
    size_t                   tlsRawDataSize{0};
    size_t                   tlsZeroFillSize{0};
    size_t                   tlsCallbackCount{0};
    uint32_t                 tlsCharacteristics{0};
};

namespace detail {

inline constexpr size_t kMaxPeInspectBytes     = 512u * 1024u * 1024u; // 512 MiB
inline constexpr size_t kMaxImportDescriptors  = 4096u;
inline constexpr size_t kMaxThunksPerDll       = 65536u;
inline constexpr size_t kMaxExportEntries      = 65536u;

struct ValidatedPe64 {
    const IMAGE_NT_HEADERS64*   nt{nullptr};
    const IMAGE_SECTION_HEADER* sections{nullptr};
    uint16_t                    numSections{0};
    uint32_t                    numDataDirs{0};
};

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

inline bool ReadFileBytesSafe(
    const std::filesystem::path& path,
    std::vector<uint8_t>& outBuf,
    std::string& outError) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        outError = "Failed to open PE file: " + path.string();
        return false;
    }
    const std::streampos rawSize = f.tellg();
    if (rawSize < 0) {
        outError = "Failed to query PE file size";
        return false;
    }
    const auto fileSize = static_cast<uint64_t>(rawSize);
    if (fileSize < sizeof(IMAGE_DOS_HEADER)) {
        outError = "File too small to be a PE image";
        return false;
    }
    if (fileSize > kMaxPeInspectBytes) {
        outError = "PE file exceeds inspection size limit";
        return false;
    }

    outBuf.resize(static_cast<size_t>(fileSize));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(outBuf.data()), static_cast<std::streamsize>(outBuf.size()));
    if (!f) {
        outBuf.clear();
        outError = "Failed to read PE file bytes";
        return false;
    }
    return true;
}

inline bool ParseValidatedPe64Headers(
    const std::vector<uint8_t>& buf,
    ValidatedPe64& out,
    std::string& outError) {
    out = {};
    const size_t fileSize = buf.size();
    if (fileSize < sizeof(IMAGE_DOS_HEADER)) {
        outError = "File too small to be a PE image";
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        outError = "Invalid DOS/NT PE64 header";
        return false;
    }
    const auto ntOff = static_cast<size_t>(dos->e_lfanew);
    if (ntOff > fileSize || fileSize - ntOff < sizeof(IMAGE_NT_HEADERS64)) {
        outError = "Invalid DOS/NT PE64 header";
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf.data() + ntOff);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = "Not a valid PE32+ (x64) executable";
        return false;
    }

    const size_t optHdrOffset = offsetof(IMAGE_NT_HEADERS64, OptionalHeader);
    const size_t optHdrSize = nt->FileHeader.SizeOfOptionalHeader;
    if (optHdrSize < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        optHdrSize > fileSize - ntOff - optHdrOffset) {
        outError = "Invalid PE64 OptionalHeader size";
        return false;
    }

    const size_t secTableOff = ntOff + optHdrOffset + optHdrSize;
    const uint16_t numSections = nt->FileHeader.NumberOfSections;
    const size_t secTableBytes = static_cast<size_t>(numSections) * sizeof(IMAGE_SECTION_HEADER);
    if (secTableOff > fileSize || secTableBytes > fileSize - secTableOff) {
        outError = "PE64 section table exceeds file size";
        return false;
    }

    out.nt = nt;
    out.sections = (numSections > 0u)
        ? reinterpret_cast<const IMAGE_SECTION_HEADER*>(buf.data() + secTableOff)
        : nullptr;
    out.numSections = numSections;
    out.numDataDirs = (std::min)(
        static_cast<uint32_t>(nt->OptionalHeader.NumberOfRvaAndSizes),
        static_cast<uint32_t>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES));
    return true;
}

inline IMAGE_DATA_DIRECTORY GetDataDir(const ValidatedPe64& pe, uint32_t index) {
    if (!pe.nt || index >= pe.numDataDirs) {
        return {};
    }
    return pe.nt->OptionalHeader.DataDirectory[index];
}

inline bool RvaToFileOffset(
    uint32_t rva,
    const IMAGE_SECTION_HEADER* sections,
    uint16_t numSections,
    size_t fileSize,
    size_t& outOffset) {
    if (!sections) {
        return false;
    }
    const uint64_t rva64 = rva;
    for (uint16_t i = 0; i < numSections; ++i) {
        const uint64_t va = sections[i].VirtualAddress;
        const uint64_t rawSize = sections[i].SizeOfRawData;
        const uint64_t rawPtr = sections[i].PointerToRawData;
        if (rawSize == 0u || rva64 < va) {
            continue;
        }
        const uint64_t delta = rva64 - va;
        if (delta < rawSize && rawPtr <= fileSize && delta < fileSize - rawPtr) {
            outOffset = static_cast<size_t>(rawPtr + delta);
            return true;
        }
    }
    return false;
}

} // namespace detail

inline PeImportAuditResult InspectPeImports(
    const std::filesystem::path& pePath,
    const std::filesystem::path& shimDir) {
    PeImportAuditResult res{};

    std::vector<uint8_t> buf;
    if (!detail::ReadFileBytesSafe(pePath, buf, res.error)) {
        return res;
    }
    const size_t fileSize = buf.size();

    detail::ValidatedPe64 pe{};
    if (!detail::ParseValidatedPe64Headers(buf, pe, res.error)) {
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

    const IMAGE_SECTION_HEADER* sections = pe.sections;
    const uint16_t numSections = pe.numSections;

    auto classifyDll = [&](uint32_t nameRva, bool isDelayLoad) {
        size_t nameOff = 0;
        if (!detail::RvaToFileOffset(nameRva, sections, numSections, fileSize, nameOff)) {
            return;
        }
        const char* rawName = reinterpret_cast<const char*>(buf.data() + nameOff);
        const size_t maxLen = fileSize - nameOff;
        const size_t len = strnlen(rawName, maxLen);
        if (len == 0u || len >= maxLen) {
            return;
        }
        std::string dllName(rawName, len);
        std::string lower = detail::ToLowerAscii(dllName);

        if (isDelayLoad) {
            res.delayLoadImports.push_back(dllName);
        }
        if (shims.contains(lower)) {
            res.shimmedImports.push_back(dllName);
        } else if (detail::IsHostSystemDll(lower)) {
            res.systemImports.push_back(dllName);
        } else {
            res.missingImports.push_back(dllName);
        }
    };

    auto countThunks = [&](uint32_t thunkRva) {
        size_t thunkOff = 0;
        if (thunkRva == 0 || !detail::RvaToFileOffset(thunkRva, sections, numSections, fileSize, thunkOff)) {
            return;
        }
        size_t seen = 0;
        while (seen < detail::kMaxThunksPerDll && thunkOff + sizeof(uint64_t) <= fileSize) {
            uint64_t val = 0;
            std::memcpy(&val, buf.data() + thunkOff, sizeof(val));
            if (val == 0) break;
            ++res.totalImportedSymbols;
            if ((val & IMAGE_ORDINAL_FLAG64) != 0) {
                ++res.ordinalImportCount;
            }
            thunkOff += sizeof(uint64_t);
            ++seen;
        }
    };

    // 1. Standard IAT (IMAGE_DIRECTORY_ENTRY_IMPORT)
    const auto impDir = detail::GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (impDir.VirtualAddress != 0 && impDir.Size != 0) {
        size_t descOff = 0;
        if (!detail::RvaToFileOffset(impDir.VirtualAddress, sections, numSections, fileSize, descOff)) {
            res.error = "Import directory RVA out of bounds";
            return res;
        }
        size_t descCount = 0;
        while (descCount < detail::kMaxImportDescriptors &&
               descOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= fileSize) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(buf.data() + descOff);
            if (desc->Name == 0 && desc->FirstThunk == 0) {
                break;
            }
            classifyDll(desc->Name, false);
            const uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            countThunks(thunkRva);
            descOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            ++descCount;
        }
    }

    // 2. Delay-Load Imports (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
    const auto delayDir = detail::GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        size_t delayOff = 0;
        if (detail::RvaToFileOffset(delayDir.VirtualAddress, sections, numSections, fileSize, delayOff)) {
            size_t descCount = 0;
            while (descCount < detail::kMaxImportDescriptors &&
                   delayOff + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= fileSize) {
                const auto* ddesc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(buf.data() + delayOff);
                if (ddesc->DllNameRVA == 0 && ddesc->ImportAddressTableRVA == 0) {
                    break;
                }
                classifyDll(ddesc->DllNameRVA, true);
                const uint32_t thunkRva = ddesc->ImportNameTableRVA ? ddesc->ImportNameTableRVA : ddesc->ImportAddressTableRVA;
                countThunks(thunkRva);
                delayOff += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
                ++descCount;
            }
        }
    }

    // 3. Implicit TLS Directory (IMAGE_DIRECTORY_ENTRY_TLS)
    const auto tlsDir = detail::GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_TLS);
    if (tlsDir.VirtualAddress != 0 && tlsDir.Size != 0) {
        size_t tlsOff = 0;
        if (detail::RvaToFileOffset(tlsDir.VirtualAddress, sections, numSections, fileSize, tlsOff) &&
            tlsOff + sizeof(IMAGE_TLS_DIRECTORY64) <= fileSize) {
            const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(buf.data() + tlsOff);
            res.hasTlsDirectory = true;
            res.tlsCharacteristics = tls->Characteristics;
            res.tlsZeroFillSize = tls->SizeOfZeroFill;
            if (tls->EndAddressOfRawData >= tls->StartAddressOfRawData) {
                res.tlsRawDataSize = static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
            }
            const uint64_t imageBase = pe.nt->OptionalHeader.ImageBase;
            if (tls->AddressOfCallBacks >= imageBase) {
                const uint64_t cbRva64 = tls->AddressOfCallBacks - imageBase;
                if (cbRva64 <= 0xFFFFFFFFULL) {
                    size_t cbOff = 0;
                    if (detail::RvaToFileOffset(static_cast<uint32_t>(cbRva64), sections, numSections, fileSize, cbOff)) {
                        constexpr size_t kMaxTlsCallbacks = 256;
                        while (res.tlsCallbackCount < kMaxTlsCallbacks && cbOff + sizeof(uint64_t) <= fileSize) {
                            uint64_t cbVa = 0;
                            std::memcpy(&cbVa, buf.data() + cbOff, sizeof(uint64_t));
                            if (cbVa == 0) {
                                break;
                            }
                            ++res.tlsCallbackCount;
                            cbOff += sizeof(uint64_t);
                        }
                    }
                }
            }
        }
    }

    return res;
}

struct ShimExportTable {
    std::unordered_set<std::string> names;
    std::unordered_set<uint16_t>    ordinals;
};

struct PeSymbolCoverageResult {
    PeImportAuditResult      audit;
    std::vector<std::string> implementedSymbols;
    std::vector<std::string> autoStubSymbols;
};

namespace detail {

inline ShimExportTable ParsePeExports(const std::filesystem::path& dllPath) {
    ShimExportTable out{};
    std::vector<uint8_t> buf;
    std::string ignoredErr;
    if (!ReadFileBytesSafe(dllPath, buf, ignoredErr)) {
        return out;
    }
    const size_t fileSize = buf.size();

    ValidatedPe64 pe{};
    if (!ParseValidatedPe64Headers(buf, pe, ignoredErr)) {
        return out;
    }

    const auto expDir = GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (expDir.VirtualAddress == 0 || expDir.Size == 0) return out;

    size_t expOff = 0;
    if (!RvaToFileOffset(expDir.VirtualAddress, pe.sections, pe.numSections, fileSize, expOff) ||
        expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > fileSize) {
        return out;
    }
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(buf.data() + expOff);

    size_t funcsOff = 0;
    if (exp->AddressOfFunctions != 0 &&
        RvaToFileOffset(exp->AddressOfFunctions, pe.sections, pe.numSections, fileSize, funcsOff)) {
        const size_t funcCount = (std::min)(static_cast<size_t>(exp->NumberOfFunctions), kMaxExportEntries);
        for (size_t i = 0; i < funcCount; ++i) {
            const size_t off = funcsOff + i * sizeof(uint32_t);
            if (off + sizeof(uint32_t) > fileSize) break;
            uint32_t fnRva = 0;
            std::memcpy(&fnRva, buf.data() + off, sizeof(uint32_t));
            if (fnRva != 0) {
                out.ordinals.insert(static_cast<uint16_t>(exp->Base + i));
            }
        }
    }

    size_t namesOff = 0;
    if (exp->AddressOfNames != 0 &&
        RvaToFileOffset(exp->AddressOfNames, pe.sections, pe.numSections, fileSize, namesOff)) {
        const size_t nameCount = (std::min)(static_cast<size_t>(exp->NumberOfNames), kMaxExportEntries);
        for (size_t i = 0; i < nameCount; ++i) {
            const size_t off = namesOff + i * sizeof(uint32_t);
            if (off + sizeof(uint32_t) > fileSize) break;
            uint32_t nameRva = 0;
            std::memcpy(&nameRva, buf.data() + off, sizeof(uint32_t));
            size_t strOff = 0;
            if (RvaToFileOffset(nameRva, pe.sections, pe.numSections, fileSize, strOff)) {
                const char* s = reinterpret_cast<const char*>(buf.data() + strOff);
                const size_t maxLen = fileSize - strOff;
                const size_t slen = ::strnlen(s, maxLen);
                if (slen > 0 && slen < maxLen) {
                    out.names.emplace(s, slen);
                }
            }
        }
    }
    return out;
}

} // namespace detail

inline PeSymbolCoverageResult InspectPeSymbolCoverage(
    const std::filesystem::path& pePath,
    const std::filesystem::path& shimDir) {
    PeSymbolCoverageResult out{};
    out.audit = InspectPeImports(pePath, shimDir);
    if (!out.audit.validPe64) {
        return out;
    }

    // Build map of lowercased shim filename -> exported symbol/ordinal sets
    std::unordered_map<std::string, ShimExportTable> shimExports;
    std::error_code ec;
    if (std::filesystem::exists(shimDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(shimDir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string lowerName = detail::ToLowerAscii(entry.path().filename().string());
            if (lowerName.size() > 4 && lowerName.substr(lowerName.size() - 4) == ".dll") {
                shimExports.emplace(lowerName, detail::ParsePeExports(entry.path()));
            }
        }
    }

    std::vector<uint8_t> buf;
    std::string ignoredErr;
    if (!detail::ReadFileBytesSafe(pePath, buf, ignoredErr)) {
        return out;
    }
    const size_t fileSize = buf.size();

    detail::ValidatedPe64 pe{};
    if (!detail::ParseValidatedPe64Headers(buf, pe, ignoredErr)) {
        return out;
    }

    const IMAGE_SECTION_HEADER* sections = pe.sections;
    const uint16_t numSections = pe.numSections;

    auto inspectThunksForDll = [&](uint32_t nameRva, uint32_t thunkRva) {
        size_t nameOff = 0;
        if (!detail::RvaToFileOffset(nameRva, sections, numSections, fileSize, nameOff)) return;
        const char* rawDll = reinterpret_cast<const char*>(buf.data() + nameOff);
        const size_t maxDll = fileSize - nameOff;
        const size_t dllLen = ::strnlen(rawDll, maxDll);
        if (dllLen == 0 || dllLen >= maxDll) return;
        const std::string dllLower = detail::ToLowerAscii(std::string(rawDll, dllLen));

        auto sit = shimExports.find(dllLower);
        if (sit == shimExports.end()) {
            return; // Only classify symbol-level coverage for shimmed DLLs
        }
        const ShimExportTable& exports = sit->second;

        size_t tOff = 0;
        if (!detail::RvaToFileOffset(thunkRva, sections, numSections, fileSize, tOff)) return;
        size_t seen = 0;
        while (seen < detail::kMaxThunksPerDll && tOff + sizeof(uint64_t) <= fileSize) {
            uint64_t val = 0;
            std::memcpy(&val, buf.data() + tOff, sizeof(uint64_t));
            if (val == 0) break;
            if ((val & IMAGE_ORDINAL_FLAG64) != 0) {
                const uint16_t ord = static_cast<uint16_t>(val & 0xFFFFu);
                const std::string label = dllLower + "!#" + std::to_string(ord);
                if (exports.ordinals.contains(ord)) {
                    out.implementedSymbols.push_back(label);
                } else {
                    out.autoStubSymbols.push_back(label);
                }
            } else {
                const uint32_t ibnRva = static_cast<uint32_t>(val & 0xFFFFFFFFULL);
                size_t ibnOff = 0;
                if (detail::RvaToFileOffset(ibnRva, sections, numSections, fileSize, ibnOff) &&
                    ibnOff + 2 < fileSize) {
                    const char* sym = reinterpret_cast<const char*>(buf.data() + ibnOff + 2);
                    const size_t maxSym = fileSize - (ibnOff + 2);
                    const size_t symLen = ::strnlen(sym, maxSym);
                    if (symLen > 0 && symLen < maxSym) {
                        const std::string symStr(sym, symLen);
                        const std::string label = dllLower + "!" + symStr;
                        if (exports.names.contains(symStr)) {
                            out.implementedSymbols.push_back(label);
                        } else {
                            out.autoStubSymbols.push_back(label);
                        }
                    }
                }
            }
            tOff += sizeof(uint64_t);
            ++seen;
        }
    };

    const auto impDir = detail::GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (impDir.VirtualAddress != 0 && impDir.Size != 0) {
        size_t descOff = 0;
        if (detail::RvaToFileOffset(impDir.VirtualAddress, sections, numSections, fileSize, descOff)) {
            size_t descCount = 0;
            while (descCount < detail::kMaxImportDescriptors &&
                   descOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= fileSize) {
                const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(buf.data() + descOff);
                if (desc->Name == 0 && desc->FirstThunk == 0) break;
                const uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                inspectThunksForDll(desc->Name, thunkRva);
                descOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
                ++descCount;
            }
        }
    }

    const auto delayDir = detail::GetDataDir(pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        size_t delayOff = 0;
        if (detail::RvaToFileOffset(delayDir.VirtualAddress, sections, numSections, fileSize, delayOff)) {
            size_t descCount = 0;
            while (descCount < detail::kMaxImportDescriptors &&
                   delayOff + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= fileSize) {
                const auto* ddesc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(buf.data() + delayOff);
                if (ddesc->DllNameRVA == 0 && ddesc->ImportAddressTableRVA == 0) break;
                const uint32_t thunkRva = ddesc->ImportNameTableRVA ? ddesc->ImportNameTableRVA : ddesc->ImportAddressTableRVA;
                inspectThunksForDll(ddesc->DllNameRVA, thunkRva);
                delayOff += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
                ++descCount;
            }
        }
    }

    return out;
}

} // namespace sc::loader
