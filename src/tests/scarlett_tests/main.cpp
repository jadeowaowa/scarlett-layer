#include "common/vfs.hpp"
#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"
#include "shims/gameinput/gameinput.hpp"
#include "shims/xg_xs/xg_xs.hpp"
#include "shims/dstorage_xs/dstorage_xs.hpp"
#include "shims/pixevt/pixevt.hpp"
#include "shims/xmem/xmem.hpp"

#include <d3dcompiler.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
    __declspec(dllimport) HRESULT WINAPI XGameRuntimeInitialize();
    __declspec(dllimport) void    WINAPI XGameRuntimeUninitialize();
    __declspec(dllimport) XSystemDeviceType WINAPI XSystemGetDeviceType();
    __declspec(dllimport) HRESULT WINAPI XTaskQueueCreate(
        XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue);
    __declspec(dllimport) bool    WINAPI XTaskQueueDispatch(
        XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeoutInMs);
    __declspec(dllimport) void    WINAPI XTaskQueueCloseHandle(XTaskQueueHandle queue);
    __declspec(dllimport) HRESULT WINAPI XUserAddAsync(XUserAddOptions options, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser);
    __declspec(dllimport) HRESULT WINAPI XUserGetGamertag(
        XUserHandle user, XUserGamertagComponent comp, size_t size, char* out, size_t* used);
    __declspec(dllimport) void    WINAPI XUserCloseHandle(XUserHandle user);

    __declspec(dllimport) HRESULT WINAPI XGameSaveInitializeProviderAsync(
        XUserHandle requestingUser, const char* configurationId, bool syncOnDemand, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveInitializeProviderResult(
        XAsyncBlock* async, XGameSaveProviderHandle* provider);
    __declspec(dllimport) void    WINAPI XGameSaveCloseProvider(XGameSaveProviderHandle provider);
    __declspec(dllimport) HRESULT WINAPI XGameSaveCreateContainer(
        XGameSaveProviderHandle provider, const char* name, XGameSaveContainerHandle* result);
    __declspec(dllimport) void    WINAPI XGameSaveCloseContainer(XGameSaveContainerHandle context);
    __declspec(dllimport) HRESULT WINAPI XGameSaveCreateUpdate(
        XGameSaveContainerHandle container, const char* displayName, XGameSaveUpdateHandle* updateContext);
    __declspec(dllimport) void    WINAPI XGameSaveCloseUpdate(XGameSaveUpdateHandle context);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitBlobWrite(
        XGameSaveUpdateHandle updateContext, const char* blobName, const uint8_t* data, size_t byteCount);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitBlobDelete(
        XGameSaveUpdateHandle updateContext, const char* blobName);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitUpdateAsync(
        XGameSaveUpdateHandle updateContext, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitUpdateResult(XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveReadBlobDataAsync(
        XGameSaveContainerHandle container, const char** blobNames, uint32_t countOfBlobs, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveReadBlobDataResult(
        XAsyncBlock* async, size_t blobsSize, XGameSaveBlob* blobData, uint32_t* countOfBlobs);
}

namespace {

int g_passed = 0;
int g_failed = 0;

#define SC_EXPECT(cond)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            return false;                                                            \
        }                                                                            \
    } while (false)

void RunTest(const char* name, bool (*fn)()) {
    std::printf("[TEST] %-34s ... ", name);
    std::fflush(stdout);
    if (fn()) {
        ++g_passed;
        std::printf("PASS\n");
    } else {
        ++g_failed;
        std::printf("[TEST] %-34s ... FAIL\n", name);
    }
}

struct AsyncWaiter {
    std::atomic<bool> done{false};
};

void CALLBACK OnDone(XAsyncBlock* async) {
    static_cast<AsyncWaiter*>(async->context)->done.store(true, std::memory_order_release);
}

void Pump(XTaskQueueHandle queue, AsyncWaiter& waiter) {
    while (!waiter.done.load(std::memory_order_acquire)) {
        XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 25);
    }
}

// 1. Virtual Console Path Translation (G:\, T:\, S:\)
bool TestVfsPathTranslation() {
    bool isVirt = false;
    auto gPath = sc::vfs::TranslatePath(L"G:\\assets\\crest_2dthin.scx", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(gPath.filename() == L"crest_2dthin.scx");

    auto tPath = sc::vfs::TranslatePath(L"t:/scratch_dir/probe.bin", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(tPath.filename() == L"probe.bin");
    SC_EXPECT(tPath.wstring().find(L"scratch") != std::wstring::npos);

    auto sPath = sc::vfs::TranslatePath(L"S:\\slot0.tmp", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(sPath.wstring().find(L"save_temp") != std::wstring::npos);

    auto hostPath = sc::vfs::TranslatePath(L"C:\\Windows\\System32\\kernel32.dll", &isVirt);
    SC_EXPECT(!isVirt);
    SC_EXPECT(hostPath == std::filesystem::path(L"C:\\Windows\\System32\\kernel32.dll"));
    return true;
}

// 2. xgameruntime: Device spoof, XTaskQueue, XUser & buffer-too-small check
bool TestRuntimeAndXUser() {
    SC_EXPECT(SUCCEEDED(XGameRuntimeInitialize()));
    SC_EXPECT(XSystemGetDeviceType() == XSystemDeviceType::XboxScarlettAnaconda);

    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                         XTaskQueueDispatchMode::Manual, &queue)));

    AsyncWaiter waiter{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &waiter;
    async.callback = OnDone;
    SC_EXPECT(SUCCEEDED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async)));
    Pump(queue, waiter);

    XUserHandle user = nullptr;
    SC_EXPECT(SUCCEEDED(XUserAddResult(&async, &user)));
    SC_EXPECT(user != nullptr);

    char tiny[4]{};
    size_t used = 0;
    HRESULT smallHr = XUserGetGamertag(user, XUserGamertagComponent::Classic, sizeof(tiny), tiny, &used);
    SC_EXPECT(smallHr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    SC_EXPECT(used > sizeof(tiny));

    char full[64]{};
    SC_EXPECT(SUCCEEDED(XUserGetGamertag(user, XUserGamertagComponent::Classic, sizeof(full), full, &used)));
    SC_EXPECT(std::strcmp(full, "ScarlettPlayer") == 0);

    XUserCloseHandle(user);
    XTaskQueueCloseHandle(queue);
    return true;
}

// 3. XGameSave: Missing blob, multi-blob write, buffer-too-small retry, and blob deletion
bool TestXGameSaveEdgeCases() {
    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                         XTaskQueueDispatchMode::Manual, &queue)));

    XGameSaveProviderHandle provider = nullptr;
    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveInitializeProviderAsync(
            nullptr, "00000000-0000-0000-0000-0000TESTSUITE", false, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveInitializeProviderResult(&a, &provider)));
    }

    XGameSaveContainerHandle container = nullptr;
    SC_EXPECT(SUCCEEDED(XGameSaveCreateContainer(provider, "edge_cases", &container)));

    // Ensure clean slate by reading a definitely-missing blob
    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "nonexistent_blob_xyz" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 1, &a)));
        Pump(queue, w);
        alignas(8) uint8_t buf[128]{};
        uint32_t count = 0;
        HRESULT hr = XGameSaveReadBlobDataResult(&a, sizeof(buf),
                                                 reinterpret_cast<XGameSaveBlob*>(buf), &count);
        SC_EXPECT(hr == E_GS_BLOB_NOT_FOUND);
    }

    // Write two blobs atomically
    const uint8_t payloadA[] = { 0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77 };
    const uint8_t payloadB[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x01 };
    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "EdgeTest", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "alpha", payloadA, sizeof(payloadA))));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "beta", payloadB, sizeof(payloadB))));

        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    // Read both back: first with an undersized buffer -> E_GS_PROVIDED_BUFFER_TOO_SMALL, then retry!
    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "alpha", "beta" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 2, &a)));
        Pump(queue, w);

        alignas(8) uint8_t tiny[32]{};
        uint32_t count = 0;
        HRESULT tinyHr = XGameSaveReadBlobDataResult(
            &a, sizeof(tiny), reinterpret_cast<XGameSaveBlob*>(tiny), &count);
        SC_EXPECT(tinyHr == E_GS_PROVIDED_BUFFER_TOO_SMALL);

        alignas(8) uint8_t full[512]{};
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataResult(
            &a, sizeof(full), reinterpret_cast<XGameSaveBlob*>(full), &count)));
        SC_EXPECT(count == 2);

        auto* blobs = reinterpret_cast<XGameSaveBlob*>(full);
        SC_EXPECT(std::strcmp(blobs[0].info.name, "alpha") == 0);
        SC_EXPECT(blobs[0].info.size == sizeof(payloadA));
        SC_EXPECT(std::memcmp(blobs[0].data, payloadA, sizeof(payloadA)) == 0);

        SC_EXPECT(std::strcmp(blobs[1].info.name, "beta") == 0);
        SC_EXPECT(blobs[1].info.size == sizeof(payloadB));
        SC_EXPECT(std::memcmp(blobs[1].data, payloadB, sizeof(payloadB)) == 0);
    }

    // Delete 'alpha', verify 'alpha' is gone and 'beta' remains, then clean up 'beta'
    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "DeleteAlpha", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobDelete(upd, "alpha")));
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "alpha" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 1, &a)));
        Pump(queue, w);
        alignas(8) uint8_t buf[256]{};
        uint32_t count = 0;
        SC_EXPECT(XGameSaveReadBlobDataResult(
            &a, sizeof(buf), reinterpret_cast<XGameSaveBlob*>(buf), &count) == E_GS_BLOB_NOT_FOUND);
    }

    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "CleanupBeta", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobDelete(upd, "beta")));
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(provider);
    XTaskQueueCloseHandle(queue);
    return true;
}

