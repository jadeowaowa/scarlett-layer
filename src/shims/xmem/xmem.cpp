#include "xmem.hpp"
#include "common/stub.hpp"

#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

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


namespace phys {

constexpr size_t kPageSize = 65536ULL; // 64 KiB Xbox Series physical page

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct KernelBaseVmApi {
    VirtualAlloc2Fn    virtualAlloc2{nullptr};
    MapViewOfFile3Fn   mapViewOfFile3{nullptr};
    UnmapViewOfFile2Fn unmapViewOfFile2{nullptr};

    static const KernelBaseVmApi& Get() {
        static const KernelBaseVmApi api = []() {
            KernelBaseVmApi a{};
            HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
            if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
            if (kb) {
                a.virtualAlloc2    = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
                a.mapViewOfFile3   = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));
                a.unmapViewOfFile2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kb, "UnmapViewOfFile2"));
            }
            return a;
        }();
        return api;
    }
};

struct SectionBlock {
    HANDLE   hMapping{nullptr};
    size_t   totalPages{0};
    size_t   livePages{0};
};

struct PhysPageEntry {
    SectionBlock* block{nullptr};
    uint64_t      sectionOffset{0};
    bool          allocated{false};
};

struct MappedView {
    uintptr_t va{0};
    size_t    len{0};
    HANDLE    hMapping{nullptr};
    uint64_t  sectionOffset{0};
};

struct PlaceholderRegion {
    uintptr_t                    base{0};
    size_t                       size{0};
    std::map<uintptr_t, size_t>  freePlaceholders;
    std::map<uintptr_t, MappedView> mappedViews;
};

class PhysicalMemoryManager {
public:
    PhysicalMemoryManager() {
        // Reserve index 0 as invalid sentinel so every valid pageId >= 1
        m_pages.push_back(PhysPageEntry{});
    }

    ~PhysicalMemoryManager() {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (auto& blk : m_blocks) {
            if (blk && blk->hMapping) {
                CloseHandle(blk->hMapping);
                blk->hMapping = nullptr;
            }
        }
    }

