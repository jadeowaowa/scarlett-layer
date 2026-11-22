#include "d3d12_xs_internal.hpp"
#include "common/config.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSwapBufferCount = 2;
constexpr wchar_t kWindowClassName[] = L"ScarlettLayerWindowClass";
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
            if (closedFlag) {
                *closedFlag = true;
            }
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
        for (auto& src : m_inFlightSrcResources) {
            src.Reset();
        }
        for (auto& bb : m_backBuffers) {
            bb.Reset();
        }
        m_swapChain.Reset();
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

    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override {
        return m_queue->Wait(pFence, Value);
    }

    void STDMETHODCALLTYPE SuspendX(UINT32 Flags) override {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "-> queue suspended (flags=0x%X)", Flags);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "SuspendX", buf);
    }

    void STDMETHODCALLTYPE ResumeX() override {
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "ResumeX", "-> queue resumed");
    }

    HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* /*pPresentParameters*/) override {
        // Xbox titles (e.g. Microsoft Xbox-GDK-Samples SimpleTriangle) call
        // PresentX(0, nullptr, nullptr) during init/teardown to clear display planes.
        if (PlaneCount == 0 && !pPlaneParameters) {
            return S_OK;
        }
        if (PlaneCount == 0 || !pPlaneParameters ||
            pPlaneParameters[0].ResourceCount == 0 || !pPlaneParameters[0].ppResources ||
            !pPlaneParameters[0].ppResources[0]) {
            return E_INVALIDARG;
        }

        ID3D12Resource* srcResource = pPlaneParameters[0].ppResources[0];
        D3D12_RESOURCE_DESC srcDesc = srcResource->GetDesc();
        if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            srcDesc.Width == 0 || srcDesc.Height == 0) {
            return E_INVALIDARG;
        }

        if (!m_pipelineInitialized) {
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

        UINT backIdx = m_headless ? m_headlessBackIdx : m_swapChain->GetCurrentBackBufferIndex();
        WaitForFenceValue(m_frameFenceValues[backIdx]);
        m_inFlightSrcResources[backIdx] = srcResource;

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

        D3D12_RESOURCE_DESC dstDesc = dstBackBuffer->GetDesc();
        if (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height &&
            srcDesc.Format == dstDesc.Format) {
            m_copyCmdList->CopyResource(dstBackBuffer, srcResource);
        } else {
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstBackBuffer;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = srcResource;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;

            D3D12_BOX srcBox{};
            srcBox.left = 0;
            srcBox.top = 0;
            srcBox.front = 0;
            srcBox.right = static_cast<UINT>((std::min)(srcDesc.Width, dstDesc.Width));
            srcBox.bottom = (std::min)(srcDesc.Height, dstDesc.Height);
            srcBox.back = 1;
            m_copyCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
        }

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

        if (!m_headless) {
            hr = m_swapChain->Present(1, 0);
            if (FAILED(hr)) {
                return hr;
            }
        } else {
            m_headlessBackIdx = (m_headlessBackIdx + 1) % kSwapBufferCount;
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

        m_headless = sc::config::IsHeadless();

        if (m_headless) {
            D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC bbDesc{};
            bbDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            bbDesc.Width = width;
            bbDesc.Height = height;
            bbDesc.DepthOrArraySize = 1;
            bbDesc.MipLevels = 1;
            bbDesc.Format = desc.Format;
            bbDesc.SampleDesc.Count = 1;
            bbDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            for (UINT i = 0; i < kSwapBufferCount; ++i) {
                HRESULT hr = m_device->CreateCommittedResource(
                    &defHeap, D3D12_HEAP_FLAG_NONE, &bbDesc,
                    D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&m_backBuffers[i]));
                if (FAILED(hr)) {
                    return hr;
                }
                hr = m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_copyAllocators[i]));
                if (FAILED(hr)) {
                    return hr;
                }
            }

            HRESULT hr = m_device->CreateCommandList(
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
            m_pipelineInitialized = true;

            char info[128];
            std::snprintf(info, sizeof(info), "-> created %ux%u offscreen PresentX pipeline (headless)", width, height);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "InitPresentationPipeline", info);
            return S_OK;
        }

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
            L"Scarlett Layer - Xbox Series X [Compute UAV + SCXS + PSO Cache]",
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
        m_pipelineInitialized = true;
        std::snprintf(info, sizeof(info), "-> created %ux%u DXGI Flip-Discard swapchain for PresentX (windowed)", width, height);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "InitPresentationPipeline", info);
        return S_OK;
    }

    void PumpMessages() {
        if (!m_hwnd) {
            return;
        }
        MSG msg{};
        while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
    ComPtr<ID3D12Resource> m_inFlightSrcResources[kSwapBufferCount];
    ComPtr<ID3D12Fence> m_copyFence;
    ComPtr<ID3D12CommandAllocator> m_copyAllocators[kSwapBufferCount];
    ComPtr<ID3D12GraphicsCommandList> m_copyCmdList;
    bool m_headless{false};
    bool m_pipelineInitialized{false};
    UINT m_headlessBackIdx{0};

    HWND m_hwnd{nullptr};
    HANDLE m_fenceEvent{nullptr};
    UINT64 m_fenceValue{0};
    UINT64 m_frameFenceValues[kSwapBufferCount]{};
    bool m_windowClosed{false};
};


} // namespace

namespace sc::d3d12xs {

ComPtr<ID3D12CommandQueueX> CreateScarlettCommandQueueX(
    ComPtr<IDXGIFactory6> factory,
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12CommandQueue> queue) {
    return MakeCom<ID3D12CommandQueueX, ScarlettCommandQueueX>(
        std::move(factory), std::move(device), std::move(queue));
}

} // namespace sc::d3d12xs