// 4. xg_xs: Non-power-of-two (77x53) 4-mip 2D_THIN swizzle & detile round-trip
bool TestXgXsMultiMipAndNpotSwizzle() {
    XG_TEXTURE2D_DESC desc{};
    desc.Width = 77;
    desc.Height = 53;
    desc.MipLevels = 4;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.TileMode = XG_TILE_MODE_2D_THIN;

    ComPtr<IXGTextureAddressComputer> computer;
    XG_RESOURCE_LAYOUT layout{};
    SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&desc, &computer)));
    SC_EXPECT(SUCCEEDED(computer->GetResourceLayout(&layout)));
    SC_EXPECT(layout.BaseAlignmentBytes == 65536ULL);
    SC_EXPECT(layout.Plane[0].MipLevels == 4u);

    const uint32_t physAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    void* tiledMem = XMemAlloc(static_cast<size_t>(layout.SizeBytes), physAttrs);
    SC_EXPECT(tiledMem != nullptr);

    for (uint32_t m = 0; m < 4; ++m) {
        const uint32_t w = std::max(1u, desc.Width >> m);
        const uint32_t h = std::max(1u, desc.Height >> m);
        const uint32_t rowPitch = w * 4;
        SC_EXPECT((layout.Plane[0].MipLayout[m].PaddedWidthElements % 8u) == 0);
        SC_EXPECT((layout.Plane[0].MipLayout[m].PaddedHeightElements % 8u) == 0);

        std::vector<uint8_t> src(static_cast<size_t>(rowPitch) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                src[idx + 0] = static_cast<uint8_t>((x * 7u + m * 19u) & 0xFFu);
                src[idx + 1] = static_cast<uint8_t>((y * 13u + m * 31u) & 0xFFu);
                src[idx + 2] = static_cast<uint8_t>(((x ^ y) + m * 53u) & 0xFFu);
                src[idx + 3] = 0xFFu;
            }
        }

        SC_EXPECT(SUCCEEDED(computer->CopyIntoSubresource(tiledMem, m, src.data(), rowPitch, 0)));

        std::vector<uint8_t> roundTrip(src.size(), 0);
        SC_EXPECT(SUCCEEDED(computer->CopyFromSubresource(roundTrip.data(), m, tiledMem, rowPitch, 0)));
        if (std::memcmp(src.data(), roundTrip.data(), src.size()) != 0) {
            XMemFree(tiledMem, physAttrs);
            SC_EXPECT(false);
        }
    }

    XMemFree(tiledMem, physAttrs);
    return true;
}

