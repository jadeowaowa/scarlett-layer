#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

// Console-specific VirtualAllocX flags (clean-room bit assignments above standard Win32 flags)
inline constexpr DWORD SC_MEM_GRAPHICS     = 0x10000000u;
inline constexpr DWORD SC_MEM_TITLE_BUDGET = 0x08000000u;

// Xbox Series X exposes 13.5 GiB of unified GDDR6 to games (out of 16 GiB total)
inline constexpr uint64_t SC_SERIES_X_TITLE_MEM_BYTES = 14495514624ULL; // 13.5 * 1024^3

struct TITLEMEMORYSTATUS {
    DWORD    dwLength;
    DWORD    dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullLegacyUsed;
    uint64_t ullLegacyPeak;
    uint64_t ullLegacyAvail;
    uint64_t ullTitleUsed;
    uint64_t ullTitlePeak;
    uint64_t ullTitleAvail;
};

struct SCARLETT_PROCESSOR_INFO {
    uint32_t TitleCoreCount;          // Series X: 7 cores available to title
    uint32_t TitleLogicalThreadCount; // Series X: 14 SMT threads available to title
    uint32_t HostLogicalThreadCount;  // Actual host CPU logical threads
    DWORD_PTR HostProcessAffinityMask;
};

extern "C" {
    BOOL      WINAPI TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer);
    void*     WINAPI VirtualAllocX(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect);
    BOOL      WINAPI VirtualFreeX(void* lpAddress, size_t dwSize, DWORD dwFreeType);
    BOOL      WINAPI GetConsoleProcessorInfo(SCARLETT_PROCESSOR_INFO* lpInfo);
    DWORD_PTR WINAPI SetThreadAffinityMaskX(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
}