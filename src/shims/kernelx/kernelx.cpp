#include "kernelx.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace {

struct VAllocRecord {
    size_t size{0};
    bool   isGraphics{false};
};

class KernelXState {
public:
    void* Alloc(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect) {
        const bool isGraphics = (flAllocationType & SC_MEM_GRAPHICS) != 0;
        const DWORD hostFlags = flAllocationType & ~(SC_MEM_GRAPHICS | SC_MEM_TITLE_BUDGET);

        void* ptr = VirtualAlloc(lpAddress, dwSize, hostFlags, flProtect);
        if (!ptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "-> VirtualAlloc failed (size=%zu, err=%lu)",
                          dwSize, GetLastError());
            sc::log::Write(sc::log::Level::Error, "kernelx", "VirtualAllocX", buf);
            return nullptr;
        }

        const size_t trackedSize = (dwSize + 0xFFFFULL) & ~0xFFFFULL; // 64 KB allocation granularity
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_regions.emplace(ptr, VAllocRecord{ trackedSize, isGraphics });
            seq = ++m_totalAllocs;
            m_titleUsed += trackedSize;
            m_titlePeak = std::max(m_titlePeak, m_titleUsed);
            if (isGraphics) {
                m_gfxUsed += trackedSize;
                m_gfxPeak = std::max(m_gfxPeak, m_gfxUsed);
            }
        }

        if (seq <= 8 || isGraphics) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> #%llu %s %zu B (committed=%zu B, base=0x%p)",
                          static_cast<unsigned long long>(seq),
                          isGraphics ? "MEM_GRAPHICS" : "MEM_TITLE",
                          dwSize, trackedSize, ptr);
            sc::log::Write(sc::log::Level::Info, "kernelx", "VirtualAllocX", buf);
        }
        return ptr;
    }

    BOOL Free(void* lpAddress, size_t dwSize, DWORD dwFreeType) {
        if (!lpAddress) {
            return FALSE;
        }
        if ((dwFreeType & MEM_RELEASE) != 0) {
            std::lock_guard<std::mutex> lock(m_mtx);
            auto it = m_regions.find(lpAddress);
            if (it != m_regions.end()) {
                m_titleUsed -= it->second.size;
                if (it->second.isGraphics) {
                    m_gfxUsed -= it->second.size;
                }
                ++m_totalFrees;
                m_regions.erase(it);
            }
        }
        return VirtualFree(lpAddress, dwSize, dwFreeType);
    }

    void QueryMemoryStatus(TITLEMEMORYSTATUS& out) {
        std::lock_guard<std::mutex> lock(m_mtx);
        out.dwReserved    = 0;
        out.ullTotalMem   = SC_SERIES_X_TITLE_MEM_BYTES;
        out.ullTitleUsed  = m_titleUsed;
        out.ullTitlePeak  = m_titlePeak;
        out.ullTitleAvail = (m_titleUsed < SC_SERIES_X_TITLE_MEM_BYTES)
                          ? (SC_SERIES_X_TITLE_MEM_BYTES - m_titleUsed)
                          : 0ULL;
        out.ullAvailMem   = out.ullTitleAvail;
        out.ullLegacyUsed = 0;
        out.ullLegacyPeak = 0;
        out.ullLegacyAvail = out.ullTitleAvail;
    }

    void DumpUnloadReport() {
        std::lock_guard<std::mutex> lock(m_mtx);
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "-> summary: vallocs=%llu, vfrees=%llu, peakTitle=%llu B (MEM_GRAPHICS peak=%llu B), leaks=%zu (%llu B)",
                      static_cast<unsigned long long>(m_totalAllocs),
                      static_cast<unsigned long long>(m_totalFrees),
                      static_cast<unsigned long long>(m_titlePeak),
                      static_cast<unsigned long long>(m_gfxPeak),
                      m_regions.size(),
                      static_cast<unsigned long long>(m_titleUsed));
        sc::log::Write(m_regions.empty() ? sc::log::Level::Info : sc::log::Level::Warn,
                       "kernelx", "DllMain[Unload]", buf);
    }

private:
    std::mutex m_mtx;
    std::unordered_map<void*, VAllocRecord> m_regions;
    uint64_t m_totalAllocs{0};
    uint64_t m_totalFrees{0};
    uint64_t m_titleUsed{0};
    uint64_t m_titlePeak{0};
    uint64_t m_gfxUsed{0};
    uint64_t m_gfxPeak{0};
};

std::atomic<KernelXState*> g_state{nullptr};

KernelXState* GetState() {
    return g_state.load(std::memory_order_acquire);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_state.store(new KernelXState(), std::memory_order_release);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "kernelx", "DllMain", path);
    } else if (reason == DLL_PROCESS_DETACH) {
        KernelXState* st = g_state.exchange(nullptr, std::memory_order_acq_rel);
        if (st) {
            st->DumpUnloadReport();
            delete st;
        }
    }
    return TRUE;
}

extern "C" {

BOOL WINAPI TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer || lpBuffer->dwLength < sizeof(TITLEMEMORYSTATUS)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    KernelXState* st = GetState();
    if (!st) {
        return FALSE;
    }
    st->QueryMemoryStatus(*lpBuffer);
    return TRUE;
}

void* WINAPI VirtualAllocX(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect) {
    KernelXState* st = GetState();
    return st ? st->Alloc(lpAddress, dwSize, flAllocationType, flProtect) : nullptr;
}

BOOL WINAPI VirtualFreeX(void* lpAddress, size_t dwSize, DWORD dwFreeType) {
    KernelXState* st = GetState();
    return st ? st->Free(lpAddress, dwSize, dwFreeType) : FALSE;
}

BOOL WINAPI GetConsoleProcessorInfo(SCARLETT_PROCESSOR_INFO* lpInfo) {
    if (!lpInfo) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);

    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);

    lpInfo->TitleCoreCount = 7;           // Xbox Series X title-accessible Zen 2 cores
    lpInfo->TitleLogicalThreadCount = 14; // With SMT enabled
    lpInfo->HostLogicalThreadCount = sysInfo.dwNumberOfProcessors;
    lpInfo->HostProcessAffinityMask = procMask;
    return TRUE;
}

DWORD_PTR WINAPI SetThreadAffinityMaskX(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || procMask == 0) {
        return SetThreadAffinityMask(hThread, dwThreadAffinityMask);
    }

    DWORD_PTR safeMask = dwThreadAffinityMask & procMask;
    if (safeMask == 0) {
        // Fallback to the lowest valid logical CPU on the host so console masks never fail
        safeMask = procMask & (~procMask + 1);
    }

    if (safeMask != dwThreadAffinityMask) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "-> clamped console mask 0x%llX -> host mask 0x%llX",
                      static_cast<unsigned long long>(dwThreadAffinityMask),
                      static_cast<unsigned long long>(safeMask));
        sc::log::Write(sc::log::Level::Info, "kernelx", __FUNCTION__, buf);
    }
    return SetThreadAffinityMask(hThread, safeMask);
}

} // extern "C"