// 5. dstorage_xs: VFS T:\ streaming + RetrieveErrorRecord on unsupported compression
bool TestDStorageVfsAndErrorRecord() {
    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\dstorage_probe.bin");
    std::error_code ec;
    std::filesystem::create_directories(hostProbe.parent_path(), ec);

    std::vector<uint8_t> expected(4096);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    {
        std::ofstream out(hostProbe, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.is_open());
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX,
                                           reinterpret_cast<void**>(factory.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "CTestQueue";

    ComPtr<IDStorageQueueX> queue;
    ComPtr<IDStorageFileX> file;
    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX,
                                             reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\dstorage_probe.bin", IID_IDStorageFileX,
                                          reinterpret_cast<void**>(file.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "CTestStatus", IID_IDStorageStatusArrayX,
                                                   reinterpret_cast<void**>(status.GetAddressOf()))));

    // Batch 1: Valid VFS read
    std::vector<uint8_t> actual(expected.size(), 0);
    DSTORAGE_REQUEST okReq{};
    okReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    okReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    okReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    okReq.Source.File.Source = file.Get();
    okReq.Source.File.Offset = 0;
    okReq.Source.File.Size = static_cast<uint32_t>(expected.size());
    okReq.Destination.Memory.Buffer = actual.data();
    okReq.Destination.Memory.Size = static_cast<uint32_t>(actual.size());
    okReq.UncompressedSize = static_cast<uint32_t>(expected.size());
    okReq.Name = "vfs_ok_read";

    queue->EnqueueRequest(&okReq);
    queue->EnqueueStatus(status.Get(), 0);
    queue->Submit();

    while (!status->IsComplete(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(SUCCEEDED(status->GetHResult(0)));
    SC_EXPECT(std::memcmp(expected.data(), actual.data(), expected.size()) == 0);

    // Batch 2: Unsupported ZLIB compression -> verify status + RetrieveErrorRecord
    DSTORAGE_REQUEST badReq = okReq;
    badReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_ZLIB;
    badReq.Name = "unsupported_zlib_req";

    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->Submit();

    while (!status->IsComplete(1)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(status->GetHResult(1) == E_NOTIMPL);

    DSTORAGE_ERROR_RECORD errRec{};
    queue->RetrieveErrorRecord(&errRec);
    SC_EXPECT(errRec.FailureCount == 1u);
    SC_EXPECT(errRec.FirstFailureHResult == E_NOTIMPL);
    SC_EXPECT(std::strcmp(errRec.RequestName, "unsupported_zlib_req") == 0);

    file->Close();
    file.Reset();
    std::filesystem::remove(hostProbe, ec);
    return true;
}

// 6. xaudio2_9: Dynamic hook redirection + WAVE_FORMAT_XMA2 (0x0166) detection hook
bool TestXAudio2Xma2Rejection() {
    ComPtr<IXAudio2> engine;
    SC_EXPECT(SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)));
    SC_EXPECT(engine != nullptr);

    WAVEFORMATEX xma2Fmt{};
    xma2Fmt.wFormatTag = 0x0166; // WAVE_FORMAT_XMA2
    xma2Fmt.nChannels = 2;
    xma2Fmt.nSamplesPerSec = 48000;
    xma2Fmt.wBitsPerSample = 16;

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = engine->CreateSourceVoice(&voice, &xma2Fmt);
    SC_EXPECT(hr == E_NOTIMPL);
    SC_EXPECT(voice == nullptr);
    return true;
}

// 7. xmem: Physical 64KB + Heap alignments, zero-init, and XMemSize invariants
bool TestXMemAlignmentAndInvariants() {
    struct Case {
        size_t size;
        XMemAlignment alignCode;
        size_t expectedAlign;
        XMemType memType;
    };
    const Case cases[] = {
        { 131072, XALLOC_ALIGNMENT_64K, 65536, XALLOC_MEMTYPE_PHYSICAL },
        { 4096,   XALLOC_ALIGNMENT_4K,  4096,  XALLOC_MEMTYPE_HEAP },
        { 1024,   XALLOC_ALIGNMENT_256, 256,   XALLOC_MEMTYPE_HEAP },
        { 320,    XALLOC_ALIGNMENT_64,  64,    XALLOC_MEMTYPE_HEAP }
    };

    for (const auto& c : cases) {
        const uint32_t attrs = MakeXAllocAttributes(XALLOC_ID_DEFAULT, c.alignCode, c.memType, true);
        void* p = XMemAlloc(c.size, attrs);
        SC_EXPECT(p != nullptr);
        SC_EXPECT((reinterpret_cast<uintptr_t>(p) % c.expectedAlign) == 0);
        SC_EXPECT(XMemSize(p, attrs) == c.size);

        const auto* bytes = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < c.size; ++i) {
            if (bytes[i] != 0) {
                XMemFree(p, attrs);
                SC_EXPECT(false);
            }
        }
        XMemFree(p, attrs);
    }
    return true;
}

