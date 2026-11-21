#include "common/codecs.hpp"
#include "common/vfs.hpp"
#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"
#include "shims/d3d12_xs/shader_inspector.hpp"
#include "shims/d3d12_xs/shader_translator.hpp"
#include "shims/gameinput/gameinput.hpp"
#include "shims/xg_xs/xg_xs.hpp"
#include "shims/dstorage_xs/dstorage_xs.hpp"
#include "shims/acphal/acphal.hpp"
#include "shims/libhttpclient/libhttpclient.hpp"
#include "shims/xcurl/xcurl.hpp"
#include "common/xvd_container.hpp"
#include "loader/pe_inspector.hpp"
#include "loader/game_config.hpp"
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

// 21. xg_xs + d3d12_xs: RDNA2 64KB_S swizzle & BC1/BC3/BC7 NPOT multi-mip round-trips + RTX 5070 BC1 sample
bool TestXgXs64KbsAndBcnBlockSwizzle() {
    auto verifyBcnSwizzle = [](DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t bpe) -> bool {
        XG_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = mips;
        d.ArraySize = 1;
        d.Format = fmt;
        d.TileMode = XG_TILE_MODE_64KB_S;

        ComPtr<IXGTextureAddressComputer> comp;
        XG_RESOURCE_LAYOUT layout{};
        SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&d, &comp)));
        SC_EXPECT(SUCCEEDED(comp->GetResourceLayout(&layout)));
        SC_EXPECT(layout.SizeBytes >= 65536ULL && (layout.SizeBytes % 65536ULL) == 0ULL);

        const uint32_t physAttrs = MakeXAllocAttributes(
            XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
        void* tiledMem = XMemAlloc(static_cast<size_t>(layout.SizeBytes), physAttrs);
        SC_EXPECT(tiledMem != nullptr);
        SC_EXPECT((reinterpret_cast<uintptr_t>(tiledMem) % 65536ULL) == 0ULL);

        std::vector<std::vector<uint8_t>> expectedMips(mips);
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t bw = (std::max<uint32_t>(1u, w >> m) + 3u) / 4u;
            const uint32_t bh = (std::max<uint32_t>(1u, h >> m) + 3u) / 4u;
            const uint32_t rowBytes = bw * bpe;
            expectedMips[m].resize(static_cast<size_t>(rowBytes) * bh);
            for (size_t i = 0; i < expectedMips[m].size(); ++i) {
                expectedMips[m][i] = static_cast<uint8_t>(((i * 37u) ^ (m * 97u) ^ (bpe * 13u)) & 0xFFu);
            }
            SC_EXPECT(SUCCEEDED(comp->CopyIntoSubresource(tiledMem, m, expectedMips[m].data(), rowBytes, 0)));
        }

        // Verify real swizzling altered layout on mip 0
        SC_EXPECT(std::memcmp(tiledMem, expectedMips[0].data(), std::min<size_t>(256, expectedMips[0].size())) != 0);

        bool allExact = true;
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t bw = (std::max<uint32_t>(1u, w >> m) + 3u) / 4u;
            const uint32_t rowBytes = bw * bpe;
            std::vector<uint8_t> roundTrip(expectedMips[m].size(), 0xCCu);
            if (FAILED(comp->CopyFromSubresource(roundTrip.data(), m, tiledMem, rowBytes, 0)) ||
                std::memcmp(roundTrip.data(), expectedMips[m].data(), expectedMips[m].size()) != 0) {
                allExact = false;
                break;
            }
        }
        XMemFree(tiledMem, physAttrs);
        SC_EXPECT(allExact);
        return true;
    };

    // NPOT multi-mip BC1 (8B), BC3 (16B), and multi-macro-tile BC7 (16B, 300x190 = 75x48 blocks > 64x64)
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC1_UNORM, 130,  90, 4,  8));
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC3_UNORM,  64,  64, 3, 16));
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC7_UNORM, 300, 190, 5, 16));

    // End-to-end GPU test: BC1 16x16 swizzled in 64KB_S -> detiled to LINEAR -> uploaded & sampled on RTX 5070
    constexpr UINT kSize = 16;
    XG_TEXTURE2D_DESC bc1Tiled{};
    bc1Tiled.Width = kSize;
    bc1Tiled.Height = kSize;
    bc1Tiled.MipLevels = 1;
    bc1Tiled.ArraySize = 1;
    bc1Tiled.Format = DXGI_FORMAT_BC1_UNORM;
    bc1Tiled.TileMode = XG_TILE_MODE_64KB_S;

    XG_TEXTURE2D_DESC bc1Linear = bc1Tiled;
    bc1Linear.TileMode = XG_TILE_MODE_LINEAR;

    ComPtr<IXGTextureAddressComputer> bc1Comp;
    XG_RESOURCE_LAYOUT tiledLayout{}, linearLayout{};
    SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&bc1Tiled, &bc1Comp)));
    SC_EXPECT(SUCCEEDED(bc1Comp->GetResourceLayout(&tiledLayout)));
    SC_EXPECT(SUCCEEDED(XGComputeTexture2DLayout(&bc1Linear, &linearLayout)));

    // Solid BC1 block: color0 = 0xFC00 (R5=31->255, G6=32->130, B5=0->0), color1 = 0x0000, all indices = 0
    std::vector<uint8_t> bc1Raw(4u * 4u * 8u, 0u);
    for (size_t blk = 0; blk < 16u; ++blk) {
        bc1Raw[blk * 8u + 0] = 0x00u;
        bc1Raw[blk * 8u + 1] = 0xFCu;
    }

    const uint32_t physAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    void* tiledBuf = XMemAlloc(static_cast<size_t>(tiledLayout.SizeBytes), physAttrs);
    SC_EXPECT(tiledBuf != nullptr);
    SC_EXPECT(SUCCEEDED(bc1Comp->CopyIntoSubresource(tiledBuf, 0, bc1Raw.data(), 4u * 8u, 0)));

    const UINT linearPitch = static_cast<UINT>(linearLayout.Plane[0].MipLayout[0].PitchBytes);
    std::vector<uint8_t> detiledUpload(static_cast<size_t>(linearLayout.SizeBytes), 0u);
    SC_EXPECT(SUCCEEDED(bc1Comp->CopyFromSubresource(detiledUpload.data(), 0, tiledBuf, linearPitch, 0)));
    XMemFree(tiledBuf, physAttrs);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qd, &cmdQueue)));

    const char* hlsl = R"HLSL(
