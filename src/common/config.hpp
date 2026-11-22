#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace sc::config {

inline bool IsFlagEnabled(const char* envName) {
    char buf[8]{};
    DWORD len = GetEnvironmentVariableA(envName, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '1';
}

inline bool IsHeadless() {
    return IsFlagEnabled("SCARLETT_HEADLESS");
}

inline bool IsStrictD3D12() {
    return IsFlagEnabled("SCARLETT_STRICT_D3D12");
}

inline bool UseWarp() {
    return IsFlagEnabled("SCARLETT_USE_WARP");
}

inline std::optional<std::filesystem::path> GetShimDir() {
    DWORD len = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", nullptr, 0);
    if (len <= 1) {
        return std::nullopt;
    }
    std::wstring buf(len, L'\0');
    DWORD written = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", buf.data(), len);
    if (written == 0 || written >= len) {
        return std::nullopt;
    }
    buf.resize(written);
    return std::filesystem::path(std::move(buf));
}

inline bool IsAutoStubEnabled() {
    char buf[8]{};
    DWORD len = GetEnvironmentVariableA("SCARLETT_AUTO_STUB", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0) {
        return true; // Default ON for shimmed DLLs; set SCARLETT_AUTO_STUB=0 to disable
    }
    return buf[0] != '0';
}

inline std::optional<std::filesystem::path> GetEnvPath(const wchar_t* envName) {
    DWORD len = GetEnvironmentVariableW(envName, nullptr, 0);
    if (len <= 1) {
        return std::nullopt;
    }
    std::wstring buf(len, L'\0');
    DWORD written = GetEnvironmentVariableW(envName, buf.data(), len);
    if (written == 0 || written >= len) {
        return std::nullopt;
    }
    buf.resize(written);
    return std::filesystem::path(std::move(buf));
}

inline std::filesystem::path ResolveDataDir(const wchar_t* subdir) {
    if (auto shimDir = GetShimDir()) {
        return shimDir->parent_path() / subdir;
    }
    return std::filesystem::current_path() / subdir;
}

} // namespace sc::config