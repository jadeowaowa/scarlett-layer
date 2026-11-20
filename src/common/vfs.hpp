#pragma once

#include <cwctype>
#include <filesystem>
#include <string>
#include <windows.h>

namespace sc::vfs {

inline std::filesystem::path ReadEnvPath(
    const wchar_t* envName,
    const std::filesystem::path& fallback) {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(envName, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(std::wstring(buf, len));
    }
    return fallback;
}

// Translates Xbox Series X virtual drive paths (G:\, T:\, S:\) onto isolated host folders.
// Sets *outWasVirtual to true whenever a virtual console prefix was rewritten.
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
            size_t pos = 2;
            while (pos < input.size() && (input[pos] == L'\\' || input[pos] == L'/')) {
                ++pos;
            }
            std::wstring rel = input.substr(pos);
            for (wchar_t& ch : rel) {
                if (ch == L'/') {
                    ch = L'\\';
                }
            }
            std::filesystem::path combined = rel.empty()
                ? root
                : (root / std::filesystem::path(rel));
            return combined.lexically_normal();
        }
    }

    return std::filesystem::path(input);
}

} // namespace sc::vfs