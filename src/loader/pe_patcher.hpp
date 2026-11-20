#pragma once

#include <filesystem>
#include <windows.h>

namespace sc::loader {

// Patches static and delay-loaded PE import DLL names inside a suspended x64
// child process so any DLL present in shimDir loads directly from shimDir.
bool RedirectImportsInSuspendedProcess(HANDLE hProcess, const std::filesystem::path& shimDir);

// Queues a pre-entrypoint user-mode APC on the suspended process's primary thread
// to load hookDllPath (which installs runtime LoadLibrary* IAT hooks).
bool InjectHookDllInSuspendedProcess(
    HANDLE hProcess,
    HANDLE hThread,
    const std::filesystem::path& hookDllPath);

} // namespace sc::loader