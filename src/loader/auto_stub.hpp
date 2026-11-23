#pragma once

#include "common/stub.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sc::loader {

struct AutoStubMeta {
    std::string           dllName;
    std::string           symbolName;
    std::string           key;
    uint64_t              defaultReturn{0x80004001ULL}; // E_NOTIMPL
    void*                 thunkAddress{nullptr};
    std::atomic<uint64_t> callCount{0};
};

using AutoStubCallObserver = void(*)(const AutoStubMeta& meta, const void* callerPc) noexcept;

inline std::atomic<AutoStubCallObserver>& AutoStubObserverSlot() noexcept {
    static std::atomic<AutoStubCallObserver> s_observer{nullptr};
    return s_observer;
}

inline void SetAutoStubCallObserver(AutoStubCallObserver observer) noexcept {
    AutoStubObserverSlot().store(observer, std::memory_order_release);
}

inline uint64_t WINAPI ScarlettAutoStubDispatch(AutoStubMeta* meta, const void* callerPc) {
    if (!meta) {
        return 0x80004001ULL;
    }
    const uint64_t hit = meta->callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (auto observer = AutoStubObserverSlot().load(std::memory_order_acquire)) {
        observer(*meta, callerPc);
    }
    if (hit == 1) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "UNIMPLEMENTED %s!%s -> auto-stub returning 0x%llX",
                      meta->dllName.c_str(), meta->symbolName.c_str(),
                      static_cast<unsigned long long>(meta->defaultReturn));
        sc::log::Write(sc::log::Level::Warn, "auto_stub", "Dispatch", buf);
    }
    return meta->defaultReturn;
}

class AutoStubEngine {
public:
    static constexpr size_t kSlabSize    = 64 * 1024; // 64 KiB -> 1,365 x64 thunks per slab
    static constexpr size_t kThunkStride = 48;        // 16-byte aligned

    ~AutoStubEngine() {
        for (void* slab : m_slabs) {
            VirtualFree(slab, 0, MEM_RELEASE);
        }
        m_slabs.clear();
    }

    static bool IsExcludedProbeSymbol(const std::string& symbol) {
        return symbol == "DllMain" ||
               symbol == "DllGetClassObject" ||
               symbol == "DllCanUnloadNow" ||
               symbol == "DllRegisterServer" ||
               symbol == "DllUnregisterServer";
    }

    static uint64_t DefaultPolicyFor(const std::string& symbol) {
        if (symbol.rfind("PIX", 0) == 0 ||
            symbol.rfind("Etw", 0) == 0 ||
            symbol.rfind("EventRegister", 0) == 0 ||
            symbol.rfind("EventUnregister", 0) == 0 ||
            symbol.rfind("EventWrite", 0) == 0) {
            return 0ULL; // S_OK / ERROR_SUCCESS for telemetry/profiler calls
        }
        return 0x80004001ULL; // E_NOTIMPL
    }

    void SetPolicyOverride(const std::string& symbolOrKey, uint64_t returnValue) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_policyOverrides[symbolOrKey] = returnValue;
        for (auto& meta : m_metas) {
            if (meta.key == symbolOrKey || meta.symbolName == symbolOrKey) {
                meta.defaultReturn = returnValue;
            }
        }
    }

    FARPROC GetOrCreate(const std::string& dllLower, const std::string& symbol) {
        if (dllLower.empty() || symbol.empty() || IsExcludedProbeSymbol(symbol)) {
            return nullptr;
        }
        const std::string key = dllLower + "!" + symbol;

        std::lock_guard<std::mutex> lock(m_mtx);
        if (auto it = m_byKey.find(key); it != m_byKey.end()) {
            return reinterpret_cast<FARPROC>(it->second->thunkAddress);
        }

        void* thunkMem = AllocateThunkSlotLocked();
        if (!thunkMem) {
            return nullptr;
        }

        uint64_t retVal = DefaultPolicyFor(symbol);
        if (auto pit = m_policyOverrides.find(key); pit != m_policyOverrides.end()) {
            retVal = pit->second;
        } else if (auto sit = m_policyOverrides.find(symbol); sit != m_policyOverrides.end()) {
            retVal = sit->second;
        }

        m_metas.emplace_back();
        AutoStubMeta& meta = m_metas.back();
        meta.dllName       = dllLower;
        meta.symbolName    = symbol;
        meta.key           = key;
        meta.defaultReturn = retVal;
        meta.thunkAddress  = thunkMem;

        if (!EmitTrampolineLocked(thunkMem, &meta)) {
            m_metas.pop_back();
            return nullptr;
        }

        m_byKey.emplace(key, &meta);
        return reinterpret_cast<FARPROC>(thunkMem);
    }

    void GetStats(uint32_t* outUniqueStubs, uint64_t* outTotalCalls) const {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (outUniqueStubs) {
            *outUniqueStubs = static_cast<uint32_t>(m_metas.size());
        }
        if (outTotalCalls) {
            uint64_t total = 0;
            for (const auto& meta : m_metas) {
                total += meta.callCount.load(std::memory_order_relaxed);
            }
            *outTotalCalls = total;
        }
    }

    uint32_t DumpHotlist(const char* reportPathUtf8, bool logEntries) const {
        struct Entry {
            std::string key;
            uint64_t    calls;
            uint64_t    retVal;
        };
        std::vector<Entry> called;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            called.reserve(m_metas.size());
            for (const auto& meta : m_metas) {
                const uint64_t c = meta.callCount.load(std::memory_order_relaxed);
                if (c > 0) {
                    called.push_back(Entry{ meta.key, c, meta.defaultReturn });
                }
            }
        }
        std::sort(called.begin(), called.end(), [](const Entry& a, const Entry& b) {
            if (a.calls != b.calls) return a.calls > b.calls;
            return a.key < b.key;
        });

        if (logEntries && !called.empty()) {
            char header[128];
            std::snprintf(header, sizeof(header),
                          "-> Missing Export Hotlist (%zu unique auto-stub(s) invoked):",
                          called.size());
            sc::log::Write(sc::log::Level::Warn, "auto_stub", "Hotlist", header);
            const size_t limit = (std::min)(called.size(), size_t{20});
            for (size_t i = 0; i < limit; ++i) {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "   #%zu %s (calls=%llu, ret=0x%llX)",
                              i + 1, called[i].key.c_str(),
                              static_cast<unsigned long long>(called[i].calls),
                              static_cast<unsigned long long>(called[i].retVal));
                sc::log::Write(sc::log::Level::Warn, "auto_stub", "Hotlist", line);
            }
        }

        if (reportPathUtf8 && reportPathUtf8[0] != '\0') {
            std::ofstream out(reportPathUtf8, std::ios::trunc);
            if (out) {
                out << "rank\tsymbol\tcalls\tdefault_return\n";
                for (size_t i = 0; i < called.size(); ++i) {
                    char hexRet[32];
                    std::snprintf(hexRet, sizeof(hexRet), "0x%llX",
                                  static_cast<unsigned long long>(called[i].retVal));
                    out << (i + 1) << '\t' << called[i].key << '\t'
                        << called[i].calls << '\t' << hexRet << '\n';
                }
            }
        }

        return static_cast<uint32_t>(called.size());
    }

