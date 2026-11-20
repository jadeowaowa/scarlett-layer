#include "common/codecs.hpp"
#include "common/vfs.hpp"
#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"
#include "shims/d3d12_xs/shader_inspector.hpp"
#include "shims/d3d12_xs/shader_translator.hpp"
#include "shims/gameinput/gameinput.hpp"
#include "shims/xg_xs/xg_xs.hpp"
#include "shims/dstorage_xs/dstorage_xs.hpp"
#include "shims/pixevt/pixevt.hpp"
#include "shims/xmem/xmem.hpp"
#include "shims/kernelx/kernelx.hpp"

#include <d3dcompiler.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

bool TestXAudio2Xma2Rejection() {
    ComPtr<IXAudio2> engine;
    SC_EXPECT(SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)));
    SC_EXPECT(engine != nullptr);

    WAVEFORMATEX xma2Fmt{};
    xma2Fmt.wFormatTag = 0x0166;
    xma2Fmt.nChannels = 2;
    xma2Fmt.nSamplesPerSec = 48000;
    xma2Fmt.wBitsPerSample = 16;

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = engine->CreateSourceVoice(&voice, &xma2Fmt);
    SC_EXPECT(hr == E_NOTIMPL);
    SC_EXPECT(voice == nullptr);
    return true;
}

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

bool TestKernelXMemoryAndTopology() {
    TITLEMEMORYSTATUS before{};
    before.dwLength = sizeof(before);
    SC_EXPECT(TitleMemoryStatus(&before) != FALSE);
    SC_EXPECT(before.ullTotalMem == SC_SERIES_X_TITLE_MEM_BYTES);

    void* gfxBuf = VirtualAllocX(
        nullptr, 256 * 1024, MEM_RESERVE | MEM_COMMIT | SC_MEM_GRAPHICS, PAGE_READWRITE);
    SC_EXPECT(gfxBuf != nullptr);
    SC_EXPECT((reinterpret_cast<uintptr_t>(gfxBuf) % 65536ULL) == 0);

    TITLEMEMORYSTATUS during{};
    during.dwLength = sizeof(during);
    SC_EXPECT(TitleMemoryStatus(&during) != FALSE);
    SC_EXPECT(during.ullTitleUsed >= before.ullTitleUsed + 256 * 1024);
    SC_EXPECT(during.ullTitleAvail == during.ullTotalMem - during.ullTitleUsed);

    SC_EXPECT(VirtualFreeX(gfxBuf, 0, MEM_RELEASE) != FALSE);

    TITLEMEMORYSTATUS after{};
    after.dwLength = sizeof(after);
    SC_EXPECT(TitleMemoryStatus(&after) != FALSE);
    SC_EXPECT(after.ullTitleUsed == before.ullTitleUsed);

    SCARLETT_PROCESSOR_INFO cpu{};
    SC_EXPECT(GetConsoleProcessorInfo(&cpu) != FALSE);
    SC_EXPECT(cpu.TitleCoreCount == 7u);
    SC_EXPECT(cpu.TitleLogicalThreadCount == 14u);
    SC_EXPECT(cpu.HostLogicalThreadCount >= 1u);
    SC_EXPECT(cpu.HostProcessAffinityMask != 0);

    DWORD_PTR prevMask = SetThreadAffinityMaskX(GetCurrentThread(), static_cast<DWORD_PTR>(~0ULL));
    SC_EXPECT(prevMask != 0);
    SC_EXPECT(SetThreadAffinityMaskX(GetCurrentThread(), prevMask) != 0);
    return true;
}

bool TestD3D12PlacedResourcesAndHeap() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_RESOURCE_DESC texA{};
    texA.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texA.Width = 128;
    texA.Height = 128;
    texA.DepthOrArraySize = 1;
    texA.MipLevels = 1;
    texA.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texA.SampleDesc.Count = 1;

    D3D12_RESOURCE_DESC texB = texA;
    texB.Width = 256;
    texB.Height = 256;

    D3D12_RESOURCE_ALLOCATION_INFO infoA = deviceX->GetResourceAllocationInfo(0, 1, &texA);
    D3D12_RESOURCE_ALLOCATION_INFO infoB = deviceX->GetResourceAllocationInfo(0, 1, &texB);
    SC_EXPECT(infoA.SizeInBytes > 0 && infoB.SizeInBytes > 0);

    const UINT64 align = std::max<UINT64>(
        std::max<UINT64>(infoA.Alignment, infoB.Alignment),
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    const UINT64 offsetB = (infoA.SizeInBytes + (align - 1ULL)) & ~(align - 1ULL);
    const UINT64 totalHeapBytes = offsetB + infoB.SizeInBytes;

    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = totalHeapBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = align;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

    ComPtr<ID3D12Heap> heap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))));

    ComPtr<ID3D12Resource> resA, resB;
    SC_EXPECT(SUCCEEDED(deviceX->CreatePlacedResource(
        heap.Get(), 0, &texA, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resA))));
    SC_EXPECT(SUCCEEDED(deviceX->CreatePlacedResource(
        heap.Get(), offsetB, &texB, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resB))));

    SC_EXPECT(resA->GetDesc().Width == 128);
    SC_EXPECT(resB->GetDesc().Width == 256);

    resB.Reset();
    resA.Reset();
    heap.Reset();
    return true;
}

