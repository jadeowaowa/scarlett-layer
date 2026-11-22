#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "loader/pe_inspector.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace sc::gui {

enum class EntryKind {
    GdkPackage,
    Executable,
    XvdContainer
};

struct LibraryEntry {
    EntryKind             kind{EntryKind::Executable};
    std::wstring          displayName;
    std::filesystem::path targetPath;     // Package dir for GdkPackage, file path otherwise
    std::filesystem::path executablePath; // Resolved .exe path (empty for XVD)
    std::string           identityName;
    std::string           titleId;
    std::string           coverageText;
    std::string           statusText;
    std::string           detailReport;
    bool                  launchable{false};
};

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string ExtractXmlAttr(const std::string& xml, const std::string& tag, const std::string& attr) {
    const std::string open = "<" + tag;
    size_t searchPos = 0;
    size_t tpos = std::string::npos;
    while ((searchPos = xml.find(open, searchPos)) != std::string::npos) {
        const size_t after = searchPos + open.size();
        if (after < xml.size() && (xml[after] == ' ' || xml[after] == '	' || xml[after] == '' || xml[after] == '
' || xml[after] == '>')) {
            tpos = searchPos;
            break;
        }
        searchPos = after;
    }
    if (tpos == std::string::npos) return {};
    const size_t tend = xml.find('>', tpos);
    if (tend == std::string::npos) return {};
    const std::string slice = xml.substr(tpos, tend - tpos);
    const std::string key = attr + "=\"";
    const size_t kpos = slice.find(key);
    if (kpos == std::string::npos) return {};
    const size_t vstart = kpos + key.size();
    const size_t vend = slice.find('"', vstart);
    if (vend == std::string::npos) return {};
    return slice.substr(vstart, vend - vstart);
}

inline std::string ExtractXmlElem(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos) return {};
    return xml.substr(a + open.size(), b - (a + open.size()));
}

inline void PopulatePeAudit(LibraryEntry& entry, const std::filesystem::path& exePath, const std::filesystem::path& shimDir) {
    std::error_code ec;
    if (!std::filesystem::exists(exePath, ec)) {
        entry.coverageText = "N/A";
        entry.statusText = "Exe Missing";
        entry.launchable = false;
        entry.detailReport += "Executable not found: " + exePath.string() + "\r\n";
        return;
    }

    const auto cov = sc::loader::InspectPeSymbolCoverage(exePath, shimDir);
    if (!cov.audit.validPe64) {
        entry.coverageText = "Invalid PE";
        entry.statusText = "Not PE64";
        entry.launchable = false;
        entry.detailReport += "PE64 Audit Error: " + cov.audit.error + "\r\n";
        return;
    }

    const size_t implN = cov.implementedSymbols.size();
    const size_t stubN = cov.autoStubSymbols.size();
    const size_t totalShimmed = implN + stubN;
    const double pct = (totalShimmed > 0) ? (100.0 * static_cast<double>(implN) / static_cast<double>(totalShimmed)) : 100.0;

    char covBuf[64];
    if (totalShimmed > 0) {
        std::snprintf(covBuf, sizeof(covBuf), "%zu/%zu (%.0f%%)", implN, totalShimmed, pct);
    } else {
        std::snprintf(covBuf, sizeof(covBuf), "Dynamic / 0 static");
    }
    entry.coverageText = covBuf;

    if (!cov.audit.missingImports.empty()) {
        entry.statusText = "Missing " + std::to_string(cov.audit.missingImports.size()) + " DLL(s)";
    } else if (stubN > 0) {
        entry.statusText = "Ready (Auto-Stub)";
    } else {
        entry.statusText = "Ready";
    }
    entry.launchable = true;

    std::ostringstream oss;
    oss << entry.detailReport;
    oss << "PE64 Imports   : " << cov.audit.totalImportedSymbols << " symbol(s), "
        << cov.audit.ordinalImportCount << " ordinal(s)\r\n";
    oss << "TLS Directory  : " << (cov.audit.hasTlsDirectory ? "Present" : "None")
        << " (raw=" << cov.audit.tlsRawDataSize << " B)\r\n";
    oss << "Shim Coverage  : " << implN << "/" << totalShimmed << " implemented, "
        << stubN << " auto-stubbed\r\n";

    oss << "Shimmed DLLs (" << cov.audit.shimmedImports.size() << "): ";
    if (cov.audit.shimmedImports.empty()) oss << "(none statically; dynamic hooks armed)";
    for (size_t i = 0; i < cov.audit.shimmedImports.size(); ++i) {
        if (i) oss << ", ";
        oss << cov.audit.shimmedImports[i];
    }
    oss << "\r\n";

    if (!cov.audit.missingImports.empty()) {
        oss << "Missing DLLs (" << cov.audit.missingImports.size() << "): ";
        for (size_t i = 0; i < cov.audit.missingImports.size(); ++i) {
            if (i) oss << ", ";
            oss << cov.audit.missingImports[i];
        }
        oss << "\r\n";
    }

    if (!cov.autoStubSymbols.empty()) {
        oss << "\r\nAuto-Stubbed Symbols:\r\n";
        for (const auto& s : cov.autoStubSymbols) {
            oss << "  - " << s << "\r\n";
        }
    }
    entry.detailReport = oss.str();
}

