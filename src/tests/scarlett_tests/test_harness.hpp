#pragma once

#include "common/codecs.hpp"
#include "common/vfs.hpp"
#include "common/xvd_container.hpp"
#include "loader/game_config.hpp"
#include "loader/pe_inspector.hpp"
#include "shims/acphal/acphal.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"
#include "shims/d3d12_xs/shader_inspector.hpp"
#include "shims/d3d12_xs/shader_translator.hpp"
#include "shims/dstorage_xs/dstorage_xs.hpp"
#include "shims/gameinput/gameinput.hpp"
#include "shims/kernelx/kernelx.hpp"
#include "shims/libhttpclient/libhttpclient.hpp"
#include "shims/pixevt/pixevt.hpp"
#include "shims/xcurl/xcurl.hpp"
#include "shims/xg_xs/xg_xs.hpp"
#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/xmem/xmem.hpp"

#include <d3dcompiler.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

#define SC_EXPECT(cond)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            return false;                                                            \
        }                                                                            \
    } while (false)

struct AsyncWaiter {
    std::atomic<bool> done{false};
};

inline void CALLBACK OnDone(XAsyncBlock* async) {
    static_cast<AsyncWaiter*>(async->context)->done.store(true, std::memory_order_release);
}

inline void Pump(XTaskQueueHandle queue, AsyncWaiter& waiter) {
    while (!waiter.done.load(std::memory_order_acquire)) {
        XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 25);
    }
}

inline size_t CurlStringWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    if (out && ptr && total > 0) {
        out->append(ptr, total);
    }
    return total;
}

bool TestD3D12AsyncComputeCrossQueueSync();
bool TestD3D12ComputePsoAndUavExecution();
bool TestD3D12HeadlessDeviceAndPso();
bool TestD3D12PlacedResourcesAndHeap();
bool TestD3D12Xs3DPerspectiveDepthRenderE2E();
bool TestD3D12XsAsyncComputeCrossQueueSyncE2E();
bool TestD3D12XsCmdListComputeDispatchE2E();
bool TestD3D12XsCmdListRenderClearE2E();
bool TestD3D12XsCommandListWrapperE2E();
bool TestD3D12XsComVtableAbiConformanceE2E();
bool TestD3D12XsFramePacingTokensE2E();
bool TestD3D12XsHeadlessPresentXModeE2E();
bool TestD3D12XsPresentXSwapChainE2E();
bool TestDStorageDeviceXBridgeAndMipChain();
bool TestDStorageGpuBufferAndFenceE2E();
bool TestDStorageGpuBufferStreaming();
bool TestDStorageGpuTextureStreaming();
bool TestDStorageLzDecompression();
bool TestDStorageVelocityDynamicE2E();
bool TestDStorageVfsAndErrorRecord();
bool TestDxilSm6BitcodeInspector();
bool TestDynamicAndMultiModuleHooks();
bool TestDynamicLoaderHooks();
bool TestGameConfigBootParser();
bool TestGameInputLivePolling();
bool TestGameInputRuntimePollingE2E();
bool TestKernelXAndPixEvtE2E();
bool TestKernelXMemoryAndTopology();
bool TestLoaderInspectAndXvdMount();
bool TestLoaderXvdAndGameConfigE2E();
bool TestNetworkingShimsE2E();
bool TestPeDelayLoadAndOrdinalAuditE2E();
bool TestPresentXDescriptorAudit();
bool TestRdna2ComputeDispatchE2E();
bool TestRdna2ComputeIsaExecution();
bool TestRdna2ImageSampleShader();
bool TestRdna2IsaDecoderAndGpuExecution();
bool TestRdna2TexturedDrawE2E();
bool TestRdna2VertexAndPixelPipelineWithCbv();
bool TestRetailBringUpShims();
bool TestRuntimeAndXUser();
bool TestRuntimeDelayLoadAndOrdinalHookE2E();
bool TestScarlettFullStack50SprintCapstoneE2E();
bool TestScarlettTitleBringUpE2E();
bool TestScarlettXConformanceSuite();
bool TestScxsTranslatorAndPsoCache();
bool TestShaderDiskCacheE2E();
bool TestVfsPathTranslation();
bool TestXAudio2ImaAdpcmDecodeBridge();
bool TestXAudio2RuntimePlaybackE2E();
bool TestXAudio2Xma2Rejection();
bool TestXGameSaveEdgeCases();
bool TestXgXs64KbsAndBcnBlockSwizzle();
bool TestXgXsMultiMipAndNpotSwizzle();
bool TestXMemAlignmentAndInvariants();
bool TestXMemAndXgXsSwizzleE2E();
bool TestXUserAndGameSaveProfileE2E();