// 12. Deep SM 6.x DXIL container & LLVM bitcode header parsing
bool TestDxilSm6BitcodeInspector() {
    // Synthesize a valid DXBC container with PSV0 + DXIL (ms_6_6, DXIL 1.6, LLVM BC\xC0\xDE)
    const uint8_t fakeLlvmBc[16] = {
        0x42u, 0x43u, 0xC0u, 0xDEu, 0x21u, 0x0Cu, 0x00u, 0x00u,
        0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u
    };

    sc::d3d12::DxilProgramHeader dxilHdr{};
    dxilHdr.programVersion = (13u << 16) | (6u << 4) | 6u; // ms_6_6 (Mesh Shader SM 6.6)
    dxilHdr.sizeInUint32   = static_cast<uint32_t>((sizeof(dxilHdr) + sizeof(fakeLlvmBc)) / 4u);
    dxilHdr.dxilMagic      = sc::d3d12::kDxilMagicFourCc;
    dxilHdr.dxilVersion    = (1u << 8) | 6u;               // DXIL 1.6
    dxilHdr.bitcodeOffset  = 16u;                          // +8 + 16 = 24 (immediately after header)
    dxilHdr.bitcodeSize    = sizeof(fakeLlvmBc);

    const uint32_t psvPayload = 0x00000002u;

    const uint32_t partCount = 2;
    const uint32_t psvOff = sizeof(sc::d3d12::DxbcHeader) + partCount * sizeof(uint32_t);
    const uint32_t dxilOff = psvOff + sizeof(sc::d3d12::DxbcPartHeader) + sizeof(psvPayload);
    const uint32_t totalSize = dxilOff + sizeof(sc::d3d12::DxbcPartHeader)
                             + sizeof(dxilHdr) + sizeof(fakeLlvmBc);

    std::vector<uint8_t> container(totalSize, 0);

    sc::d3d12::DxbcHeader hdr{};
    std::memcpy(hdr.magic, "DXBC", 4);
    hdr.majorVersion = 1;
    hdr.minorVersion = 0;
    hdr.fileSize = totalSize;
    hdr.partCount = partCount;
    std::memcpy(container.data(), &hdr, sizeof(hdr));
    std::memcpy(container.data() + sizeof(hdr), &psvOff, sizeof(uint32_t));
    std::memcpy(container.data() + sizeof(hdr) + sizeof(uint32_t), &dxilOff, sizeof(uint32_t));

    sc::d3d12::DxbcPartHeader psvPart{ {'P','S','V','0'}, sizeof(psvPayload) };
    std::memcpy(container.data() + psvOff, &psvPart, sizeof(psvPart));
    std::memcpy(container.data() + psvOff + sizeof(psvPart), &psvPayload, sizeof(psvPayload));

    sc::d3d12::DxbcPartHeader dxilPart{ {'D','X','I','L'}, sizeof(dxilHdr) + sizeof(fakeLlvmBc) };
    std::memcpy(container.data() + dxilOff, &dxilPart, sizeof(dxilPart));
    std::memcpy(container.data() + dxilOff + sizeof(dxilPart), &dxilHdr, sizeof(dxilHdr));
    std::memcpy(container.data() + dxilOff + sizeof(dxilPart) + sizeof(dxilHdr),
                fakeLlvmBc, sizeof(fakeLlvmBc));

    D3D12_SHADER_BYTECODE bc{ container.data(), container.size() };
    auto info = sc::d3d12::InspectShaderBytecodeDetailed(bc);
    SC_EXPECT(info.kind == sc::d3d12::ShaderContainerKind::Dxil);
    SC_EXPECT(info.profile == "ms_6_6");
    SC_EXPECT(info.dxilMajor == 1u);
    SC_EXPECT(info.dxilMinor == 6u);
    SC_EXPECT(info.hasLlvmBitcode);
    SC_EXPECT(info.bitcodeSize == sizeof(fakeLlvmBc));
    SC_EXPECT(info.hasPsv0);
    SC_EXPECT(info.summary.find("ms_6_6/DXIL 1.6 (LLVM BC)") != std::string::npos);
    return true;
}

