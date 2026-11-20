#include "common/stub.hpp"
#include "pe_patcher.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path GetExecutableDir() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

void PrependShimDirToPath(const std::filesystem::path& shimDir) {
    SetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDir.c_str());

    DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring currentPath;
    if (len > 1) {
        currentPath.resize(len - 1);
        GetEnvironmentVariableW(L"PATH", currentPath.data(), len);
    }
    std::wstring updatedPath = shimDir.wstring() + L";" + currentPath;
    SetEnvironmentVariableW(L"PATH", updatedPath.c_str());
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::filesystem::path loaderDir = GetExecutableDir();
    const std::filesystem::path shimDir = loaderDir / L"shims";
    const std::filesystem::path hookDll = loaderDir / L"scarlett_hook.dll";

    std::filesystem::path targetPath = (argc > 1)
        ? std::filesystem::absolute(argv[1])
        : (loaderDir / L"titles" / L"dummy_title" / L"dummy_title.exe");

    if (!std::filesystem::exists(targetPath)) {
        std::string err = "Target executable not found: " + targetPath.string();
        sc::log::Write(sc::log::Level::Error, "loader", "wmain", err.c_str());
        return 1;
    }

    std::wstring cmdLine = L"\"" + targetPath.wstring() + L"\"";
    for (int i = 2; i < argc; ++i) {
        cmdLine += L" \"";
        cmdLine += argv[i];
        cmdLine += L"\"";
    }

    PrependShimDirToPath(shimDir);

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