Texture2D<float4> gTex : register(t0);
SamplerState gSamp : register(s0);
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    return float4((id == 2) ? 3.0f : -1.0f, (id == 1) ? 3.0f : -1.0f, 0.0f, 1.0f);
}
float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
    return gTex.Sample(gSamp, float2(0.5f, 0.5f));
}
)HLSL";

    ComPtr<ID3DBlob> vsBlob, psBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "bc1_sample.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "bc1_sample.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    D3D12_DESCRIPTOR_RANGE srvRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 1, &srvRange };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

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

    D3D12_HEAP_PROPERTIES upHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC bc1Desc{};
    bc1Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bc1Desc.Width = kSize;
    bc1Desc.Height = kSize;
    bc1Desc.DepthOrArraySize = 1;
    bc1Desc.MipLevels = 1;
    bc1Desc.Format = DXGI_FORMAT_BC1_UNORM;
    bc1Desc.SampleDesc.Count = 1;

    D3D12_RESOURCE_DESC rtDesc = bc1Desc;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    auto mkBuf = [](UINT64 sz) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = sz;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };

    constexpr UINT kRtPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT kRbBytes = kRtPitch * kSize;
    D3D12_RESOURCE_DESC upDesc = mkBuf(detiledUpload.size());
    D3D12_RESOURCE_DESC rbDesc = mkBuf(kRbBytes);

    ComPtr<ID3D12Resource> bc1Tex, upBuf, rtTex, rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bc1Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bc1Tex))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap,  D3D12_HEAP_FLAG_NONE, &upDesc,  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtTex))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap,  D3D12_HEAP_FLAG_NONE, &rbDesc,  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(upBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, detiledUpload.data(), detiledUpload.size());
    upBuf->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC srvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&srvHd, IID_PPV_ARGS(&srvHeap))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&rtvHeap))));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_BC1_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    deviceX->CreateShaderResourceView(bc1Tex.Get(), &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    D3D12_TEXTURE_COPY_LOCATION dstTex{};
    dstTex.pResource = bc1Tex.Get();
    dstTex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstTex.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcBuf{};
    srcBuf.pResource = upBuf.Get();
    srcBuf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcBuf.PlacedFootprint.Footprint = { DXGI_FORMAT_BC1_UNORM, kSize, kSize, 1, linearPitch };
    cmdList->CopyTextureRegion(&dstTex, 0, 0, 0, &srcBuf, nullptr);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition = { bc1Tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    cmdList->ResourceBarrier(1, &b1);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize) };
    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmdList->ResourceBarrier(1, &b2);

    D3D12_TEXTURE_COPY_LOCATION rtSrc{};
    rtSrc.pResource = rtTex.Get();
    rtSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rtSrc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION rbDst{};
    rbDst.pResource = rbBuf.Get();
    rbDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rbDst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kRtPitch };
    cmdList->CopyTextureRegion(&rbDst, 0, 0, 0, &rtSrc, nullptr);
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
    const auto* px = static_cast<const uint8_t*>(mapped) + (8u * kRtPitch) + (8u * 4u);
    const bool ok = (px[0] == 255) && (std::abs(static_cast<int>(px[1]) - 130) <= 2) && (px[2] == 0) && (px[3] == 255);
    rbBuf->Unmap(0, &noRead);
    SC_EXPECT(ok);
    return true;
}