inline LibraryEntry InspectPackageFile(const std::filesystem::path& cfgPath, const std::filesystem::path& shimDir) {
    LibraryEntry e{};
    e.kind = EntryKind::GdkPackage;
    e.targetPath = cfgPath.parent_path();

    std::ifstream f(cfgPath, std::ios::binary);
    std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    e.identityName = ExtractXmlAttr(xml, "Identity", "Name");
    e.titleId = ExtractXmlElem(xml, "TitleId");
    std::string disp = ExtractXmlAttr(xml, "ShellVisuals", "DefaultDisplayName");
    std::string exeName = ExtractXmlAttr(xml, "Executable", "Name");

    if (disp.empty()) disp = e.identityName.empty() ? e.targetPath.filename().string() : e.identityName;
    e.displayName = Utf8ToWide(disp);
    e.executablePath = exeName.empty() ? std::filesystem::path{} : (e.targetPath / Utf8ToWide(exeName));

    std::ostringstream oss;
    oss << "Type           : Xbox GDK Package (MicrosoftGame.Config)\r\n";
    oss << "Package Root   : " << e.targetPath.string() << "\r\n";
    oss << "Identity       : " << (e.identityName.empty() ? "(unspecified)" : e.identityName) << "\r\n";
    oss << "TitleId        : " << (e.titleId.empty() ? "(unspecified)" : e.titleId) << "\r\n";
    oss << "Executable     : " << e.executablePath.string() << "\r\n";
    e.detailReport = oss.str();

    PopulatePeAudit(e, e.executablePath, shimDir);
    return e;
}

inline LibraryEntry InspectExecutableFile(const std::filesystem::path& exePath, const std::filesystem::path& shimDir) {
    LibraryEntry e{};
    e.kind = EntryKind::Executable;
    e.displayName = exePath.stem().wstring();
    e.targetPath = exePath;
    e.executablePath = exePath;

    std::ostringstream oss;
    oss << "Type           : Standalone Executable\r\n";
    oss << "Path           : " << exePath.string() << "\r\n";
    e.detailReport = oss.str();

    PopulatePeAudit(e, exePath, shimDir);
    return e;
}

inline LibraryEntry InspectXvdFileReadOnly(const std::filesystem::path& xvdPath) {
    LibraryEntry e{};
    e.kind = EntryKind::XvdContainer;
    e.displayName = xvdPath.filename().wstring();
    e.targetPath = xvdPath;
    e.launchable = false;

    std::ifstream f(xvdPath, std::ios::binary);
    std::vector<uint8_t> hdr(0x220, 0);
    if (!f || !f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()))) {
        e.coverageText = "N/A";
        e.statusText = "Unreadable";
        e.detailReport = "Read-only XVD/XVC audit failed: file smaller than 0x220 bytes.\r\n";
        return e;
    }

    const bool magicOk = (std::memcmp(hdr.data() + 0x200, "msft-xvd", 8) == 0);
    uint32_t volumeFlags = 0;
    uint32_t formatVer = 0;
    uint64_t driveSize = 0;
    std::memcpy(&volumeFlags, hdr.data() + 0x208, sizeof(volumeFlags));
    std::memcpy(&formatVer, hdr.data() + 0x20C, sizeof(formatVer));
    std::memcpy(&driveSize, hdr.data() + 0x218, sizeof(driveSize));

    // Per emoose/xvdtool: EncryptionDisabled is bit 0x2; if cleared, image is encrypted.
    const bool encrypted = ((volumeFlags & 0x2u) == 0u);
    e.coverageText = magicOk ? (encrypted ? "Encrypted" : "Unencrypted") : "Unknown";
    e.statusText = !magicOk ? "Invalid Magic" : (encrypted ? "Encrypted (Rejected)" : "Metadata OK");

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Type           : XVD/XVC Container (Read-Only Header Audit)\r\n"
                  "Path           : %s\r\n"
                  "Magic @0x200   : %s\r\n"
                  "VolumeFlags    : 0x%08X (EncryptionDisabled=%s)\r\n"
                  "FormatVersion  : %u\r\n"
                  "DriveSize      : %llu bytes\r\n"
                  "Policy         : Read-only header inspection; encrypted payloads are strictly rejected.\r\n",
                  xvdPath.string().c_str(),
                  magicOk ? "msft-xvd (valid)" : "INVALID",
                  volumeFlags,
                  encrypted ? "NO -> ENCRYPTED" : "YES -> unencrypted",
                  formatVer,
                  static_cast<unsigned long long>(driveSize));
    e.detailReport = buf;
    return e;
}

