#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc::loader {

enum class SymbolStatus : uint8_t {
    Resolved    = 0,
    AutoStubbed = 1,
    Unresolved  = 2,
};

class ImportTelemetry {
public:
    static constexpr size_t kRingCapacity = 128; // Power of 2
    static constexpr size_t kMaxDllLen    = 48;
    static constexpr size_t kMaxSymLen    = 96;

    static ImportTelemetry& Instance() noexcept {
        // Immortal singleton avoids static destruction races during DLL_PROCESS_DETACH.
        static auto* inst = new ImportTelemetry();
        return *inst;
    }

    // Loader-lock safe initialization using raw Win32 APIs (no std::filesystem).
    bool InitializeFromEnv(HMODULE targetExe) noexcept {
        wchar_t pathBuf[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"SCARLETT_TRACE_OUT", pathBuf, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            m_enabled.store(false, std::memory_order_release);
            return false;
        }

        bool installVeh = true;
        wchar_t vehBuf[16]{};
        if (GetEnvironmentVariableW(L"SCARLETT_TRACE_VEH", vehBuf, 16) > 0) {
            if (std::wcscmp(vehBuf, L"0") == 0 || _wcsicmp(vehBuf, L"false") == 0) {
                installVeh = false;
            }
        }
        return Enable(pathBuf, targetExe, installVeh);
    }

    bool Enable(const wchar_t* outputPathW, HMODULE targetExe, bool installVeh) noexcept {
        if (!outputPathW || outputPathW[0] == L'\0') {
            return false;
        }
        CopyTruncatedW(m_outputPath, MAX_PATH, outputPathW);
        CaptureModuleRange(targetExe ? targetExe : GetModuleHandleW(nullptr),
                           m_targetBase, m_targetSize);

        if (installVeh && !m_vehHandle) {
            m_vehHandle = AddVectoredExceptionHandler(1, &VehHandler);
        } else if (!installVeh && m_vehHandle) {
            RemoveVectoredExceptionHandler(m_vehHandle);
            m_vehHandle = nullptr;
        }

        m_flushed.store(false, std::memory_order_release);
        m_enabled.store(true, std::memory_order_release);
        return true;
    }

    void Disable() noexcept {
        m_enabled.store(false, std::memory_order_release);
        if (m_vehHandle) {
            RemoveVectoredExceptionHandler(m_vehHandle);
            m_vehHandle = nullptr;
        }
    }