// 8. scarlett_hook: Dynamic unlinked pixevt + multi-module engine_plugin.dll
bool TestDynamicAndMultiModuleHooks() {
    HMODULE hPix = LoadLibraryW(L"pixevt.dll");
    SC_EXPECT(hPix != nullptr);
    char pixPath[MAX_PATH]{};
    GetModuleFileNameA(hPix, pixPath, MAX_PATH);
    SC_EXPECT(std::strstr(pixPath, "\\shims\\pixevt.dll") != nullptr);

    auto pixBegin = reinterpret_cast<PIXBeginEventFn>(GetProcAddress(hPix, "PIXBeginEvent"));
    auto pixEnd   = reinterpret_cast<PIXEndEventFn>(GetProcAddress(hPix, "PIXEndEvent"));
    auto pixMark  = reinterpret_cast<PIXSetMarkerFn>(GetProcAddress(hPix, "PIXSetMarker"));
    SC_EXPECT(pixBegin && pixEnd && pixMark);
    pixBegin(0xFF00FF00ULL, "CTestScope");
    pixMark(0xFF00FF00ULL, "CTestMarker");
    pixEnd();
    FreeLibrary(hPix);

    HMODULE hPlugin = LoadLibraryW(L"..\\dummy_title\\engine_plugin.dll");
    SC_EXPECT(hPlugin != nullptr);
    using PluginFn = bool(WINAPI*)();
    auto runDiag = reinterpret_cast<PluginFn>(GetProcAddress(hPlugin, "PluginRunDiagnostics"));
    SC_EXPECT(runDiag != nullptr);
    const bool pluginOk = runDiag();
    FreeLibrary(hPlugin);
    SC_EXPECT(pluginOk);
    return true;
}

