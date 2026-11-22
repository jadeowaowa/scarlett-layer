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
inline constexpr GUID kClsidXGameSave      = MakeGuid(0x704c3f58, 0xe629, 0x4cc2, 0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2);
inline constexpr GUID kIidXGameSave        = MakeGuid(0x1bfff3af, 0xf14a, 0x40a3, 0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9);
inline constexpr GUID kClsidXFeature       = MakeGuid(0x8836fe87, 0xedb9, 0x4fe3, 0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40);
inline constexpr GUID kClsidXError         = MakeGuid(0x8ca467f7, 0x22e8, 0x4096, 0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8);
inline constexpr GUID kClsidXGame          = MakeGuid(0x973a344e, 0x24bf, 0x4d0f, 0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29);
inline constexpr GUID kIidXGame            = MakeGuid(0x2549f142, 0x6419, 0x4a06, 0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34);
inline constexpr GUID kClsidXAnalyticsInfo = MakeGuid(0xb884675d, 0xb738, 0x4a9c, 0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b);
inline constexpr GUID kClsidXSystem        = MakeGuid(0xe349bd1a, 0xfc20, 0x4e40, 0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f);
inline constexpr GUID kIidXSystem          = MakeGuid(0x1861cf2e, 0xe18b, 0x4834, 0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf);

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
        if (!outH) return E_POINTER;
        *outH = h;
        return S_OK;
    }
    virtual bool    STDMETHODCALLTYPE QueueDispatch(XTaskQueueHandle q, XTaskQueuePort p, uint32_t timeoutMs) { // +0x80 (16)
        return XTaskQueueDispatch(q, p, timeoutMs);
    }
    virtual void    STDMETHODCALLTYPE QueueCloseHandle(XTaskQueueHandle q) { XTaskQueueCloseHandle(q); } // +0x88 (17)
    virtual HRESULT STDMETHODCALLTYPE QueueSubmitCallback(XTaskQueueHandle, XTaskQueuePort, void* ctx, void(CALLBACK* cb)(void*, bool)) { // +0x90 (18)
        if (cb) cb(ctx, false);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE QueueSubmitDelayedCallback(XTaskQueueHandle, XTaskQueuePort, uint32_t, void* ctx, void(CALLBACK* cb)(void*, bool)) { // +0x98 (19)
        if (cb) cb(ctx, false);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE QueueRegisterWaiter(XTaskQueueHandle, XTaskQueuePort, HANDLE, void*, void*, uint64_t*) { return S_OK; } // +0xA0 (20)
    virtual void    STDMETHODCALLTYPE QueueUnregisterWaiter(XTaskQueueHandle, uint64_t) {} // +0xA8 (21)
    virtual HRESULT STDMETHODCALLTYPE QueueTerminate(XTaskQueueHandle, bool, void*, void*) { return S_OK; } // +0xB0 (22)
    virtual HRESULT STDMETHODCALLTYPE QueueRegisterMonitor(XTaskQueueHandle, void*, void*, uint64_t*) { return S_OK; } // +0xB8 (23)
    virtual void    STDMETHODCALLTYPE QueueUnregisterMonitor(XTaskQueueHandle, uint64_t) {} // +0xC0 (24)
    virtual bool    STDMETHODCALLTYPE QueueGetCurrentProcess(XTaskQueueHandle* outQ) { // +0xC8 (25)
        if (outQ) *outQ = nullptr;
        return false;
    }
    virtual void    STDMETHODCALLTYPE QueueSetCurrentProcess(XTaskQueueHandle) {} // +0xD0 (26)
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
    virtual HRESULT STDMETHODCALLTYPE DuplicateHandle(XUserHandle u, XUserHandle* outU) { // +0x18 (3)
        if (!outU) return E_POINTER;
        *outU = u;
        return S_OK;
    }
    virtual void    STDMETHODCALLTYPE CloseHandle(XUserHandle u) { XUserCloseHandle(u); } // +0x20 (4)
    virtual int32_t STDMETHODCALLTYPE Compare(XUserHandle a, XUserHandle b) { return a == b ? 0 : (a < b ? -1 : 1); } // +0x28 (5)
    virtual HRESULT STDMETHODCALLTYPE GetMaxUsers(uint32_t* maxU) { if (maxU) *maxU = 4; return S_OK; } // +0x30 (6)
    virtual HRESULT STDMETHODCALLTYPE AddAsync(XUserAddOptions opt, XAsyncBlock* ab) { return XUserAddAsync(opt, ab); } // +0x38 (7)
    virtual HRESULT STDMETHODCALLTYPE AddResult(XAsyncBlock* ab, XUserHandle* u) { return XUserAddResult(ab, u); } // +0x40 (8)
    virtual HRESULT STDMETHODCALLTYPE GetLocalId(XUserHandle u, XUserLocalId* id) { return XUserGetLocalId(u, id); } // +0x48 (9)
    virtual HRESULT STDMETHODCALLTYPE FindUserByLocalId(XUserLocalId, XUserHandle* u) { if (u) *u = nullptr; return E_NOTIMPL; } // +0x50 (10)
    virtual HRESULT STDMETHODCALLTYPE GetId(XUserHandle u, uint64_t* id) { return XUserGetId(u, id); } // +0x58 (11)
    virtual HRESULT STDMETHODCALLTYPE FindUserById(uint64_t, XUserHandle* u) { if (u) *u = nullptr; return E_NOTIMPL; } // +0x60 (12)
    virtual HRESULT STDMETHODCALLTYPE GetIsGuest(XUserHandle, bool* isGuest) { if (isGuest) *isGuest = false; return S_OK; } // +0x68 (13)
    virtual HRESULT STDMETHODCALLTYPE GetState(XUserHandle u, XUserState* st) { return XUserGetState(u, st); } // +0x70 (14)
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
        if (GuidEquals(*iid, kClsidXUser)) {
            *ppv = &g_userSvc;
            return S_OK;
        }
    }
    if (GuidEquals(*clsid, kClsidXGameSave) && GuidEquals(*iid, kIidXGameSave)) {
        *ppv = &g_gameSaveSvc;
        return S_OK;
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "-> unmapped GDK service CLSID={%08lX-%04X-%04X-...} IID={%08lX-%04X-%04X-...}",
                  clsid->Data1, clsid->Data2, clsid->Data3,
                  iid->Data1, iid->Data2, iid->Data3);
    sc::log::Write(sc::log::Level::Warn, "xgameruntime", "QueryApiImpl", buf);
    return kRegDbClassNotReg;
}

void WINAPI XErrorReport(HRESULT hr, const char* message) {
    g_errorSvc.Report(hr, message);
}

} // extern "C"
