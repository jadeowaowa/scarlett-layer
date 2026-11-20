#include "d3d12_xs.hpp"
#include "common/stub.hpp"

#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSwapBufferCount = 2;
constexpr wchar_t kWindowClassName[] = L"ScarlettLayerWindowClass";

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}

const char* ClassifyShaderBytecode(const D3D12_SHADER_BYTECODE& bc) {
    if (!bc.pShaderBytecode || bc.BytecodeLength < 4) {
        return "none";
    }
    const auto* bytes = static_cast<const uint8_t*>(bc.pShaderBytecode);
    if (std::memcmp(bytes, "DXBC", 4) == 0) {
        return "DXBC";
    }
    if (std::memcmp(bytes, "DXIL", 4) == 0) {
        return "DXIL";
    }
    return "SCARLETT_ISA_OR_CUSTOM";
}

LRESULT CALLBACK ScarlettWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }

    auto* closedFlag = reinterpret_cast<bool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (closedFlag) {
                    *closedFlag = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (closedFlag) {
                *closedFlag = true;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

class ScarlettCommandQueueX final : public ID3D12CommandQueueX {
public:
    ScarlettCommandQueueX(
        ComPtr<IDXGIFactory6> factory,
        ComPtr<ID3D12Device> device,
        ComPtr<ID3D12CommandQueue> queue)
        : m_factory(std::move(factory))
        , m_device(std::move(device))
        , m_queue(std::move(queue)) {}

    ~ScarlettCommandQueueX() {
        FlushGpu();
        if (m_fenceEvent) {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        if (m_hwnd && IsWindow(m_hwnd)) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "~ScarlettCommandQueueX", "-> released queue & swapchain");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_ID3D12CommandQueueX)) {
            *ppvObject = static_cast<ID3D12CommandQueueX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) override {
        m_queue->ExecuteCommandLists(NumCommandLists, ppCommandLists);
    }

    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override {
        return m_queue->Signal(pFence, Value);
    }

    HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* /*pPresentParameters*/) override {
        if (PlaneCount == 0 || !pPlaneParameters ||
            pPlaneParameters[0].ResourceCount == 0 || !pPlaneParameters[0].ppResources ||
            !pPlaneParameters[0].ppResources[0]) {
            return E_INVALIDARG;
        }

        ID3D12Resource* srcResource = pPlaneParameters[0].ppResources[0];

        if (!m_swapChain) {
            HRESULT hr = InitPresentationPipeline(srcResource);
            if (FAILED(hr)) {
                return hr;
            }
        }

        PumpMessages();
        if (m_windowClosed) {
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "PresentX", "-> host window closed");
            return S_FALSE;
        }

        UINT backIdx = m_swapChain->GetCurrentBackBufferIndex();
        WaitForFenceValue(m_frameFenceValues[backIdx]);

        HRESULT hr = m_copyAllocators[backIdx]->Reset();
        if (FAILED(hr)) {
            return hr;
        }
        hr = m_copyCmdList->Reset(m_copyAllocators[backIdx].Get(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }

        ID3D12Resource* dstBackBuffer = m_backBuffers[backIdx].Get();

        D3D12_RESOURCE_BARRIER preBarriers[2]{};
        preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarriers[0].Transition.pResource = srcResource;
        preBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        preBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarriers[1].Transition.pResource = dstBackBuffer;
        preBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        preBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_copyCmdList->ResourceBarrier(2, preBarriers);

        m_copyCmdList->CopyResource(dstBackBuffer, srcResource);

        D3D12_RESOURCE_BARRIER postBarriers[2]{};
        postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[0].Transition.pResource = dstBackBuffer;
        postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        postBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

        postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[1].Transition.pResource = srcResource;
        postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        postBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_copyCmdList->ResourceBarrier(2, postBarriers);

        hr = m_copyCmdList->Close();
        if (FAILED(hr)) {
            return hr;
        }

        ID3D12CommandList* lists[] = { m_copyCmdList.Get() };
        m_queue->ExecuteCommandLists(1, lists);

        hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) {
            return hr;
        }

        ++m_fenceValue;
        m_queue->Signal(m_copyFence.Get(), m_fenceValue);
        m_frameFenceValues[backIdx] = m_fenceValue;
        return S_OK;
    }