private:
    void* AllocateThunkSlotLocked() {
        if (m_slabs.empty() || m_slabOffset + kThunkStride > kSlabSize) {
            void* slab = VirtualAlloc(nullptr, kSlabSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!slab) {
                return nullptr;
            }
            std::memset(slab, 0xCC, kSlabSize);
            DWORD oldProt = 0;
            VirtualProtect(slab, kSlabSize, PAGE_EXECUTE_READ, &oldProt);
            m_slabs.push_back(slab);
            m_slabOffset = 0;
        }
        auto* base = static_cast<uint8_t*>(m_slabs.back());
        void* slot = base + m_slabOffset;
        m_slabOffset += kThunkStride;
        return slot;
    }

    bool EmitTrampolineLocked(void* slot, AutoStubMeta* meta) {
        void* slab = m_slabs.back();
        DWORD oldProt = 0;
        if (!VirtualProtect(slab, kSlabSize, PAGE_READWRITE, &oldProt)) {
            return false;
        }

        // Strict x64 Windows ABI trampoline (39 bytes + 9x INT3 = 48 bytes):
        //   sub rsp, 0x28          ; align stack to 16B + 32B shadow space
        //   mov rdx, [rsp + 0x28]  ; arg2 = caller return address
        //   mov rcx, <meta*>       ; arg1 = AutoStubMeta*
        //   mov rax, <dispatch*>   ; target = ScarlettAutoStubDispatch
        //   call rax               ; rax = defaultReturn
        //   xorps xmm0, xmm0       ; zero float return register safely
        //   add rsp, 0x28
        //   ret
        uint8_t code[kThunkStride];
        std::memset(code, 0xCC, sizeof(code));

        const uint64_t metaAddr     = reinterpret_cast<uint64_t>(meta);
        const uint64_t dispatchAddr = reinterpret_cast<uint64_t>(&ScarlettAutoStubDispatch);

        size_t o = 0;
        code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xEC; code[o++] = 0x28; // sub rsp, 0x28
        code[o++] = 0x48; code[o++] = 0x8B; code[o++] = 0x54; code[o++] = 0x24; code[o++] = 0x28; // mov rdx, [rsp+0x28]
        code[o++] = 0x48; code[o++] = 0xB9;                                     // mov rcx, imm64
        std::memcpy(code + o, &metaAddr, sizeof(metaAddr)); o += sizeof(metaAddr);
        code[o++] = 0x48; code[o++] = 0xB8;                                     // mov rax, imm64
        std::memcpy(code + o, &dispatchAddr, sizeof(dispatchAddr)); o += sizeof(dispatchAddr);
        code[o++] = 0xFF; code[o++] = 0xD0;                                     // call rax
        code[o++] = 0x0F; code[o++] = 0x57; code[o++] = 0xC0;                   // xorps xmm0, xmm0
        code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xC4; code[o++] = 0x28; // add rsp, 0x28
        code[o++] = 0xC3;                                                       // ret

        std::memcpy(slot, code, sizeof(code));

        DWORD restoreProt = 0;
        const bool ok = VirtualProtect(slab, kSlabSize, PAGE_EXECUTE_READ, &restoreProt) != FALSE;
        FlushInstructionCache(GetCurrentProcess(), slot, kThunkStride);
        return ok;
    }

    mutable std::mutex                             m_mtx;
    std::vector<void*>                             m_slabs;
    size_t                                         m_slabOffset{0};
    std::deque<AutoStubMeta>                       m_metas;
    std::unordered_map<std::string, AutoStubMeta*> m_byKey;
    std::unordered_map<std::string, uint64_t>      m_policyOverrides;
};

} // namespace sc::loader