bool TestDStorageGpuBufferStreaming() {
    ComPtr<ID3D12Device> device;
    HMODULE hD3d12 = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(hD3d12 != nullptr);
    auto pfnCreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(hD3d12, "D3D12CreateDevice"));
    SC_EXPECT(pfnCreateDevice != nullptr);
    SC_EXPECT(SUCCEEDED(pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));

    constexpr uint32_t kPayloadSize = 65536u;
    constexpr uint64_t kDstOffset = 256u;
    constexpr uint64_t kTotalBufSize = kPayloadSize + kDstOffset;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kTotalBufSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> gpuBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuBuf))));

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc = bufDesc;
    rbDesc.Width = kPayloadSize;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    std::vector<uint8_t> original(kPayloadSize);
    for (uint32_t i = 0; i < kPayloadSize; ++i) {
        original[i] = static_cast<uint8_t>((i / 16u) ^ (i & 0x0Fu) ^ 0xA5u);
    }
    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(original.data(), original.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_stream_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\gpu_stream_probe.sclz", IID_IDStorageFileX, reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qDesc.Name = "gpu_stream_test_queue";
    qDesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "gpu_status", IID_IDStorageStatusArrayX, reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.Buffer.Resource = gpuBuf.Get();
    req.Destination.Buffer.Offset = kDstOffset;
    req.Destination.Buffer.Size = kPayloadSize;
    req.UncompressedSize = kPayloadSize;
    req.Name = "gpu_lz_stream";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    // Copy from DEFAULT buffer back to READBACK buffer on a DIRECT queue and verify bit-for-bit
    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> directQueue;
    SC_EXPECT(SUCCEEDED(device->CreateCommandQueue(&dq, IID_PPV_ARGS(&directQueue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    cmdList->CopyBufferRegion(rbBuf.Get(), 0, gpuBuf.Get(), kDstOffset, kPayloadSize);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kPayloadSize };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const bool match = (std::memcmp(mapped, original.data(), kPayloadSize) == 0);
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(match);

    // Negative test: null GPU resource must record E_INVALIDARG
    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.Buffer.Resource = nullptr;
    badReq.Name = "null_gpu_resource";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
    return true;
}

static size_t TestXCurlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool TestRdna2TexturedDrawE2E() {
    using namespace sc::d3d12::rdna2;
    using Microsoft::WRL::ComPtr;

    // 1. Build, decode, lift, and compile RDNA2 IMAGE_SAMPLE pixel shader
    std::vector<uint32_t> isa;
    AppendVintrpMov(isa, 0, 0, 0);
    AppendVintrpMov(isa, 1, 0, 1);
    AppendMimgImageSample(isa, 2, 0, 0, 0, 0xFu, 1u);
    AppendExp(isa, kExpTargetMrt0, 2, 3, 4, 5, 0xFu, true, false);
    AppendSopp(isa, kSoppEndpgm);

    DecodedProgram prog{};
    std::string err;
    SC_EXPECT(DecodeProgram(isa.data(), isa.size() * sizeof(uint32_t), prog, err));

    std::string psHlsl;
    SC_EXPECT(LiftPixelShaderToHlsl(prog, psHlsl, err));

    ComPtr<ID3DBlob> psBlob, vsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(
        psHlsl.data(), psHlsl.size(), "rdna2_e2e_ps.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob)));

    const char* vsHlsl = R"(
struct VSOut {
    float4 svPos  : SV_POSITION;
    float4 param0 : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.svPos  = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.param0 = float4(uv, 0.0f, 1.0f);
    return o;
}
)";
    SC_EXPECT(SUCCEEDED(D3DCompile(
        vsHlsl, std::strlen(vsHlsl), "rdna2_e2e_vs.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob)));

    // 2. Create D3D12 device, queue, allocator, list, and fence
    HMODULE d3d12Mod = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(d3d12Mod != nullptr);
    auto pD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Mod, "D3D12CreateDevice"));
    SC_EXPECT(pD3D12CreateDevice != nullptr);

    ComPtr<ID3D12Device> dev;
    SC_EXPECT(SUCCEEDED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmd;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvent != nullptr);

    // 3. Root signature: 1 SRV descriptor table (t0) + 1 static sampler (s0)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC staticSamp{};
    staticSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamp.MaxLOD = D3D12_FLOAT32_MAX;
    staticSamp.ShaderRegister = 0;
    staticSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSamp;

    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    // 4. Graphics PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))));

    // 5. Create 2x2 SRV texture + upload buffer, 64x64 RTV texture + readback buffer
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 2;
    texDesc.Height = 2;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> srcTex;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&srcTex))));

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = 512; // 2 rows * 256-byte aligned pitch
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf))));

    uint8_t* upPtr = nullptr;
    SC_EXPECT(SUCCEEDED(upBuf->Map(0, nullptr, reinterpret_cast<void**>(&upPtr))));
    std::memset(upPtr, 0, 512);
    const uint8_t kExpected[4] = { 40, 180, 240, 255 };
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            std::memcpy(upPtr + y * 256 + x * 4, kExpected, 4);
        }
    }
    upBuf->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC rtDesc = texDesc;
    rtDesc.Width = 64;
    rtDesc.Height = 64;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc = upDesc;
    rbDesc.Width = 64 * 256; // 64 rows * 256 bytes/row (64 * 4 = 256)
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    // 6. Descriptor heaps (RTV + shader-visible SRV)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));
    dev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap))));
    dev->CreateShaderResourceView(srcTex.Get(), nullptr, srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 7. Record upload -> transition -> draw -> readback
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upBuf.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = srcTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition = { srcTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    cmd->ResourceBarrier(1, &toSrv);

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, 64, 64 };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmd->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION rbDst{};
    rbDst.pResource = rbBuf.Get();
    rbDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rbDst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };

    D3D12_TEXTURE_COPY_LOCATION rtSrc{};
    rtSrc.pResource = rtTex.Get();
    rtSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&rbDst, 0, 0, 0, &rtSrc, nullptr);

    SC_EXPECT(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, fenceEvent)));
    WaitForSingleObject(fenceEvent, 5000);
    CloseHandle(fenceEvent);

    // 8. Verify center pixel (32, 32) matches sampled RGBA texel
    uint8_t* rbPtr = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&rbPtr))));
    const uint8_t* px = rbPtr + 32 * 256 + 32 * 4;
    const bool match = (px[0] == kExpected[0] && px[1] == kExpected[1] &&
                        px[2] == kExpected[2] && px[3] == kExpected[3]);
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(match);

    return true;
}