// 13. Pluggable SCXS console shader container translator + translation/PSO cache + RDNA2/CRC traps
bool TestScxsTranslatorAndPsoCache() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    const char* hlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION { return float4(0, 0, 0, 1); }\n"
        "float4 PSMain() : SV_TARGET { return float4(0.2, 0.8, 0.9, 1.0); }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "scxs_test.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "scxs_test.hlsl", nullptr, nullptr,
                                   "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    auto vsScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 5, 0);
    auto psScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), 5, 0);

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsScxs.data(), vsScxs.size() };
    psoDesc.PS = { psScxs.data(), psScxs.size() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Pass 1: Translate SCXS & create/load PSO
    ComPtr<ID3D12PipelineState> pso1;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(pso1 != nullptr);

    // Pass 2: Same SCXS containers -> exercises in-memory translation cache + PSO library HIT
    ComPtr<ID3D12PipelineState> pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso2 != nullptr);

    // Pass 3: Corrupted SCXS payload -> must fail checksum validation with E_INVALIDARG
    auto badCrcVs = vsScxs;
    badCrcVs.back() ^= 0xFFu;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC badCrcDesc = psoDesc;
    badCrcDesc.VS = { badCrcVs.data(), badCrcVs.size() };
    ComPtr<ID3D12PipelineState> psoBadCrc;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&badCrcDesc, IID_PPV_ARGS(&psoBadCrc)) == E_INVALIDARG);
    SC_EXPECT(psoBadCrc == nullptr);

    // Pass 4: Raw RDNA2 ISA SCXS container -> must cleanly trap with E_NOTIMPL at translator seam
    const uint32_t fakeRdna2Words[4] = { 0xBF810000u, 0xBF800000u, 0xBF800000u, 0xBF810000u };
    auto rdna2Vs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        fakeRdna2Words, sizeof(fakeRdna2Words), 6, 6);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rdna2Desc = psoDesc;
    rdna2Desc.VS = { rdna2Vs.data(), rdna2Vs.size() };
    ComPtr<ID3D12PipelineState> psoRdna2;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&rdna2Desc, IID_PPV_ARGS(&psoRdna2)) == E_NOTIMPL);
    SC_EXPECT(psoRdna2 == nullptr);
    return true;
}

// 14. dstorage_xs: Live SCLZ CPU decompression over VFS + corrupt stream error trapping
bool TestDStorageLzDecompression() {
    constexpr size_t kRawSize = 65536;
    std::vector<uint8_t> raw(kRawSize);
    for (size_t i = 0; i < kRawSize; ++i) {
        // Structured repeating tile-like pattern that compresses well
        raw[i] = static_cast<uint8_t>(((i & 0x3Fu) * 3u + ((i >> 8) & 0x0Fu)) & 0xFFu);
    }

    std::vector<uint8_t> comp = sc::codec::CompressLzBlock(raw.data(), raw.size());
    SC_EXPECT(!comp.empty());
    SC_EXPECT(comp.size() < raw.size() / 2u); // Expect > 2x compression

    const std::filesystem::path hostPath = sc::vfs::TranslatePath(L"T:\\compressed_probe.sclz");
    std::error_code ec;
    std::filesystem::create_directories(hostPath.parent_path(), ec);
    {
        std::ofstream out(hostPath, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.is_open());
        out.write(reinterpret_cast<const char*>(comp.data()),
                  static_cast<std::streamsize>(comp.size()));
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX,
                                           reinterpret_cast<void**>(factory.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_HIGH;
    qd.Name = "CTestLzQueue";

    ComPtr<IDStorageQueueX> queue;
    ComPtr<IDStorageFileX> file;
    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX,
                                             reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\compressed_probe.sclz", IID_IDStorageFileX,
                                          reinterpret_cast<void**>(file.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "CTestLzStatus", IID_IDStorageStatusArrayX,
                                                   reinterpret_cast<void**>(status.GetAddressOf()))));

    std::vector<uint8_t> decompressed(kRawSize, 0);
    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(comp.size());
    req.Destination.Memory.Buffer = decompressed.data();
    req.Destination.Memory.Size = static_cast<uint32_t>(decompressed.size());
    req.UncompressedSize = static_cast<uint32_t>(kRawSize);
    req.Name = "sclz_vfs_read";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->Submit();

    while (!status->IsComplete(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(SUCCEEDED(status->GetHResult(0)));
    SC_EXPECT(std::memcmp(raw.data(), decompressed.data(), kRawSize) == 0);

    // Corrupt compressed stream in memory -> must fail cleanly with E_FAIL and log error record
    std::vector<uint8_t> corrupt = comp;
    corrupt.back() ^= 0xFFu;
    std::vector<uint8_t> discard(kRawSize, 0);

    DSTORAGE_REQUEST badReq{};
    badReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    badReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    badReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    badReq.Source.Memory.Source = corrupt.data();
    badReq.Source.Memory.Size = static_cast<uint32_t>(corrupt.size());
    badReq.Destination.Memory.Buffer = discard.data();
    badReq.Destination.Memory.Size = static_cast<uint32_t>(discard.size());
    badReq.UncompressedSize = static_cast<uint32_t>(kRawSize);
    badReq.Name = "corrupt_sclz_req";

    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->Submit();

    while (!status->IsComplete(1)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(status->GetHResult(1) == E_FAIL);

    DSTORAGE_ERROR_RECORD errRec{};
    queue->RetrieveErrorRecord(&errRec);
    SC_EXPECT(errRec.FailureCount == 1u);
    SC_EXPECT(errRec.FirstFailureHResult == E_FAIL);
    SC_EXPECT(std::strcmp(errRec.RequestName, "corrupt_sclz_req") == 0);

    file->Close();
    file.Reset();
    std::filesystem::remove(hostPath, ec);
    return true;
}

// 15. xaudio2_9: Live 4:1 IMA-ADPCM codec accuracy + ScarlettDecodingSourceVoice bridge
bool TestXAudio2ImaAdpcmDecodeBridge() {
    constexpr size_t kSamples = 4800; // 100 ms @ 48 kHz
    std::vector<int16_t> orig(kSamples);
    for (size_t i = 0; i < kSamples; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        orig[i] = static_cast<int16_t>(std::sin(6.28318530718 * 440.0 * t) * 24000.0);
    }

    auto adpcm = sc::codec::EncodeImaAdpcmMono(orig.data(), orig.size());
    SC_EXPECT(!adpcm.empty());
    SC_EXPECT(adpcm.size() < (orig.size() * sizeof(int16_t)) / 3u); // ~3.95x reduction

    std::vector<int16_t> decoded;
    SC_EXPECT(sc::codec::DecodeImaAdpcmMono(adpcm.data(), adpcm.size(),
                                            sc::codec::kImaAdpcmBlockAlign, decoded));
    SC_EXPECT(decoded.size() >= orig.size());

    double signalEnergy = 0.0;
    double noiseEnergy = 0.0;
    for (size_t i = 0; i < orig.size(); ++i) {
        const double s = static_cast<double>(orig[i]);
        const double e = static_cast<double>(orig[i]) - static_cast<double>(decoded[i]);
        signalEnergy += s * s;
        noiseEnergy += e * e;
    }
    const double snrDb = 10.0 * std::log10(signalEnergy / std::max(noiseEnergy, 1.0));
    SC_EXPECT(snrDb > 30.0);

    // Exercise live ScarlettDecodingSourceVoice through xaudio2_9.dll (silent, never started)
    ComPtr<IXAudio2> engine;
    SC_EXPECT(SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)));

    IXAudio2MasteringVoice* master = nullptr;
    if (SUCCEEDED(engine->CreateMasteringVoice(&master)) && master) {
        WAVEFORMATEX adpcmFmt{};
        adpcmFmt.wFormatTag      = sc::codec::kWaveFormatImaAdpcm;
        adpcmFmt.nChannels       = 1;
        adpcmFmt.nSamplesPerSec  = 48000;
        adpcmFmt.wBitsPerSample  = 4;
        adpcmFmt.nBlockAlign     = sc::codec::kImaAdpcmBlockAlign;
        adpcmFmt.nAvgBytesPerSec = (48000 * sc::codec::kImaAdpcmBlockAlign) / sc::codec::kImaAdpcmSamplesPerBlock;

        IXAudio2SourceVoice* voice = nullptr;
        SC_EXPECT(SUCCEEDED(engine->CreateSourceVoice(&voice, &adpcmFmt)));
        SC_EXPECT(voice != nullptr);

        int userSentinel = 0x53434152;
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(adpcm.size());
        buf.pAudioData = adpcm.data();
        buf.Flags = XAUDIO2_END_OF_STREAM;
        buf.pContext = &userSentinel;

        SC_EXPECT(SUCCEEDED(voice->SubmitSourceBuffer(&buf)));

        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st);
        SC_EXPECT(st.BuffersQueued == 1u);
        SC_EXPECT(st.pCurrentBufferContext == &userSentinel);

        SC_EXPECT(SUCCEEDED(voice->FlushSourceBuffers()));
        voice->DestroyVoice();
        master->DestroyVoice();
    }
    return true;
}

