#include "common/stub.hpp"
#include <string>

static std::wstring GetSiblingPath(const wchar_t* filename) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    return path + filename;
}

int wmain(int argc, wchar_t* argv[]) {
    std::wstring target = (argc > 1) ? argv[1] : GetSiblingPath(L"dummy_title.exe");
    std::wstring cmdLine = L"\"" + target + L"\"";

    sc::log::Write(sc::log::Level::Info, "loader", "wmain", "Spawning target suspended...");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Launch suspended so we have an injection hook point before the game's entry point runs
    BOOL ok = CreateProcessW(
        target.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE, // inherit stdout/stderr
        CREATE_SUSPENDED,
        nullptr,
        nullptr,
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
    std::snprintf(pidBuf, sizeof(pidBuf), "PID=%lu (ready for Sprint 2 hooks)", pi.dwProcessId);
    sc::log::Write(sc::log::Level::Info, "loader", "wmain", pidBuf);

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char exitBuf[64];
    std::snprintf(exitBuf, sizeof(exitBuf), "Target exited with code %lu", exitCode);
    sc::log::Write(sc::log::Level::Info, "loader", "wmain", exitBuf);
    return static_cast<int>(exitCode);
}