    void* ReservePlaceholder(void* lpAddress, size_t dwSize) {
        const auto& api = KernelBaseVmApi::Get();
        if (!api.virtualAlloc2) return nullptr;

        const size_t alignedSize = (dwSize + kPageSize - 1ULL) & ~(kPageSize - 1ULL);
        if (lpAddress != nullptr) {
            std::lock_guard<std::mutex> lock(m_mtx);
            const uintptr_t raw = reinterpret_cast<uintptr_t>(lpAddress);
            PlaceholderRegion* existing = FindRegionLocked(raw);
            if (existing && (raw & (kPageSize - 1ULL)) == 0 &&
                raw + alignedSize <= existing->base + existing->size) {
                return lpAddress;
            }
        }
        void* p = api.virtualAlloc2(
            GetCurrentProcess(), lpAddress, alignedSize,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (!p && lpAddress != nullptr) {
            p = api.virtualAlloc2(
                GetCurrentProcess(), nullptr, alignedSize,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        }
        if (!p) return nullptr;

        std::lock_guard<std::mutex> lock(m_mtx);
        PlaceholderRegion reg{};
        reg.base = reinterpret_cast<uintptr_t>(p);
        reg.size = alignedSize;
        reg.freePlaceholders.emplace(reg.base, alignedSize);
        m_regions.emplace(reg.base, std::move(reg));
        return p;
    }

    bool TryCommitInPlaceholder(void* lpAddress, size_t dwSize, DWORD winProt, void** outPtr) {
        if (!lpAddress) return false;
        const auto& api = KernelBaseVmApi::Get();
        if (!api.virtualAlloc2) return false;

        std::lock_guard<std::mutex> lock(m_mtx);
        const uintptr_t raw = reinterpret_cast<uintptr_t>(lpAddress);
        PlaceholderRegion* reg = FindRegionLocked(raw);
        if (!reg) return false;

        const uintptr_t alignedAddr = raw & ~(kPageSize - 1ULL);
        const uintptr_t alignedEnd = (raw + dwSize + kPageSize - 1ULL) & ~(kPageSize - 1ULL);
        const size_t alignedLen = alignedEnd - alignedAddr;
        if (alignedEnd > reg->base + reg->size) return false;

        if (!EnsurePlaceholderExactLocked(*reg, alignedAddr, alignedLen)) {
            return false;
        }
        void* committed = api.virtualAlloc2(
            GetCurrentProcess(), reinterpret_cast<void*>(alignedAddr), alignedLen,
            MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, winProt, nullptr, 0);
        if (!committed) return false;

        reg->freePlaceholders.erase(alignedAddr);
        if (outPtr) *outPtr = committed;
        return true;
    }

    BOOL AllocatePages(DWORD flags, ULONG_PTR* numPagesInOut, ULONG_PTR* pageArray) {
        if (!numPagesInOut || !pageArray) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const size_t count = static_cast<size_t>(*numPagesInOut);
        if (count == 0) return TRUE;

        const uint64_t totalBytes = static_cast<uint64_t>(count) * kPageSize;
        const DWORD hi = static_cast<DWORD>(totalBytes >> 32);
        const DWORD lo = static_cast<DWORD>(totalBytes & 0xFFFFFFFFULL);
        HANDLE hMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, hi, lo, nullptr);
        if (!hMap) {
            DWORD err = GetLastError();
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> CreateFileMappingW FAILED for %zu pages (%llu MB, flags=0x%lX, err=%lu)",
                          count, static_cast<unsigned long long>(totalBytes / (1024ULL * 1024ULL)), flags, err);
            sc::log::Write(sc::log::Level::Error, "xmem", "XMemAllocatePhysicalPages", buf);
            SetLastError(err);
            return FALSE;
        }

        auto blk = std::make_unique<SectionBlock>();
        blk->hMapping   = hMap;
        blk->totalPages = count;
        blk->livePages  = count;
        SectionBlock* rawBlk = blk.get();

        std::lock_guard<std::mutex> lock(m_mtx);
        const size_t firstId = m_pages.size();
        m_pages.reserve(firstId + count);
        for (size_t i = 0; i < count; ++i) {
            m_pages.push_back(PhysPageEntry{ rawBlk, static_cast<uint64_t>(i) * kPageSize, true });
            pageArray[i] = static_cast<ULONG_PTR>((firstId + i) << 4);
        }
        m_blocks.push_back(std::move(blk));
        m_totalAllocatedPages += count;

        static std::atomic<uint32_t> s_allocLogs{0};
        if (s_allocLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "-> allocated %zu x 64KB pages (%llu MB, flags=0x%lX, ids=%zu..%zu, total=%llu MB)",
                          count, static_cast<unsigned long long>(totalBytes / (1024ULL * 1024ULL)),
                          flags, firstId, firstId + count - 1,
                          static_cast<unsigned long long>((m_totalAllocatedPages * kPageSize) / (1024ULL * 1024ULL)));
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemAllocatePhysicalPages", buf);
        }
        return TRUE;
    }

    BOOL FreePages(ULONG_PTR numPages, const ULONG_PTR* pageArray) {
        if (numPages == 0 || !pageArray) return TRUE;
        std::lock_guard<std::mutex> lock(m_mtx);
        for (size_t i = 0; i < static_cast<size_t>(numPages); ++i) {
            const size_t id = static_cast<size_t>(pageArray[i] >> 4);
            if (id == 0 || id >= m_pages.size()) continue;
            auto& entry = m_pages[id];
            if (!entry.allocated) continue;
            entry.allocated = false;
            if (entry.block && entry.block->livePages > 0) {
                --entry.block->livePages;
                if (entry.block->livePages == 0 && entry.block->hMapping) {
                    CloseHandle(entry.block->hMapping);
                    entry.block->hMapping = nullptr;
                }
            }
            entry.block = nullptr;
        }
        return TRUE;
    }

    void* MapContiguous(void* va, ULONG_PTR numPages, const ULONG_PTR* pageArray) {
        static std::atomic<uint32_t> s_enter{0};
        if (s_enter.fetch_add(1, std::memory_order_relaxed) < 32) {
            char eb[160];
            std::snprintf(eb, sizeof(eb), "-> enter va=%p pages=%llu pageArr=%p",
                          va, static_cast<unsigned long long>(numPages), static_cast<const void*>(pageArray));
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemMapPhysicalPages", eb);
        }
        if (!va) {
            sc::log::Write(sc::log::Level::Error, "xmem", "XMemMapPhysicalPages", "-> FAIL null va");
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        if (numPages == 0) return va;
        const uintptr_t baseVa = reinterpret_cast<uintptr_t>(va);
        const size_t count = static_cast<size_t>(numPages);
        if ((baseVa & (kPageSize - 1ULL)) != 0) {
            char eb[128]; std::snprintf(eb, sizeof(eb), "-> FAIL unaligned va=%p", va);
            sc::log::Write(sc::log::Level::Error, "xmem", "XMemMapPhysicalPages", eb);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(m_mtx);
        PlaceholderRegion* reg = FindRegionLocked(baseVa);
        if (!reg || baseVa + count * kPageSize > reg->base + reg->size) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "-> VA=%p (%zu pages) not inside tracked placeholder region", va, count);
            sc::log::Write(sc::log::Level::Error, "xmem", "XMemMapPhysicalPages", buf);
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }

        if (!pageArray) {
            return EnsurePlaceholderExactLocked(*reg, baseVa, count * kPageSize) ? va : nullptr;
        }

        // Coalesce contiguous runs sharing the same backing section and consecutive offsets
        size_t i = 0;
        while (i < count) {
            const ULONG_PTR pfn = pageArray[i];
            if (pfn == 0) {
                size_t j = i + 1;
                while (j < count && pageArray[j] == 0) ++j;
                if (!EnsurePlaceholderExactLocked(*reg, baseVa + i * kPageSize, (j - i) * kPageSize)) {
                    return nullptr;
                }
                i = j;
                continue;
            }
            const PhysPageEntry* e0 = LookupPageLocked(pfn);
            if (!e0) {
                char eb[160];
                std::snprintf(eb, sizeof(eb), "-> FAIL bad pfn=0x%llX (idx=%zu/%zu, m_pages=%zu)",
                              static_cast<unsigned long long>(pfn), i, count, m_pages.size());
                sc::log::Write(sc::log::Level::Error, "xmem", "XMemMapPhysicalPages", eb);
                SetLastError(ERROR_INVALID_PARAMETER);
                return nullptr;
            }
            size_t j = i + 1;
            while (j < count && pageArray[j] != 0) {
                const PhysPageEntry* ej = LookupPageLocked(pageArray[j]);
                if (!ej || ej->block != e0->block ||
                    ej->sectionOffset != e0->sectionOffset + (j - i) * kPageSize) {
                    break;
                }
                ++j;
            }
            if (!MapRangeLocked(*reg, baseVa + i * kPageSize, (j - i) * kPageSize,
                                e0->block->hMapping, e0->sectionOffset)) {
                return nullptr;
            }
            i = j;
        }

        static std::atomic<uint32_t> s_mapLogs{0};
        if (s_mapLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "-> mapped %zu x 64KB pages (%llu MB) at VA=%p",
                          count, static_cast<unsigned long long>((count * kPageSize) / (1024ULL * 1024ULL)), va);
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemMapPhysicalPages", buf);
        }
        return va;
    }

    BOOL MapScatter(void** vaArray, ULONG_PTR count, const ULONG_PTR* pageArray) {
        static std::atomic<uint32_t> s_scEnter{0};
        if (s_scEnter.fetch_add(1, std::memory_order_relaxed) < 32) {
            char eb[160];
            std::snprintf(eb, sizeof(eb), "-> enter vaArr=%p count=%llu pageArr=%p",
                          static_cast<const void*>(vaArray), static_cast<unsigned long long>(count),
                          static_cast<const void*>(pageArray));
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemMapPhysicalPagesScatter", eb);
        }
        if (!vaArray || count == 0) return TRUE;
        std::lock_guard<std::mutex> lock(m_mtx);
        const size_t n = static_cast<size_t>(count);

        size_t i = 0;
        while (i < n) {
            const uintptr_t va0 = reinterpret_cast<uintptr_t>(vaArray[i]);
            PlaceholderRegion* reg = FindRegionLocked(va0);
            if (!reg || (va0 & (kPageSize - 1ULL)) != 0 || va0 + kPageSize > reg->base + reg->size) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "-> scatter VA=%p (idx=%zu/%zu) outside placeholder region",
                              vaArray[i], i, n);
                sc::log::Write(sc::log::Level::Error, "xmem", "XMemMapPhysicalPagesScatter", buf);
                SetLastError(ERROR_INVALID_ADDRESS);
                return FALSE;
            }

            if (!pageArray || pageArray[i] == 0) {
                size_t j = i + 1;
                while (j < n &&
                       (!pageArray || pageArray[j] == 0) &&
                       reinterpret_cast<uintptr_t>(vaArray[j]) == va0 + (j - i) * kPageSize &&
                       va0 + (j - i + 1) * kPageSize <= reg->base + reg->size) {
                    ++j;
                }
                if (!EnsurePlaceholderExactLocked(*reg, va0, (j - i) * kPageSize)) {
                    return FALSE;
                }
                i = j;
                continue;
            }

            const PhysPageEntry* e0 = LookupPageLocked(pageArray[i]);
            if (!e0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            size_t j = i + 1;
            while (j < n && pageArray[j] != 0 &&
                   reinterpret_cast<uintptr_t>(vaArray[j]) == va0 + (j - i) * kPageSize &&
                   va0 + (j - i + 1) * kPageSize <= reg->base + reg->size) {
                const PhysPageEntry* ej = LookupPageLocked(pageArray[j]);
                if (!ej || ej->block != e0->block ||
                    ej->sectionOffset != e0->sectionOffset + (j - i) * kPageSize) {
                    break;
                }
                ++j;
            }
            if (!MapRangeLocked(*reg, va0, (j - i) * kPageSize,
                                e0->block->hMapping, e0->sectionOffset)) {
                return FALSE;
            }
            i = j;
        }

        static std::atomic<uint32_t> s_scatLogs{0};
        if (s_scatLogs.fetch_add(1, std::memory_order_relaxed) < 12) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "-> scatter %s %zu page(s) (first VA=%p)",
                          pageArray ? "mapped" : "unmapped", n, vaArray[0]);
            sc::log::Write(sc::log::Level::Info, "xmem", "XMemMapPhysicalPagesScatter", buf);
        }
        return TRUE;
    }