bool TestGameConfigBootParser() {
    // 1. Synthetic MicrosoftGame.Config round-trip
    const auto tmpDir = std::filesystem::temp_directory_path() / "scarlett_gameconfig_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const auto cfgPath = tmpDir / "MicrosoftGame.Config";
    {
        std::ofstream ofs(cfgPath);
        ofs << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            << "<Game configVersion=\"1\">\n"
            << "  <Identity Name=\"ScarlettTestTitle\" Publisher=\"CN=Test\" Version=\"1.2.3.0\" />\n"
            << "  <TitleId>1234ABCD</TitleId>\n"
            << "  <ExecutableList>\n"
            << "    <Executable Name=\"game_main.exe\" Id=\"Game\" />\n"
            << "  </ExecutableList>\n"
            << "  <ShellVisuals DefaultDisplayName=\"Scarlett Test Title\" />\n"
            << "</Game>\n";
    }

    const auto parsed = sc::loader::ParseGameConfig(tmpDir);
    SC_EXPECT(parsed.valid);
    SC_EXPECT(parsed.identityName == "ScarlettTestTitle");
    SC_EXPECT(parsed.version == "1.2.3.0");
    SC_EXPECT(parsed.titleId == "1234ABCD");
    SC_EXPECT(parsed.displayName == "Scarlett Test Title");
    SC_EXPECT(parsed.executableName == "game_main.exe");
    SC_EXPECT(parsed.executableId == "Game");
    SC_EXPECT(parsed.executablePath == tmpDir / "game_main.exe");

    // 2. Reject missing directory cleanly
    const auto bad = sc::loader::ParseGameConfig(tmpDir / "does_not_exist");
    SC_EXPECT(!bad.valid && !bad.error.empty());

    std::filesystem::remove_all(tmpDir, ec);

    // 3. Validate all 4 ScarlettX synthetic game manifests when sibling repo is present
    const std::filesystem::path sxSynthRoot = "E:\\gta\\test\\ScarlettX\\tests\\synthetic_games";
    if (std::filesystem::exists(sxSynthRoot)) {
        const char* kTitles[] = { "hello_scarlett", "iat_probe", "rt_showcase", "scarlett_arena" };
        for (const char* title : kTitles) {
            const auto pkg = sc::loader::ParseGameConfig(sxSynthRoot / title);
            SC_EXPECT(pkg.valid);
            SC_EXPECT(!pkg.executableName.empty());
        }
    }

    return true;
}