// 16. d3d12_xs: SCXS Compute PSO + cache + UAV RWStructuredBuffer GPU dispatch & readback
bool TestD3D12ComputePsoAndUavExecution() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue)));

    const char* csHlsl =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void CSMain(uint3 dtid : SV_DispatchThreadID) {\n"
        "    gOut[dtid.x] = dtid.x * 7u + 0x53430000u;\n"
        "}\n";

    ComPtr<ID3DBlob> csBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(csHlsl, std::strlen(csHlsl), "ctest_compute.hlsl", nullptr, nullptr,
                                   "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errBlob)));

    auto csScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), 5, 0);

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &uavRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csScxs.data(), csScxs.size() };

    // Pass 1 & 2: Translate SCXS compute shader + hit translation & PSO library cache
    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso1 != nullptr && pso2 != nullptr);

    // Corrupted CRC & RDNA2 ISA traps on Compute PSO path
    auto badCrcCs = csScxs;
    badCrcCs.back() ^= 0xFFu;
    D3D12_COMPUTE_PIPELINE_STATE_DESC badDesc = psoDesc;
    badDesc.CS = { badCrcCs.data(), badCrcCs.size() };
    ComPtr<ID3D12PipelineState> badPso;
    SC_EXPECT(deviceX->CreateComputePipelineState(&badDesc, IID_PPV_ARGS(&badPso)) == E_INVALIDARG);

    const uint32_t fakeRdna2[4] = { 0xBF810000u, 0xBF800000u, 0xBF800000u, 0xBF810000u };
    auto rdna2Cs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        fakeRdna2, sizeof(fakeRdna2), 6, 6);
    D3D12_COMPUTE_PIPELINE_STATE_DESC rdna2Desc = psoDesc;
    rdna2Desc.CS = { rdna2Cs.data(), rdna2Cs.size() };
    SC_EXPECT(deviceX->CreateComputePipelineState(&rdna2Desc, IID_PPV_ARGS(&badPso)) == E_NOTIMPL);

    // Allocate GPU UAV buffer + CPU readback buffer (64 uint32_t = 256 bytes)
    constexpr UINT kElemCount = 64;
    constexpr UINT kBufBytes = kElemCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC uavBufDesc{};
    uavBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavBufDesc.Width = kBufBytes;
    uavBufDesc.Height = 1;
    uavBufDesc.DepthOrArraySize = 1;
    uavBufDesc.MipLevels = 1;
    uavBufDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavBufDesc.SampleDesc.Count = 1;
    uavBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &uavBufDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uavBuf))));

    D3D12_RESOURCE_DESC rbDesc = uavBufDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readbackBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kElemCount;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    deviceX->CreateUnorderedAccessView(
        uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(rootSig.Get());
    cmdList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = uavBuf.Get();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = uavBuf.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(2, barriers);

    cmdList->CopyResource(readbackBuf.Get(), uavBuf.Get());
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);

    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvt != nullptr);
    SC_EXPECT(SUCCEEDED(cmdQueue->Signal(fence.Get(), 1)));
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
    CloseHandle(fenceEvt);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kBufBytes };
    SC_EXPECT(SUCCEEDED(readbackBuf->Map(0, &readRange, &mapped)));
    const auto* results = static_cast<const uint32_t*>(mapped);
    bool allExact = true;
    for (UINT i = 0; i < kElemCount; ++i) {
        if (results[i] != (i * 7u + 0x53430000u)) {
            allExact = false;
            break;
        }
    }
    D3D12_RANGE emptyWrite{ 0, 0 };
    readbackBuf->Unmap(0, &emptyWrite);
    SC_EXPECT(allExact);
    return true;
}