private:
    PlaceholderRegion* FindRegionLocked(uintptr_t addr) {
        auto it = m_regions.upper_bound(addr);
        if (it == m_regions.begin()) return nullptr;
        --it;
        if (addr >= it->second.base && addr < it->second.base + it->second.size) {
            return &it->second;
        }
        return nullptr;
    }

    const PhysPageEntry* LookupPageLocked(ULONG_PTR pfn) const {
        const size_t id = static_cast<size_t>(pfn >> 4);
        if (id == 0 || id >= m_pages.size()) return nullptr;
        const auto& e = m_pages[id];
        return (e.allocated && e.block && e.block->hMapping) ? &e : nullptr;
    }

    bool MapPlaceholderExactLocked(PlaceholderRegion& reg, uintptr_t addr, size_t len, HANDLE hMap, uint64_t secOff) {
        const auto& api = KernelBaseVmApi::Get();
        if (!api.mapViewOfFile3) {
            sc::log::Write(sc::log::Level::Error, "xmem", "MapPlaceholder", "-> mapViewOfFile3 missing");
            return false;
        }

        void* mapped = api.mapViewOfFile3(
            hMap, GetCurrentProcess(), reinterpret_cast<void*>(addr),
            secOff, len, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!mapped) {
            DWORD err = GetLastError();
            char buf[160];
            std::snprintf(buf, sizeof(buf), "-> MapViewOfFile3 FAILED addr=0x%llX len=0x%llX err=%lu",
                          static_cast<unsigned long long>(addr),
                          static_cast<unsigned long long>(len), err);
            sc::log::Write(sc::log::Level::Error, "xmem", "MapPlaceholder", buf);
            SetLastError(err);
            return false;
        }
        reg.freePlaceholders.erase(addr);
        reg.mappedViews.emplace(addr, MappedView{ addr, len, hMap, secOff });
        return true;
    }

    bool SplitAndCoalesceFreeLocked(PlaceholderRegion& reg, uintptr_t addr, size_t len) {
        (void)reg;
        const uintptr_t end = addr + len;
        uintptr_t cursor = addr;
        size_t pieceCount = 0;

        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) == 0) {
                DWORD err = GetLastError();
                char eb[160];
                std::snprintf(eb, sizeof(eb), "-> FAIL VirtualQuery cursor=0x%llX err=%lu",
                              static_cast<unsigned long long>(cursor), err);
                sc::log::Write(sc::log::Level::Error, "xmem", "SplitAndCoalesce", eb);
                SetLastError(err);
                return false;
            }
            const uintptr_t pStart = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            const uintptr_t pEnd   = cursor + mbi.RegionSize;
            if (mbi.State != MEM_RESERVE || mbi.RegionSize == 0) {
                char eb[192];
                std::snprintf(eb, sizeof(eb),
                              "-> FAIL non-reserve cursor=0x%llX state=0x%lX type=0x%lX allocBase=0x%llX regSz=0x%llX",
                              static_cast<unsigned long long>(cursor), mbi.State, mbi.Type,
                              static_cast<unsigned long long>(pStart), static_cast<unsigned long long>(mbi.RegionSize));
                sc::log::Write(sc::log::Level::Error, "xmem", "SplitAndCoalesce", eb);
                SetLastError(ERROR_INVALID_ADDRESS);
                return false;
            }

            // Already exact single placeholder
            if (cursor == addr && pStart == addr && pEnd == end) {
                return true;
            }

            // Split off leading portion if placeholder starts before cursor
            if (pStart < cursor) {
                if (!VirtualFree(reinterpret_cast<void*>(pStart), cursor - pStart,
                                 MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    DWORD err = GetLastError();
                    char eb[192];
                    std::snprintf(eb, sizeof(eb),
                                  "-> FAIL lead split pStart=0x%llX len=0x%llX (state=0x%lX type=0x%lX) err=%lu",
                                  static_cast<unsigned long long>(pStart),
                                  static_cast<unsigned long long>(cursor - pStart),
                                  mbi.State, mbi.Type, err);
                    sc::log::Write(sc::log::Level::Error, "xmem", "SplitAndCoalesce", eb);
                    SetLastError(err);
                    return false;
                }
            }

            // Split off trailing portion if placeholder extends past end
            if (pEnd > end) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), end - cursor,
                                 MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    DWORD err = GetLastError();
                    char eb[192];
                    std::snprintf(eb, sizeof(eb),
                                  "-> FAIL trail split cursor=0x%llX len=0x%llX (pEnd=0x%llX) err=%lu",
                                  static_cast<unsigned long long>(cursor),
                                  static_cast<unsigned long long>(end - cursor),
                                  static_cast<unsigned long long>(pEnd), err);
                    sc::log::Write(sc::log::Level::Error, "xmem", "SplitAndCoalesce", eb);
                    SetLastError(err);
                    return false;
                }
                cursor = end;
            } else {
                cursor = pEnd;
            }
            ++pieceCount;
        }

        if (pieceCount > 1) {
            if (!VirtualFree(reinterpret_cast<void*>(addr), len,
                             MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
                DWORD err = GetLastError();
                char eb[160];
                std::snprintf(eb, sizeof(eb),
                              "-> FAIL coalesce addr=0x%llX len=0x%llX pieces=%zu err=%lu",
                              static_cast<unsigned long long>(addr),
                              static_cast<unsigned long long>(len), pieceCount, err);
                sc::log::Write(sc::log::Level::Error, "xmem", "SplitAndCoalesce", eb);
                SetLastError(err);
                return false;
            }
        }
        return true;
    }

    bool EnsurePlaceholderExactLocked(PlaceholderRegion& reg, uintptr_t addr, size_t len) {
        const auto& api = KernelBaseVmApi::Get();
        if (!api.unmapViewOfFile2) {
            sc::log::Write(sc::log::Level::Error, "xmem", "EnsurePlaceholder", "-> unmapViewOfFile2 missing");
            return false;
        }

        const uintptr_t end = addr + len;
        std::vector<MappedView> toUnmap;
        auto it = reg.mappedViews.upper_bound(addr);
        if (it != reg.mappedViews.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.len > addr) {
                it = prev;
            }
        }
        while (it != reg.mappedViews.end() && it->first < end) {
            toUnmap.push_back(it->second);
            ++it;
        }

        std::vector<MappedView> remaps;
        for (const auto& mv : toUnmap) {
            if (!api.unmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(mv.va), MEM_PRESERVE_PLACEHOLDER)) {
                DWORD err = GetLastError(); char eb[160];
                std::snprintf(eb, sizeof(eb), "-> FAIL UnmapViewOfFile2 va=0x%llX err=%lu",
                              static_cast<unsigned long long>(mv.va), err);
                sc::log::Write(sc::log::Level::Error, "xmem", "EnsurePlaceholder", eb);
                SetLastError(err); return false;
            }
            reg.mappedViews.erase(mv.va);
            reg.freePlaceholders.emplace(mv.va, mv.len);

            const uintptr_t mvEnd = mv.va + mv.len;
            if (mv.va < addr) {
                remaps.push_back(MappedView{ mv.va, addr - mv.va, mv.hMapping, mv.sectionOffset });
            }
            if (mvEnd > end) {
                const uint64_t delta = static_cast<uint64_t>(end - mv.va);
                remaps.push_back(MappedView{ end, mvEnd - end, mv.hMapping, mv.sectionOffset + delta });
            }
        }

        for (const auto& rm : remaps) {
            if (!SplitAndCoalesceFreeLocked(reg, rm.va, rm.len) ||
                !MapPlaceholderExactLocked(reg, rm.va, rm.len, rm.hMapping, rm.sectionOffset)) {
                return false;
            }
        }

        return SplitAndCoalesceFreeLocked(reg, addr, len);
    }

    bool MapRangeLocked(PlaceholderRegion& reg, uintptr_t addr, size_t len, HANDLE hMap, uint64_t secOff) {
        if (!EnsurePlaceholderExactLocked(reg, addr, len)) {
            return false;
        }
        return MapPlaceholderExactLocked(reg, addr, len, hMap, secOff);
    }

    std::mutex                                  m_mtx;
    std::vector<std::unique_ptr<SectionBlock>>  m_blocks;
    std::vector<PhysPageEntry>                  m_pages;
    std::map<uintptr_t, PlaceholderRegion>      m_regions;
    uint64_t                                    m_totalAllocatedPages{0};
};

