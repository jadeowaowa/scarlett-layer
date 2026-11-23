#include "gdk_types.hpp"
#include "runtime_internal.hpp"
#include "common/stub.hpp"

#include <unknwn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

// Forward declarations of existing flat C shim exports implemented in xgameruntime.cpp / task_queue.cpp / game_save.cpp
extern "C" {
HRESULT WINAPI XGameRuntimeInitialize();
void    WINAPI XGameRuntimeUninitialize();
HRESULT WINAPI XTaskQueueCreate(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*);
bool    WINAPI XTaskQueueDispatch(XTaskQueueHandle, XTaskQueuePort, uint32_t);
void    WINAPI XTaskQueueCloseHandle(XTaskQueueHandle);
HRESULT WINAPI XAsyncGetStatus(XAsyncBlock*, bool);
HRESULT WINAPI XUserAddAsync(XUserAddOptions, XAsyncBlock*);
HRESULT WINAPI XUserAddResult(XAsyncBlock*, XUserHandle*);
HRESULT WINAPI XUserGetGamertag(XUserHandle, XUserGamertagComponent, size_t, char*, size_t*);
HRESULT WINAPI XUserGetId(XUserHandle, uint64_t*);
HRESULT WINAPI XUserGetState(XUserHandle, XUserState*);
HRESULT WINAPI XUserGetLocalId(XUserHandle, XUserLocalId*);
void    WINAPI XUserCloseHandle(XUserHandle);
HRESULT WINAPI XGameSaveInitializeProviderAsync(XUserHandle, const char*, bool, XAsyncBlock*);
HRESULT WINAPI XGameSaveInitializeProviderResult(XAsyncBlock*, XGameSaveProviderHandle*);
void    WINAPI XGameSaveCloseProvider(XGameSaveProviderHandle);
HRESULT WINAPI XGameSaveCreateContainer(XGameSaveProviderHandle, const char*, XGameSaveContainerHandle*);
void    WINAPI XGameSaveCloseContainer(XGameSaveContainerHandle);
HRESULT WINAPI XGameSaveCreateUpdate(XGameSaveContainerHandle, const char*, XGameSaveUpdateHandle*);
void    WINAPI XGameSaveCloseUpdate(XGameSaveUpdateHandle);
HRESULT WINAPI XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle, const char*, const uint8_t*, size_t);
HRESULT WINAPI XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle, const char*);
HRESULT WINAPI XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle, XAsyncBlock*);
HRESULT WINAPI XGameSaveSubmitUpdateResult(XAsyncBlock*);
HRESULT WINAPI XGameSaveReadBlobDataAsync(XGameSaveContainerHandle, const char**, uint32_t, XAsyncBlock*);
HRESULT WINAPI XGameSaveReadBlobDataResult(XAsyncBlock*, uint32_t, uint8_t*, uint32_t*, uint32_t*);
}

