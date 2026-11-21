#include "common/stub.hpp"
#include "pe_patcher.hpp"
#include "pe_inspector.hpp"
#include "game_config.hpp"
#include "common/xvd_container.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path GetExecutableDir() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

void ConfigureChildEnvironment(
    const std::filesystem::path& loaderDir,
    const std::filesystem::path& shimDir,
    const std::filesystem::path& targetPath) {
    SetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDir.c_str());

    const std::filesystem::path gameRoot = targetPath.parent_path();
    const std::filesystem::path scratchDir = loaderDir / L"vfs" / L"scratch";
    const std::filesystem::path saveTempDir = loaderDir / L"vfs" / L"save_temp";

    std::error_code ec;
    std::filesystem::create_directories(scratchDir, ec);
    std::filesystem::create_directories(saveTempDir, ec);

    SetEnvironmentVariableW(L"SCARLETT_GAME_ROOT", gameRoot.c_str());
    SetEnvironmentVariableW(L"SCARLETT_SCRATCH_DIR", scratchDir.c_str());
    SetEnvironmentVariableW(L"SCARLETT_SAVE_TEMP_DIR", saveTempDir.c_str());

    DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring currentPath;
    if (len > 1) {
        currentPath.resize(len - 1);
        GetEnvironmentVariableW(L"PATH", currentPath.data(), len);
    }
    std::wstring updatedPath = shimDir.wstring() + L";" + currentPath;
    SetEnvironmentVariableW(L"PATH", updatedPath.c_str());

    std::string vfsMsg = "VFS mounted: G:\\ -> " + gameRoot.string()
                       + " | T:\\ -> " + scratchDir.string();
    sc::log::Write(sc::log::Level::Info, "loader", "MountVfs", vfsMsg.c_str());
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::filesystem::path loaderDir = GetExecutableDir();
    const std::filesystem::path shimDir = loaderDir / L"shims";
    const std::filesystem::path hookDll = loaderDir / L"scarlett_hook.dll";

    if (argc >= 3 && (std::wstring_view(argv[1]) == L"--inspect" || std::wstring_view(argv[1]) == L"inspect")) {
        const std::filesystem::path pePath = std::filesystem::absolute(argv[2]);
        const auto cov = sc::loader::InspectPeSymbolCoverage(pePath, shimDir);
        const auto& audit = cov.audit;
        if (!audit.validPe64) {
            std::fprintf(stderr, "[inspect] ERROR: %s\n", audit.error.c_str());
            return 1;
        }
        const size_t totalShimmedSyms = cov.implementedSymbols.size() + cov.autoStubSymbols.size();
        const double pct = totalShimmedSyms > 0
            ? (100.0 * static_cast<double>(cov.implementedSymbols.size()) / static_cast<double>(totalShimmedSyms))
            : 100.0;
        std::printf("[inspect] PE64: %s (%zu imported symbols, %zu ordinals)\n",
                    pePath.string().c_str(), audit.totalImportedSymbols, audit.ordinalImportCount);
        std::printf("  TLS     : %s (raw=%zu B, zeroFill=%zu B, callbacks=%zu)\n",
                    audit.hasTlsDirectory ? "present" : "none",
                    audit.tlsRawDataSize, audit.tlsZeroFillSize, audit.tlsCallbackCount);
        std::printf("  Shimmed (%zu):", audit.shimmedImports.size());
        for (const auto& d : audit.shimmedImports) std::printf(" %s", d.c_str());
        std::printf("\n  System  (%zu):", audit.systemImports.size());
        for (const auto& d : audit.systemImports) std::printf(" %s", d.c_str());
        std::printf("\n  Missing (%zu):", audit.missingImports.size());
        for (const auto& d : audit.missingImports) std::printf(" %s", d.c_str());
        std::printf("\n  Coverage: %zu/%zu shimmed symbol(s) implemented (%.1f%%), %zu auto-stubbed\n",
                    cov.implementedSymbols.size(), totalShimmedSyms, pct, cov.autoStubSymbols.size());
        for (const auto& s : cov.autoStubSymbols) {
            std::printf("    [AUTO_STUB] %s\n", s.c_str());
        }
        return 0;
    }

    if (argc >= 3 && std::wstring_view(argv[1]) == L"--inspect-xvd") {
        const std::filesystem::path xvdPath = std::filesystem::absolute(argv[2]);
        sc::vfs::XvdContainer xvd;
        std::string err;
        if (!xvd.Open(xvdPath, err)) {
            std::fprintf(stderr, "[inspect-xvd] ERROR: %s\n", err.c_str());
            return 1;
        }
        const auto& h = xvd.Header();
        std::printf("[inspect-xvd] OK: %s | ver=%u | driveSize=%llu | userDataOff=0x%llX | userDataLen=%llu\n",
                    xvdPath.string().c_str(), h.formatVersion,
                    static_cast<unsigned long long>(h.driveSize),
                    static_cast<unsigned long long>(h.userDataOffset),
                    static_cast<unsigned long long>(h.userDataLength));
        return 0;
    }

    if (argc >= 3 && std::wstring_view(argv[1]) == L"--inspect-package") {
        const auto pkg = sc::loader::ParseGameConfig(argv[2]);
        if (!pkg.valid) {
            std::fprintf(stderr, "[package] ERROR: %s\n", pkg.error.c_str());
            return 1;
        }
        std::printf("[package] Identity:   %s (%s)\n", pkg.identityName.c_str(), pkg.version.c_str());
        std::printf("[package] TitleId:    %s\n", pkg.titleId.c_str());
        std::printf("[package] Display:    %s\n", pkg.displayName.c_str());
        std::printf("[package] Executable: %s (exists=%s)\n",
                    pkg.executablePath.string().c_str(),
                    std::filesystem::exists(pkg.executablePath) ? "yes" : "no");
        return 0;
    }

    int firstArgIdx = 2;
    std::filesystem::path targetPath;
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--boot") {
        const auto pkg = sc::loader::ParseGameConfig(argv[2]);
        if (!pkg.valid) {
            std::fprintf(stderr, "[boot] ERROR: %s\n", pkg.error.c_str());
            return 1;
        }
        targetPath = pkg.executablePath;
        firstArgIdx = 3;
        std::printf("[boot] Package: %s (TitleId=%s) -> %s\n",
                    pkg.identityName.c_str(), pkg.titleId.c_str(), targetPath.string().c_str());
    } else {
        targetPath = (argc > 1)
            ? std::filesystem::absolute(argv[1])
            : (loaderDir / L"titles" / L"dummy_title" / L"dummy_title.exe");
    }

    if (!std::filesystem::exists(targetPath)) {
        std::string err = "Target executable not found: " + targetPath.string();
        sc::log::Write(sc::log::Level::Error, "loader", "wmain", err.c_str());
        return 1;
    }

    std::wstring cmdLine = L"\"" + targetPath.wstring() + L"\"";
    for (int i = firstArgIdx; i < argc; ++i) {
        cmdLine += L" \"";
        cmdLine += argv[i];
        cmdLine += L"\"";
    }

    ConfigureChildEnvironment(loaderDir, shimDir, targetPath);

    std::string targetMsg = "Target: " + targetPath.string();
    sc::log::Write(sc::log::Level::Info, "loader", "wmain", targetMsg.c_str());

    std::wstring workingDir = targetPath.parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        targetPath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_SUSPENDED,
        nullptr,
        workingDir.c_str(),
        &si,
        &pi
    );

    if (!ok) {
        char errBuf[64];
        std::snprintf(errBuf, sizeof(errBuf), "CreateProcessW failed (err=%lu)", GetLastError());
        sc::log::Write(sc::log::Level::Error, "loader", "wmain", errBuf);
        return 1;
    }

    char pidBuf[64];
    std::snprintf(pidBuf, sizeof(pidBuf), "PID=%lu (suspended; patching PE imports...)", pi.dwProcessId);
    sc::log::Write(sc::log::Level::Info, "loader", "wmain", pidBuf);

    if (!sc::loader::RedirectImportsInSuspendedProcess(pi.hProcess, shimDir) ||
        !sc::loader::InjectHookDllInSuspendedProcess(pi.hProcess, pi.hThread, hookDll)) {
        sc::log::Write(sc::log::Level::Error, "loader", "wmain", "Bootstrap failed; terminating target");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char exitBuf[64];
    std::snprintf(exitBuf, sizeof(exitBuf), "Target exited with code %lu (0x%08lX)", exitCode, exitCode);
    sc::log::Write(sc::log::Level::Info, "loader", "wmain", exitBuf);
    return static_cast<int>(exitCode);
}