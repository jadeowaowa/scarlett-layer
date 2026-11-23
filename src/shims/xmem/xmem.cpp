#include "xmem.hpp"
#include "common/stub.hpp"

#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

size_t DecodeAlignment(uint32_t attrs) {
    const uint32_t code = (attrs >> 8) & 0xFu;
    if (code == XALLOC_ALIGNMENT_DEFAULT) {
        return 16;
    }
    const size_t requested = size_t{1} << (code + 1u); // code 1 -> 4 B ... code 15 -> 65536 B (64 KB)
    // Xbox Series X|S / SlimEra invariant: XMemAllocDefault guarantees >= 16-byte alignment.
    return (std::max)(requested, size_t{16});
}

uint8_t DecodeAllocatorId(uint32_t attrs) {
    return static_cast<uint8_t>(attrs & 0xFFu);
}

XMemType DecodeMemType(uint32_t attrs) {
    return ((attrs >> 12) & 0x3u) == XALLOC_MEMTYPE_PHYSICAL
        ? XALLOC_MEMTYPE_PHYSICAL
        : XALLOC_MEMTYPE_HEAP;
}

bool DecodeZeroInit(uint32_t attrs) {
    return ((attrs >> 14) & 0x1u) != 0;
}

const char* AllocatorIdName(uint8_t id) {
    switch (id) {
        case XALLOC_ID_DEFAULT:        return "Default";
        case XALLOC_ID_TEXTURE_TILED:  return "TextureTiled";
        case XALLOC_ID_TEXTURE_UPLOAD: return "TextureUpload";
        case XALLOC_ID_AUDIO_PCM:      return "AudioPCM";
        default:                       return "Custom";
    }
}

struct AllocRecord {
    size_t   requestedSize{0};
    size_t   alignment{16};
    uint8_t  allocatorId{0};
    XMemType memType{XALLOC_MEMTYPE_HEAP};
    bool     usedVirtualAlloc{false};
};

struct PoolStats {
    uint64_t allocCount{0};
    uint64_t freeCount{0};
    uint64_t liveBytes{0};
    uint64_t peakBytes{0};
};

class ScarlettMemoryTracker {
public:
    void* Allocate(size_t dwSize, uint32_t attrs) {
        const uint8_t id = DecodeAllocatorId(attrs);
        const XMemType memType = DecodeMemType(attrs);
        const bool zeroInit = DecodeZeroInit(attrs);
        size_t alignment = DecodeAlignment(attrs);
        if (memType == XALLOC_MEMTYPE_PHYSICAL && alignment < 65536) {
            alignment = 65536;
        }

        const size_t allocSize = std::max<size_t>(dwSize, 1);
        const bool useVirtual = (memType == XALLOC_MEMTYPE_PHYSICAL) || (alignment >= 65536);

        void* ptr = nullptr;
        if (useVirtual) {
            // VirtualAlloc reserves/commits on 64 KB boundaries and always zero-initializes
            ptr = VirtualAlloc(nullptr, allocSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        } else {
            ptr = _aligned_malloc(allocSize, alignment);
            if (ptr && zeroInit) {
                std::memset(ptr, 0, allocSize);
            }
        }

        if (!ptr) {
            sc::log::Write(sc::log::Level::Error, "xmem", "XMemAlloc", "-> allocation failed (OOM)");
            return nullptr;
        }

        uint64_t allocSeq = 0;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_live.emplace(ptr, AllocRecord{ dwSize, alignment, id, memType, useVirtual });
            allocSeq = ++m_totalAllocs;
            m_liveBytes += dwSize;
            m_peakBytes = std::max(m_peakBytes, m_liveBytes);

            if (memType == XALLOC_MEMTYPE_PHYSICAL) {
                m_physLiveBytes += dwSize;
                m_physPeakBytes = std::max(m_physPeakBytes, m_physLiveBytes);
            } else {
                m_heapLiveBytes += dwSize;
                m_heapPeakBytes = std::max(m_heapPeakBytes, m_heapLiveBytes);
            }

            PoolStats& pool = m_pools[id];
            ++pool.allocCount;
            pool.liveBytes += dwSize;
            pool.peakBytes = std::max(pool.peakBytes, pool.liveBytes);
        }

        if (allocSeq <= 10 || useVirtual) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> #%llu %s[%u] %s %zu B (align=%zu B, backend=%s)",
                          static_cast<unsigned long long>(allocSeq),
                          AllocatorIdName(id), static_cast<unsigned>(id),
                          memType == XALLOC_MEMTYPE_PHYSICAL ? "PHYSICAL" : "HEAP",
                          dwSize, alignment,
                          useVirtual ? "VirtualAlloc" : "aligned_malloc");
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemAlloc", buf);
        }
        return ptr;
    }

    void Free(void* ptr) {
        if (!ptr) {
            return;
        }

        AllocRecord rec{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            auto it = m_live.find(ptr);
            if (it != m_live.end()) {
                rec = it->second;
                m_live.erase(it);
                found = true;

                ++m_totalFrees;
                m_liveBytes -= rec.requestedSize;
                if (rec.memType == XALLOC_MEMTYPE_PHYSICAL) {
                    m_physLiveBytes -= rec.requestedSize;
                } else {
                    m_heapLiveBytes -= rec.requestedSize;
                }

                PoolStats& pool = m_pools[rec.allocatorId];
                ++pool.freeCount;
                pool.liveBytes -= rec.requestedSize;
            }
        }

        if (!found) {
            sc::log::Write(sc::log::Level::Warn, "xmem", "XMemFree",
                           "-> attempted to free untracked pointer (ignored)");
            return;
        }

        if (rec.usedVirtualAlloc) {
            VirtualFree(ptr, 0, MEM_RELEASE);
        } else {
            _aligned_free(ptr);
        }
    }

    size_t SizeOf(void* ptr) {
        if (!ptr) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_live.find(ptr);
        return (it != m_live.end()) ? it->second.requestedSize : 0;
    }

    void DumpShutdownReport() {
        std::lock_guard<std::mutex> lock(m_mtx);
        char summary[224];
        std::snprintf(summary, sizeof(summary),
                      "-> summary: allocs=%llu, frees=%llu, peak=%llu B (phys=%llu B, heap=%llu B), leaks=%zu (%llu B)",
                      static_cast<unsigned long long>(m_totalAllocs),
                      static_cast<unsigned long long>(m_totalFrees),
                      static_cast<unsigned long long>(m_peakBytes),
                      static_cast<unsigned long long>(m_physPeakBytes),
                      static_cast<unsigned long long>(m_heapPeakBytes),
                      m_live.size(),
                      static_cast<unsigned long long>(m_liveBytes));
        sc::log::Write(m_live.empty() ? sc::log::Level::Info : sc::log::Level::Warn,
                       "xmem", "DllMain[Unload]", summary);

        for (const auto& [id, st] : m_pools) {
            char poolLine[160];
            std::snprintf(poolLine, sizeof(poolLine),
                          "-> pool %s[%u]: allocs=%llu, frees=%llu, peak=%llu B, live=%llu B",
                          AllocatorIdName(id), static_cast<unsigned>(id),
                          static_cast<unsigned long long>(st.allocCount),
                          static_cast<unsigned long long>(st.freeCount),
                          static_cast<unsigned long long>(st.peakBytes),
                          static_cast<unsigned long long>(st.liveBytes));
            sc::log::Write(st.liveBytes == 0 ? sc::log::Level::Info : sc::log::Level::Warn,
                           "xmem", "PoolReport", poolLine);
        }
    }