    void Shutdown() noexcept {
        if (!m_enabled.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        bool expected = false;
        if (m_flushed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            WriteReport("detach", nullptr, false);
        }
        if (m_vehHandle) {
            RemoveVectoredExceptionHandler(m_vehHandle);
            m_vehHandle = nullptr;
        }
    }

    void ResetForTests() noexcept {
        Disable();
        AcquireSRWLockExclusive(&m_lock);
        m_symbols.clear();
        m_symbolIndex.clear();
        m_redirects.clear();
        m_redirectIndex.clear();
        ReleaseSRWLockExclusive(&m_lock);

        for (auto& slot : m_ring) {
            slot.seq.store(0, std::memory_order_relaxed);
        }
        m_ringTicket.store(0, std::memory_order_relaxed);
        m_outputPath[0] = L'\0';
        m_flushed.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool IsEnabled() const noexcept {
        return m_enabled.load(std::memory_order_relaxed);
    }

    void RecordRedirect(const char* apiName,
                        const char* requestedDll,
                        const char* targetPath,
                        const void* callerPc) noexcept {
        if (!IsEnabled()) {
            return;
        }
        bool inTarget = false;
        const uintptr_t rva = ComputeRva(callerPc, inTarget);
        const std::string key = std::string(apiName ? apiName : "") + "|" +
                                (requestedDll ? requestedDll : "");

        AcquireSRWLockExclusive(&m_lock);
        auto it = m_redirectIndex.find(key);
        if (it == m_redirectIndex.end()) {
            auto entry = std::make_unique<RedirectEntry>();
            entry->api = apiName ? apiName : "";
            entry->requested = requestedDll ? requestedDll : "";
            entry->target = targetPath ? targetPath : "";
            entry->count = 1;
            entry->firstCallerRva = rva;
            entry->firstCallerInTarget = inTarget;
            m_redirectIndex.emplace(key, m_redirects.size());
            m_redirects.push_back(std::move(entry));
        } else {
            m_redirects[it->second]->count += 1;
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    void RecordSymbolLookup(const char* dllName,
                            const char* symbolName,
                            uint16_t ordinal,
                            SymbolStatus status,
                            uint64_t defaultReturn,
                            const void* callerPc) noexcept {
        if (!IsEnabled()) {
            return;
        }
        bool inTarget = false;
        const uintptr_t rva = ComputeRva(callerPc, inTarget);
        const std::string key = MakeSymbolKey(dllName, symbolName, ordinal);

        if (status != SymbolStatus::Resolved) {
            char ordBuf[16]{};
            const char* ringSym = symbolName;
            if (!ringSym || ringSym[0] == '\0') {
                std::snprintf(ordBuf, sizeof(ordBuf), "#%u", static_cast<unsigned>(ordinal));
                ringSym = ordBuf;
            }
            PushRing(dllName, ringSym, status, rva, inTarget);
        }

        AcquireSRWLockExclusive(&m_lock);
        auto it = m_symbolIndex.find(key);
        if (it == m_symbolIndex.end()) {
            auto entry = std::make_unique<SymbolEntry>();
            entry->dll = dllName ? dllName : "<unknown>";
            entry->symbol = FormatSymbolField(symbolName, ordinal);
            entry->ordinal = ordinal;
            entry->status = status;
            entry->defaultReturn.store(defaultReturn, std::memory_order_relaxed);
            entry->lookupCount.store(1, std::memory_order_relaxed);
            entry->firstCallerRva.store(rva, std::memory_order_relaxed);
            entry->firstCallerInTarget.store(inTarget, std::memory_order_relaxed);
            m_symbolIndex.emplace(key, m_symbols.size());
            m_symbols.push_back(std::move(entry));
        } else {
            auto& entry = *m_symbols[it->second];
            entry.lookupCount.fetch_add(1, std::memory_order_relaxed);
            if (ordinal != 0 && entry.ordinal == 0) {
                entry.ordinal = ordinal;
            }
            entry.status = status;
            if (status == SymbolStatus::AutoStubbed) {
                entry.defaultReturn.store(defaultReturn, std::memory_order_relaxed);
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    void RecordRuntimeStubCall(const char* dllName,
                               const char* symbolName,
                               uint64_t returnValue,
                               const void* callerPc) noexcept {
        if (!IsEnabled()) {
            return;
        }
        bool inTarget = false;
        const uintptr_t rva = ComputeRva(callerPc, inTarget);
        PushRing(dllName, symbolName, SymbolStatus::AutoStubbed, rva, inTarget);

        const uint16_t parsedOrd = ParseOrdinalSymbol(symbolName);
        const std::string key = MakeSymbolKey(dllName, symbolName, parsedOrd);

        AcquireSRWLockShared(&m_lock);
        auto it = m_symbolIndex.find(key);
        if (it != m_symbolIndex.end()) {
            auto& entry = *m_symbols[it->second];
            entry.hitCount.fetch_add(1, std::memory_order_relaxed);
            entry.defaultReturn.store(returnValue, std::memory_order_relaxed);
            uintptr_t expectedZero = 0;
            if (entry.firstCallRva.compare_exchange_strong(expectedZero, rva, std::memory_order_relaxed)) {
                entry.firstCallInTarget.store(inTarget, std::memory_order_relaxed);
            }
            ReleaseSRWLockShared(&m_lock);
            return;
        }
        ReleaseSRWLockShared(&m_lock);

        AcquireSRWLockExclusive(&m_lock);
        auto exIt = m_symbolIndex.find(key);
        if (exIt == m_symbolIndex.end()) {
            auto entry = std::make_unique<SymbolEntry>();
            entry->dll = dllName ? dllName : "<unknown>";
            entry->symbol = FormatSymbolField(symbolName, parsedOrd);
            entry->ordinal = parsedOrd;
            entry->status = SymbolStatus::AutoStubbed;
            entry->defaultReturn.store(returnValue, std::memory_order_relaxed);
            entry->hitCount.store(1, std::memory_order_relaxed);
            entry->firstCallRva.store(rva, std::memory_order_relaxed);
            entry->firstCallInTarget.store(inTarget, std::memory_order_relaxed);
            m_symbolIndex.emplace(key, m_symbols.size());
            m_symbols.push_back(std::move(entry));
        } else {
            auto& entry = *m_symbols[exIt->second];
            entry.hitCount.fetch_add(1, std::memory_order_relaxed);
            entry.defaultReturn.store(returnValue, std::memory_order_relaxed);
            uintptr_t expectedZero = 0;
            if (entry.firstCallRva.compare_exchange_strong(expectedZero, rva, std::memory_order_relaxed)) {
                entry.firstCallInTarget.store(inTarget, std::memory_order_relaxed);
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    bool Flush(const char* reasonUtf8) noexcept {
        if (!IsEnabled()) {
            return false;
        }
        return WriteReport(reasonUtf8 ? reasonUtf8 : "manual", nullptr, false);
    }

    void GetStats(uint32_t* outUniqueSymbols,
                  uint64_t* outTotalStubCalls,
                  uint32_t* outRedirects) noexcept {
        AcquireSRWLockShared(&m_lock);
        if (outUniqueSymbols) {
            *outUniqueSymbols = static_cast<uint32_t>(m_symbols.size());
        }
        if (outTotalStubCalls) {
            uint64_t total = 0;
            for (const auto& sym : m_symbols) {
                total += sym->hitCount.load(std::memory_order_relaxed);
            }
            *outTotalStubCalls = total;
        }
        if (outRedirects) {
            *outRedirects = static_cast<uint32_t>(m_redirects.size());
        }
        ReleaseSRWLockShared(&m_lock);
    }

private:
    ImportTelemetry() = default;

    struct SymbolEntry {
        std::string           dll;
        std::string           symbol;
        uint16_t              ordinal{0};
        SymbolStatus          status{SymbolStatus::Resolved};
        std::atomic<uint64_t> defaultReturn{0};
        std::atomic<uint64_t> lookupCount{0};
        std::atomic<uint64_t> hitCount{0};
        std::atomic<uintptr_t> firstCallerRva{0};
        std::atomic<bool>     firstCallerInTarget{false};
        std::atomic<uintptr_t> firstCallRva{0};
        std::atomic<bool>     firstCallInTarget{false};
    };

    struct RedirectEntry {
        std::string api;
        std::string requested;
        std::string target;
        uint64_t    count{0};
        uintptr_t   firstCallerRva{0};
        bool        firstCallerInTarget{false};
    };

    struct RingSlot {
        std::atomic<uint64_t> seq{0}; // 0 = empty, odd = writing, even = committed
        char                  dll[kMaxDllLen]{};
        char                  symbol[kMaxSymLen]{};
        SymbolStatus          status{SymbolStatus::AutoStubbed};
        uint32_t              tid{0};
        uintptr_t             callerRva{0};
        bool                  inTarget{false};
    };

    struct CrashInfo {
        DWORD     exceptionCode{0};
        DWORD     threadId{0};
        uintptr_t faultPc{0};
        uintptr_t faultRva{0};
        bool      faultInTarget{false};
        ULONG_PTR accessType{0};
        uintptr_t accessAddress{0};
    };

    class SafeJsonWriter {
    public:
        explicit SafeJsonWriter(HANDLE file) noexcept : m_file(file) {}
        ~SafeJsonWriter() { Flush(); }

        void WriteRaw(const char* data, size_t len) noexcept {
            if (m_file == INVALID_HANDLE_VALUE || !data || len == 0) {
                return;
            }
            size_t offset = 0;
            while (offset < len) {
                const size_t avail = sizeof(m_buf) - m_pos;
                const size_t chunk = (len - offset < avail) ? (len - offset) : avail;
                std::memcpy(m_buf + m_pos, data + offset, chunk);
                m_pos += chunk;
                offset += chunk;
                if (m_pos == sizeof(m_buf)) {
                    Flush();
                }
            }
        }

        void WriteStr(const char* s) noexcept {
            if (s) {
                WriteRaw(s, std::strlen(s));
            }
        }

        void WriteJsonString(const char* s) noexcept {
            WriteRaw("\"", 1);
            if (s) {
                for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
                    switch (*p) {
                        case '"':  WriteRaw("\\\"", 2); break;
                        case '\\': WriteRaw("\\\\", 2); break;
                        case '\b': WriteRaw("\\b", 2);  break;
                        case '\f': WriteRaw("\\f", 2);  break;
                        case '\n': WriteRaw("\\n", 2);  break;
                        case '\r': WriteRaw("\\r", 2);  break;
                        case '\t': WriteRaw("\\t", 2);  break;
                        default:
                            if (*p < 0x20) {
                                char esc[8];
                                const int n = std::snprintf(esc, sizeof(esc), "\\u%04x",
                                                            static_cast<unsigned>(*p));
                                if (n > 0) {
                                    WriteRaw(esc, static_cast<size_t>(n));
                                }
                            } else {
                                WriteRaw(reinterpret_cast<const char*>(p), 1);
                            }
                            break;
                    }
                }
            }
            WriteRaw("\"", 1);
        }

        void WriteUInt64(uint64_t v) noexcept {
            char num[32];
            const int n = std::snprintf(num, sizeof(num), "%llu",
                                        static_cast<unsigned long long>(v));
            if (n > 0) {
                WriteRaw(num, static_cast<size_t>(n));
            }
        }

        void WriteHexString(uint64_t v) noexcept {
            char num[32];
            const int n = std::snprintf(num, sizeof(num), "\"0x%llx\"",
                                        static_cast<unsigned long long>(v));
            if (n > 0) {
                WriteRaw(num, static_cast<size_t>(n));
            }
        }

        void WriteBool(bool v) noexcept {
            WriteStr(v ? "true" : "false");
        }

        void Flush() noexcept {
            if (m_file == INVALID_HANDLE_VALUE || m_pos == 0) {
                return;
            }
            size_t writtenTotal = 0;
            while (writtenTotal < m_pos) {
                DWORD written = 0;
                if (!WriteFile(m_file, m_buf + writtenTotal,
                               static_cast<DWORD>(m_pos - writtenTotal), &written, nullptr)) {
                    break;
                }
                if (written == 0) {
                    break;
                }
                writtenTotal += written;
            }
            m_pos = 0;
        }

    private:
        HANDLE m_file{INVALID_HANDLE_VALUE};
        char   m_buf[4096]{};
        size_t m_pos{0};
    };

    static void CopyTruncatedA(char* dst, size_t cap, const char* src) noexcept {
        if (!dst || cap == 0) {
            return;
        }
        if (!src) {
            dst[0] = '\0';
            return;
        }
        size_t i = 0;
        for (; i + 1 < cap && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
        dst[i] = '\0';
    }

    static void CopyTruncatedW(wchar_t* dst, size_t cap, const wchar_t* src) noexcept {
        if (!dst || cap == 0) {
            return;
        }
        if (!src) {
            dst[0] = L'\0';
            return;
        }
        size_t i = 0;
        for (; i + 1 < cap && src[i] != L'\0'; ++i) {
            dst[i] = src[i];
        }
        dst[i] = L'\0';
    }

    static const char* StatusToStr(SymbolStatus st) noexcept {
        switch (st) {
            case SymbolStatus::Resolved:    return "resolved";
            case SymbolStatus::AutoStubbed: return "auto_stubbed";
            case SymbolStatus::Unresolved:  return "unresolved";
        }
        return "unknown";
    }

    static uint16_t ParseOrdinalSymbol(const char* sym) noexcept {
        if (!sym || sym[0] != '#' || sym[1] == '\0') {
            return 0;
        }
        uint32_t val = 0;
        for (const char* p = sym + 1; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                return 0;
            }
            val = val * 10u + static_cast<uint32_t>(*p - '0');
            if (val > 0xFFFFu) {
                return 0;
            }
        }
        return static_cast<uint16_t>(val);
    }

    static std::string FormatSymbolField(const char* sym, uint16_t ordinal) {
        if (sym && sym[0] != '\0') {
            return std::string(sym);
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%u", static_cast<unsigned>(ordinal));
        return std::string(buf);
    }

    static std::string MakeSymbolKey(const char* dll, const char* sym, uint16_t ordinal) {
        std::string key(dll ? dll : "<unknown>");
        key.push_back('!');
        key.append(FormatSymbolField(sym, ordinal));
        return key;
    }

    static void CaptureModuleRange(HMODULE mod, uintptr_t& baseOut, size_t& sizeOut) noexcept {
        baseOut = 0;
        sizeOut = 0;
        if (!mod) {
            return;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(mod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
            return;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return;
        }
        baseOut = reinterpret_cast<uintptr_t>(mod);
        sizeOut = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    }

    uintptr_t ComputeRva(const void* addr, bool& inTarget) const noexcept {
        inTarget = false;
        if (!addr) {
            return 0;
        }
        const auto pc = reinterpret_cast<uintptr_t>(addr);
        if (m_targetBase != 0 && pc >= m_targetBase && pc < m_targetBase + m_targetSize) {
            inTarget = true;
            return pc - m_targetBase;
        }
        return pc;
    }

    void PushRing(const char* dll,
                  const char* sym,
                  SymbolStatus status,
                  uintptr_t callerRva,
                  bool inTarget) noexcept {
        const uint64_t ticket = m_ringTicket.fetch_add(1, std::memory_order_acq_rel);
        auto& slot = m_ring[ticket & (kRingCapacity - 1)];
        const uint64_t oddSeq = (ticket * 2ULL) + 1ULL;
        slot.seq.store(oddSeq, std::memory_order_release);
        CopyTruncatedA(slot.dll, kMaxDllLen, dll ? dll : "<unknown>");
        CopyTruncatedA(slot.symbol, kMaxSymLen, sym ? sym : "");
        slot.status = status;
        slot.tid = GetCurrentThreadId();
        slot.callerRva = callerRva;
        slot.inTarget = inTarget;
        slot.seq.store(oddSeq + 1ULL, std::memory_order_release);
    }

    bool WriteReport(const char* reason, const CrashInfo* crash, bool nonBlockingLock) noexcept {
        if (m_outputPath[0] == L'\0') {
            return false;
        }

        HANDLE file = CreateFileW(m_outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        SafeJsonWriter w(file);
        w.WriteStr("{\n");
        w.WriteStr("  \"schema_version\": 1,\n");
        w.WriteStr("  \"flush_reason\": ");
        w.WriteJsonString(reason ? reason : "unknown");
        w.WriteStr(",\n  \"target_image_base\": ");
        w.WriteHexString(static_cast<uint64_t>(m_targetBase));
        w.WriteStr(",\n  \"target_image_size\": ");
        w.WriteUInt64(static_cast<uint64_t>(m_targetSize));

        if (crash) {
            w.WriteStr(",\n  \"crash\": {\n");
            w.WriteStr("    \"exception_code\": ");
            w.WriteHexString(static_cast<uint64_t>(crash->exceptionCode));
            w.WriteStr(",\n    \"thread_id\": ");
            w.WriteUInt64(crash->threadId);
            w.WriteStr(",\n    \"fault_pc\": ");
            w.WriteHexString(static_cast<uint64_t>(crash->faultPc));
            w.WriteStr(",\n    \"fault_rva\": ");
            w.WriteHexString(static_cast<uint64_t>(crash->faultRva));
            w.WriteStr(",\n    \"fault_in_target\": ");
            w.WriteBool(crash->faultInTarget);
            if (crash->exceptionCode == EXCEPTION_ACCESS_VIOLATION) {
                w.WriteStr(",\n    \"access_type\": ");
                w.WriteUInt64(static_cast<uint64_t>(crash->accessType));
                w.WriteStr(",\n    \"access_address\": ");
                w.WriteHexString(static_cast<uint64_t>(crash->accessAddress));
            }
            w.WriteStr("\n  }");
        }

        bool locked = false;
        if (nonBlockingLock) {
            locked = (TryAcquireSRWLockShared(&m_lock) != 0);
        } else {
            AcquireSRWLockShared(&m_lock);
            locked = true;
        }

        w.WriteStr(",\n  \"ledger_locked\": ");
        w.WriteBool(locked);
        w.WriteStr(",\n  \"redirects\": [\n");
        if (locked) {
            for (size_t i = 0; i < m_redirects.size(); ++i) {
                const auto& r = *m_redirects[i];
                w.WriteStr("    {\"api\": ");
                w.WriteJsonString(r.api.c_str());
                w.WriteStr(", \"requested\": ");
                w.WriteJsonString(r.requested.c_str());
                w.WriteStr(", \"target\": ");
                w.WriteJsonString(r.target.c_str());
                w.WriteStr(", \"count\": ");
                w.WriteUInt64(r.count);
                w.WriteStr(", \"first_caller_rva\": ");
                w.WriteHexString(static_cast<uint64_t>(r.firstCallerRva));
                w.WriteStr(", \"in_target\": ");
                w.WriteBool(r.firstCallerInTarget);
                w.WriteStr(i + 1 < m_redirects.size() ? "},\n" : "}\n");
            }
        }
        w.WriteStr("  ],\n  \"imports\": [\n");
        if (locked) {
            for (size_t i = 0; i < m_symbols.size(); ++i) {
                const auto& s = *m_symbols[i];
                const uintptr_t callRva = s.firstCallRva.load(std::memory_order_relaxed);
                const bool useCallRva = (callRva != 0);
                const uintptr_t bestRva = useCallRva
                    ? callRva
                    : s.firstCallerRva.load(std::memory_order_relaxed);
                const bool bestInTarget = useCallRva
                    ? s.firstCallInTarget.load(std::memory_order_relaxed)
                    : s.firstCallerInTarget.load(std::memory_order_relaxed);

                w.WriteStr("    {\"dll\": ");
                w.WriteJsonString(s.dll.c_str());
                w.WriteStr(", \"symbol\": ");
                w.WriteJsonString(s.symbol.c_str());
                w.WriteStr(", \"ordinal\": ");
                w.WriteUInt64(s.ordinal);
                w.WriteStr(", \"status\": ");
                w.WriteJsonString(StatusToStr(s.status));
                w.WriteStr(", \"lookups\": ");
                w.WriteUInt64(s.lookupCount.load(std::memory_order_relaxed));
                w.WriteStr(", \"hits\": ");
                w.WriteUInt64(s.hitCount.load(std::memory_order_relaxed));
                w.WriteStr(", \"default_return\": ");
                w.WriteHexString(s.defaultReturn.load(std::memory_order_relaxed));
                w.WriteStr(", \"first_caller_rva\": ");
                w.WriteHexString(static_cast<uint64_t>(bestRva));
                w.WriteStr(", \"in_target\": ");
                w.WriteBool(bestInTarget);
                w.WriteStr(i + 1 < m_symbols.size() ? "},\n" : "}\n");
            }
            ReleaseSRWLockShared(&m_lock);
        }
        w.WriteStr("  ],\n  \"recent_calls\": [\n");

        const uint64_t total = m_ringTicket.load(std::memory_order_acquire);
        const uint64_t start = (total > kRingCapacity) ? (total - kRingCapacity) : 0;
        bool firstOut = true;
        for (uint64_t t = start; t < total; ++t) {
            const auto& slot = m_ring[t & (kRingCapacity - 1)];
            const uint64_t s1 = slot.seq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1ULL) != 0) {
                continue;
            }
            char dllCopy[kMaxDllLen];
            char symCopy[kMaxSymLen];
            std::memcpy(dllCopy, slot.dll, sizeof(dllCopy));
            std::memcpy(symCopy, slot.symbol, sizeof(symCopy));
            dllCopy[kMaxDllLen - 1] = '\0';
            symCopy[kMaxSymLen - 1] = '\0';
            const SymbolStatus st = slot.status;
            const uint32_t tid = slot.tid;
            const uintptr_t rva = slot.callerRva;
            const bool inTarget = slot.inTarget;

            const uint64_t s2 = slot.seq.load(std::memory_order_acquire);
            if (s1 != s2) {
                continue;
            }
            if (!firstOut) {
                w.WriteStr(",\n");
            }
            firstOut = false;
            w.WriteStr("    {\"seq\": ");
            w.WriteUInt64(t);
            w.WriteStr(", \"tid\": ");
            w.WriteUInt64(tid);
            w.WriteStr(", \"dll\": ");
            w.WriteJsonString(dllCopy);
            w.WriteStr(", \"symbol\": ");
            w.WriteJsonString(symCopy);
            w.WriteStr(", \"status\": ");
            w.WriteJsonString(StatusToStr(st));
            w.WriteStr(", \"caller_rva\": ");
            w.WriteHexString(static_cast<uint64_t>(rva));
            w.WriteStr(", \"in_target\": ");
            w.WriteBool(inTarget);
            w.WriteStr("}");
        }
        if (!firstOut) {
            w.WriteStr("\n");
        }
        w.WriteStr("  ]\n}\n");
        w.Flush();
        CloseHandle(file);
        return true;
    }

    static bool IsFatalException(DWORD code) noexcept {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
                return true;
            default:
                return false;
        }
    }

    static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) noexcept {
        if (!ep || !ep->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (!IsFatalException(code)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        auto& self = Instance();
        if (!self.IsEnabled()) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        bool expected = false;
        if (!self.m_flushed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CrashInfo crash{};
        crash.exceptionCode = code;
        crash.threadId = GetCurrentThreadId();
        crash.faultPc = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        crash.faultRva = self.ComputeRva(ep->ExceptionRecord->ExceptionAddress, crash.faultInTarget);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            crash.accessType = ep->ExceptionRecord->ExceptionInformation[0];
            crash.accessAddress = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        }

        self.WriteReport("veh_exception", &crash, true);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::atomic<bool>                          m_enabled{false};
    std::atomic<bool>                          m_flushed{false};
    SRWLOCK                                    m_lock{SRWLOCK_INIT};
    std::vector<std::unique_ptr<SymbolEntry>>  m_symbols;
    std::unordered_map<std::string, size_t>    m_symbolIndex;
    std::vector<std::unique_ptr<RedirectEntry>> m_redirects;
    std::unordered_map<std::string, size_t>    m_redirectIndex;
    std::array<RingSlot, kRingCapacity>        m_ring{};
    std::atomic<uint64_t>                      m_ringTicket{0};
    wchar_t                                    m_outputPath[MAX_PATH]{};
    uintptr_t                                  m_targetBase{0};
    size_t                                     m_targetSize{0};
    PVOID                                      m_vehHandle{nullptr};
};

} // namespace sc::loader