bool TestScarlettXConformanceSuite() {
    // 1. Raw COM vtable ABI conformance against gameinput.dll (matches ScarlettX test_gameinput.cpp)
    HMODULE giMod = LoadLibraryW(L"gameinput.dll");
    if (!giMod) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path shimDll =
            std::filesystem::path(exePath).parent_path().parent_path().parent_path() / "shims" / "gameinput.dll";
        giMod = LoadLibraryW(shimDll.c_str());
    }
    SC_EXPECT(giMod != nullptr);

    using PfnGameInputCreate = HRESULT(WINAPI*)(void**);
    auto pGameInputCreate = reinterpret_cast<PfnGameInputCreate>(GetProcAddress(giMod, "GameInputCreate"));
    SC_EXPECT(pGameInputCreate != nullptr);

    void* gi = nullptr;
    SC_EXPECT(SUCCEEDED(pGameInputCreate(&gi)) && gi != nullptr);

    void** giVtbl = *reinterpret_cast<void***>(gi);
    using PfnRelease = ULONG(STDMETHODCALLTYPE*)(void*);
    using PfnGetCurrentTimestamp = uint64_t(STDMETHODCALLTYPE*)(void*);
    using PfnGetCurrentReading = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*, void**);

    // Slot 3: GetCurrentTimestamp, Slot 4: GetCurrentReading (retail GameInput.dll ABI)
    const uint64_t ts = reinterpret_cast<PfnGetCurrentTimestamp>(giVtbl[3])(gi);
    SC_EXPECT(ts > 0);

    constexpr uint32_t kKindGamepad = 0x00020000u;
    void* reading = nullptr;
    HRESULT hrRead = reinterpret_cast<PfnGetCurrentReading>(giVtbl[4])(gi, kKindGamepad, nullptr, &reading);
    if (SUCCEEDED(hrRead) && reading != nullptr) {
        void** rdVtbl = *reinterpret_cast<void***>(reading);
        using PfnGetInputKind = uint32_t(STDMETHODCALLTYPE*)(void*);
        using PfnGetTimestamp = uint64_t(STDMETHODCALLTYPE*)(void*);
        SC_EXPECT(reinterpret_cast<PfnGetInputKind>(rdVtbl[3])(reading) == kKindGamepad);
        SC_EXPECT(reinterpret_cast<PfnGetTimestamp>(rdVtbl[4])(reading) > 0);
        reinterpret_cast<PfnRelease>(rdVtbl[2])(reading);
    } else {
        // GAMEINPUT_E_READING_NOT_FOUND is valid when no physical gamepad is connected
        SC_EXPECT(hrRead == static_cast<HRESULT>(0x838A0002L));
    }
    reinterpret_cast<PfnRelease>(giVtbl[2])(gi);
    FreeLibrary(giMod);

    // 2. xgameruntime.dll ABI conformance (matches ScarlettX test_xgameruntime_abi.cpp)
    SC_EXPECT(SUCCEEDED(XGameRuntimeInitialize()));
    SC_EXPECT(XSystemGetDeviceType() != XSystemDeviceType::Unknown);

    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(
        XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue)) && queue != nullptr);
    XTaskQueueDispatch(queue, XTaskQueuePort::Work, 0);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    // 3. ScarlettX synthetic game manifest audit (if sibling repo is present)
    const std::filesystem::path sxSynthRoot = "E:\\gta\\test\\ScarlettX\\tests\\synthetic_games";
    if (std::filesystem::exists(sxSynthRoot)) {
        const char* kTitles[] = { "hello_scarlett", "iat_probe", "rt_showcase", "scarlett_arena" };
        for (const char* title : kTitles) {
            const auto cfgPath = sxSynthRoot / title / "MicrosoftGame.Config";
            SC_EXPECT(std::filesystem::exists(cfgPath));
            std::ifstream ifs(cfgPath);
            std::string xml((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SC_EXPECT(xml.find("<Game") != std::string::npos);
            SC_EXPECT(xml.find("Executable") != std::string::npos);
        }
    }

    return true;
}

bool TestPresentXDescriptorAudit() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = D3D12_SDK_VERSION;
    ID3D12DeviceX* dev = nullptr;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(&dev))) && dev != nullptr);

    // 1. Audit descriptor handle increment sizes for all 4 heap types
    const UINT incCbv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT incSmp = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const UINT incRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT incDsv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    SC_EXPECT(incCbv > 0 && incSmp > 0 && incRtv > 0 && incDsv > 0);

    // 2. Create command queue, allocator, command list, and RTV heap
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueueX* queue = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueueX(&qDesc, &queue)) && queue != nullptr);

    // Reject invalid PresentX arguments cleanly
    SC_EXPECT(FAILED(queue->PresentX(0, nullptr, nullptr)));

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))) && rtvHeap != nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv1{ rtv0.ptr + incRtv };
    SC_EXPECT(rtv0.ptr != 0 && rtv1.ptr == rtv0.ptr + incRtv);

    // 3. Create 64x64 R8G8B8A8_UNORM render target + readback buffer
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 64;
    rtDesc.Height = 64;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* rt = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt))) && rt != nullptr);

    dev->CreateRenderTargetView(rt, nullptr, rtv0);

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 64; // 256-byte aligned row pitch * 64 rows
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) && readback != nullptr);

    // 4. Bind RTV via descriptor handle, clear to non-white RGBA, copy to readback, transition to PRESENT
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))));

    const float clearColor[4] = { 0.25f, 0.50f, 0.75f, 1.0f };
    cmd->OMSetRenderTargets(1, &rtv0, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv0, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = rt;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &b0);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = readback;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = 64;
    dstLoc.PlacedFootprint.Footprint.Height = 64;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = 256;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rt;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = rt;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &b1);

    SC_EXPECT(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);

    // 5. Submit PresentX with the rendered target
    ID3D12Resource* planes[] = { rt };
    D3D12XBOX_PRESENT_PLANE_PARAMETERS planeParams{};
    planeParams.ResourceCount = 1;
    planeParams.ppResources = planes;
    SC_EXPECT(SUCCEEDED(queue->PresentX(1, &planeParams, nullptr)));

    // Fence wait and verify non-white pixel values
    ID3D12Fence* fence = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    SC_EXPECT(SUCCEEDED(queue->Signal(fence, 1)));
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, 5000);
    }
    CloseHandle(evt);

    uint8_t* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped != nullptr);
    // Expected: R~64, G~128, B~191, A=255
    SC_EXPECT(mapped[0] >= 62 && mapped[0] <= 66);
    SC_EXPECT(mapped[1] >= 126 && mapped[1] <= 130);
    SC_EXPECT(mapped[2] >= 189 && mapped[2] <= 193);
    SC_EXPECT(mapped[3] == 255);
    readback->Unmap(0, nullptr);

    fence->Release();
    cmd->Release();
    alloc->Release();
    readback->Release();
    rt->Release();
    rtvHeap->Release();
    queue->Release();
    dev->Release();
    return true;
}