// 17. d3d12_xs: Real binary RDNA2 ISA pixel shader -> Decode -> Lift -> DXBC -> RTX 5070 execution & readback
bool TestRdna2IsaDecoderAndGpuExecution() {
    using namespace sc::d3d12::rdna2;

    // Assemble a real 12-DWORD RDNA2 (GFX10.3) binary pixel shader:
    //   v0 = 0.25f                                  -> R = 0.2500 (64/255)
    //   v1 = 0.50f; v1 = v0 + v1                    -> G = 0.7500 (191/255)
    //   v2 = 0.50f; v2 = 0.5*v0 + v2 = 0.625
    //   v2 = saturate(mad(v2, 0.5, v0)) = 0.5625    -> B = 0.5625 (143/255)
    //   v3 = 1.00f                                  -> A = 1.0000 (255/255)
    //   exp mrt0, v0, v1, v2, v3 done vm
    //   s_endpgm
    std::vector<uint32_t> rdna2Words;
    AppendVop1LiteralF32(rdna2Words, kVop1MovB32, 0, 0.25f);
    AppendVop1(rdna2Words, kVop1MovB32, 1, kSrcInlineHalf);
    AppendVop2(rdna2Words, kVop2AddF32, 1, VgprSrc(0), 1);
    AppendVop1(rdna2Words, kVop1MovB32, 2, kSrcInlineHalf);
    AppendVop2(rdna2Words, kVop2FmacF32, 2, kSrcInlineHalf, 0);
    AppendVop3(rdna2Words, kVop3MadF32, 2, VgprSrc(2), kSrcInlineHalf, VgprSrc(0), true);
    AppendVop1(rdna2Words, kVop1MovB32, 3, kSrcInlineOne);
    AppendExpMrt0(rdna2Words, 0, 1, 2, 3, 0xFu, true, true);
    AppendSopp(rdna2Words, kSoppEndpgm);

    DecodedProgram decoded{};
    std::string decErr;
    SC_EXPECT(DecodeProgram(rdna2Words.data(), rdna2Words.size() * sizeof(uint32_t), decoded, decErr));
    SC_EXPECT(decoded.insts.size() == 9u);

    const std::string asmText = Disassemble(decoded);
    SC_EXPECT(asmText.find("v_fmac_f32") != std::string::npos);
    SC_EXPECT(asmText.find("v_mad_f32") != std::string::npos);
    SC_EXPECT(asmText.find("exp mrt0") != std::string::npos);
    SC_EXPECT(asmText.find("s_endpgm") != std::string::npos);

    auto psRdna2Scxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        rdna2Words.data(), rdna2Words.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue)));

    const char* vsHlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION {\n"
        "    float2 p = float2((id == 2) ? 3.0f : -1.0f, (id == 1) ? 3.0f : -1.0f);\n"
        "    return float4(p, 0.0f, 1.0f);\n"
        "}\n";
    ComPtr<ID3DBlob> vsBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(vsHlsl, std::strlen(vsHlsl), "rdna2_fs_vs.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psRdna2Scxs.data(), psRdna2Scxs.size() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Pass 1 & Pass 2: RDNA2 binary lift + PSO cache hit
    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso1 != nullptr && pso2 != nullptr);

    // Corrupted RDNA2 SCXS payload -> E_INVALIDARG
    auto corruptPs = psRdna2Scxs;
    corruptPs.back() ^= 0xFFu;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC corruptDesc = psoDesc;
    corruptDesc.PS = { corruptPs.data(), corruptPs.size() };
    ComPtr<ID3D12PipelineState> badPso;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&corruptDesc, IID_PPV_ARGS(&badPso)) == E_INVALIDARG);

    // Render fullscreen triangle into 16x16 RT and read back RGBA8 pixels from GPU
    constexpr UINT kRtSize = 16;
    constexpr UINT kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; // 256 B
    constexpr UINT kReadbackBytes = kRowPitch * kRtSize;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kReadbackBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kRtSize), static_cast<float>(kRtSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kRtSize), static_cast<LONG>(kRtSize) };
    cmdList->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rtTex.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rtTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = kRtSize;
    dstLoc.PlacedFootprint.Footprint.Height = kRtSize;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);

    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvt != nullptr);
    SC_EXPECT(SUCCEEDED(cmdQueue->Signal(fence.Get(), 1)));
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
    CloseHandle(fenceEvt);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kReadbackBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    const uint8_t* centerPx = pixels + (8u * kRowPitch) + (8u * 4u);
    const bool rOk = std::abs(static_cast<int>(centerPx[0]) - 64)  <= 1;
    const bool gOk = std::abs(static_cast<int>(centerPx[1]) - 191) <= 1;
    const bool bOk = std::abs(static_cast<int>(centerPx[2]) - 143) <= 1;
    const bool aOk = (centerPx[3] == 255);
    D3D12_RANGE emptyWrite{ 0, 0 };
    rbBuf->Unmap(0, &emptyWrite);

    SC_EXPECT(rOk && gOk && bOk && aOk);
    return true;
}

