#pragma once

#include <windows.h>

#include <string>

namespace sc::path {

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

inline bool WideToCodePageLossless(UINT codePage, const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) {
        return true;
    }

    const bool isUtf8 = (codePage == CP_UTF8) ||
                        (codePage == CP_ACP && GetACP() == CP_UTF8);
    if (isUtf8) {
        const int needed = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return false;
        }
        std::string buf(static_cast<size_t>(needed - 1), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, w.c_str(), -1, buf.data(), needed, nullptr, nullptr);
        if (written <= 1) {
            return false;
        }
        out = std::move(buf);
        return true;
    }

    BOOL usedDefault = FALSE;
    const int needed = WideCharToMultiByte(
        codePage, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &usedDefault);
    if (needed <= 1 || usedDefault != FALSE) {
        return false;
    }

    std::string buf(static_cast<size_t>(needed - 1), '\0');
    usedDefault = FALSE;
    const int written = WideCharToMultiByte(
        codePage, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, buf.data(), needed, nullptr, &usedDefault);
    if (written <= 1 || usedDefault != FALSE) {
        return false;
    }

    out = std::move(buf);
    return true;
}

// Losslessly converts a Windows path to CP_ACP. If CP_ACP would substitute replacement
// characters, attempts an 8.3 short path fallback via GetShortPathNameW and re-verifies.
// Returns false (leaving outAnsi empty) if no lossless ANSI representation exists.
inline bool WidePathToAnsiLossless(
    const std::wstring& widePath,
    std::string& outAnsi,
    bool* outUsedShortPath = nullptr) {
    if (outUsedShortPath) {
        *outUsedShortPath = false;
    }
    if (WideToCodePageLossless(CP_ACP, widePath, outAnsi)) {
        return true;
    }

    const DWORD shortLen = GetShortPathNameW(widePath.c_str(), nullptr, 0);
    if (shortLen > 0) {
        std::wstring shortBuf(static_cast<size_t>(shortLen), L'\0');
        const DWORD written = GetShortPathNameW(widePath.c_str(), shortBuf.data(), shortLen);
        if (written > 0 && written < shortLen) {
            shortBuf.resize(written);
            if (WideToCodePageLossless(CP_ACP, shortBuf, outAnsi)) {
                if (outUsedShortPath) {
                    *outUsedShortPath = true;
                }
                return true;
            }
        }
    }

    outAnsi.clear();
    return false;
}

} // namespace sc::path