private:
    HRESULT InitPresentationPipeline(ID3D12Resource* srcResource) {
        D3D12_RESOURCE_DESC desc = srcResource->GetDesc();
        const UINT width = static_cast<UINT>(desc.Width);
        const UINT height = desc.Height;

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ScarlettWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);

        RECT rc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        m_hwnd = CreateWindowExW(
            0,
            kWindowClassName,
            L"Scarlett Layer - Xbox Series X [d3d12_xs + gameinput]",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInst, &m_windowClosed);

        if (!m_hwnd) {
            sc::log::Write(sc::log::Level::Error, "d3d12_xs", "InitPresentationPipeline", "-> CreateWindowExW failed");
            return E_FAIL;
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width = width;
        scDesc.Height = height;
        scDesc.Format = desc.Format;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = kSwapBufferCount;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = m_factory->CreateSwapChainForHwnd(
            m_queue.Get(), m_hwnd, &scDesc, nullptr, nullptr, &sc1);
        if (FAILED(hr)) {
            sc::log::Write(sc::log::Level::Error, "d3d12_xs", "InitPresentationPipeline", "-> CreateSwapChainForHwnd failed");
            return hr;
        }

        m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
        hr = sc1.As(&m_swapChain);
        if (FAILED(hr)) {
            return hr;
        }

        for (UINT i = 0; i < kSwapBufferCount; ++i) {
            hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
            if (FAILED(hr)) {
                return hr;
            }
            hr = m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_copyAllocators[i]));
            if (FAILED(hr)) {
                return hr;
            }
        }

        hr = m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_copyAllocators[0].Get(), nullptr,
            IID_PPV_ARGS(&m_copyCmdList));
        if (FAILED(hr)) {
            return hr;
        }
        m_copyCmdList->Close();

        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence));
        if (FAILED(hr)) {
            return hr;
        }
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        char info[128];
        std::snprintf(info, sizeof(info), "-> created %ux%u DXGI Flip-Discard swapchain for PresentX", width, height);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "InitPresentationPipeline", info);
        return S_OK;
    }

    void PumpMessages() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                m_windowClosed = true;
            }
        }
    }

    void WaitForFenceValue(UINT64 target) {
        if (!m_copyFence || target == 0) {
            return;
        }
        if (m_copyFence->GetCompletedValue() < target) {
            m_copyFence->SetEventOnCompletion(target, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    void FlushGpu() {
        if (m_queue && m_copyFence) {
            ++m_fenceValue;
            if (SUCCEEDED(m_queue->Signal(m_copyFence.Get(), m_fenceValue))) {
                WaitForFenceValue(m_fenceValue);
            }
        }
    }

    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_backBuffers[kSwapBufferCount];
    ComPtr<ID3D12Fence> m_copyFence;
    ComPtr<ID3D12CommandAllocator> m_copyAllocators[kSwapBufferCount];
    ComPtr<ID3D12GraphicsCommandList> m_copyCmdList;

    HWND m_hwnd{nullptr};
    HANDLE m_fenceEvent{nullptr};
    UINT64 m_fenceValue{0};
    UINT64 m_frameFenceValues[kSwapBufferCount]{};
    bool m_windowClosed{false};
};

class ScarlettDeviceX final : public ID3D12DeviceX {
public:
    ScarlettDeviceX(ComPtr<IDXGIFactory6> factory, ComPtr<ID3D12Device> device)
        : m_factory(std::move(factory))
        , m_device(std::move(device)) {}

    ~ScarlettDeviceX() {
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "~ScarlettDeviceX", "-> destroyed device");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_ID3D12DeviceX)) {
            *ppvObject = static_cast<ID3D12DeviceX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandQueueX(
        const D3D12_COMMAND_QUEUE_DESC* pDesc,
        ID3D12CommandQueueX** ppCommandQueue) override {
        if (!pDesc || !ppCommandQueue) {
            return E_INVALIDARG;
        }
        ComPtr<ID3D12CommandQueue> nativeQueue;
        HRESULT hr = m_device->CreateCommandQueue(pDesc, IID_PPV_ARGS(&nativeQueue));
        if (FAILED(hr)) {
            return hr;
        }
        *ppCommandQueue = new ScarlettCommandQueueX(m_factory, m_device, std::move(nativeQueue));
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, "-> wrapped ID3D12CommandQueueX");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override {
        return m_device->CreateCommandAllocator(type, riid, ppCommandAllocator);
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState,
        REFIID riid, void** ppCommandList) override {
        return m_device->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override {
        return m_device->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override {
        return m_device->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override {
        return m_device->CreateCommittedResource(
            pHeapProperties, HeapFlags, pDesc, InitialResourceState,
            pOptimizedClearValue, riidResource, ppvResource);
    }

    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    }

    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override {
        return m_device->CreateFence(InitialValue, Flags, riid, ppFence);
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask,
        const void* pBlobWithRootSignature,
        SIZE_T blobLengthInBytes,
        REFIID riid,
        void** ppvRootSignature) override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "-> creating root signature (%zu bytes)", blobLengthInBytes);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateRootSignature", buf);
        return m_device->CreateRootSignature(
            nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
        REFIID riid,
        void** ppPipelineState) override {
        if (!pDesc) {
            return E_INVALIDARG;
        }
        // Future RDNA2-to-DXIL shader translation pipeline plugs in right here
        char buf[160];
        std::snprintf(buf, sizeof(buf), "-> PSO VS=%s (%zu B), PS=%s (%zu B)",
                      ClassifyShaderBytecode(pDesc->VS), pDesc->VS.BytecodeLength,
                      ClassifyShaderBytecode(pDesc->PS), pDesc->PS.BytecodeLength);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateGraphicsPipelineState", buf);
        return m_device->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
    }

    HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* /*pOutput*/, uint32_t PeriodInMicroseconds,
        uint32_t /*MaxLatency*/, uint32_t /*Flags*/) override {
        m_framePeriodUs = PeriodInMicroseconds;
        char buf[96];
        double hz = (PeriodInMicroseconds > 0) ? (1000000.0 / PeriodInMicroseconds) : 0.0;
        std::snprintf(buf, sizeof(buf), "-> interval=%u us (%.1f Hz)", PeriodInMicroseconds, hz);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, buf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT /*Event*/, UINT64 /*OffsetMicroseconds*/,
        HANDLE hEvent, uint32_t /*Flags*/) override {
        if (hEvent) {
            SetEvent(hEvent);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT /*Event*/, DWORD /*TimeoutInMilliseconds*/,
        HANDLE /*hEvent*/, D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken) override {
        if (pToken) {
            *pToken = m_nextToken.fetch_add(1, std::memory_order_relaxed);
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    uint32_t m_framePeriodUs{16667};
    std::atomic<D3D12XBOX_FRAME_PIPELINE_TOKEN> m_nextToken{1};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI D3D12XboxCreateDevice(
    void* /*pAdapter*/,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* /*pParameters*/,
    REFIID riid,
    void** ppDevice) {
    if (!ppDevice) {
        return E_POINTER;
    }
    *ppDevice = nullptr;

    UINT factoryFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugCtrl;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugCtrl)))) {
        debugCtrl->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, "-> D3D12 debug layer enabled");
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory));
    if (FAILED(hr) && factoryFlags != 0) {
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    }
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByGpuPreference(
        0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        hr = factory->EnumAdapters1(0, &adapter);
        if (FAILED(hr)) {
            return hr;
        }
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::string gpuMsg = "-> selected host GPU: " + WideToUtf8(desc.Description);
    sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, gpuMsg.c_str());

    ComPtr<ID3D12Device> nativeDevice;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&nativeDevice));
    if (FAILED(hr)) {
        sc::log::Write(sc::log::Level::Error, "d3d12_xs", __FUNCTION__, "-> D3D12CreateDevice failed");
        return hr;
    }

    auto* wrapper = new ScarlettDeviceX(std::move(factory), std::move(nativeDevice));
    hr = wrapper->QueryInterface(riid, ppDevice);
    wrapper->Release();
    return hr;
}

HRESULT WINAPI ScarlettSerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    return ::D3D12SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
}

} // extern "C"