// 18. d3d12_xs: Dedicated COMPUTE queue -> GPU-side fence Wait() on DIRECT queue -> readback
bool TestD3D12AsyncComputeCrossQueueSync() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueueX> computeQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&cqDesc, &computeQueue)));

    D3D12_COMMAND_QUEUE_DESC dqDesc{};
    dqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> directQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&dqDesc, &directQueue)));

    const char* csHlsl =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void CSMain(uint3 dtid : SV_DispatchThreadID) {\n"
        "    gOut[dtid.x] = dtid.x * 13u + 0xA5A50000u;\n"
        "}\n";

    ComPtr<ID3DBlob> csBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(csHlsl, std::strlen(csHlsl), "async_cs.hlsl", nullptr, nullptr,
                                   "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errBlob)));

    auto csScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), 5, 0);

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &uavRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csScxs.data(), csScxs.size() };

    ComPtr<ID3D12PipelineState> computePso;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePso))));

    constexpr UINT kElemCount = 64;
    constexpr UINT kBufBytes = kElemCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC uavBufDesc{};
    uavBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavBufDesc.Width = kBufBytes;
    uavBufDesc.Height = 1;
    uavBufDesc.DepthOrArraySize = 1;
    uavBufDesc.MipLevels = 1;
    uavBufDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavBufDesc.SampleDesc.Count = 1;
    uavBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &uavBufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));

    D3D12_RESOURCE_DESC rbDesc = uavBufDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readbackBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kElemCount;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    deviceX->CreateUnorderedAccessView(
        uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> compAlloc, dirAlloc;
    ComPtr<ID3D12GraphicsCommandList> compList, dirList;
    ComPtr<ID3D12Fence> syncFence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compAlloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compAlloc.Get(), computePso.Get(), IID_PPV_ARGS(&compList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dirAlloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dirAlloc.Get(), nullptr, IID_PPV_ARGS(&dirList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&syncFence))));

    // Record & submit on COMPUTE queue
    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    compList->SetDescriptorHeaps(1, heaps);
    compList->SetComputeRootSignature(rootSig.Get());
    compList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    compList->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = uavBuf.Get();
    compList->ResourceBarrier(1, &uavBar);
    SC_EXPECT(SUCCEEDED(compList->Close()));

    ID3D12CommandList* compLists[] = { compList.Get() };
    computeQueue->ExecuteCommandLists(1, compLists);
    SC_EXPECT(SUCCEEDED(computeQueue->Signal(syncFence.Get(), 1)));

    // GPU-side cross-queue Wait() on DIRECT queue (no CPU wait between compute & copy)
    SC_EXPECT(SUCCEEDED(directQueue->Wait(syncFence.Get(), 1)));
    dirList->CopyResource(readbackBuf.Get(), uavBuf.Get());
    SC_EXPECT(SUCCEEDED(dirList->Close()));

    ID3D12CommandList* dirLists[] = { dirList.Get() };
    directQueue->ExecuteCommandLists(1, dirLists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(syncFence.Get(), 2)));

    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvt != nullptr);
    if (syncFence->GetCompletedValue() < 2) {
        syncFence->SetEventOnCompletion(2, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
    CloseHandle(fenceEvt);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kBufBytes };
    SC_EXPECT(SUCCEEDED(readbackBuf->Map(0, &readRange, &mapped)));
    const auto* results = static_cast<const uint32_t*>(mapped);
    bool allExact = true;
    for (UINT i = 0; i < kElemCount; ++i) {
        if (results[i] != (i * 13u + 0xA5A50000u)) {
            allExact = false;
            break;
        }
    }
    D3D12_RANGE emptyWrite{ 0, 0 };
    readbackBuf->Unmap(0, &emptyWrite);
    SC_EXPECT(allExact);
    return true;
}