private:
    std::mutex m_mtx;
    std::unordered_map<void*, AllocRecord> m_live;
    std::unordered_map<uint8_t, PoolStats> m_pools;
    uint64_t m_totalAllocs{0};
    uint64_t m_totalFrees{0};
    uint64_t m_liveBytes{0};
    uint64_t m_peakBytes{0};
    uint64_t m_physLiveBytes{0};
    uint64_t m_physPeakBytes{0};
    uint64_t m_heapLiveBytes{0};
    uint64_t m_heapPeakBytes{0};
};

std::atomic<ScarlettMemoryTracker*> g_tracker{nullptr};

ScarlettMemoryTracker* GetTracker() {
    return g_tracker.load(std::memory_order_acquire);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_tracker.store(new ScarlettMemoryTracker(), std::memory_order_release);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xmem", "DllMain", path);
    } else if (reason == DLL_PROCESS_DETACH) {
        ScarlettMemoryTracker* tracker = g_tracker.exchange(nullptr, std::memory_order_acq_rel);
        if (tracker) {
            tracker->DumpShutdownReport();
            delete tracker;
        }
    }
    return TRUE;
}

extern "C" {

void* WINAPI XMemAllocDefault(size_t dwSize, uint32_t dwAllocAttributes) {
    auto* tracker = GetTracker();
    return tracker ? tracker->Allocate(dwSize, dwAllocAttributes) : nullptr;
}

void WINAPI XMemFreeDefault(void* pAddress, uint32_t /*dwAllocAttributes*/) {
    auto* tracker = GetTracker();
    if (tracker) {
        tracker->Free(pAddress);
    }
}

size_t WINAPI XMemSizeDefault(void* pAddress, uint32_t /*dwAllocAttributes*/) {
    auto* tracker = GetTracker();
    return tracker ? tracker->SizeOf(pAddress) : 0;
}

void* WINAPI XMemAlloc(size_t dwSize, uint32_t dwAllocAttributes) {
    return XMemAllocDefault(dwSize, dwAllocAttributes);
}

void WINAPI XMemFree(void* pAddress, uint32_t dwAllocAttributes) {
    XMemFreeDefault(pAddress, dwAllocAttributes);
}

size_t WINAPI XMemSize(void* pAddress, uint32_t dwAllocAttributes) {
    return XMemSizeDefault(pAddress, dwAllocAttributes);
}


void* WINAPI XMemVirtualAlloc(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect) {
    DWORD winType = flAllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET | MEM_RESET_UNDO);
    if (winType == 0) winType = MEM_COMMIT | MEM_RESERVE;
    DWORD winProt = (flProtect & 0xFFu);
    if (winProt == 0) winProt = PAGE_READWRITE;
    void* p = VirtualAlloc(lpAddress, dwSize, winType, winProt);
    if (!p && lpAddress != nullptr) {
        p = VirtualAlloc(nullptr, dwSize, winType, winProt);
    }
    return p;
}

BOOL WINAPI XMemAllocatePhysicalPages(size_t /*pageSize*/, size_t /*numPages*/, uint32_t /*flags*/, uint64_t* /*pageNumbers*/) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI XMemMapPhysicalPages(void* /*virtualAddress*/, size_t /*numPages*/, uint32_t /*flags*/, const uint64_t* /*pageNumbers*/) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI XMemFreePhysicalPages(size_t /*numPages*/, const uint64_t* /*pageNumbers*/) {
    return TRUE;
}

BOOL WINAPI XMemGetWorkingSetStatistics(void* /*stats*/) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

} // extern "C"