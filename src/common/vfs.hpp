#pragma once

#include "common/config.hpp"

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace sc::vfs {

namespace detail {

inline std::wstring NormalizeForCompare(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    for (wchar_t& ch : s) {
        if (ch == L'/') {
            ch = L'\\';
        } else {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }
    while (s.size() > 3 && s.back() == L'\\') {
        s.pop_back();
    }
    return s;
}

inline bool IsPathInsideRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& normalizedRoot) {
    const std::wstring rootStr = NormalizeForCompare(normalizedRoot);
    const std::wstring candStr = NormalizeForCompare(candidate);
    if (rootStr.empty() || candStr.size() < rootStr.size()) {
        return false;
    }
    if (candStr.compare(0, rootStr.size(), rootStr) != 0) {
        return false;
    }
    if (candStr.size() == rootStr.size()) {
        return true;
    }
    return rootStr.back() == L'\\' || candStr[rootStr.size()] == L'\\';
}

inline void LogVfsWarn(const wchar_t* reason, const wchar_t* rawPath) {
    std::fwprintf(stderr, L"[WARN] [vfs] TranslatePath: %ls: %ls\n",
                  reason, rawPath ? rawPath : L"(null)");
    std::wstring dbg = L"[scarlett][vfs] TranslatePath: ";
    dbg += reason;
    dbg += L": ";
    dbg += (rawPath ? rawPath : L"(null)");
    dbg += L"\n";
    OutputDebugStringW(dbg.c_str());
}

} // namespace detail

inline std::filesystem::path ReadEnvPath(
    const wchar_t* envName,
    const std::filesystem::path& fallback) {
    if (auto val = sc::config::GetEnvPath(envName)) {
        return *val;
    }
    return fallback;
}

// Translates Xbox Series X virtual drive paths (G:\, T:\, S:\) onto isolated host folders:
//   G:\ -> SCARLETT_GAME_ROOT       (default: current_path)
//   T:\ -> SCARLETT_SCRATCH_DIR     (default: current_path/vfs/scratch)
//   S:\ -> SCARLETT_SAVE_TEMP_DIR   (default: current_path/vfs/save_temp)
// Unmapped host drives (e.g., C:\) pass through unchanged with *outWasVirtual = false.
// Sets *outWasVirtual to true whenever a virtual console prefix is matched.
// Rejects any path traversal outside the mapped root (returns empty path and logs a warning),
// strips trailing dots on path components, normalizes mixed '/' and '\', and supports >MAX_PATH.
inline std::filesystem::path TranslatePath(
    const wchar_t* rawPath,
    bool* outWasVirtual = nullptr) {
    if (outWasVirtual) {
        *outWasVirtual = false;
    }
    if (!rawPath || !*rawPath) {
        return {};
    }

    std::wstring input(rawPath);
    if (input.size() >= 2 && input[1] == L':') {
        const wchar_t drive = static_cast<wchar_t>(std::towupper(input[0]));
        std::filesystem::path root;
        bool matched = false;

        if (drive == L'G') {
            root = ReadEnvPath(L"SCARLETT_GAME_ROOT", std::filesystem::current_path());
            matched = true;
        } else if (drive == L'T') {
            root = ReadEnvPath(L"SCARLETT_SCRATCH_DIR",
                               std::filesystem::current_path() / L"vfs" / L"scratch");
            matched = true;
        } else if (drive == L'S') {
            root = ReadEnvPath(L"SCARLETT_SAVE_TEMP_DIR",
                               std::filesystem::current_path() / L"vfs" / L"save_temp");
            matched = true;
        }

        if (matched) {
            if (outWasVirtual) {
                *outWasVirtual = true;
            }

            std::error_code ec;
            std::filesystem::path absRoot = std::filesystem::absolute(root, ec);
            std::filesystem::path normRoot = (ec ? root : absRoot).lexically_normal();

            size_t pos = 2;
            while (pos < input.size() && (input[pos] == L'\\' || input[pos] == L'/')) {
                ++pos;
            }

            std::vector<std::wstring> components;
            while (pos < input.size()) {
                size_t next = input.find_first_of(L"\\/", pos);
                std::wstring seg = (next == std::wstring::npos)
                    ? input.substr(pos)
                    : input.substr(pos, next - pos);
                pos = (next == std::wstring::npos) ? input.size() : next + 1;

                if (seg.empty() || seg == L".") {
                    continue;
                }
                if (seg == L"..") {
                    if (components.empty()) {
                        detail::LogVfsWarn(L"rejected virtual path escape", rawPath);
                        return {};
                    }
                    components.pop_back();
                    continue;
                }
                if (seg.find(L':') != std::wstring::npos) {
                    detail::LogVfsWarn(L"rejected colon/drive token inside virtual path", rawPath);
                    return {};
                }

                // Strip Windows-ignored trailing dots from non-dot components.
                while (!seg.empty() && seg.back() == L'.') {
                    seg.pop_back();
                }
                if (seg.empty() || seg == L"." || seg == L"..") {
                    detail::LogVfsWarn(L"rejected dot-only path component", rawPath);
                    return {};
                }
                components.push_back(std::move(seg));
            }

            std::filesystem::path combined = normRoot;
            for (const auto& comp : components) {
                combined /= comp;
            }
            combined = combined.lexically_normal();

            if (!detail::IsPathInsideRoot(combined, normRoot)) {
                detail::LogVfsWarn(L"containment check failed", rawPath);
                return {};
            }
            return combined;
        }
    }

    return std::filesystem::path(input);
}

} // namespace sc::vfs