// 19. d3d12_xs: Real binary RDNA2 ISA compute shader -> Decode -> Lift -> cs_5_0 -> RTX 5070 UAV readback
bool TestRdna2ComputeIsaExecution() {
    using namespace sc::d3d12::rdna2;

    // Binary RDNA2 compute kernel: gOut[dtid.x] = dtid.x * 17u + 0x52440000u
    std::vector<uint32_t> csWords;
    AppendVop3(csWords, kVop3MulLoU32, 1, VgprSrc(0), 128u + 17u, 128u); // v1 = v0 * 17
    AppendVop2LiteralU32(csWords, kVop2AddNcU32, 1, 0x52440000u, 1);     // v1 = v1 + 0x52440000
    AppendMubufStoreDword(csWords, 1, 0, 0);                             // gOut[v0] = v1
    AppendSopp(csWords, kSoppEndpgm);

    DecodedProgram decoded{};
    std::string err;
    SC_EXPECT(DecodeProgram(csWords.data(), csWords.size() * sizeof(uint32_t), decoded, err));
    SC_EXPECT(decoded.insts.size() == 4u);
    const std::string asmText = Disassemble(decoded);
    SC_EXPECT(asmText.find("v_mul_lo_u32") != std::string::npos);
    SC_EXPECT(asmText.find("v_add_nc_u32") != std::string::npos);
    SC_EXPECT(asmText.find("buffer_store_dword") != std::string::npos);

    auto csRdna2Scxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        csWords.data(), csWords.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{ D3D12_COMMAND_LIST_TYPE_COMPUTE };
    ComPtr<ID3D12CommandQueueX> compQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &compQueue)));

    D3D12_DESCRIPTOR_RANGE uavRange{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 1, &uavRange };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    ComPtr<ID3DBlob> rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csRdna2Scxs.data(), csRdna2Scxs.size() };

    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));

    constexpr UINT kElems = 64;
    constexpr UINT kBytes = kElems * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf, rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uavHeap))));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer = { 0, kElems, sizeof(uint32_t), 0, D3D12_BUFFER_UAV_FLAG_NONE };
    deviceX->CreateUnorderedAccessView(uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(rootSig.Get());
    cmdList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = uavBuf.Get();
    D3D12_RESOURCE_BARRIER cb[2]{}; cb[0]=uavBar; cb[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; cb[1].Transition={uavBuf.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE}; cmdList->ResourceBarrier(2, cb);
    cmdList->CopyResource(rbBuf.Get(), uavBuf.Get());
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    compQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(compQueue->Signal(fence.Get(), 1)));

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, kBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &rr, &mapped)));
    const auto* out = static_cast<const uint32_t*>(mapped);
    bool exact = true;
    for (UINT i = 0; i < kElems; ++i) {
        if (out[i] != (i * 17u + 0x52440000u)) { exact = false; break; }
    }
    D3D12_RANGE ew{ 0, 0 };
    rbBuf->Unmap(0, &ew);
    SC_EXPECT(exact);
    return true;
}