PhysicalMemoryManager& GetPhysMgr() {
    static PhysicalMemoryManager s_mgr;
    return s_mgr;
}

} // namespace phys

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
    DWORD winProt = PAGE_READWRITE;
    switch (flProtect & 0xFFu) {
        case PAGE_NOACCESS:
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            winProt = (flProtect & 0xFFu);
            break;
        default:
            winProt = PAGE_READWRITE;
            break;
    }

    void* p = nullptr;
    const bool isReserveOnly = ((winType & MEM_RESERVE) != 0) && ((winType & MEM_COMMIT) == 0);
    if (isReserveOnly) {
        p = phys::GetPhysMgr().ReservePlaceholder(lpAddress, dwSize);
    } else if (lpAddress != nullptr && (winType & MEM_COMMIT) != 0) {
        if (!phys::GetPhysMgr().TryCommitInPlaceholder(lpAddress, dwSize, winProt, &p)) {
            p = VirtualAlloc(lpAddress, dwSize, winType, winProt);
        }
    }
    if (!p) {
        p = VirtualAlloc(lpAddress, dwSize, winType, winProt);
        if (!p && lpAddress != nullptr) {
            p = VirtualAlloc(nullptr, dwSize, winType, winProt);
        }
    }

    static std::atomic<uint32_t> s_xvaLogs{0};
    if (s_xvaLogs.fetch_add(1, std::memory_order_relaxed) < 16 || !p) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "-> addr=%p size=0x%llX type=0x%lX->0x%lX prot=0x%lX->0x%lX %s=> %p (err=%lu)",
                      lpAddress, static_cast<unsigned long long>(dwSize),
                      flAllocationType, winType, flProtect, winProt,
                      isReserveOnly ? "[placeholder] " : "",
                      p, p ? 0UL : GetLastError());
        sc::log::Write(p ? sc::log::Level::Info : sc::log::Level::Error, "xmem", __FUNCTION__, buf);
    }
    return p;
}