// 9. d3d12_xs: Headless device, frame tokens, DXBC shader inspection & PSO cache
bool TestD3D12HeadlessDeviceAndPso() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    SC_EXPECT(SUCCEEDED(deviceX->SetFrameIntervalX(nullptr, 16667, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)));

    D3D12XBOX_FRAME_PIPELINE_TOKEN t1 = 0;
    D3D12XBOX_FRAME_PIPELINE_TOKEN t2 = 0;
    SC_EXPECT(SUCCEEDED(deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, &t1)));
    SC_EXPECT(SUCCEEDED(deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, &t2)));
    SC_EXPECT(t2 == t1 + 1);

    const char* hlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION { return float4(0, 0, 0, 1); }\n"
        "float4 PSMain() : SV_TARGET { return float4(0.1, 0.9, 0.4, 1.0); }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "ctest.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "ctest.hlsl", nullptr, nullptr,
                                   "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))));
    SC_EXPECT(pso != nullptr);
    return true;
}

} // namespace

int main() {
    std::printf("=== Scarlett Layer Automated Shim Verification Suite ===\n");

    RunTest("VfsPathTranslation",         TestVfsPathTranslation);
    RunTest("RuntimeAndXUser",            TestRuntimeAndXUser);
    RunTest("XGameSaveEdgeCases",         TestXGameSaveEdgeCases);
    RunTest("XgXsMultiMipAndNpotSwizzle", TestXgXsMultiMipAndNpotSwizzle);
    RunTest("DStorageVfsAndErrorRecord",  TestDStorageVfsAndErrorRecord);
    RunTest("XAudio2Xma2Rejection",       TestXAudio2Xma2Rejection);
    RunTest("XMemAlignmentAndInvariants", TestXMemAlignmentAndInvariants);
    RunTest("DynamicAndMultiModuleHooks", TestDynamicAndMultiModuleHooks);
    RunTest("D3D12HeadlessDeviceAndPso",  TestD3D12HeadlessDeviceAndPso);

    XGameRuntimeUninitialize();

    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}