// 20. d3d12_xs: Full binary RDNA2 VS + PS pipeline with SMEM CBV loads, VINTRP handoff & GPU readback
bool TestRdna2VertexAndPixelPipelineWithCbv() {
    using namespace sc::d3d12::rdna2;

    // Binary RDNA2 Vertex Shader:
    //   s_load_dwordx4 s[4:7], s[0:1], 0x00   -> scale=(8,8), offset=(7,7)
    //   s_waitcnt
    //   v0 = s4 * v0 + s6; v1 = s5 * v1 + s7  -> maps [-1,-0.5] triangle onto [-1,3] full viewport
    //   exp pos0, v0, v1, v2, v3
    //   exp param0, v4, v5, v6, v7 done       -> forwards vertex color (0.8, 0.5, 0.8, 1.0)
    //   s_endpgm
    std::vector<uint32_t> vsWords;
    AppendSmemLoad(vsWords, kSmemLoadDwordX4, 4, 0, 0x00u);
    AppendSopp(vsWords, kSoppWaitcnt, 0);
    AppendVop2(vsWords, kVop2MulF32, 0, SgprSrc(4), 0);
    AppendVop2(vsWords, kVop2AddF32, 0, SgprSrc(6), 0);
    AppendVop2(vsWords, kVop2MulF32, 1, SgprSrc(5), 1);
    AppendVop2(vsWords, kVop2AddF32, 1, SgprSrc(7), 1);
    AppendExp(vsWords, kExpTargetPos0,   0, 1, 2, 3, 0xFu, false, false);
    AppendExp(vsWords, kExpTargetParam0, 4, 5, 6, 7, 0xFu, true,  false);
    AppendSopp(vsWords, kSoppEndpgm);

    // Binary RDNA2 Pixel Shader:
    //   s_load_dwordx4 s[8:11], s[0:1], 0x10  -> tint=(0.5, 1.0, 0.25, 1.0)
    //   s_waitcnt
    //   v0..v2 = param0.xyz via VINTRP
    //   v0 *= s8 (0.40); v1 *= s9 (0.50); v2 *= s10 (0.20); v3 = s11 (1.0)
    //   exp mrt0, v0, v1, v2, v3 done vm
    //   s_endpgm
    std::vector<uint32_t> psWords;
    AppendSmemLoad(psWords, kSmemLoadDwordX4, 8, 0, 0x10u);
    AppendSopp(psWords, kSoppWaitcnt, 0);
    AppendVintrpMov(psWords, 0, 0, 0);
    AppendVintrpMov(psWords, 1, 0, 1);
    AppendVintrpMov(psWords, 2, 0, 2);
    AppendVop2(psWords, kVop2MulF32, 0, SgprSrc(8),  0);
    AppendVop2(psWords, kVop2MulF32, 1, SgprSrc(9),  1);
    AppendVop2(psWords, kVop2MulF32, 2, SgprSrc(10), 2);
    AppendVop1(psWords, kVop1MovB32, 3, SgprSrc(11));
    AppendExpMrt0(psWords, 0, 1, 2, 3, 0xFu, true, true);
    AppendSopp(psWords, kSoppEndpgm);

    DecodedProgram vsDec{}, psDec{};
    std::string err;
    SC_EXPECT(DecodeProgram(vsWords.data(), vsWords.size() * sizeof(uint32_t), vsDec, err));
    SC_EXPECT(DecodeProgram(psWords.data(), psWords.size() * sizeof(uint32_t), psDec, err));
    SC_EXPECT(Disassemble(vsDec).find("s_load_dwordx4") != std::string::npos);
    SC_EXPECT(Disassemble(vsDec).find("exp pos0") != std::string::npos);
    SC_EXPECT(Disassemble(psDec).find("v_interp_mov_f32") != std::string::npos);

    auto vsScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        vsWords.data(), vsWords.size() * sizeof(uint32_t), 6, 6);
    auto psScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        psWords.data(), psWords.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue)));

    D3D12_ROOT_PARAMETER cbvParam{};
    cbvParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbvParam.Descriptor = { 0, 0 };
    cbvParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &cbvParam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_INPUT_ELEMENT_DESC inputElems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsScxs.data(), vsScxs.size() };
    psoDesc.PS = { psScxs.data(), psScxs.size() };
    psoDesc.InputLayout = { inputElems, static_cast<UINT>(_countof(inputElems)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));

    struct TestVert { float pos[4]; float col[4]; };
    const TestVert verts[3] = {
        { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } },
        { { -1.0f, -0.5f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } },
        { { -0.5f, -1.0f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } }
    };

    alignas(256) float cbData[64]{};
    cbData[0] = 8.0f; cbData[1] = 8.0f; cbData[2] = 7.0f;  cbData[3] = 7.0f; // gCb0[0]
    cbData[4] = 0.5f; cbData[5] = 1.0f; cbData[6] = 0.25f; cbData[7] = 1.0f; // gCb0[1]

    D3D12_HEAP_PROPERTIES upHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    auto makeBufDesc = [](UINT64 bytes) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };

    ComPtr<ID3D12Resource> vbBuf, cbBuf, rtTex, rbBuf;
    D3D12_RESOURCE_DESC vbDesc = makeBufDesc(sizeof(verts));
    D3D12_RESOURCE_DESC cbDesc = makeBufDesc(sizeof(cbData));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbBuf))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbBuf))));

    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(vbBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, verts, sizeof(verts));
    vbBuf->Unmap(0, nullptr);
    SC_EXPECT(SUCCEEDED(cbBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, cbData, sizeof(cbData));
    cbBuf->Unmap(0, nullptr);

    constexpr UINT kRtSize = 16;
    constexpr UINT kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT kRbBytes = kRowPitch * kRtSize;

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc = makeBufDesc(kRbBytes);
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&rtvHeap))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kRtSize), static_cast<float>(kRtSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kRtSize), static_cast<LONG>(kRtSize) };
    D3D12_VERTEX_BUFFER_VIEW vbv{ vbBuf->GetGPUVirtualAddress(), sizeof(verts), sizeof(TestVert) };

    cmdList->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, cbBuf->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmdList->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rtTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kRtSize, kRtSize, 1, kRowPitch };
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(cmdQueue->Signal(fence.Get(), 1)));

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    D3D12_RANGE rr{ 0, kRbBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &rr, &mapped)));
    const auto* px = static_cast<const uint8_t*>(mapped) + (8u * kRowPitch) + (8u * 4u);
    const bool ok = (std::abs(static_cast<int>(px[0]) - 102) <= 1) &&
                    (std::abs(static_cast<int>(px[1]) - 128) <= 1) &&
                    (std::abs(static_cast<int>(px[2]) - 51)  <= 1) &&
                    (px[3] == 255);
    rbBuf->Unmap(0, &noRead);
    SC_EXPECT(ok);
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
    RunTest("KernelXMemoryAndTopology",   TestKernelXMemoryAndTopology);
    RunTest("D3D12PlacedResourcesAndHeap", TestD3D12PlacedResourcesAndHeap);
    RunTest("DxilSm6BitcodeInspector",    TestDxilSm6BitcodeInspector);
    RunTest("ScxsTranslatorAndPsoCache",  TestScxsTranslatorAndPsoCache);
    RunTest("DStorageLzDecompression",    TestDStorageLzDecompression);
    RunTest("XAudio2ImaAdpcmDecodeBridge", TestXAudio2ImaAdpcmDecodeBridge);
    RunTest("D3D12ComputePsoAndUavExecution", TestD3D12ComputePsoAndUavExecution);
    RunTest("Rdna2IsaDecoderAndGpuExecution", TestRdna2IsaDecoderAndGpuExecution);
    RunTest("D3D12AsyncComputeCrossQueueSync", TestD3D12AsyncComputeCrossQueueSync);
    RunTest("Rdna2ComputeIsaExecution", TestRdna2ComputeIsaExecution);
    RunTest("Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv);

    XGameRuntimeUninitialize();

    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}