BOOL WINAPI XMemAllocatePhysicalPages(DWORD flags, ULONG_PTR* numberOfPages, ULONG_PTR* pageArray) {
    return phys::GetPhysMgr().AllocatePages(flags, numberOfPages, pageArray);
}

void* WINAPI XMemMapPhysicalPages(void* virtualAddress, ULONG_PTR numberOfPages, const ULONG_PTR* pageArray) {
    return phys::GetPhysMgr().MapContiguous(virtualAddress, numberOfPages, pageArray);
}

BOOL WINAPI XMemMapPhysicalPagesScatter(void** virtualAddressArray, ULONG_PTR numberOfPages, const ULONG_PTR* pageArray) {
    return phys::GetPhysMgr().MapScatter(virtualAddressArray, numberOfPages, pageArray);
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR numberOfPages, const ULONG_PTR* pageArray) {
    return phys::GetPhysMgr().FreePages(numberOfPages, pageArray);
}

BOOL WINAPI XMemGetWorkingSetStatistics(void* stats) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
        sc::log::Write(sc::log::Level::Warn, "xmem", __FUNCTION__, "-> UNIMPLEMENTED (returning FALSE)");
    }
    (void)stats;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI XMemIsToolsMemAvailable(void) {
    return FALSE;
}

BOOL WINAPI XMemTransferMemory(uint32_t direction, size_t sizeInBytes) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "-> stub TRUE (dir=%u size=0x%llX)",
                      direction, static_cast<unsigned long long>(sizeInBytes));
        sc::log::Write(sc::log::Level::Info, "xmem", __FUNCTION__, buf);
    }
    return TRUE;
}

size_t WINAPI XMemVirtualQuery(const void* lpAddress, MEMORY_BASIC_INFORMATION* lpBuffer, size_t dwLength) {
    return VirtualQuery(lpAddress, lpBuffer, dwLength);
}

} // extern "C"