inline std::vector<LibraryEntry> ScanLibraryRoots(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& shimDir) {
    std::vector<LibraryEntry> results;
    std::set<std::wstring> claimedExes;
    std::set<std::wstring> seenTargets;

    auto normKey = [](const std::filesystem::path& p) {
        std::wstring s = p.lexically_normal().wstring();
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };

    // Pass 1: discover MicrosoftGame.Config packages first so their executables aren't duplicated
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        if (std::filesystem::is_regular_file(root, ec)) {
            const std::string ext = ToLowerAscii(root.extension().string());
            const std::string fname = ToLowerAscii(root.filename().string());
            if (fname == "microsoftgame.config" || ext == ".mgc") {
                auto entry = InspectPackageFile(root, shimDir);
                if (seenTargets.insert(normKey(entry.targetPath)).second) {
                    if (!entry.executablePath.empty()) claimedExes.insert(normKey(entry.executablePath));
                    results.push_back(std::move(entry));
                }
            }
            continue;
        }
        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (const auto& dirEntry : it) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (!dirEntry.is_regular_file(ec)) continue;
            const std::string fname = ToLowerAscii(dirEntry.path().filename().string());
            if (fname == "microsoftgame.config") {
                auto entry = InspectPackageFile(dirEntry.path(), shimDir);
                if (seenTargets.insert(normKey(entry.targetPath)).second) {
                    if (!entry.executablePath.empty()) claimedExes.insert(normKey(entry.executablePath));
                    results.push_back(std::move(entry));
                }
            }
        }
    }

    // Pass 2: standalone .exe titles and .xvd/.xvc containers
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;

        auto considerFile = [&](const std::filesystem::path& p) {
            const std::string ext = ToLowerAscii(p.extension().string());
            const std::string fname = ToLowerAscii(p.filename().string());
            if (ext == ".exe") {
                if (fname == "loader.exe" || fname == "scarlett_studio.exe") return;
                if (claimedExes.contains(normKey(p))) return;
                if (seenTargets.insert(normKey(p)).second) {
                    results.push_back(InspectExecutableFile(p, shimDir));
                }
            } else if (ext == ".xvd" || ext == ".xvc") {
                if (seenTargets.insert(normKey(p)).second) {
                    results.push_back(InspectXvdFileReadOnly(p));
                }
            }
        };

        if (std::filesystem::is_regular_file(root, ec)) {
            considerFile(root);
            continue;
        }
        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (const auto& dirEntry : it) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (!dirEntry.is_regular_file(ec)) continue;
            considerFile(dirEntry.path());
        }
    }

    std::sort(results.begin(), results.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        return a.displayName < b.displayName;
    });
    return results;
}

inline std::vector<std::filesystem::path> LoadRootsConfig(const std::filesystem::path& cfgFile) {
    std::vector<std::filesystem::path> roots;
    std::ifstream f(cfgFile);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) roots.emplace_back(Utf8ToWide(line));
    }
    return roots;
}

inline void SaveRootsConfig(const std::filesystem::path& cfgFile, const std::vector<std::filesystem::path>& roots) {
    std::ofstream f(cfgFile, std::ios::trunc);
    for (const auto& r : roots) {
        f << WideToUtf8(r.wstring()) << "\n";
    }
}

} // namespace sc::gui