namespace {

inline constexpr GUID MakeGuid(
    uint32_t d1, uint16_t d2, uint16_t d3,
    uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
    uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7) {
    return GUID{ d1, d2, d3, { b0, b1, b2, b3, b4, b5, b6, b7 } };
}

inline bool GuidEquals(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Verified GDK 261000 service CLSIDs & IIDs extracted from xgameruntime.lib (main.obj)
inline constexpr GUID kClsidXAsync         = MakeGuid(0x073b7dcb, 0x1fcf, 0x4030, 0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28);
inline constexpr GUID kClsidXUser          = MakeGuid(0x01acd177, 0x91f9, 0x4763, 0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0);
inline constexpr GUID kIidXUserGamertag    = MakeGuid(0xcef4fac0, 0x7676, 0x4a94, 0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74);
inline constexpr GUID kIidXUser2022        = MakeGuid(0x1bf2f8c5, 0xd507, 0x4e52, 0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61);
inline constexpr GUID kClsidXUserDevAssoc  = MakeGuid(0x7d824997, 0x10dc, 0x45ab, 0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1);
inline constexpr GUID kIidXUserDevAssoc    = MakeGuid(0x0cc6a956, 0xe7e1, 0x4fdf, 0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8);
inline constexpr GUID kClsidXGameSave      = MakeGuid(0x704c3f58, 0xe629, 0x4cc2, 0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2);
inline constexpr GUID kIidXGameSave        = MakeGuid(0x1bfff3af, 0xf14a, 0x40a3, 0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9);
inline constexpr GUID kClsidXFeature       = MakeGuid(0x8836fe87, 0xedb9, 0x4fe3, 0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40);
inline constexpr GUID kClsidXError         = MakeGuid(0x8ca467f7, 0x22e8, 0x4096, 0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8);
inline constexpr GUID kClsidXGame          = MakeGuid(0x973a344e, 0x24bf, 0x4d0f, 0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29);
inline constexpr GUID kIidXGame            = MakeGuid(0x2549f142, 0x6419, 0x4a06, 0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34);
inline constexpr GUID kClsidXAnalyticsInfo = MakeGuid(0xb884675d, 0xb738, 0x4a9c, 0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b);
inline constexpr GUID kClsidXSystem        = MakeGuid(0xe349bd1a, 0xfc20, 0x4e40, 0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f);
inline constexpr GUID kIidXSystem          = MakeGuid(0x1861cf2e, 0xe18b, 0x4834, 0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf);
inline constexpr GUID kClsidXNetworking    = MakeGuid(0x37e56907, 0x2f10, 0x41e8, 0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a);
inline constexpr GUID kIidXNetworking2     = MakeGuid(0xbf2346b2, 0x39af, 0x4658, 0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3);

inline constexpr GUID    kIidIUnknown      = MakeGuid(0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
inline constexpr HRESULT kRegDbClassNotReg = static_cast<HRESULT>(0x80040154L);

std::atomic<uint32_t> g_brokerInitCount{0};
thread_local bool     g_threadTimeSensitive = false;

// Reusable singleton COM base (AddRef/Release are safe no-op refcounts on static service instances)
struct StaticComService : public IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (GuidEquals(riid, kIidIUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

// --- Service 1: XAsync / XTaskQueue / XThread (CLSID 073b7dcb-...) ---
struct XAsyncServiceImpl final : public StaticComService {
    virtual HRESULT STDMETHODCALLTYPE GetStatus(XAsyncBlock* asyncBlock, bool wait) { // +0x18 (slot 3)
        return XAsyncGetStatus(asyncBlock, wait);
    }
    virtual HRESULT STDMETHODCALLTYPE GetResultSize(XAsyncBlock*, size_t* bufSize) { // +0x20 (slot 4)
        if (bufSize) *bufSize = 0;
        return S_OK;
    }
    virtual void STDMETHODCALLTYPE Cancel(XAsyncBlock*) {} // +0x28 (slot 5)
    virtual HRESULT STDMETHODCALLTYPE Run(XAsyncBlock* asyncBlock, XAsyncWork work) { // +0x30 (slot 6)
        if (!asyncBlock || !work) return E_INVALIDARG;
        // Mark in-flight, execute synchronously/safely and fire completion callback
        HRESULT hr = work(asyncBlock);
        std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
        if (asyncBlock->callback) {
            asyncBlock->callback(asyncBlock);
        }
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Begin(XAsyncBlock*, void*, const void*, const char*, void*) { return E_NOTIMPL; } // +0x38 (7)
    virtual HRESULT STDMETHODCALLTYPE Reserved8() { return E_NOTIMPL; } // +0x40 (8)
    virtual HRESULT STDMETHODCALLTYPE Schedule(XAsyncBlock*, uint32_t) { return E_NOTIMPL; } // +0x48 (9)
    virtual void    STDMETHODCALLTYPE Complete(XAsyncBlock*, HRESULT, size_t) {} // +0x50 (10)
    virtual HRESULT STDMETHODCALLTYPE GetResult(XAsyncBlock*, const void*, size_t, void*, size_t*) { return E_NOTIMPL; } // +0x58 (11)
    virtual HRESULT STDMETHODCALLTYPE QueueCreate(XTaskQueueDispatchMode w, XTaskQueueDispatchMode c, XTaskQueueHandle* q) { // +0x60 (12)
        return XTaskQueueCreate(w, c, q);
    }
    virtual HRESULT STDMETHODCALLTYPE QueueCreateComposite(void*, void*, XTaskQueueHandle* q) { // +0x68 (13)
        return XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, q);
    }
    virtual HRESULT STDMETHODCALLTYPE QueueGetPort(XTaskQueueHandle, XTaskQueuePort, void** outPort) { // +0x70 (14)
        if (outPort) *outPort = nullptr;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE QueueDuplicateHandle(XTaskQueueHandle h, XTaskQueueHandle* outH) { // +0x78 (15)
        return sc::runtime::DuplicateTaskQueueHandle(h, outH);
    }
    virtual bool    STDMETHODCALLTYPE QueueDispatch(XTaskQueueHandle q, XTaskQueuePort p, uint32_t timeoutMs) { // +0x80 (16)
        return XTaskQueueDispatch(q, p, timeoutMs);
    }
    virtual void    STDMETHODCALLTYPE QueueCloseHandle(XTaskQueueHandle q) { XTaskQueueCloseHandle(q); } // +0x88 (17)
    virtual HRESULT STDMETHODCALLTYPE QueueSubmitCallback(XTaskQueueHandle q, XTaskQueuePort port, void* ctx, void(CALLBACK* cb)(void*, bool)) { // +0x90 (18)
        return sc::runtime::SubmitTaskCallback(q, port, 0, ctx, cb);
    }
    virtual HRESULT STDMETHODCALLTYPE QueueSubmitDelayedCallback(XTaskQueueHandle q, XTaskQueuePort port, uint32_t delayMs, void* ctx, void(CALLBACK* cb)(void*, bool)) { // +0x98 (19)
        return sc::runtime::SubmitTaskCallback(q, port, delayMs, ctx, cb);
    }
    virtual HRESULT STDMETHODCALLTYPE QueueRegisterWaiter(XTaskQueueHandle, XTaskQueuePort, HANDLE, void*, void*, uint64_t*) { return S_OK; } // +0xA0 (20)
    virtual void    STDMETHODCALLTYPE QueueUnregisterWaiter(XTaskQueueHandle, uint64_t) {} // +0xA8 (21)
    virtual HRESULT STDMETHODCALLTYPE QueueTerminate(XTaskQueueHandle q, bool wait, void* ctx, void(CALLBACK* cb)(void*)) { // +0xB0 (22)
        return sc::runtime::TerminateTaskQueue(q, wait, ctx, cb);
    }
    virtual HRESULT STDMETHODCALLTYPE QueueRegisterMonitor(XTaskQueueHandle, void*, void*, uint64_t*) { return S_OK; } // +0xB8 (23)
    virtual void    STDMETHODCALLTYPE QueueUnregisterMonitor(XTaskQueueHandle, uint64_t) {} // +0xC0 (24)
    virtual bool    STDMETHODCALLTYPE QueueGetCurrentProcess(XTaskQueueHandle* outQ) { // +0xC8 (25)
        if (!outQ) return false;
        XTaskQueueHandle pq = sc::runtime::GetProcessTaskQueue();
        if (!pq) {
            *outQ = nullptr;
            return false;
        }
        return SUCCEEDED(sc::runtime::DuplicateTaskQueueHandle(pq, outQ));
    }
    virtual void    STDMETHODCALLTYPE QueueSetCurrentProcess(XTaskQueueHandle q) { // +0xD0 (26)
        sc::runtime::SetProcessTaskQueue(q);
    }
    virtual HRESULT STDMETHODCALLTYPE ThreadSetTimeSensitive(bool sensitive) { // +0xD8 (27)
        g_threadTimeSensitive = sensitive;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ThreadVerifyNotTimeSensitive() { return S_OK; } // +0xE0 (28)
    virtual void    STDMETHODCALLTYPE ThreadAssertNotTimeSensitive() {} // +0xE8 (29)
    virtual bool    STDMETHODCALLTYPE ThreadIsTimeSensitive() { return g_threadTimeSensitive; } // +0xF0 (30)
};

// --- Service 2: XSystem (CLSID e349bd1a-..., IID 1861cf2e-...) ---
struct XSystemServiceImpl final : public StaticComService {
    virtual HRESULT STDMETHODCALLTYPE GetConsoleId(size_t bufSize, char* buf, size_t* used) { // +0x18 (3)
        static constexpr char kConsoleId[] = "SCARLETT.00000000.00000000.00000001";
        const size_t need = sizeof(kConsoleId);
        if (used) *used = need;
        if (!buf || bufSize < need) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::memcpy(buf, kConsoleId, need);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetXboxLiveSandboxId(size_t bufSize, char* buf, size_t* used) { // +0x20 (4)
        static constexpr char kSandbox[] = "RETAIL";
        const size_t need = sizeof(kSandbox);
        if (used) *used = need;
        if (!buf || bufSize < need) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::memcpy(buf, kSandbox, need);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetAppSpecificDeviceId(size_t bufSize, char* buf, size_t* used) { // +0x28 (5)
        static constexpr char kDevId[] = "scarlett-layer-device-id-0001";
        const size_t need = sizeof(kDevId);
        if (used) *used = need;
        if (!buf || bufSize < need) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::memcpy(buf, kDevId, need);
        return S_OK;
    }
    virtual void STDMETHODCALLTYPE HandleTrack(void*, void*) {} // +0x30 (6)
    virtual bool STDMETHODCALLTYPE IsHandleValid(void* h) { return h != nullptr; } // +0x38 (7)
    virtual void STDMETHODCALLTYPE AllowFullDownloadBandwidth(bool) {} // +0x40 (8)
};

// --- Service 3: XSystemAnalyticsInfo (CLSID b884675d-...) ---
struct XAnalyticsInfoServiceImpl final : public StaticComService {
    // Hidden sret ABI on x64 MSVC: rcx=this, rdx=&outInfo, returns &outInfo
    virtual XSystemAnalyticsInfo STDMETHODCALLTYPE GetAnalyticsInfo() { // +0x18 (3)
        XSystemAnalyticsInfo info{};
        info.osVersion.major = 10;
        info.osVersion.minor = 0;
        info.osVersion.build = 26100;
        info.osVersion.revision = 0;
        info.hostingOsVersion = info.osVersion;
        std::snprintf(info.family, sizeof(info.family), "Windows.Xbox");
        std::snprintf(info.form, sizeof(info.form), "Xbox Series X");
        return info;
    }
};

// --- Service 9: XNetworking (CLSID 37e56907-2f10-41e8-b72f-36edb185331a) ---
struct XNetworkingConnectivityHintShim {
    uint32_t connectivityLevel;    // 3 = InternetAccess
    uint32_t connectivityCost;     // 1 = Unrestricted
    uint32_t ianaInterfaceType;    // 6 = Ethernet
    bool     networkInitialized;
    bool     approachingDataLimit;
    bool     overDataLimit;
    bool     roaming;
};

struct XNetworkingServiceImpl final : public StaticComService {
    std::atomic<uint64_t> nextToken{1};

    // slot 3 (+0x18)
    virtual HRESULT STDMETHODCALLTYPE QueryPreferredLocalUdpMultiplayerPort(uint16_t* port) {
        if (!port) return E_POINTER;
        *port = 3074;
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XNetworking::QueryPreferredLocalUdpMultiplayerPort", "-> 3074");
        return S_OK;
    }
    // slot 4 (+0x20)
    virtual HRESULT STDMETHODCALLTYPE QueryPreferredLocalUdpMultiplayerPortAsync(XAsyncBlock* ab) {
        if (!ab) return E_POINTER;
        std::memset(ab->internal, 0, sizeof(ab->internal));
        if (ab->callback) ab->callback(ab);
        return S_OK;
    }
    // slot 5 (+0x28)
    virtual HRESULT STDMETHODCALLTYPE QueryPreferredLocalUdpMultiplayerPortAsyncResult(XAsyncBlock*, uint16_t* port) {
        if (!port) return E_POINTER;
        *port = 3074;
        return S_OK;
    }
    // slot 6 (+0x30)
    virtual HRESULT STDMETHODCALLTYPE RegisterPreferredLocalUdpMultiplayerPortChanged(
        XTaskQueueHandle, void*, void*, uint64_t* token) {
        if (!token) return E_POINTER;
        *token = nextToken.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    // slot 7 (+0x38)
    virtual bool STDMETHODCALLTYPE UnregisterPreferredLocalUdpMultiplayerPortChanged(uint64_t, bool) {
        return true;
    }
    // slot 8 (+0x40)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlAsync(const char*, XAsyncBlock*) {
        return E_NOTIMPL;
    }
    // slot 9 (+0x48)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlAsyncResultSize(XAsyncBlock*, size_t* sz) {
        if (sz) *sz = 0;
        return E_NOTIMPL;
    }
    // slot 10 (+0x50)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlAsyncResult(
        XAsyncBlock*, size_t, size_t*, uint8_t*, void**) {
        return E_NOTIMPL;
    }
    // slot 11 (+0x58)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlUtf16Async(const wchar_t*, XAsyncBlock*) {
        return E_NOTIMPL;
    }
    // slot 12 (+0x60)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlUtf16AsyncResultSize(XAsyncBlock*, size_t* sz) {
        if (sz) *sz = 0;
        return E_NOTIMPL;
    }
    // slot 13 (+0x68)
    virtual HRESULT STDMETHODCALLTYPE QuerySecurityInformationForUrlUtf16AsyncResult(
        XAsyncBlock*, size_t, size_t*, uint8_t*, void**) {
        return E_NOTIMPL;
    }
    // slot 14 (+0x70)
    virtual HRESULT STDMETHODCALLTYPE VerifyServerCertificate(void*, const void*) {
        return S_OK;
    }
    // slot 15 (+0x78)
    virtual HRESULT STDMETHODCALLTYPE GetConnectivityHint(XNetworkingConnectivityHintShim* hint) {
        if (!hint) return E_POINTER;
        hint->connectivityLevel    = 3; // InternetAccess
        hint->connectivityCost     = 1; // Unrestricted
        hint->ianaInterfaceType    = 6; // IF_TYPE_ETHERNET_CSMACD
        hint->networkInitialized   = true;
        hint->approachingDataLimit = false;
        hint->overDataLimit        = false;
        hint->roaming              = false;
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XNetworking::GetConnectivityHint", "-> InternetAccess");
        return S_OK;
    }
    // slot 16 (+0x80)
    virtual HRESULT STDMETHODCALLTYPE RegisterConnectivityHintChanged(
        XTaskQueueHandle, void*, void*, uint64_t* token) {
        if (!token) return E_POINTER;
        *token = nextToken.fetch_add(1, std::memory_order_relaxed);
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XNetworking::RegisterConnectivityHintChanged", "-> S_OK");
        return S_OK;
    }
    // slot 17 (+0x88)
    virtual bool STDMETHODCALLTYPE UnregisterConnectivityHintChanged(uint64_t, bool) {
        return true;
    }
    // slot 18 (+0x90)
    virtual HRESULT STDMETHODCALLTYPE QueryConfigurationSetting(uint32_t, uint64_t* value) {
        if (value) *value = 65536;
        return S_OK;
    }
    // slot 19 (+0x98)
    virtual HRESULT STDMETHODCALLTYPE SetConfigurationSetting(uint32_t, uint64_t) {
        return S_OK;
    }
    // slot 20 (+0xA0)
    virtual HRESULT STDMETHODCALLTYPE QueryStatistics(uint32_t, void*) {
        return E_NOTIMPL;
    }
};

// --- Service 4: XGame (CLSID 973a344e-..., IID 2549f142-...) ---
struct XGameServiceImpl final : public StaticComService {
    virtual HRESULT STDMETHODCALLTYPE GetXboxTitleId(uint32_t* titleId) { // +0x18 (3)
        if (!titleId) return E_POINTER;
        *titleId = 0x53434152u; // 'SCAR' default synthetic title ID
        return S_OK;
    }
};

// --- Service 5: XError (CLSID 8ca467f7-...) ---
struct XErrorServiceImpl final : public StaticComService {
    virtual bool STDMETHODCALLTYPE Report(HRESULT hr, const char* msg) { // +0x18 (3)
        char buf[256];
        std::snprintf(buf, sizeof(buf), "hr=0x%08X msg=%s",
                      static_cast<unsigned>(hr), msg ? msg : "");
        sc::log::Write(sc::log::Level::Warn, "xgameruntime", "XErrorReport", buf);
        return true;
    }
    virtual void STDMETHODCALLTYPE SetCallback(void*, void*) {} // +0x20 (4)
    virtual void STDMETHODCALLTYPE SetOptions(uint32_t, uint32_t) {} // +0x28 (5)
};

// --- Service 6: XGameRuntimeFeature (CLSID 8836fe87-...) ---
struct XFeatureServiceImpl final : public StaticComService {
    virtual bool STDMETHODCALLTYPE IsFeatureAvailable(uint32_t) { return true; } // +0x18 (3)
};

// --- Service 7: XUser (CLSID 01acd177-...) & XUserGamertag (IID cef4fac0-...) ---
struct XUserGamertagServiceImpl final : public StaticComService {
    virtual HRESULT STDMETHODCALLTYPE GetGamertag(
        XUserHandle user, XUserGamertagComponent comp, size_t size, char* out, size_t* used) { // +0x18 (3)
        return XUserGetGamertag(user, comp, size, out, used);
    }
};

struct XUserServiceImpl final : public StaticComService {
    std::atomic<uint64_t> nextToken{1};

    static HRESULT CompleteTrivialAsync(XAsyncBlock* ab) {
        return sc::runtime::LaunchAsync(ab, [](std::shared_ptr<void>&) -> HRESULT { return S_OK; });
    }
    static HRESULT FinishTrivialAsync(XAsyncBlock* ab) {
        if (!ab) return E_INVALIDARG;
        auto* st = sc::runtime::LoadAsyncState(ab);
        if (!st) return S_OK;
        HRESULT hr = E_PENDING;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (!st->completed) return E_PENDING;
            hr = st->status;
        }
        sc::runtime::DetachAsyncState(ab, st);
        return hr;
    }

    virtual HRESULT STDMETHODCALLTYPE DuplicateHandle(XUserHandle u, XUserHandle* outU) { // +0x18 (3)
        return sc::runtime::DuplicateUserHandle(u, outU);
    }
    virtual void    STDMETHODCALLTYPE CloseHandle(XUserHandle u) { XUserCloseHandle(u); } // +0x20 (4)
    virtual int32_t STDMETHODCALLTYPE Compare(XUserHandle a, XUserHandle b) { // +0x28 (5)
        const uint64_t xa = a ? sc::runtime::GetUserXuid(a) : 0;
        const uint64_t xb = b ? sc::runtime::GetUserXuid(b) : 0;
        return xa == xb ? 0 : (xa < xb ? -1 : 1);
    }
    virtual HRESULT STDMETHODCALLTYPE GetMaxUsers(uint32_t* maxU) { if (maxU) *maxU = 4; return S_OK; } // +0x30 (6)
    virtual HRESULT STDMETHODCALLTYPE AddAsync(XUserAddOptions opt, XAsyncBlock* ab) { return XUserAddAsync(opt, ab); } // +0x38 (7)
    virtual HRESULT STDMETHODCALLTYPE AddResult(XAsyncBlock* ab, XUserHandle* u) { return XUserAddResult(ab, u); } // +0x40 (8)
    virtual HRESULT STDMETHODCALLTYPE GetLocalId(XUserHandle u, XUserLocalId* id) { return XUserGetLocalId(u, id); } // +0x48 (9)
    virtual HRESULT STDMETHODCALLTYPE FindUserByLocalId(XUserLocalId id, XUserHandle* u) { // +0x50 (10)
        if (!u) return E_POINTER;
        *u = (id.value == 0x0009000000000001ULL) ? sc::runtime::AcquireDefaultUser() : nullptr;
        return *u ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    virtual HRESULT STDMETHODCALLTYPE GetId(XUserHandle u, uint64_t* id) { return XUserGetId(u, id); } // +0x58 (11)
    virtual HRESULT STDMETHODCALLTYPE FindUserById(uint64_t id, XUserHandle* u) { // +0x60 (12)
        if (!u) return E_POINTER;
        *u = (id == 0x0009000000000001ULL) ? sc::runtime::AcquireDefaultUser() : nullptr;
        return *u ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    virtual HRESULT STDMETHODCALLTYPE GetIsGuest(XUserHandle, bool* isGuest) { if (isGuest) *isGuest = false; return S_OK; } // +0x68 (13)
    virtual HRESULT STDMETHODCALLTYPE GetState(XUserHandle u, XUserState* st) { return XUserGetState(u, st); } // +0x70 (14)
    virtual HRESULT STDMETHODCALLTYPE GetGamertag(XUserHandle u, XUserGamertagComponent c, size_t sz, char* out, size_t* used) { // +0x78 (15)
        return XUserGetGamertag(u, c, sz, out, used);
    }
    virtual HRESULT STDMETHODCALLTYPE GetGamerPictureAsync(XUserHandle, uint32_t, XAsyncBlock*) { return E_NOTIMPL; } // +0x80 (16)
    virtual HRESULT STDMETHODCALLTYPE GetGamerPictureResultSize(XAsyncBlock*, size_t* sz) { if (sz) *sz = 0; return E_NOTIMPL; } // +0x88 (17)
    virtual HRESULT STDMETHODCALLTYPE GetGamerPictureResult(XAsyncBlock*, size_t, void*, size_t* used) { if (used) *used = 0; return E_NOTIMPL; } // +0x90 (18)
    virtual HRESULT STDMETHODCALLTYPE GetAgeGroup(XUserHandle, uint32_t* ageGroup) { // +0x98 (19)
        if (!ageGroup) return E_POINTER;
        *ageGroup = 3u; // XUserAgeGroup::Adult
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE CheckPrivilege(XUserHandle, uint32_t, uint32_t, bool* hasPriv, uint32_t* reason) { // +0xA0 (20)
        if (hasPriv) *hasPriv = true;
        if (reason) *reason = 0u; // XUserPrivilegeDenyReason::None
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ResolvePrivilegeWithUiAsync(XUserHandle, uint32_t, uint32_t, XAsyncBlock* ab) { // +0xA8 (21)
        return CompleteTrivialAsync(ab);
    }
    virtual HRESULT STDMETHODCALLTYPE ResolvePrivilegeWithUiResult(XAsyncBlock* ab) { // +0xB0 (22)
        return FinishTrivialAsync(ab);
    }
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync(XUserHandle, uint32_t, const char*, const char*, size_t, const void*, size_t, const void*, XAsyncBlock*) { return E_NOTIMPL; } // +0xB8 (23)
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureResultSize(XAsyncBlock*, size_t* sz) { if (sz) *sz = 0; return E_NOTIMPL; } // +0xC0 (24)
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureResult(XAsyncBlock*, size_t, void*, void**, size_t* used) { if (used) *used = 0; return E_NOTIMPL; } // +0xC8 (25)
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureUtf16Async(XUserHandle, uint32_t, const wchar_t*, const wchar_t*, size_t, const void*, size_t, const void*, XAsyncBlock*) { return E_NOTIMPL; } // +0xD0 (26)
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureUtf16ResultSize(XAsyncBlock*, size_t* sz) { if (sz) *sz = 0; return E_NOTIMPL; } // +0xD8 (27)
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureUtf16Result(XAsyncBlock*, size_t, void*, void**, size_t* used) { if (used) *used = 0; return E_NOTIMPL; } // +0xE0 (28)
    virtual HRESULT STDMETHODCALLTYPE ResolveIssueWithUiAsync(XUserHandle, const char*, XAsyncBlock* ab) { return CompleteTrivialAsync(ab); } // +0xE8 (29)
    virtual HRESULT STDMETHODCALLTYPE ResolveIssueWithUiResult(XAsyncBlock* ab) { return FinishTrivialAsync(ab); } // +0xF0 (30)
    virtual HRESULT STDMETHODCALLTYPE ResolveIssueWithUiUtf16Async(XUserHandle, const wchar_t*, XAsyncBlock* ab) { return CompleteTrivialAsync(ab); } // +0xF8 (31)
    virtual HRESULT STDMETHODCALLTYPE ResolveIssueWithUiUtf16Result(XAsyncBlock* ab) { return FinishTrivialAsync(ab); } // +0x100 (32)
    virtual HRESULT STDMETHODCALLTYPE RegisterForChangeEvent(XTaskQueueHandle, void*, void*, uint64_t* token) { // +0x108 (33)
        if (!token) return E_POINTER;
        *token = nextToken.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    virtual bool STDMETHODCALLTYPE UnregisterForChangeEvent(uint64_t, bool) { return true; } // +0x110 (34)
    virtual HRESULT STDMETHODCALLTYPE GetSignOutDeferral(void** deferral) { // +0x118 (35)
        if (!deferral) return E_POINTER;
        *deferral = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        return S_OK;
    }
    virtual void STDMETHODCALLTYPE CloseSignOutDeferralHandle(void*) {} // +0x120 (36)
    virtual HRESULT STDMETHODCALLTYPE AddByIdWithUiAsync(uint64_t, XAsyncBlock* ab) { // +0x128 (37)
        return XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, ab);
    }
    virtual HRESULT STDMETHODCALLTYPE AddByIdWithUiResult(XAsyncBlock* ab, XUserHandle* u) { // +0x130 (38)
        return XUserAddResult(ab, u);
    }
};

// --- Service 7b: XUserDeviceAssociation (CLSID 7d824997-..., IID 0cc6a956-...) ---
struct XUserDevAssocServiceImpl final : public StaticComService {
    std::atomic<uint64_t> nextToken{1};

    virtual HRESULT STDMETHODCALLTYPE FindForDevice(const void* deviceId, XUserHandle* outUser) { // +0x18 (3)
        if (!deviceId || !outUser) return E_POINTER;
        *outUser = sc::runtime::AcquireDefaultUser();
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XUserFindForDevice",
                       "-> bound device to default offline XUser");
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE RegisterForDeviceAssociationChanged(XTaskQueueHandle, void*, void*, uint64_t* token) { // +0x20 (4)
        if (!token) return E_POINTER;
        *token = nextToken.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    virtual bool STDMETHODCALLTYPE UnregisterForDeviceAssociationChanged(uint64_t, bool) { return true; } // +0x28 (5)
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpointUtf16(XUserHandle, uint32_t, size_t, wchar_t*, size_t* used) { // +0x30 (6)
        if (used) *used = 0;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    virtual HRESULT STDMETHODCALLTYPE RegisterForDefaultAudioEndpointUtf16Changed(XTaskQueueHandle, void*, void*, uint64_t* token) { // +0x38 (7)
        if (!token) return E_POINTER;
        *token = nextToken.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    virtual bool STDMETHODCALLTYPE UnregisterForDefaultAudioEndpointUtf16Changed(uint64_t, bool) { return true; } // +0x40 (8)
    virtual HRESULT STDMETHODCALLTYPE FindControllerForUserWithUiAsync(XUserHandle, XAsyncBlock* ab) { // +0x48 (9)
        return sc::runtime::LaunchAsync(ab, [](std::shared_ptr<void>&) -> HRESULT { return S_OK; });
    }
    virtual HRESULT STDMETHODCALLTYPE FindControllerForUserWithUiResult(XAsyncBlock* ab, void* outDeviceId32) { // +0x50 (10)
        if (!ab || !outDeviceId32) return E_INVALIDARG;
        auto* st = sc::runtime::LoadAsyncState(ab);
        if (!st) return E_INVALIDARG;
        HRESULT hr = E_PENDING;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (!st->completed) return E_PENDING;
            hr = st->status;
        }
        sc::runtime::DetachAsyncState(ab, st);
        if (SUCCEEDED(hr)) {
            static constexpr char kGpId[] = "SCARLETT-XBOXONE-GAMEPAD-000000";
            std::memset(outDeviceId32, 0, 32);
            std::memcpy(outDeviceId32, kGpId, sizeof(kGpId) - 1);
        }
        return hr;
    }
};

// --- Service 8: XGameSave (CLSID 704c3f58-..., IID 1bfff3af-...) ---
struct XGameSaveServiceImpl final : public StaticComService {
    virtual HRESULT STDMETHODCALLTYPE InitProvider(XUserHandle, const char*, bool, XGameSaveProviderHandle*) { return E_NOTIMPL; } // +0x18 (3)
    virtual HRESULT STDMETHODCALLTYPE InitProviderAsync(XUserHandle u, const char* scid, bool sync, XAsyncBlock* ab) { return XGameSaveInitializeProviderAsync(u, scid, sync, ab); } // +0x20 (4)
    virtual HRESULT STDMETHODCALLTYPE InitProviderResult(XAsyncBlock* ab, XGameSaveProviderHandle* p) { return XGameSaveInitializeProviderResult(ab, p); } // +0x28 (5)
    virtual void    STDMETHODCALLTYPE CloseProvider(XGameSaveProviderHandle p) { XGameSaveCloseProvider(p); } // +0x30 (6)
    virtual HRESULT STDMETHODCALLTYPE GetQuota(XGameSaveProviderHandle, int64_t* q) { if (q) *q = 64LL * 1024 * 1024; return S_OK; } // +0x38 (7)
    virtual HRESULT STDMETHODCALLTYPE GetQuotaAsync(XGameSaveProviderHandle, XAsyncBlock*) { return E_NOTIMPL; } // +0x40 (8)
    virtual HRESULT STDMETHODCALLTYPE GetQuotaResult(XAsyncBlock*, int64_t*) { return E_NOTIMPL; } // +0x48 (9)
    virtual HRESULT STDMETHODCALLTYPE DeleteContainer(XGameSaveProviderHandle, const char*) { return E_NOTIMPL; } // +0x50 (10)
    virtual HRESULT STDMETHODCALLTYPE DeleteContainerAsync(XGameSaveProviderHandle, const char*, XAsyncBlock*) { return E_NOTIMPL; } // +0x58 (11)
    virtual HRESULT STDMETHODCALLTYPE DeleteContainerResult(XAsyncBlock*) { return E_NOTIMPL; } // +0x60 (12)
    virtual HRESULT STDMETHODCALLTYPE GetContainerInfo(XGameSaveProviderHandle, const char*, void*, void*) { return E_NOTIMPL; } // +0x68 (13)
    virtual HRESULT STDMETHODCALLTYPE EnumContainerInfo(XGameSaveProviderHandle, void*, void*) { return E_NOTIMPL; } // +0x70 (14)
    virtual HRESULT STDMETHODCALLTYPE EnumContainerInfoByName(XGameSaveProviderHandle, const char*, void*, void*) { return E_NOTIMPL; } // +0x78 (15)
    virtual HRESULT STDMETHODCALLTYPE CreateContainer(XGameSaveProviderHandle p, const char* n, XGameSaveContainerHandle* c) { return XGameSaveCreateContainer(p, n, c); } // +0x80 (16)
    virtual void    STDMETHODCALLTYPE CloseContainer(XGameSaveContainerHandle c) { XGameSaveCloseContainer(c); } // +0x88 (17)
    virtual HRESULT STDMETHODCALLTYPE EnumBlobInfo(XGameSaveContainerHandle, void*, void*) { return E_NOTIMPL; } // +0x90 (18)
    virtual HRESULT STDMETHODCALLTYPE EnumBlobInfoByName(XGameSaveContainerHandle, const char*, void*, void*) { return E_NOTIMPL; } // +0x98 (19)
    virtual HRESULT STDMETHODCALLTYPE ReadBlobData(XGameSaveContainerHandle, const char**, uint32_t*, uint32_t, uint8_t*) { return E_NOTIMPL; } // +0xA0 (20)
    virtual HRESULT STDMETHODCALLTYPE ReadBlobDataAsync(XGameSaveContainerHandle c, const char** n, uint32_t cnt, XAsyncBlock* ab) { return XGameSaveReadBlobDataAsync(c, n, cnt, ab); } // +0xA8 (21)
    virtual HRESULT STDMETHODCALLTYPE ReadBlobDataResult(XAsyncBlock* ab, uint32_t sz, uint8_t* b, uint32_t* cnt, uint32_t* used) { return XGameSaveReadBlobDataResult(ab, sz, b, cnt, used); } // +0xB0 (22)
    virtual HRESULT STDMETHODCALLTYPE CreateUpdate(XGameSaveContainerHandle c, const char* n, XGameSaveUpdateHandle* u) { return XGameSaveCreateUpdate(c, n, u); } // +0xB8 (23)
    virtual void    STDMETHODCALLTYPE CloseUpdate(XGameSaveUpdateHandle u) { XGameSaveCloseUpdate(u); } // +0xC0 (24)
    virtual HRESULT STDMETHODCALLTYPE SubmitBlobWrite(XGameSaveUpdateHandle u, const char* n, const uint8_t* d, size_t s) { return XGameSaveSubmitBlobWrite(u, n, d, s); } // +0xC8 (25)
    virtual HRESULT STDMETHODCALLTYPE SubmitBlobDelete(XGameSaveUpdateHandle u, const char* n) { return XGameSaveSubmitBlobDelete(u, n); } // +0xD0 (26)
    virtual HRESULT STDMETHODCALLTYPE SubmitUpdate(XGameSaveUpdateHandle) { return E_NOTIMPL; } // +0xD8 (27)
    virtual HRESULT STDMETHODCALLTYPE SubmitUpdateAsync(XGameSaveUpdateHandle u, XAsyncBlock* ab) { return XGameSaveSubmitUpdateAsync(u, ab); } // +0xE0 (28)
    virtual HRESULT STDMETHODCALLTYPE SubmitUpdateResult(XAsyncBlock* ab) { return XGameSaveSubmitUpdateResult(ab); } // +0xE8 (29)
};

XAsyncServiceImpl         g_asyncSvc;
XSystemServiceImpl        g_systemSvc;
XAnalyticsInfoServiceImpl g_analyticsSvc;
XGameServiceImpl          g_gameSvc;
XErrorServiceImpl         g_errorSvc;
XFeatureServiceImpl       g_featureSvc;
XUserServiceImpl          g_userSvc;
XUserGamertagServiceImpl  g_userGamertagSvc;
XUserDevAssocServiceImpl  g_userDevAssocSvc;
XGameSaveServiceImpl      g_gameSaveSvc;

} // namespace

extern "C" {

HRESULT WINAPI DllCanUnloadNow() {
    return g_brokerInitCount.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

HRESULT WINAPI InitializeApiImplEx2(
    uint64_t version, uint64_t revision, uint64_t flags, void* /*options*/) {
    const uint32_t prev = g_brokerInitCount.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) {
        XGameRuntimeInitialize();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "-> GDK service broker initialized (ver=0x%llX rev=0x%llX flags=0x%llX refs=%u)",
                  static_cast<unsigned long long>(version),
                  static_cast<unsigned long long>(revision),
                  static_cast<unsigned long long>(flags),
                  prev + 1);
    sc::log::Write(sc::log::Level::Info, "xgameruntime", "InitializeApiImplEx2", buf);
    return S_OK;
}

HRESULT WINAPI InitializeApiImplEx(uint64_t version, uint64_t revision, uint64_t flags) {
    return InitializeApiImplEx2(version, revision, flags, nullptr);
}

HRESULT WINAPI InitializeApiImpl(uint64_t version, uint64_t revision) {
    return InitializeApiImplEx2(version, revision, 0, nullptr);
}

void WINAPI UninitializeApiImpl() {
    uint32_t cur = g_brokerInitCount.load(std::memory_order_acquire);
    while (cur > 0 && !g_brokerInitCount.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {
    }
    if (cur == 1) {
        XGameRuntimeUninitialize();
    }
    sc::log::Write(sc::log::Level::Info, "xgameruntime", "UninitializeApiImpl",
                   "-> GDK service broker uninitialized");
}

HRESULT WINAPI QueryApiImpl(const GUID* clsid, const GUID* iid, void** ppv) {
    if (!clsid || !iid || !ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (GuidEquals(*clsid, kClsidXAsync) && GuidEquals(*iid, kClsidXAsync)) {
        *ppv = &g_asyncSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXSystem) && GuidEquals(*iid, kIidXSystem)) {
        *ppv = &g_systemSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXAnalyticsInfo) && GuidEquals(*iid, kClsidXAnalyticsInfo)) {
        *ppv = &g_analyticsSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXGame) && GuidEquals(*iid, kIidXGame)) {
        *ppv = &g_gameSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXError) && GuidEquals(*iid, kClsidXError)) {
        *ppv = &g_errorSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXFeature) && GuidEquals(*iid, kClsidXFeature)) {
        *ppv = &g_featureSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXUser)) {
        if (GuidEquals(*iid, kIidXUserGamertag)) {
            *ppv = &g_userGamertagSvc;
            return S_OK;
        }
        if (GuidEquals(*iid, kClsidXUser) || GuidEquals(*iid, kIidXUser2022)) {
            *ppv = &g_userSvc;
            return S_OK;
        }
    }
    if (GuidEquals(*clsid, kClsidXUserDevAssoc) && GuidEquals(*iid, kIidXUserDevAssoc)) {
        *ppv = &g_userDevAssocSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXGameSave) && GuidEquals(*iid, kIidXGameSave)) {
        *ppv = &g_gameSaveSvc;
        return S_OK;
    }
    if (GuidEquals(*clsid, kClsidXNetworking) && (GuidEquals(*iid, kClsidXNetworking) || GuidEquals(*iid, kIidXNetworking2))) {
        static XNetworkingServiceImpl g_networkingSvc;
        *ppv = &g_networkingSvc;
        return S_OK;
    }

    char buf[220];
    std::snprintf(buf, sizeof(buf),
                  "-> unmapped GDK service CLSID={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} "
                  "IID={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  clsid->Data1, clsid->Data2, clsid->Data3,
                  clsid->Data4[0], clsid->Data4[1], clsid->Data4[2], clsid->Data4[3],
                  clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7],
                  iid->Data1, iid->Data2, iid->Data3,
                  iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3],
                  iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
    sc::log::Write(sc::log::Level::Warn, "xgameruntime", "QueryApiImpl", buf);
    return kRegDbClassNotReg;
}

void WINAPI XErrorReport(HRESULT hr, const char* message) {
    g_errorSvc.Report(hr, message);
}

} // extern "C"