bool TestRdna2ImageSampleShader() {
    using namespace sc::d3d12::rdna2;

    // Build RDNA2 PS:
    //   v0 = param0.x (U), v1 = param0.y (V)
    //   v2..v5 = image_sample(v0..v1, t0, s0, dmask=0xF)
    //   exp mrt0, v2, v3, v4, v5 (done)
    //   s_endpgm
    std::vector<uint32_t> isa;
    AppendVintrpMov(isa, 0, 0, 0);
    AppendVintrpMov(isa, 1, 0, 1);
    AppendMimgImageSample(isa, 2, 0, 0, 0, 0xFu, 1u);
    AppendExp(isa, kExpTargetMrt0, 2, 3, 4, 5, 0xFu, true, false);
    AppendSopp(isa, kSoppEndpgm);

    DecodedProgram prog{};
    std::string err;
    SC_EXPECT(DecodeProgram(isa.data(), isa.size() * sizeof(uint32_t), prog, err));
    SC_EXPECT(prog.insts.size() == 5u);
    SC_EXPECT(prog.insts[2].format == InstFormat::Mimg);
    SC_EXPECT(prog.insts[2].opcode == kMimgImageSample);
    SC_EXPECT(prog.insts[2].mimgDmask == 0xFu);
    SC_EXPECT(prog.insts[2].mimgVdata == 2u);
    SC_EXPECT(prog.insts[2].mimgVaddr == 0u);

    std::string hlsl;
    SC_EXPECT(LiftPixelShaderToHlsl(prog, hlsl, err));
    SC_EXPECT(hlsl.find("Texture2D<float4> gTex0 : register(t0);") != std::string::npos);
    SC_EXPECT(hlsl.find("SamplerState gSamp0 : register(s0);") != std::string::npos);
    SC_EXPECT(hlsl.find("gTex0.Sample(gSamp0, float2(v0, v1))") != std::string::npos);

    // Compile lifted HLSL to real DXBC ps_5_0 bytecode
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(), "rdna2_image_sample_ps.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &bytecode, &errors);
    if (errors) errors->Release();
    SC_EXPECT(SUCCEEDED(hr) && bytecode != nullptr && bytecode->GetBufferSize() > 0);
    bytecode->Release();

    return true;
}

bool TestLoaderInspectAndXvdMount() {
    // 1. PE64 import auditor on scarlett_tests.exe itself
    wchar_t selfBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfBuf, MAX_PATH);
    std::filesystem::path selfPath(selfBuf);

    wchar_t shimDirBuf[MAX_PATH]{};
    DWORD shimLen = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDirBuf, MAX_PATH);
    SC_EXPECT(shimLen > 0);
    std::filesystem::path shimDir(shimDirBuf);

    const auto audit = sc::loader::InspectPeImports(selfPath, shimDir);
    SC_EXPECT(audit.validPe64);
    SC_EXPECT(audit.totalImportedSymbols > 0);
    SC_EXPECT(audit.missingImports.empty());

    auto hasShimmed = [&](const char* target) {
        std::string t = sc::loader::detail::ToLowerAscii(target);
        for (const auto& s : audit.shimmedImports) {
            if (sc::loader::detail::ToLowerAscii(s) == t) return true;
        }
        return false;
    };
    SC_EXPECT(hasShimmed("d3d12_xs.dll"));
    SC_EXPECT(hasShimmed("xg_xs.dll"));
    SC_EXPECT(hasShimmed("acphal.dll"));
    SC_EXPECT(hasShimmed("libhttpclient.gdk.dll"));
    SC_EXPECT(hasShimmed("xcurl.dll"));

    // 2. Unencrypted XVD container mount + ReadUserData round-trip
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
    const std::filesystem::path validXvd = tmpDir / L"scarlett_test_valid.xvd";
    const std::filesystem::path encXvd   = tmpDir / L"scarlett_test_enc.xvd";

    const std::string payloadStr = "SCARLETT_XVD_VFS_PAYLOAD_OK";
    std::vector<uint8_t> payload(payloadStr.begin(), payloadStr.end());
    auto validImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, 0u);
    {
        std::ofstream out(validXvd, std::ios::binary);
        out.write(reinterpret_cast<const char*>(validImg.data()), static_cast<std::streamsize>(validImg.size()));
    }

    sc::vfs::XvdContainer xvd;
    std::string err;
    SC_EXPECT(xvd.Open(validXvd, err));
    SC_EXPECT(xvd.Header().magic == "msft-xvd");
    SC_EXPECT(xvd.Header().userDataLength == payload.size());
    std::vector<uint8_t> readBack;
    SC_EXPECT(xvd.ReadUserData(0, payload.size(), readBack, err));
    SC_EXPECT(readBack == payload);

    // 3. Encrypted XVD must be cleanly rejected
    auto encImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagEncrypted);
    {
        std::ofstream out(encXvd, std::ios::binary);
        out.write(reinterpret_cast<const char*>(encImg.data()), static_cast<std::streamsize>(encImg.size()));
    }
    SC_EXPECT(!xvd.Open(encXvd, err));

    std::error_code ec;
    std::filesystem::remove(validXvd, ec);
    std::filesystem::remove(encXvd, ec);
    return true;
}

