#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"

#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>

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
}

struct SignInContext {
    std::atomic<bool> done{false};
    XUserHandle user{nullptr};
};

static void CALLBACK OnUserAdded(XAsyncBlock* async) {
    auto* ctx = static_cast<SignInContext*>(async->context);
    HRESULT hr = XUserAddResult(async, &ctx->user);
    if (SUCCEEDED(hr) && ctx->user) {
        char tag[64]{};
        size_t used = 0;
        if (SUCCEEDED(XUserGetGamertag(ctx->user, XUserGamertagComponent::Classic,
                                       sizeof(tag), tag, &used))) {
            std::printf("[TITLE][T:%05lu] Signed in as: %s\n", GetCurrentThreadId(), tag);
        }
    }
    ctx->done.store(true, std::memory_order_release);
}

int main() {
    std::printf("[TITLE][T:%05lu] Booting synthetic Series X title...\n", GetCurrentThreadId());

    if (FAILED(XGameRuntimeInitialize())) {
        return 1;
    }

    auto deviceType = XSystemGetDeviceType();
    std::printf("[TITLE] Device type: 0x%02X\n", static_cast<uint32_t>(deviceType));

    // 1. Async profile sign-in via xgameruntime.dll
    XTaskQueueHandle queue = nullptr;
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                XTaskQueueDispatchMode::Manual, &queue))) {
        return 1;
    }

    SignInContext ctx{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &ctx;
    async.callback = OnUserAdded;

    if (SUCCEEDED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async))) {
        while (!ctx.done.load(std::memory_order_acquire)) {
            XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 50);
        }
    }

    // 2. Scarlett D3D12.x graphics bootstrap via d3d12_xs.dll
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;

    ComPtr<ID3D12DeviceX> deviceX;
    if (FAILED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX,
                                     reinterpret_cast<void**>(deviceX.GetAddressOf())))) {
        std::printf("[TITLE] D3D12XboxCreateDevice failed!\n");
        return 1;
    }

    deviceX->SetFrameIntervalX(nullptr, 16667, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE);

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    if (FAILED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue))) {
        return 1;
    }

    // Console-owned render target texture (no DXGI swapchain visible to the title!)
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 1280;
    rtDesc.Height = 720;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> renderTarget;
    if (FAILED(deviceX->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&renderTarget)))) {
        return 1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(deviceX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
        return 1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(renderTarget.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc))) ||
        FAILED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))) ||
        FAILED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return 1;
    }
    cmdList->Close();

    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;

    std::printf("[TITLE] Entering PresentX render loop -- press ESC or close the window to exit.\n");

    uint64_t frame = 0;
    while (true) {
        D3D12XBOX_FRAME_PIPELINE_TOKEN token = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
        deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, &token);

        cmdAlloc->Reset();
        cmdList->Reset(cmdAlloc.Get(), nullptr);

        D3D12_RESOURCE_BARRIER toRT{};
        toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource = renderTarget.Get();
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmdList->ResourceBarrier(1, &toRT);

        // Smooth pulsing Xbox-green / teal color cycle
        float t = static_cast<float>(frame) * 0.03f;
        float clearColor[4] = {
            0.05f,
            0.40f + 0.35f * std::sinf(t),
            0.15f + 0.15f * std::cosf(t * 0.7f),
            1.0f
        };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = renderTarget.Get();
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &toPresent);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        ID3D12Resource* planeResources[] = { renderTarget.Get() };
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.ResourceCount = 1;
        plane.ppResources = planeResources;

        D3D12XBOX_PRESENT_PARAMETERS presentParams{};
        HRESULT presentHr = cmdQueue->PresentX(1, &plane, &presentParams);

        ++fenceVal;
        cmdQueue->Signal(fence.Get(), fenceVal);
        if (fence->GetCompletedValue() < fenceVal) {
            fence->SetEventOnCompletion(fenceVal, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        if (presentHr == S_FALSE || FAILED(presentHr)) {
            break;
        }
        ++frame;
    }

    std::printf("[TITLE] Rendered %llu frames via PresentX. Shutting down...\n",
                static_cast<unsigned long long>(frame));

    CloseHandle(fenceEvent);
    fence.Reset();
    cmdList.Reset();
    cmdAlloc.Reset();
    rtvHeap.Reset();
    renderTarget.Reset();
    cmdQueue.Reset();
    deviceX.Reset();

    if (ctx.user) {
        XUserCloseHandle(ctx.user);
    }
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::printf("[TITLE] Shutdown complete.\n");
    return 0;
}