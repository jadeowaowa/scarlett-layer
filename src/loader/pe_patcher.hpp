#pragma once

#include <filesystem>
#include <windows.h>

namespace sc::loader {

// Patches static and delay-loaded PE import DLL names inside a suspended x64
// child process so any DLL present in shimDir loads directly from shimDir
// (bypassing System32 and requiring no files in the title directory).
bool RedirectImportsInSuspendedProcess(HANDLE hProcess, const std::filesystem::path& shimDir);

} // namespace sc::loader