bool TestRetailBringUpShims() {
    // 1. GameInput.dll ordinal #1 export check
    HMODULE hGi = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(hGi != nullptr);
    FARPROC ord1 = GetProcAddress(hGi, MAKEINTRESOURCEA(1));
    FARPROC byName = GetProcAddress(hGi, "GameInputCreate");
    SC_EXPECT(ord1 != nullptr && ord1 == byName);
    FreeLibrary(hGi);

    // 2. AcpHal.dll vtable lifecycle + ApuAlloc alignment
    void* rawAcp = nullptr;
    SC_EXPECT(SUCCEEDED(AcpHalCreate(&rawAcp)) && rawAcp != nullptr);
    auto* acp = static_cast<IAcpHal*>(rawAcp);
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->Connect(acp, 64, 64)));
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->SubmitCommand(acp, 1, 1, 0, nullptr, 0)));
    uint8_t msg[16]{};
    SC_EXPECT(acp->lpVtbl->PopMessage(acp, msg) == 0);
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->Disconnect(acp)));
    SC_EXPECT(acp->lpVtbl->PopMessage(acp, msg) == 1);
    SC_EXPECT(msg[0] == 0x80u);

    ACP_SHAPE_CONTEXT_COUNTS counts{ 4, 2, 2, 2, 4, 2 };
    SC_EXPECT(SUCCEEDED(AcpHalAllocateShapeContexts(&counts)));
    SC_EXPECT(SUCCEEDED(AcpHalReleaseShapeContexts()));

    void* apuPtr = nullptr;
    uint64_t apuPhys = 0;
    SC_EXPECT(SUCCEEDED(ApuAlloc(&apuPtr, &apuPhys, 4096, 256)));
    SC_EXPECT(apuPtr != nullptr && (reinterpret_cast<uintptr_t>(apuPtr) % 256u) == 0u);
    SC_EXPECT(apuPhys == reinterpret_cast<uint64_t>(apuPtr));
    SC_EXPECT(SUCCEEDED(ApuFree(apuPtr)));

    // 3. libHttpClient.GDK.dll offline mock round-trip
    SC_EXPECT(SUCCEEDED(HCInitialize(nullptr)));
    SC_EXPECT(HCIsInitialized() == 1u);
    SC_EXPECT(SUCCEEDED(HCHttpCallRegisterMock("GET", "https://xbox.local/status", 200u, "{\"ok\":true}")));

    HCCallHandle hc = nullptr;
    SC_EXPECT(SUCCEEDED(HCHttpCallCreate(&hc)) && hc != nullptr);
    SC_EXPECT(SUCCEEDED(HCHttpCallRequestSetUrl(hc, "GET", "https://xbox.local/status")));
    SC_EXPECT(SUCCEEDED(HCHttpCallPerformAsync(hc, nullptr)));

    uint32_t hcStatus = 0;
    const char* hcBody = nullptr;
    SC_EXPECT(SUCCEEDED(HCHttpCallResponseGetStatusCode(hc, &hcStatus)) && hcStatus == 200u);
    SC_EXPECT(SUCCEEDED(HCHttpCallResponseGetResponseString(hc, &hcBody)) && std::strcmp(hcBody, "{\"ok\":true}") == 0);
    SC_EXPECT(SUCCEEDED(HCHttpCallCloseHandle(hc)));
    HCCleanup();

    // 4. XCurl.dll offline mock round-trip
    SC_EXPECT(curl_global_init(0) == CURLE_OK);
    SC_EXPECT(xcurl_register_mock_response("https://cdn.local/patch", 200, "scarlett-ok") == CURLE_OK);
    CURL* curl = curl_easy_init();
    SC_EXPECT(curl != nullptr);
    std::string captured;
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_URL, "https://cdn.local/patch") == CURLE_OK);
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &TestXCurlWriteCb) == CURLE_OK);
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &captured) == CURLE_OK);
    SC_EXPECT(curl_easy_perform(curl) == CURLE_OK);
    long httpCode = 0;
    SC_EXPECT(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode) == CURLE_OK && httpCode == 200);
    SC_EXPECT(captured == "scarlett-ok");
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    return true;
}

bool TestDStorageDeviceXBridgeAndMipChain() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    constexpr uint32_t kMipCount = 3u;
    constexpr uint32_t kBaseSize = 64u;
    constexpr uint32_t kMipSizes[kMipCount] = { 64u, 32u, 16u };
    constexpr uint32_t kAlignedPitch = 256u;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kBaseSize;
    texDesc.Height = kBaseSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(kMipCount);
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> gpuTex;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuTex))));

    std::vector<uint8_t> tightChain;
    uint32_t mipTightOffsets[kMipCount]{};
    for (uint32_t m = 0; m < kMipCount; ++m) {
        mipTightOffsets[m] = static_cast<uint32_t>(tightChain.size());
        const uint32_t dim = kMipSizes[m];
        for (uint32_t y = 0; y < dim; ++y) {
            for (uint32_t x = 0; x < dim; ++x) {
                tightChain.push_back(static_cast<uint8_t>((m + 1u) * 60u + x));
                tightChain.push_back(static_cast<uint8_t>(y * 4u));
                tightChain.push_back(static_cast<uint8_t>((x ^ y) + m * 37u));
                tightChain.push_back(255u);
            }
        }
    }

    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(tightChain.data(), tightChain.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_mipchain_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(
        IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(
        L"T:\\gpu_mipchain_probe.sclz", IID_IDStorageFileX,
        reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_HIGH;
    qDesc.Name = "devicex_mipchain_queue";
    qDesc.Device = deviceX.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(
        &qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(
        2, "mipchain_status", IID_IDStorageStatusArrayX,
        reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.MultipleSubresources.Resource = gpuTex.Get();
    req.Destination.MultipleSubresources.FirstSubresource = 0;
    req.UncompressedSize = static_cast<uint32_t>(tightChain.size());
    req.Name = "gpu_lz_mipchain";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    constexpr uint64_t kRbOffsets[kMipCount] = { 0u, 16384u, 24576u };
    constexpr uint64_t kRbTotalBytes = 28672u;

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kRbTotalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> directQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&dq, &directQueue)));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    for (uint32_t m = 0; m < kMipCount; ++m) {
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = gpuTex.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = rbBuf.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = kRbOffsets[m];
        dstLoc.PlacedFootprint.Footprint = {
            DXGI_FORMAT_R8G8B8A8_UNORM, kMipSizes[m], kMipSizes[m], 1, kAlignedPitch
        };
        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(kRbTotalBytes) };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const auto* rbBytes = static_cast<const uint8_t*>(mapped);
    bool allMatch = true;
    for (uint32_t m = 0; m < kMipCount && allMatch; ++m) {
        const uint32_t dim = kMipSizes[m];
        const uint32_t tightRow = dim * 4u;
        for (uint32_t y = 0; y < dim; ++y) {
            const uint8_t* actualRow = rbBytes + kRbOffsets[m] + static_cast<uint64_t>(y) * kAlignedPitch;
            const uint8_t* expectRow = tightChain.data() + mipTightOffsets[m] + static_cast<uint64_t>(y) * tightRow;
            if (std::memcmp(actualRow, expectRow, tightRow) != 0) {
                allMatch = false;
                break;
            }
        }
    }
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(allMatch);

    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.MultipleSubresources.FirstSubresource = kMipCount;
    badReq.Name = "oob_first_subresource";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
    return true;
}

bool TestDStorageGpuTextureStreaming() {
    HMODULE hD3d12 = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(hD3d12 != nullptr);
    auto pfnCreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(hD3d12, "D3D12CreateDevice"));
    SC_EXPECT(pfnCreateDevice != nullptr);

    ComPtr<ID3D12Device> device;
    SC_EXPECT(SUCCEEDED(pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));

    constexpr uint32_t kWidth = 64u;
    constexpr uint32_t kHeight = 64u;
    constexpr uint32_t kRowPitch = kWidth * 4u; // 256 bytes (D3D12 aligned)
    constexpr uint32_t kTexBytes = kRowPitch * kHeight;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kWidth;
    texDesc.Height = kHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> gpuTex;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuTex))));

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kTexBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    std::vector<uint8_t> pixels(kTexBytes);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kWidth + x) * 4u;
            pixels[idx + 0] = static_cast<uint8_t>(x * 4u);
            pixels[idx + 1] = static_cast<uint8_t>(y * 4u);
            pixels[idx + 2] = static_cast<uint8_t>((x ^ y) * 3u);
            pixels[idx + 3] = 255u;
        }
    }

    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(pixels.data(), pixels.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_tex_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\gpu_tex_probe.sclz", IID_IDStorageFileX, reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qDesc.Name = "gpu_tex_stream_queue";
    qDesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "gpu_tex_status", IID_IDStorageStatusArrayX, reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.Texture.Resource = gpuTex.Get();
    req.Destination.Texture.SubresourceIndex = 0;
    req.Destination.Texture.Region = { 0, 0, 0, kWidth, kHeight, 1 };
    req.UncompressedSize = kTexBytes;
    req.Name = "gpu_lz_tex_stream";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    // Readback texture on DIRECT queue and verify bit-for-bit
    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> directQueue;
    SC_EXPECT(SUCCEEDED(device->CreateCommandQueue(&dq, IID_PPV_ARGS(&directQueue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = gpuTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, kRowPitch };

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kTexBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const bool match = (std::memcmp(mapped, pixels.data(), kTexBytes) == 0);
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(match);

    // Negative test: empty/inverted region must record E_INVALIDARG
    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.Texture.Region = { 16, 0, 0, 16, kHeight, 1 };
    badReq.Name = "invalid_tex_region";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
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
    RunTest("XgXs64KbsAndBcnBlockSwizzle",       TestXgXs64KbsAndBcnBlockSwizzle);
    RunTest("DStorageGpuBufferStreaming",        TestDStorageGpuBufferStreaming);
    RunTest("DStorageGpuTextureStreaming",       TestDStorageGpuTextureStreaming);
    RunTest("DStorageDeviceXBridgeAndMipChain",  TestDStorageDeviceXBridgeAndMipChain);
    RunTest("RetailBringUpShims",                TestRetailBringUpShims);
    RunTest("LoaderInspectAndXvdMount",          TestLoaderInspectAndXvdMount);
    RunTest("Rdna2ImageSampleShader",            TestRdna2ImageSampleShader);
    RunTest("PresentXDescriptorAudit",           TestPresentXDescriptorAudit);
    RunTest("ScarlettXConformanceSuite",         TestScarlettXConformanceSuite);
    RunTest("GameConfigBootParser",              TestGameConfigBootParser);
    RunTest("Rdna2TexturedDrawE2E",              TestRdna2TexturedDrawE2E);

    XGameRuntimeUninitialize();

    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}