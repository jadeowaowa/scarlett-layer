#include "d3d12_xs_internal.hpp"
#include "shader_inspector.hpp"
#include "shader_stripper.hpp"
#include "shader_translator.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using sc::d3d12xs::Fnv1a64;
using sc::d3d12xs::HashComputePsoDesc;
using sc::d3d12xs::HashGraphicsPsoDesc;
using sc::d3d12xs::ResolvePsoCachePath;

namespace {

bool FormatDisallowsDesktopUav(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

D3D12_RESOURCE_DESC SanitizeXboxResourceDesc(const D3D12_RESOURCE_DESC& desc, const char* caller) {
    const uint32_t rawFlags = static_cast<uint32_t>(desc.Flags);
    const uint32_t rawLayout = static_cast<uint32_t>(desc.Layout);
    const bool stripDenySrv =
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) &&
        ((rawFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) != 0u);
    const bool stripUav =
        (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) &&
        ((rawFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0u) &&
        (FormatDisallowsDesktopUav(desc.Format) || desc.SampleDesc.Count > 1u);
    const uint32_t kDepthConflictMask =
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    const bool stripDepthConflicts =
        ((rawFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0u) &&
        ((rawFlags & kDepthConflictMask) != 0u);
    const bool fixLayout = (rawLayout > 3u);
    if ((rawFlags & D3D12XBOX_RESOURCE_FLAG_MASK) == 0u && !stripDenySrv && !stripUav && !stripDepthConflicts && !fixLayout) {
        return desc;
    }
    D3D12_RESOURCE_DESC clean = desc;
    uint32_t cleanedFlags = rawFlags & ~D3D12XBOX_RESOURCE_FLAG_MASK;
    if (stripDepthConflicts) {
        cleanedFlags &= ~kDepthConflictMask;
    }
    if (stripDenySrv) {
        cleanedFlags &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    if (stripUav) {
        cleanedFlags &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        static std::atomic<uint32_t> s_uavStripLogs{0};
        if (s_uavStripLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "-> stripped ALLOW_UNORDERED_ACCESS for desktop-incompatible fmt=%u msaa=%u",
                          static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", caller, buf);
        }
    }
    clean.Flags = static_cast<D3D12_RESOURCE_FLAGS>(cleanedFlags);
    if (fixLayout) {
        clean.Layout = (clean.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
            : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        static std::atomic<uint32_t> s_layoutLogCount{0};
        if (s_layoutLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "-> mapped console-only layout %u -> %u (dim=%u)",
                          rawLayout, static_cast<uint32_t>(clean.Layout),
                          static_cast<uint32_t>(clean.Dimension));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", caller, buf);
        }
    }
    if ((rawFlags & D3D12XBOX_RESOURCE_FLAG_MASK) != 0u) {
        static std::atomic<uint32_t> s_flagLogCount{0};
        if (s_flagLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "-> stripped console-only resource flags 0x%08X (kept 0x%08X)",
                          rawFlags & D3D12XBOX_RESOURCE_FLAG_MASK,
                          static_cast<uint32_t>(clean.Flags));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", caller, buf);
        }
    }
    return clean;
}

class ScarlettDeviceX final : public ID3D12DeviceX {
public:
    ScarlettDeviceX(
        ComPtr<IDXGIFactory6> factory,
        ComPtr<ID3D12Device> device,
        UINT vendorId,
        UINT deviceId)
        : m_factory(std::move(factory))
        , m_device(std::move(device))
        , m_psoCachePath(ResolvePsoCachePath(vendorId, deviceId)) {
        InitPipelineLibrary();
    }

    ~ScarlettDeviceX() {
        SavePipelineLibrary();
        m_pipelineLibrary.Reset();
        m_psoCacheBlob.clear();
        m_xlatCache.clear();
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
        if (IsEqualGUID(riid, IID_ScarlettNativeD3D12Device)) {
            return m_device->QueryInterface(__uuidof(ID3D12Device), ppvObject);
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
        *ppCommandQueue = sc::d3d12xs::CreateScarlettCommandQueueX(m_factory, m_device, std::move(nativeQueue)).Detach();
        const char* typeName = (pDesc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE) ? "COMPUTE"
                             : (pDesc->Type == D3D12_COMMAND_LIST_TYPE_COPY)    ? "COPY"
                                                                                : "DIRECT";
        char qBuf[96];
        std::snprintf(qBuf, sizeof(qBuf), "-> wrapped ID3D12CommandQueueX (type=%s)", typeName);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, qBuf);
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
        if (IsEqualIID(riid, IID_ID3D12GraphicsCommandListX)) {
            if (!ppCommandList) return E_POINTER;
            *ppCommandList = nullptr;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> nativeCl;
            HRESULT hr = m_device->CreateCommandList(
                nodeMask, type, pCommandAllocator, pInitialState, IID_PPV_ARGS(&nativeCl));
            if (FAILED(hr)) return hr;
            auto wrapped = sc::d3d12xs::CreateScarlettGraphicsCommandListX(nativeCl.Get());
            return wrapped->QueryInterface(riid, ppCommandList);
        }
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
        D3D12_RESOURCE_DESC cleanDesc{};
        const D3D12_RESOURCE_DESC* descPtr = pDesc;
        if (pDesc && (static_cast<uint32_t>(pDesc->Flags) & D3D12XBOX_RESOURCE_FLAG_MASK) != 0u) {
            cleanDesc = SanitizeXboxResourceDesc(*pDesc, "CreateCommittedResource");
            descPtr = &cleanDesc;
        }
        return m_device->CreateCommittedResource(
            pHeapProperties, HeapFlags, descPtr, InitialResourceState,
            pOptimizedClearValue, riidResource, ppvResource);
    }

    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override {
        if (pResourceDescs && numResourceDescs > 0) {
            bool anyConsoleFlags = false;
            for (UINT i = 0; i < numResourceDescs; ++i) {
                if ((static_cast<uint32_t>(pResourceDescs[i].Flags) & D3D12XBOX_RESOURCE_FLAG_MASK) != 0u) {
                    anyConsoleFlags = true;
                    break;
                }
            }
            if (anyConsoleFlags) {
                std::vector<D3D12_RESOURCE_DESC> clean(numResourceDescs);
                for (UINT i = 0; i < numResourceDescs; ++i) {
                    clean[i] = SanitizeXboxResourceDesc(pResourceDescs[i], "GetResourceAllocationInfo");
                }
                return m_device->GetResourceAllocationInfo(visibleMask, numResourceDescs, clean.data());
            }
        }
        return m_device->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
    }

    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        if (!pDesc || !ppvHeap) {
            return E_INVALIDARG;
        }
        HRESULT hr = m_device->CreateHeap(pDesc, riid, ppvHeap);
        if (SUCCEEDED(hr)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> created GPU heap (%llu B, align=%llu B, type=%d, flags=0x%X)",
                          static_cast<unsigned long long>(pDesc->SizeInBytes),
                          static_cast<unsigned long long>(pDesc->Alignment),
                          static_cast<int>(pDesc->Properties.Type),
                          static_cast<unsigned>(pDesc->Flags));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateHeap", buf);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override {
        if (!pHeap || !pDesc || !ppvResource) {
            return E_INVALIDARG;
        }
        D3D12_RESOURCE_DESC cleanDesc{};
        const D3D12_RESOURCE_DESC* descPtr = pDesc;
        if ((static_cast<uint32_t>(pDesc->Flags) & D3D12XBOX_RESOURCE_FLAG_MASK) != 0u) {
            cleanDesc = SanitizeXboxResourceDesc(*pDesc, "CreatePlacedResource");
            descPtr = &cleanDesc;
        }
        HRESULT hr = m_device->CreatePlacedResource(
            pHeap, HeapOffset, descPtr, InitialState, pOptimizedClearValue, riid, ppvResource);
        if (SUCCEEDED(hr)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> placed resource @ heap+0x%llX (dim=%d, %llux%u, fmt=%d)",
                          static_cast<unsigned long long>(HeapOffset),
                          static_cast<int>(pDesc->Dimension),
                          static_cast<unsigned long long>(pDesc->Width),
                          static_cast<unsigned>(pDesc->Height),
                          static_cast<int>(pDesc->Format));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreatePlacedResource", buf);
        }
        return hr;
    }

    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
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

        HRESULT hr = m_device->CreateRootSignature(
            nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
        if (SUCCEEDED(hr) && ppvRootSignature && *ppvRootSignature) {
            Fnv1a64 rsHash;
            rsHash.Update(pBlobWithRootSignature, blobLengthInBytes);
            std::lock_guard<std::mutex> lock(m_psoMtx);
            m_rootSigHashes[*ppvRootSignature] = rsHash.Digest();
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
        REFIID riid,
        void** ppPipelineState) override {
        if (!pDesc || !ppPipelineState) {
            return E_INVALIDARG;
        }

        const auto startTime = std::chrono::steady_clock::now();

        // 1. Run pluggable shader translator boundary on every populated shader stage
        D3D12_GRAPHICS_PIPELINE_STATE_DESC hostDesc = *pDesc;
        std::shared_ptr<const sc::d3d12::TranslatedShader> vsXlat, psXlat, dsXlat, hsXlat, gsXlat;
        std::string vsTag, psTag, xlatErr;

        HRESULT xlatHr = TranslateStage(hostDesc.VS, vsXlat, vsTag, xlatErr);
        if (SUCCEEDED(xlatHr)) xlatHr = TranslateStage(hostDesc.PS, psXlat, psTag, xlatErr);
        if (SUCCEEDED(xlatHr)) { std::string ignored; xlatHr = TranslateStage(hostDesc.DS, dsXlat, ignored, xlatErr); }
        if (SUCCEEDED(xlatHr)) { std::string ignored; xlatHr = TranslateStage(hostDesc.HS, hsXlat, ignored, xlatErr); }
        if (SUCCEEDED(xlatHr)) { std::string ignored; xlatHr = TranslateStage(hostDesc.GS, gsXlat, ignored, xlatErr); }

        if (FAILED(xlatHr)) {
            std::string errMsg = "-> shader translation rejected: " + xlatErr;
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "CreateGraphicsPipelineState", errMsg.c_str());
            return xlatHr;
        }

        std::string vsInfo = vsTag + sc::d3d12::InspectShaderBytecode(hostDesc.VS);
        std::string psInfo = psTag + sc::d3d12::InspectShaderBytecode(hostDesc.PS);

        uint64_t rsHash = 0;
        {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            auto it = m_rootSigHashes.find(hostDesc.pRootSignature);
            if (it != m_rootSigHashes.end()) {
                rsHash = it->second;
            }
        }

        const uint64_t psoHash = HashGraphicsPsoDesc(hostDesc, rsHash);
        wchar_t keyW[32];
        std::swprintf(keyW, _countof(keyW), L"GFX_%016llX", static_cast<unsigned long long>(psoHash));
        char keyA[32];
        std::snprintf(keyA, sizeof(keyA), "GFX_%016llX", static_cast<unsigned long long>(psoHash));

        // 2. Try disk-backed ID3D12PipelineLibrary cache first
        if (m_pipelineLibrary) {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            HRESULT cacheHr = m_pipelineLibrary->LoadGraphicsPipeline(keyW, &hostDesc, riid, ppPipelineState);
            if (SUCCEEDED(cacheHr)) {
                ++m_psoCacheHits;
                const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                              "-> PSO CACHE HIT [%s] in %.2f ms | VS=%s | PS=%s",
                              keyA, static_cast<double>(elapsedUs) / 1000.0,
                              vsInfo.c_str(), psInfo.c_str());
                sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateGraphicsPipelineState", msg);
                return S_OK;
            }
        }

        // 3. Cache miss -> compile natively & store into pipeline library for future launches
        HRESULT hr = m_device->CreateGraphicsPipelineState(&hostDesc, riid, ppPipelineState);
        if (SUCCEEDED(hr)) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            bool stored = false;
            if (m_pipelineLibrary && *ppPipelineState) {
                ComPtr<ID3D12PipelineState> psoObj;
                if (SUCCEEDED(reinterpret_cast<IUnknown*>(*ppPipelineState)->QueryInterface(
                        IID_PPV_ARGS(&psoObj)))) {
                    std::lock_guard<std::mutex> lock(m_psoMtx);
                    if (SUCCEEDED(m_pipelineLibrary->StorePipeline(keyW, psoObj.Get()))) {
                        m_psoCacheDirty = true;
                        stored = true;
                    }
                }
            }
            ++m_psoCacheMisses;
            char msg[320];
            std::snprintf(msg, sizeof(msg),
                          "-> PSO CACHE MISS [%s] compiled in %.2f ms%s | VS=%s | PS=%s",
                          keyA, static_cast<double>(elapsedUs) / 1000.0,
                          stored ? " (stored in library)" : "",
                          vsInfo.c_str(), psInfo.c_str());
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateGraphicsPipelineState", msg);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
        REFIID riid,
        void** ppPipelineState) override {
        if (!pDesc || !ppPipelineState) {
            return E_INVALIDARG;
        }

        const auto startTime = std::chrono::steady_clock::now();

        D3D12_COMPUTE_PIPELINE_STATE_DESC hostDesc = *pDesc;
        std::shared_ptr<const sc::d3d12::TranslatedShader> csXlat;
        std::string csTag, xlatErr;

        HRESULT xlatHr = TranslateStage(hostDesc.CS, csXlat, csTag, xlatErr);
        if (FAILED(xlatHr)) {
            std::string errMsg = "-> compute shader translation rejected: " + xlatErr;
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "CreateComputePipelineState", errMsg.c_str());
            return xlatHr;
        }

        std::string csInfo = csTag + sc::d3d12::InspectShaderBytecode(hostDesc.CS);

        uint64_t rsHash = 0;
        {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            auto it = m_rootSigHashes.find(hostDesc.pRootSignature);
            if (it != m_rootSigHashes.end()) {
                rsHash = it->second;
            }
        }

        const uint64_t psoHash = HashComputePsoDesc(hostDesc, rsHash);
        wchar_t keyW[32];
        std::swprintf(keyW, _countof(keyW), L"CMP_%016llX", static_cast<unsigned long long>(psoHash));
        char keyA[32];
        std::snprintf(keyA, sizeof(keyA), "CMP_%016llX", static_cast<unsigned long long>(psoHash));

        if (m_pipelineLibrary) {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            HRESULT cacheHr = m_pipelineLibrary->LoadComputePipeline(keyW, &hostDesc, riid, ppPipelineState);
            if (SUCCEEDED(cacheHr)) {
                ++m_psoCacheHits;
                const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                              "-> COMPUTE PSO CACHE HIT [%s] in %.2f ms | CS=%s",
                              keyA, static_cast<double>(elapsedUs) / 1000.0, csInfo.c_str());
                sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateComputePipelineState", msg);
                return S_OK;
            }
        }

        HRESULT hr = m_device->CreateComputePipelineState(&hostDesc, riid, ppPipelineState);
        if (SUCCEEDED(hr)) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            bool stored = false;
            if (m_pipelineLibrary && *ppPipelineState) {
                ComPtr<ID3D12PipelineState> psoObj;
                if (SUCCEEDED(reinterpret_cast<IUnknown*>(*ppPipelineState)->QueryInterface(
                        IID_PPV_ARGS(&psoObj)))) {
                    std::lock_guard<std::mutex> lock(m_psoMtx);
                    if (SUCCEEDED(m_pipelineLibrary->StorePipeline(keyW, psoObj.Get()))) {
                        m_psoCacheDirty = true;
                        stored = true;
                    }
                }
            }
            ++m_psoCacheMisses;
            char msg[320];
            std::snprintf(msg, sizeof(msg),
                          "-> COMPUTE PSO CACHE MISS [%s] compiled in %.2f ms%s | CS=%s",
                          keyA, static_cast<double>(elapsedUs) / 1000.0,
                          stored ? " (stored in library)" : "", csInfo.c_str());
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "CreateComputePipelineState", msg);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* /*pOutput*/, uint32_t PeriodInMicroseconds,
        uint32_t /*MaxLatency*/, uint32_t /*Flags*/) override {
        m_framePeriodUs.store(PeriodInMicroseconds, std::memory_order_relaxed);
        m_lastFrameQpc.store(0, std::memory_order_relaxed);
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
        D3D12XBOX_FRAME_EVENT /*Event*/, DWORD TimeoutInMilliseconds,
        HANDLE hEvent, uint32_t /*Flags*/, D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken) override {
        if (hEvent) {
            DWORD waitRes = WaitForSingleObject(hEvent, TimeoutInMilliseconds);
            if (waitRes == WAIT_TIMEOUT) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }

        const uint32_t periodUs = m_framePeriodUs.load(std::memory_order_relaxed);
        if (periodUs > 0) {
            LARGE_INTEGER freq{}, now{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            const int64_t periodTicks = (freq.QuadPart * static_cast<int64_t>(periodUs)) / 1000000LL;
            const int64_t prevTicks = m_lastFrameQpc.load(std::memory_order_relaxed);

            if (prevTicks != 0) {
                const int64_t targetTicks = prevTicks + periodTicks;
                while (now.QuadPart < targetTicks) {
                    const int64_t remTicks = targetTicks - now.QuadPart;
                    const DWORD remMs = static_cast<DWORD>((remTicks * 1000LL) / freq.QuadPart);
                    if (remMs > 1) {
                        Sleep(remMs - 1);
                    } else {
                        SwitchToThread();
                    }
                    QueryPerformanceCounter(&now);
                }
            }
            m_lastFrameQpc.store(now.QuadPart, std::memory_order_relaxed);
        }

        if (pToken) {
            *pToken = m_nextToken.fetch_add(1, std::memory_order_relaxed);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature,
        void* pFeatureSupportData,
        UINT FeatureSupportDataSize) override {
        if (!pFeatureSupportData || FeatureSupportDataSize == 0) {
            return E_INVALIDARG;
        }
        return m_device->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }

private:
    HRESULT TranslateStage(
        D3D12_SHADER_BYTECODE& stageBc,
        std::shared_ptr<const sc::d3d12::TranslatedShader>& outHold,
        std::string& outTag,
        std::string& outError) {
        outHold.reset();
        outTag.clear();
        if (!stageBc.pShaderBytecode || stageBc.BytecodeLength == 0) {
            return S_OK;
        }

        // Fast-path passthrough for clean PC DXBC/DXIL; strip Xbox-specific chunks if present
        const bool isDxbc = stageBc.BytecodeLength >= 4 &&
                            std::memcmp(stageBc.pShaderBytecode, "DXBC", 4) == 0;
        const bool needsXboxStrip = isDxbc &&
            sc::d3d12::HasNonPcDxbcChunks(stageBc.pShaderBytecode, stageBc.BytecodeLength);
        if (isDxbc && !needsXboxStrip) {
            return S_OK;
        }

        Fnv1a64 keyHash;
        keyHash.Update(stageBc.pShaderBytecode, stageBc.BytecodeLength);
        const uint64_t key = keyHash.Digest();

        {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            auto it = m_xlatCache.find(key);
            if (it != m_xlatCache.end()) {
                ++m_xlatHits;
                outHold = it->second;
                if (outHold->wasTranslated) {
                    stageBc.pShaderBytecode = outHold->bytecode.data();
                    stageBc.BytecodeLength = outHold->bytecode.size();
                    outTag = outHold->sourceTag;
                }
                return S_OK;
            }
        }

        auto result = std::make_shared<sc::d3d12::TranslatedShader>();
        if (needsXboxStrip) {
            sc::d3d12::DxbcStripResult stripped{};
            if (!sc::d3d12::StripXboxDxbcChunks(stageBc.pShaderBytecode, stageBc.BytecodeLength, stripped) ||
                !stripped.hasExecutableShader) {
                outError = "Xbox DXBC container missing executable DXIL/SHDR/SHEX chunk after stripping";
                return E_INVALIDARG;
            }
            result->wasTranslated = true;
            result->bytecode      = std::move(stripped.bytecode);
            result->sourceTag     = "[stripped:" + stripped.strippedChunkNames + "] ";
            char stripMsg[192];
            std::snprintf(stripMsg, sizeof(stripMsg),
                          "-> stripped %u Xbox DXBC chunk(s) [%s], kept %u PC chunk(s) (%zu -> %zu B)",
                          stripped.strippedCount, stripped.strippedChunkNames.c_str(),
                          stripped.keptCount, stageBc.BytecodeLength, result->bytecode.size());
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "StripXboxDxbcChunks", stripMsg);
        } else {
            HRESULT hr = m_translators.Translate(stageBc, *result, outError);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_psoMtx);
            ++m_xlatMisses;
            m_xlatCache[key] = result;
        }

        outHold = result;
        if (result->wasTranslated) {
            stageBc.pShaderBytecode = result->bytecode.data();
            stageBc.BytecodeLength = result->bytecode.size();
            outTag = result->sourceTag;
        }
        return S_OK;
    }

    void InitPipelineLibrary() {
        ComPtr<ID3D12Device1> device1;
        if (FAILED(m_device.As(&device1))) {
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "InitPipelineLibrary",
                           "-> ID3D12Device1 unavailable; PSO cache disabled");
            return;
        }

        std::error_code ec;
        if (std::filesystem::exists(m_psoCachePath, ec)) {
            std::ifstream in(m_psoCachePath, std::ios::binary | std::ios::ate);
            if (in.is_open()) {
                std::streamsize sz = in.tellg();
                if (sz > 0) {
                    in.seekg(0, std::ios::beg);
                    m_psoCacheBlob.resize(static_cast<size_t>(sz));
                    if (in.read(reinterpret_cast<char*>(m_psoCacheBlob.data()), sz)) {
                        HRESULT hr = device1->CreatePipelineLibrary(
                            m_psoCacheBlob.data(), m_psoCacheBlob.size(),
                            IID_PPV_ARGS(&m_pipelineLibrary));
                        if (SUCCEEDED(hr)) {
                            char buf[256];
                            std::snprintf(buf, sizeof(buf),
                                          "-> loaded disk PSO cache (%zu B) from %s",
                                          m_psoCacheBlob.size(), m_psoCachePath.string().c_str());
                            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "InitPipelineLibrary", buf);
                            return;
                        }
                        char warnBuf[160];
                        std::snprintf(warnBuf, sizeof(warnBuf),
                                      "-> disk PSO cache invalidated (hr=0x%08lX); rebuilding fresh library",
                                      static_cast<unsigned long>(hr));
                        sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "InitPipelineLibrary", warnBuf);
                    }
                }
            }
            m_psoCacheBlob.clear();
        }

        if (SUCCEEDED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_pipelineLibrary)))) {
            std::string msg = "-> initialized fresh PSO cache library -> " + m_psoCachePath.string();
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "InitPipelineLibrary", msg.c_str());
        }
    }

    void SavePipelineLibrary() {
        std::lock_guard<std::mutex> lock(m_psoMtx);
        if (!m_pipelineLibrary) {
            return;
        }

        if (!m_psoCacheDirty) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> PSO cache stats: hits=%llu, misses=%llu (up to date) | xlat: hits=%llu, misses=%llu",
                          static_cast<unsigned long long>(m_psoCacheHits),
                          static_cast<unsigned long long>(m_psoCacheMisses),
                          static_cast<unsigned long long>(m_xlatHits),
                          static_cast<unsigned long long>(m_xlatMisses));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "SavePipelineLibrary", buf);
            return;
        }

        const SIZE_T serializedSize = m_pipelineLibrary->GetSerializedSize();
        if (serializedSize == 0) {
            return;
        }

        std::vector<uint8_t> outBlob(serializedSize);
        if (FAILED(m_pipelineLibrary->Serialize(outBlob.data(), outBlob.size()))) {
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "SavePipelineLibrary",
                           "-> ID3D12PipelineLibrary::Serialize failed");
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(m_psoCachePath.parent_path(), ec);
        std::filesystem::path tmpPath = m_psoCachePath;
        tmpPath += L".tmp";

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return;
            }
            out.write(reinterpret_cast<const char*>(outBlob.data()),
                      static_cast<std::streamsize>(outBlob.size()));
            out.flush();
            if (!out.good()) {
                return;
            }
        }

        if (MoveFileExW(tmpPath.c_str(), m_psoCachePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            char buf[288];
            std::snprintf(buf, sizeof(buf),
                          "-> saved PSO cache (%zu B, hits=%llu, misses=%llu | xlat: hits=%llu, misses=%llu) -> %s",
                          outBlob.size(),
                          static_cast<unsigned long long>(m_psoCacheHits),
                          static_cast<unsigned long long>(m_psoCacheMisses),
                          static_cast<unsigned long long>(m_xlatHits),
                          static_cast<unsigned long long>(m_xlatMisses),
                          m_psoCachePath.string().c_str());
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "SavePipelineLibrary", buf);
        }
    }

    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    std::filesystem::path m_psoCachePath;

    std::vector<uint8_t> m_psoCacheBlob;
    ComPtr<ID3D12PipelineLibrary> m_pipelineLibrary;

    sc::d3d12::ShaderTranslatorRegistry m_translators;
    std::mutex m_psoMtx;
    std::unordered_map<const void*, uint64_t> m_rootSigHashes;
    std::unordered_map<uint64_t, std::shared_ptr<const sc::d3d12::TranslatedShader>> m_xlatCache;
    bool m_psoCacheDirty{false};
    uint64_t m_psoCacheHits{0};
    uint64_t m_psoCacheMisses{0};
    uint64_t m_xlatHits{0};
    uint64_t m_xlatMisses{0};

    std::atomic<uint32_t> m_framePeriodUs{16667};
    std::atomic<int64_t>  m_lastFrameQpc{0};
    std::atomic<D3D12XBOX_FRAME_PIPELINE_TOKEN> m_nextToken{1};
};


} // namespace

namespace sc::d3d12xs {

ComPtr<ID3D12DeviceX> CreateScarlettDeviceX(
    ComPtr<IDXGIFactory6> factory,
    ComPtr<ID3D12Device> device,
    UINT vendorId,
    UINT deviceId) {
    return MakeCom<ID3D12DeviceX, ScarlettDeviceX>(
        std::move(factory), std::move(device), vendorId, deviceId);
}

} // namespace sc::d3d12xs
namespace {

std::string FormatGuidSimple(REFGUID g) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

std::string FormatCallerAddr(void* retAddr) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(retAddr), &mod) && mod) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        const auto rva = reinterpret_cast<uintptr_t>(retAddr) - reinterpret_cast<uintptr_t>(mod);
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s+0x%llX", name, static_cast<unsigned long long>(rva));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%p", retAddr);
    return buf;
}

class XboxD3D12Device;

class XboxDXGIDevice final : public IDXGIDevice {
public:
    XboxDXGIDevice(ComPtr<IDXGIAdapter1> adapter, ID3D12Device* parentDevice)
        : m_adapter(std::move(adapter)), m_parent(parentDevice) {
        if (m_parent) m_parent->AddRef();
    }

    ~XboxDXGIDevice() {
        if (m_parent) m_parent->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(IDXGIObject)) ||
            IsEqualGUID(riid, __uuidof(IDXGIDevice))) {
            *ppvObject = static_cast<IDXGIDevice*>(this);
            AddRef();
            return S_OK;
        }
        return m_parent ? m_parent->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_adapter ? m_adapter->SetPrivateData(Name, DataSize, pData) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_adapter ? m_adapter->SetPrivateDataInterface(Name, pUnknown) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_adapter ? m_adapter->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        if (!ppParent) return E_POINTER;
        *ppParent = nullptr;
        if (!m_adapter) return E_NOINTERFACE;
        HRESULT hr = m_adapter->QueryInterface(riid, ppParent);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "-> riid=%s hr=0x%08lX", FormatGuidSimple(riid).c_str(), hr);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxDXGIDevice::GetParent", msg);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override {
        if (!pAdapter) return E_POINTER;
        *pAdapter = nullptr;
        return m_adapter ? m_adapter->QueryInterface(IID_PPV_ARGS(pAdapter)) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(
        const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*, IDXGISurface**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY* pResidencyStatus, UINT NumResources) override {
        if (pResidencyStatus) {
            for (UINT i = 0; i < NumResources; ++i) pResidencyStatus[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override {
        if (pPriority) *pPriority = 0;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIAdapter1> m_adapter;
    ID3D12Device* m_parent = nullptr;
};

// Private IID to unwrap XboxD3D12DescriptorHeap -> native ID3D12DescriptorHeap
constexpr GUID IID_ScarlettNativeDescriptorHeap =
    { 0x5c4a12F1, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xF1 } };

// Xbox d3d12_x returns CPU/GPU descriptor handles directly in RAX (scalar),
// whereas PC MSVC ID3D12DescriptorHeap uses a hidden sret pointer in RDX.
struct XboxXMemAllocationHooks {
    void*  (WINAPI* pAlloc)(SIZE_T dwSize, uint64_t dwAttributes);
    void   (WINAPI* pFree)(void* lpAddress, uint64_t dwAttributes);
    SIZE_T (WINAPI* pSize)(void* lpAddress, uint64_t dwAttributes);
};

constexpr uint64_t kXboxD3DDescriptorHeapAllocAttrs = (0x80ULL << 16) | 4ULL;

static const XboxXMemAllocationHooks* GetTitleXMemAllocationHooks() {
    static const XboxXMemAllocationHooks* s_hooks = []() -> const XboxXMemAllocationHooks* {
        HMODULE exeMod = GetModuleHandleW(nullptr);
        if (!exeMod) return nullptr;
        auto* hooks = reinterpret_cast<const XboxXMemAllocationHooks*>(
            GetProcAddress(exeMod, "__XMemAllocationHooks__"));
        if (hooks && hooks->pAlloc) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> resolved __XMemAllocationHooks__ pAlloc=%p pFree=%p",
                          reinterpret_cast<void*>(hooks->pAlloc),
                          reinterpret_cast<void*>(hooks->pFree));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XMemHooks", msg);
            return hooks;
        }
        return nullptr;
    }();
    return s_hooks;
}

struct GuestDescriptorHeapRange {
    SIZE_T guestStart = 0;
    SIZE_T guestEnd = 0;
    SIZE_T nativeStart = 0;
    SIZE_T nativeSize = 0;
    UINT   increment = 0;
};

static std::mutex g_descHeapRangeMtx;
static std::vector<GuestDescriptorHeapRange> g_descHeapRanges;

static void RegisterGuestDescriptorHeapRange(const GuestDescriptorHeapRange& r) {
    if (r.guestStart == 0 || r.guestEnd <= r.guestStart || r.nativeSize == 0) return;
    std::lock_guard<std::mutex> lock(g_descHeapRangeMtx);
    g_descHeapRanges.push_back(r);
}

static void UnregisterGuestDescriptorHeapRange(SIZE_T guestStart) {
    if (guestStart == 0) return;
    std::lock_guard<std::mutex> lock(g_descHeapRangeMtx);
    for (size_t i = 0; i < g_descHeapRanges.size(); ++i) {
        if (g_descHeapRanges[i].guestStart == guestStart) {
            g_descHeapRanges[i] = g_descHeapRanges.back();
            g_descHeapRanges.pop_back();
            return;
        }
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE TranslateCpuDescriptorHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (h.ptr == 0) return h;
    std::lock_guard<std::mutex> lock(g_descHeapRangeMtx);
    for (const auto& r : g_descHeapRanges) {
        if (h.ptr >= r.guestStart && h.ptr < r.guestEnd) {
            SIZE_T off = h.ptr - r.guestStart;
            if (off >= r.nativeSize) {
                static std::atomic<uint32_t> s_oob{0};
                if (s_oob.fetch_add(1, std::memory_order_relaxed) < 8) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg),
                                  "-> clamped OOB descriptor offset 0x%llX >= nativeSize 0x%llX",
                                  static_cast<unsigned long long>(off),
                                  static_cast<unsigned long long>(r.nativeSize));
                    sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "TranslateCpuDescriptorHandle", msg);
                }
                off = (r.increment > 0 && r.nativeSize >= r.increment) ? (r.nativeSize - r.increment) : 0;
            }
            return D3D12_CPU_DESCRIPTOR_HANDLE{ r.nativeStart + off };
        }
    }
    return h;
}

class XboxD3D12DescriptorHeap final : public ID3D12Pageable {
public:
    XboxD3D12DescriptorHeap(
        ComPtr<ID3D12DescriptorHeap> nativeHeap,
        ID3D12Device* parentDevice,
        bool shaderVisible,
        const D3D12_DESCRIPTOR_HEAP_DESC& origDesc,
        SIZE_T guestCpuStart,
        SIZE_T guestByteSize,
        UINT incrementSize)
        : m_heap(std::move(nativeHeap)),
          m_parent(parentDevice),
          m_shaderVisible(shaderVisible),
          m_origDesc(origDesc),
          m_guestCpuStart(guestCpuStart) {
        if (m_parent) m_parent->AddRef();
        if (m_guestCpuStart != 0 && m_heap) {
            const D3D12_DESCRIPTOR_HEAP_DESC nativeDesc = m_heap->GetDesc();
            GuestDescriptorHeapRange range{};
            range.guestStart = m_guestCpuStart;
            range.guestEnd = m_guestCpuStart + guestByteSize;
            range.nativeStart = m_heap->GetCPUDescriptorHandleForHeapStart().ptr;
            range.nativeSize = static_cast<SIZE_T>(nativeDesc.NumDescriptors) * static_cast<SIZE_T>(incrementSize);
            range.increment = incrementSize;
            RegisterGuestDescriptorHeapRange(range);
        }
    }

    ~XboxD3D12DescriptorHeap() {
        if (m_guestCpuStart != 0) {
            UnregisterGuestDescriptorHeapRange(m_guestCpuStart);
            m_guestCpuStart = 0;
        }
        if (m_parent) m_parent->Release();
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DescriptorHeap))) {
            *ppvObject = static_cast<ID3D12Pageable*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, IID_ScarlettNativeDescriptorHeap)) {
            return m_heap ? m_heap->QueryInterface(__uuidof(ID3D12DescriptorHeap), ppvObject) : E_NOINTERFACE;
        }
        return m_heap ? m_heap->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_heap->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_heap->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_heap->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_heap->SetName(Name);
    }

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_parent ? m_parent->QueryInterface(riid, ppvDevice) : m_heap->GetDevice(riid, ppvDevice);
    }

    // ID3D12DescriptorHeap (slots 8..10) with Xbox scalar RAX ABI on slots 9 & 10
    virtual D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() {
        return m_origDesc;
    }
    virtual SIZE_T STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() {
        if (m_guestCpuStart != 0) {
            return m_guestCpuStart;
        }
        return m_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    }
    virtual UINT64 STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() {
        if (!m_shaderVisible) {
            return 0;
        }
        return m_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    }

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<ID3D12DescriptorHeap> m_heap;
    ID3D12Device* m_parent = nullptr;
    bool m_shaderVisible = false;
    D3D12_DESCRIPTOR_HEAP_DESC m_origDesc{};
    SIZE_T m_guestCpuStart = 0;
};

// Xbox ID3D12XboxDmaCommandList IID ({30BC77B9-8379-4B25-95DE-B8588C42275D})
constexpr GUID IID_ID3D12XboxDmaCommandList =
    { 0x30BC77B9, 0x8379, 0x4B25, { 0x95, 0xDE, 0xB8, 0x58, 0x8C, 0x42, 0x27, 0x5D } };

// Private IID to unwrap Xbox command lists -> native ID3D12CommandList
constexpr GUID IID_ScarlettNativeCommandList =
    { 0x5c4a12F2, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xF2 } };

struct D3D12XBOX_COMMAND_LIST_DESC {
    D3D12_COMMAND_LIST_TYPE Type;
    UINT Flags;
    UINT NodeMask;
    UINT Reserved;
};

#define SCARLETT_XBOX_SUBOBJ_EXT_STUB(CLS, SLOT) \
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot##SLOT(uint64_t a1, uint64_t a2, uint64_t a3) { \
        char msg[256]; \
        std::snprintf(msg, sizeof(msg), \
                      "UNIMPLEMENTED " #CLS " slot=%d (+0x%03X) a1=0x%llX a2=0x%llX a3=0x%llX ret=%s", \
                      SLOT, (SLOT) * 8, \
                      static_cast<unsigned long long>(a1), \
                      static_cast<unsigned long long>(a2), \
                      static_cast<unsigned long long>(a3), \
                      FormatCallerAddr(_ReturnAddress()).c_str()); \
        sc::log::Write(sc::log::Level::Warn, "d3d12_xs", #CLS "::Ext", msg); \
        return S_OK; \
    }

struct XboxRenderStats {
    std::atomic<uint64_t> psoGfxOk{0};
    std::atomic<uint64_t> psoGfxFail{0};
    std::atomic<uint64_t> psoCmpOk{0};
    std::atomic<uint64_t> psoCmpFail{0};
    std::atomic<uint64_t> rsOk{0};
    std::atomic<uint64_t> rsFail{0};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> drawIndexed{0};
    std::atomic<uint64_t> dispatches{0};
    std::atomic<uint64_t> clears{0};
    std::atomic<uint64_t> setPsoValid{0};
    std::atomic<uint64_t> setPsoNull{0};
    std::atomic<uint64_t> omSetRt{0};
    std::atomic<uint64_t> shadowAllocs{0};
    std::atomic<uint64_t> shadowUploads{0};
    std::atomic<uint64_t> cbvBounced{0};
    std::atomic<uint64_t> cbvBounceMiss{0};
};
XboxRenderStats g_xboxStats;

struct ScopedWinHandle {
    HANDLE h = nullptr;
    ~ScopedWinHandle() { if (h) CloseHandle(h); }
};

ID3D12Resource* UnwrapXboxResourcePtr(ID3D12Resource* res);
void FlushDirtyXboxShadowTextures(
    ID3D12Device* nativeDevice,
    ID3D12CommandQueue* nativeQueue,
    ComPtr<ID3D12CommandAllocator>& uploadAlloc,
    ComPtr<ID3D12GraphicsCommandList>& uploadCl,
    ComPtr<ID3D12Fence>& uploadFence,
    ScopedWinHandle& uploadFenceEvent,
    uint64_t& uploadFenceVal,
    ComPtr<ID3D12Resource>& stagingBuf,
    uint64_t& stagingBufSize);

// Private IID used by XboxD3D12Fence::FromInterface() / Unwrap()
static constexpr GUID IID_ScarlettXboxFenceSelf = {
    0x9b4f81a2, 0x3c17, 0x4e6d, { 0x8a, 0x55, 0x19, 0x7c, 0x42, 0xd0, 0x6e, 0x31 }
};

class XboxD3D12Fence;

class FenceSyncManager {
public:
    static FenceSyncManager& Instance() {
        static auto* s_inst = new FenceSyncManager();
        return *s_inst;
    }

    void Register(XboxD3D12Fence* fence);
    void Unregister(XboxD3D12Fence* fence);
    void NotifyGpuSignal();

private:
    void WorkerLoop();

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::vector<XboxD3D12Fence*> m_fences;
    bool m_started = false;
};

class XboxD3D12Fence final : public ID3D12Fence1 {
public:
    XboxD3D12Fence(ComPtr<ID3D12Fence> fence, UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Device* parentDevice)
        : m_fence(std::move(fence)), m_flags(flags), m_parentDevice(parentDevice),
          m_completedShadow(initialValue), m_pendingTarget(initialValue) {
        m_fence.As(&m_fence1);
        FenceSyncManager::Instance().Register(this);
    }

    ~XboxD3D12Fence() {
        FenceSyncManager::Instance().Unregister(this);
    }

    static XboxD3D12Fence* TryFromInterface(ID3D12Fence* pFence) {
        if (!pFence) return nullptr;
        void* raw = nullptr;
        if (SUCCEEDED(pFence->QueryInterface(IID_ScarlettXboxFenceSelf, &raw)) && raw) {
            return static_cast<XboxD3D12Fence*>(raw); // caller holds 1 ref
        }
        return nullptr;
    }

    static ID3D12Fence* Unwrap(ID3D12Fence* pFence) {
        if (!pFence) return nullptr;
        if (auto* self = TryFromInterface(pFence)) {
            ID3D12Fence* native = self->m_fence.Get();
            self->Release();
            return native;
        }
        return pFence;
    }

    static void NotifyGpuSignal(ID3D12Fence* pFence, UINT64 value) {
        if (auto* self = TryFromInterface(pFence)) {
            self->TrackGpuSignal(value);
            self->Release();
        }
    }

    void TrackGpuSignal(UINT64 value) {
        uint64_t cur = m_pendingTarget.load(std::memory_order_relaxed);
        while (value > cur && !m_pendingTarget.compare_exchange_weak(cur, value, std::memory_order_release)) {}
        SyncShadowFromNative();
        FenceSyncManager::Instance().NotifyGpuSignal();
    }

    bool SyncShadowFromNative() {
        if (!m_fence) return false;
        const UINT64 v = m_fence->GetCompletedValue();
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_completedShadow), static_cast<LONG64>(v));
        const uint64_t target = m_pendingTarget.load(std::memory_order_acquire);
        return (v != UINT64_MAX && v < target);
    }

    bool HasPendingGpuSignal() const {
        const uint64_t cur = static_cast<uint64_t>(
            InterlockedCompareExchange64(const_cast<volatile LONG64*>(reinterpret_cast<const volatile LONG64*>(&m_completedShadow)), 0, 0));
        const uint64_t target = m_pendingTarget.load(std::memory_order_acquire);
        return (cur != UINT64_MAX && cur < target);
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_ScarlettXboxFenceSelf)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = static_cast<ID3D12Fence1*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_fence->GetPrivateData(guid, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_fence->SetPrivateData(guid, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_fence->SetPrivateDataInterface(guid, pData);
    }

    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_fence->SetName(Name);
    }

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (m_parentDevice) return m_parentDevice->QueryInterface(riid, ppvDevice);
        return m_fence->GetDevice(riid, ppvDevice);
    }

    // ID3D12Fence (slots 8..10)
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        const UINT64 v = m_fence->GetCompletedValue();
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_completedShadow), static_cast<LONG64>(v));
        return v;
    }

    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value, HANDLE hEvent) override {
        return m_fence->SetEventOnCompletion(Value, hEvent);
    }

    HRESULT STDMETHODCALLTYPE Signal(UINT64 Value) override {
        HRESULT hr = m_fence->Signal(Value);
        if (SUCCEEDED(hr)) {
            SyncShadowFromNative();
        }
        return hr;
    }

    // ID3D12Fence1 (slot 11 / +0x58)
    D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override {
        return m_fence1 ? m_fence1->GetCreationFlags() : m_flags;
    }

    // Xbox Scarlett Extended Slot 12 (+0x60): GetCompletedValueAddress()
    virtual const volatile UINT64* STDMETHODCALLTYPE GetCompletedValueAddress() {
        SyncShadowFromNative();
        static std::atomic<uint32_t> s_addrLogs{0};
        if (s_addrLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "-> slot12 fence=%p addr=%p curVal=%llu",
                          static_cast<void*>(this),
                          static_cast<const void*>(const_cast<const UINT64*>(&m_completedShadow)),
                          static_cast<unsigned long long>(m_completedShadow));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Fence::GetCompletedValueAddress", msg);
        }
        return &m_completedShadow;
    }

    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12Fence, 13)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12Fence, 14)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12Fence, 15)

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D12Fence1> m_fence1;
    D3D12_FENCE_FLAGS m_flags = D3D12_FENCE_FLAG_NONE;
    ID3D12Device* m_parentDevice = nullptr;
    alignas(8) volatile UINT64 m_completedShadow = 0;
    std::atomic<uint64_t> m_pendingTarget{0};
};

inline void FenceSyncManager::Register(XboxD3D12Fence* fence) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_fences.push_back(fence);
}

inline void FenceSyncManager::Unregister(XboxD3D12Fence* fence) {
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = std::find(m_fences.begin(), m_fences.end(), fence);
    if (it != m_fences.end()) m_fences.erase(it);
}

inline void FenceSyncManager::NotifyGpuSignal() {
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_started) {
            m_started = true;
            std::thread(&FenceSyncManager::WorkerLoop, this).detach();
        }
    }
    m_cv.notify_one();
}

inline void FenceSyncManager::WorkerLoop() {
    for (;;) {
        bool anyPending = false;
        {
            std::unique_lock<std::mutex> lock(m_mtx);
            for (XboxD3D12Fence* f : m_fences) {
                if (f->SyncShadowFromNative()) {
                    anyPending = true;
                }
            }
            if (!anyPending) {
                m_cv.wait(lock, [this]() {
                    for (XboxD3D12Fence* f : m_fences) {
                        if (f->HasPendingGpuSignal()) return true;
                    }
                    return false;
                });
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

class XboxD3D12CommandQueue final : public ID3D12Pageable {
public:
    XboxD3D12CommandQueue(
        ComPtr<IDXGIFactory6> factory,
        ComPtr<ID3D12Device> nativeDevice,
        ComPtr<ID3D12CommandQueue> nativeQueue,
        ID3D12Device* parentDevice,
        bool isGraphicsQueue = true)
        : m_factory(std::move(factory)),
          m_nativeDevice(std::move(nativeDevice)),
          m_queue(std::move(nativeQueue)),
          m_parent(parentDevice),
          m_isGraphicsQueue(isGraphicsQueue) {
        if (m_parent) m_parent->AddRef();
    }

    ~XboxD3D12CommandQueue() {
        if (m_parent) m_parent->Release();
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(riid, __uuidof(ID3D12CommandQueue))) {
            *ppvObject = static_cast<ID3D12Pageable*>(this);
            AddRef();
            return S_OK;
        }
        return m_queue ? m_queue->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_queue->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_queue->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_queue->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_queue->SetName(Name);
    }

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_parent ? m_parent->QueryInterface(riid, ppvDevice) : m_queue->GetDevice(riid, ppvDevice);
    }

    // Scarlett ID3D12CommandQueue (slots 8..20; +2 shift starting at slot 14)
    virtual void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource* pResource, UINT NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap,
        UINT NumRanges, const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
        const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS Flags) {
        m_queue->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates,
                                    pResourceRegionSizes, pHeap, NumRanges, pRangeFlags,
                                    pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    }
    virtual void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) {
        m_queue->CopyTileMappings(pDstResource, pDstRegionStartCoordinate,
                                  pSrcResource, pSrcRegionStartCoordinate, pRegionSize, Flags);
    }
    virtual void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
        if (!ppCommandLists || NumCommandLists == 0) {
            m_queue->ExecuteCommandLists(NumCommandLists, ppCommandLists);
            return;
        }
        std::vector<ComPtr<ID3D12CommandList>> refs;
        std::vector<ID3D12CommandList*> unwrapped;
        refs.reserve(NumCommandLists);
        unwrapped.reserve(NumCommandLists);
        for (UINT i = 0; i < NumCommandLists; ++i) {
            ID3D12CommandList* cl = ppCommandLists[i];
            if (cl) {
                ComPtr<ID3D12CommandList> nativeCl;
                if (SUCCEEDED(cl->QueryInterface(IID_ScarlettNativeCommandList, reinterpret_cast<void**>(nativeCl.ReleaseAndGetAddressOf()))) && nativeCl) {
                    unwrapped.push_back(nativeCl.Get());
                    refs.push_back(std::move(nativeCl));
                    continue;
                }
            }
            unwrapped.push_back(cl);
        }
        if (m_isGraphicsQueue && m_nativeDevice && m_queue && m_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            FlushDirtyXboxShadowTextures(
                m_nativeDevice.Get(), m_queue.Get(),
                m_uploadAlloc, m_uploadCl, m_uploadFence, m_uploadFenceEvent,
                m_uploadFenceVal, m_stagingBuf, m_stagingBufSize);
        }
        m_queue->ExecuteCommandLists(NumCommandLists, unwrapped.data());
    }
    virtual void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) {
        m_queue->SetMarker(Metadata, pData, Size);
    }
    virtual void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) {
        m_queue->BeginEvent(Metadata, pData, Size);
    }
    virtual void STDMETHODCALLTYPE EndEvent() {
        m_queue->EndEvent();
    }
    // Slots 14..15: Scarlett Xbox-only queue methods inserted before Signal
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 14)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 15)
    // Slot 16 (+0x080): Signal(pFence, Value)
    virtual HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) {
        HRESULT hr = m_queue->Signal(XboxD3D12Fence::Unwrap(pFence), Value);
        if (SUCCEEDED(hr)) {
            XboxD3D12Fence::NotifyGpuSignal(pFence, Value);
        }
        return hr;
    }
    // Slot 17 (+0x088): Wait(pFence, Value)
    virtual HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) {
        return m_queue->Wait(XboxD3D12Fence::Unwrap(pFence), Value);
    }
    // Slot 18 (+0x090): GetTimestampFrequency(UINT64* pFrequency)
    virtual HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) {
        return m_queue->GetTimestampFrequency(pFrequency);
    }
    // Slot 19 (+0x098): GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp)
    virtual HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) {
        return m_queue->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    }
    // Slot 20 (+0x0A0): GetDesc()
    virtual D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() {
        return m_queue->GetDesc();
    }

    // Xbox ID3D12CommandQueue extension slots (21..29)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 21)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 22)
    // Slot 23 (+0x0B8): SuspendX(Flags)
    virtual HRESULT STDMETHODCALLTYPE SuspendX(UINT /*Flags*/) { return S_OK; }
    // Slot 24 (+0x0C0): ResumeX()
    virtual HRESULT STDMETHODCALLTYPE ResumeX() { return S_OK; }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 25)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 26)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 27)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 28)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 29)
    // Slot 30 (+0x0F0): Xbox PresentX(PlaneCount, pPlaneParameters, pPresentParameters)
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        if (PlaneCount == 0 && !pPlaneParameters) {
            return S_OK;
        }
        if (PlaneCount == 0 || !pPlaneParameters) {
            return E_INVALIDARG;
        }
        if (pPlaneParameters[0].ResourceCount == 0) {
            return S_OK;
        }
        if (!m_presenter && m_factory && m_nativeDevice && m_queue) {
            m_presenter = sc::d3d12xs::CreateScarlettCommandQueueX(m_factory, m_nativeDevice, m_queue);
        }
        if (!m_presenter) {
            return E_FAIL;
        }
        ID3D12Resource* rawRes = pPlaneParameters[0].ppResources ? pPlaneParameters[0].ppResources[0] : nullptr;
        ID3D12Resource* unwrappedRes = UnwrapXboxResourcePtr(rawRes);
        const uint64_t idx = m_presentCount.fetch_add(1, std::memory_order_relaxed);
        if (idx == 0 || idx == 9 || idx == 59 || idx == 119) {
            char st[320];
            std::snprintf(st, sizeof(st),
                          "frame=%llu | psoGfx=%llu/%llu psoCmp=%llu/%llu rs=%llu/%llu | omRT=%llu clr=%llu pso=%llu(null:%llu) drw=%llu idxDrw=%llu disp=%llu | sh=%llu/%llu cbvBnc=%llu(miss:%llu)",
                          static_cast<unsigned long long>(idx + 1),
                          static_cast<unsigned long long>(g_xboxStats.psoGfxOk.load()),
                          static_cast<unsigned long long>(g_xboxStats.psoGfxFail.load()),
                          static_cast<unsigned long long>(g_xboxStats.psoCmpOk.load()),
                          static_cast<unsigned long long>(g_xboxStats.psoCmpFail.load()),
                          static_cast<unsigned long long>(g_xboxStats.rsOk.load()),
                          static_cast<unsigned long long>(g_xboxStats.rsFail.load()),
                          static_cast<unsigned long long>(g_xboxStats.omSetRt.load()),
                          static_cast<unsigned long long>(g_xboxStats.clears.load()),
                          static_cast<unsigned long long>(g_xboxStats.setPsoValid.load()),
                          static_cast<unsigned long long>(g_xboxStats.setPsoNull.load()),
                          static_cast<unsigned long long>(g_xboxStats.draws.load()),
                          static_cast<unsigned long long>(g_xboxStats.drawIndexed.load()),
                          static_cast<unsigned long long>(g_xboxStats.dispatches.load()),
                          static_cast<unsigned long long>(g_xboxStats.shadowAllocs.load()),
                          static_cast<unsigned long long>(g_xboxStats.shadowUploads.load()),
                          static_cast<unsigned long long>(g_xboxStats.cbvBounced.load()),
                          static_cast<unsigned long long>(g_xboxStats.cbvBounceMiss.load()));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxRenderStats", st);
        }
        if (idx == 0) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "-> first PresentX: planes=%u token=%llu resCount=%u ppRes=%p raw=%p unwrapped=%p",
                          PlaneCount,
                          static_cast<unsigned long long>(pPlaneParameters[0].Token),
                          pPlaneParameters[0].ResourceCount,
                          static_cast<const void*>(pPlaneParameters[0].ppResources),
                          static_cast<const void*>(rawRes),
                          static_cast<const void*>(unwrappedRes));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12CommandQueue::PresentX", msg);
        }
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane = pPlaneParameters[0];
        plane.ResourceCount = unwrappedRes ? 1u : 0u;
        plane.ppResources = unwrappedRes ? &unwrappedRes : nullptr;
        HRESULT hr = m_presenter->PresentX(1, &plane, pPresentParameters);
        if (hr == S_FALSE) {
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12CommandQueue::PresentX",
                           "-> host window closed; terminating title cleanly");
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }
        if (FAILED(hr) && idx < 5) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "-> PresentX failed hr=0x%08lX", static_cast<unsigned long>(hr));
            sc::log::Write(sc::log::Level::Error, "d3d12_xs", "XboxD3D12CommandQueue::PresentX", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 31)
    // Slot 32 (+0x100): Scarlett PresentX(PlaneCount, pPlaneParameters, pPresentParameters)
    virtual HRESULT STDMETHODCALLTYPE PresentXScarlett(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        return PresentX(PlaneCount, pPlaneParameters, pPresentParameters);
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 33)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 34)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 35)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 36)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 37)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 38)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 39)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12CommandQueue, 40)

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_nativeDevice;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandQueueX> m_presenter;
    ID3D12Device* m_parent = nullptr;
    bool m_isGraphicsQueue{true};
    std::atomic<uint64_t> m_presentCount{0};
    ComPtr<ID3D12CommandAllocator> m_uploadAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_uploadCl;
    ComPtr<ID3D12Fence> m_uploadFence;
    ScopedWinHandle m_uploadFenceEvent;
    uint64_t m_uploadFenceVal{0};
    ComPtr<ID3D12Resource> m_stagingBuf;
    uint64_t m_stagingBufSize{0};
};

// Wraps non-buffer (texture) ID3D12Resource objects so GetGPUVirtualAddress() returns a valid
// CPU-writable shadow address (matching Xbox unified memory expectations in Unity AsyncUploadManager).
struct GcnFormatMetrics {
    uint32_t blockW;
    uint32_t blockH;
    uint32_t bpe;
};

static GcnFormatMetrics GetGcnFormatMetrics(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return { 4, 4, 8 };
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return { 4, 4, 16 };
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return { 1, 1, 1 };
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return { 1, 1, 2 };
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            return { 1, 1, 8 };
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return { 1, 1, 16 };
        default:
            return { 1, 1, 4 };
    }
}

struct Gcn1DThinMipInfo {
    uint64_t offsetBytes = 0;
    uint64_t sliceBytes = 0;
    uint32_t ew = 1;
    uint32_t eh = 1;
    uint32_t pw = 8;
    uint32_t ph = 8;
    uint32_t bpe = 4;
};

static uint64_t ComputeGcn1DThinLayout(
    const D3D12_RESOURCE_DESC& d,
    std::vector<Gcn1DThinMipInfo>* outMips) {
    const GcnFormatMetrics fm = GetGcnFormatMetrics(d.Format);
    const uint32_t w0 = static_cast<uint32_t>(std::max<uint64_t>(d.Width, 1ULL));
    const uint32_t h0 = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) ? 1u : std::max<uint32_t>(d.Height, 1u);
    const bool is3D = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    const uint32_t d0 = std::max<uint32_t>(d.DepthOrArraySize, 1u);
    const uint32_t mips = std::max<uint32_t>(d.MipLevels, 1u);
    if (outMips) {
        outMips->clear();
        outMips->reserve(mips);
    }
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1u, w0 >> m);
        const uint32_t mh = std::max<uint32_t>(1u, h0 >> m);
        const uint32_t md = is3D ? std::max<uint32_t>(1u, d0 >> m) : d0;
        const uint32_t ew = (mw + fm.blockW - 1u) / fm.blockW;
        const uint32_t eh = (mh + fm.blockH - 1u) / fm.blockH;
        const uint32_t ph = (eh + 7u) & ~7u;
        uint32_t pw = (ew + 7u) & ~7u;
        while ((static_cast<uint64_t>(pw) * ph * fm.bpe) % 256ULL != 0ULL) {
            pw += 8u;
        }
        off = (off + 255ULL) & ~255ULL;
        const uint64_t sliceBytes = static_cast<uint64_t>(pw) * ph * fm.bpe;
        const uint64_t mipBytes = (sliceBytes * md + 255ULL) & ~255ULL;
        if (outMips) {
            outMips->push_back(Gcn1DThinMipInfo{ off, sliceBytes, ew, eh, pw, ph, fm.bpe });
        }
        off += mipBytes;
    }
    return (off + 255ULL) & ~255ULL;
}

static void DetileGcn1DThinSlice(
    const uint8_t* srcSlice,
    uint64_t srcAvailBytes,
    uint8_t* dstSlice,
    uint64_t dstRowPitch,
    uint64_t dstAvailBytes,
    const Gcn1DThinMipInfo& mi,
    uint64_t& nonZeroSample) {
    static constexpr uint8_t kMorton8x8[8][8] = {
        {  0,  1,  4,  5, 16, 17, 20, 21 },
        {  2,  3,  6,  7, 18, 19, 22, 23 },
        {  8,  9, 12, 13, 24, 25, 28, 29 },
        { 10, 11, 14, 15, 26, 27, 30, 31 },
        { 32, 33, 36, 37, 48, 49, 52, 53 },
        { 34, 35, 38, 39, 50, 51, 54, 55 },
        { 40, 41, 44, 45, 56, 57, 60, 61 },
        { 42, 43, 46, 47, 58, 59, 62, 63 },
    };
    const uint32_t tilesX = mi.pw >> 3;
    const uint32_t bpe = mi.bpe;
    const uint64_t tileBytes = 64ULL * bpe;

    for (uint32_t y = 0; y < mi.eh; ++y) {
        const uint64_t dstRowOff = static_cast<uint64_t>(y) * dstRowPitch;
        if (dstRowOff + static_cast<uint64_t>(mi.ew) * bpe > dstAvailBytes) break;
        uint8_t* dstRow = dstSlice + dstRowOff;
        const uint32_t ty = y >> 3;
        const uint32_t ly = y & 7u;
        const uint64_t tileRowBase = static_cast<uint64_t>(ty) * tilesX;
        const uint8_t* mRow = kMorton8x8[ly];

        for (uint32_t x = 0; x < mi.ew; ++x) {
            const uint32_t tx = x >> 3;
            const uint32_t lx = x & 7u;
            const uint64_t sOff = (tileRowBase + tx) * tileBytes + static_cast<uint64_t>(mRow[lx]) * bpe;
            if (sOff + bpe <= srcAvailBytes) {
                const uint8_t* sp = srcSlice + sOff;
                uint8_t* dp = dstRow + static_cast<uint64_t>(x) * bpe;
                if (bpe == 1u) {
                    *dp = *sp;
                    if (nonZeroSample == 0 && *sp != 0) nonZeroSample = 1;
                } else if (bpe == 4u) {
                    std::memcpy(dp, sp, 4);
                    if (nonZeroSample == 0 && (sp[0] | sp[1] | sp[2] | sp[3]) != 0) nonZeroSample = 1;
                } else {
                    std::memcpy(dp, sp, bpe);
                    if (nonZeroSample == 0) {
                        for (uint32_t k = 0; k < bpe; ++k) {
                            if (sp[k] != 0) { nonZeroSample = 1; break; }
                        }
                    }
                }
            }
        }
    }
}

class XboxD3D12Resource final : public ID3D12Resource {
public:
    XboxD3D12Resource(
        ComPtr<ID3D12Resource> nativeRes,
        ID3D12Device* parentDevice,
        uint64_t approxSizeBytes,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON)
        : m_res(std::move(nativeRes)),
          m_parent(parentDevice),
          m_approxSize(approxSizeBytes) {
        constexpr uint32_t kBufOnly = static_cast<uint32_t>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
                                      static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDEX_BUFFER);
        m_currentState.store(static_cast<uint32_t>(initialState) & ~kBufOnly, std::memory_order_relaxed);
        if (m_parent) m_parent->AddRef();
        s_vtbl.store(*reinterpret_cast<void* const*>(this), std::memory_order_release);
        InitCpuBufferTracking();
    }

    ~XboxD3D12Resource() {
        ShutdownCpuBufferTracking();
        UnregisterShadow(this);
        if (m_shadowMem) {
            VirtualFree(m_shadowMem, 0, MEM_RELEASE);
            m_shadowMem = nullptr;
        }
        if (m_parent) m_parent->Release();
    }

    static XboxD3D12Resource* FromInterface(ID3D12Resource* res) {
        if (!res) return nullptr;
        void* vtbl = s_vtbl.load(std::memory_order_acquire);
        if (vtbl && *reinterpret_cast<void* const*>(res) == vtbl) {
            return static_cast<XboxD3D12Resource*>(res);
        }
        return nullptr;
    }

    bool TryAddRef() {
        ULONG cur = m_refs.load(std::memory_order_relaxed);
        while (cur != 0) {
            if (m_refs.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    ID3D12Resource* Native() const { return m_res.Get(); }
    D3D12_RESOURCE_STATES GetTrackedState() const {
        return static_cast<D3D12_RESOURCE_STATES>(m_currentState.load(std::memory_order_acquire));
    }
    void SetTrackedState(D3D12_RESOURCE_STATES st) {
        m_currentState.store(static_cast<uint32_t>(st), std::memory_order_release);
    }

    static void RegisterShadow(XboxD3D12Resource* r) {
        std::lock_guard<std::mutex> lock(s_shadowRegMtx);
        s_shadowReg.push_back(r);
    }
    static void UnregisterShadow(XboxD3D12Resource* r) {
        std::lock_guard<std::mutex> lock(s_shadowRegMtx);
        auto it = std::find(s_shadowReg.begin(), s_shadowReg.end(), r);
        if (it != s_shadowReg.end()) {
            *it = s_shadowReg.back();
            s_shadowReg.pop_back();
        }
    }
    static void SnapshotShadowResources(std::vector<ComPtr<XboxD3D12Resource>>& out) {
        std::lock_guard<std::mutex> lock(s_shadowRegMtx);
        out.reserve(s_shadowReg.size());
        for (XboxD3D12Resource* r : s_shadowReg) {
            if (r && r->TryAddRef()) {
                ComPtr<XboxD3D12Resource> ref;
                ref.Attach(r);
                out.push_back(std::move(ref));
            }
        }
    }

    bool HasDirtyShadowAndReset(void*& outPtr, uint64_t& outBytes) {
        std::lock_guard<std::mutex> lock(m_shadowMtx);
        if (!m_shadowMem || m_shadowBytes == 0) return false;
        void* dirtyAddrs[1]{};
        ULONG_PTR count = 1;
        DWORD gran = 0;
        if (GetWriteWatch(0, m_shadowMem, static_cast<SIZE_T>(m_shadowBytes), dirtyAddrs, &count, &gran) == 0 && count > 0) {
            ResetWriteWatch(m_shadowMem, static_cast<SIZE_T>(m_shadowBytes));
            outPtr = m_shadowMem;
            outBytes = m_shadowBytes;
            return true;
        }
        return false;
    }

    static ID3D12Resource* Unwrap(ID3D12Resource* res) {
        if (!res) return nullptr;
        void* vtbl = s_vtbl.load(std::memory_order_acquire);
        if (vtbl && *reinterpret_cast<void* const*>(res) == vtbl) {
            return static_cast<XboxD3D12Resource*>(res)->m_res.Get();
        }
        return res;
    }

    static ID3D12Pageable* UnwrapPageable(ID3D12Pageable* p) {
        if (!p) return nullptr;
        void* vtbl = s_vtbl.load(std::memory_order_acquire);
        if (vtbl && *reinterpret_cast<void* const*>(p) == vtbl) {
            return static_cast<XboxD3D12Resource*>(p)->m_res.Get();
        }
        return p;
    }

    struct MappedUploadBuf {
        uint64_t gpuVa = 0;
        uint64_t size = 0;
        void* cpuPtr = nullptr;
        XboxD3D12Resource* owner = nullptr;
    };

    static bool CopyFromMappedUploadBuffer(
        D3D12_GPU_VIRTUAL_ADDRESS srcGpuVa,
        uint64_t copyBytes,
        void* dstPtr,
        uint64_t totalPadBytes,
        bool& outNonZero,
        float outFloats[4]) {
        outNonZero = false;
        for (int i = 0; i < 4; ++i) outFloats[i] = 0.0f;
        if (!dstPtr || srcGpuVa == 0 || totalPadBytes == 0) return false;
        std::lock_guard<std::mutex> lock(s_uploadBufMtx);
        for (const auto& e : s_uploadBufs) {
            if (e.cpuPtr && srcGpuVa >= e.gpuVa && srcGpuVa < e.gpuVa + e.size) {
                const uint64_t off = srcGpuVa - e.gpuVa;
                const uint64_t avail = e.size - off;
                const uint64_t n = std::min<uint64_t>(copyBytes, std::min<uint64_t>(avail, totalPadBytes));
                const uint8_t* sp = static_cast<const uint8_t*>(e.cpuPtr) + off;
                uint8_t* dp = static_cast<uint8_t*>(dstPtr);
                if (n > 0) {
                    std::memcpy(dp, sp, static_cast<size_t>(n));
                    for (size_t k = 0; k < static_cast<size_t>(n); ++k) {
                        if (dp[k] != 0) { outNonZero = true; break; }
                    }
                    if (n >= 16) {
                        std::memcpy(outFloats, dp, 16);
                    }
                }
                if (n < totalPadBytes) {
                    std::memset(dp + n, 0, static_cast<size_t>(totalPadBytes - n));
                }
                return true;
            }
        }
        return false;
    }

    static ID3D12DeviceChild* UnwrapDeviceChild(ID3D12DeviceChild* c) {
        if (!c) return nullptr;
        void* vtbl = s_vtbl.load(std::memory_order_acquire);
        if (vtbl && *reinterpret_cast<void* const*>(c) == vtbl) {
            return static_cast<XboxD3D12Resource*>(c)->m_res.Get();
        }
        return c;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Resource))) {
            *ppvObject = static_cast<ID3D12Resource*>(this);
            AddRef();
            return S_OK;
        }
        return m_res ? m_res->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_res->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_res->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_res->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_res->SetName(Name);
    }

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_parent ? m_parent->QueryInterface(riid, ppvDevice) : m_res->GetDevice(riid, ppvDevice);
    }

    // ID3D12Resource
    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, const D3D12_RANGE* pReadRange, void** ppData) override {
        D3D12_HEAP_PROPERTIES hp{};
        if (SUCCEEDED(m_res->GetHeapProperties(&hp, nullptr)) &&
            hp.Type == D3D12_HEAP_TYPE_DEFAULT) {
            if (ppData) {
                D3D12_RESOURCE_DESC d = m_res->GetDesc();
                void* ptr = nullptr;
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                    ptr = reinterpret_cast<void*>(GetGPUVirtualAddress());
                } else {
                    std::lock_guard<std::mutex> lock(m_shadowMtx);
                    if (!m_shadowMem) {
                        const uint64_t bytes = (std::max<uint64_t>(d.Width, 65536ULL) + 65535ULL) & ~65535ULL;
                        m_shadowMem = VirtualAlloc(nullptr, static_cast<size_t>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                        if (m_shadowMem) m_shadowBytes = bytes;
                    }
                    ptr = m_shadowMem;
                }
                if (ptr) {
                    *ppData = ptr;
                    m_mappedViaShadow.store(true, std::memory_order_relaxed);
                    return S_OK;
                }
            }
        }
        return m_res->Map(Subresource, pReadRange, ppData);
    }
    void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE* pWrittenRange) override {
        if (m_mappedViaShadow.load(std::memory_order_relaxed)) {
            return;
        }
        m_res->Unmap(Subresource, pWrittenRange);
    }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        return m_res->GetDesc();
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        D3D12_RESOURCE_DESC d = m_res->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            return m_res->GetGPUVirtualAddress();
        }
        std::lock_guard<std::mutex> lock(m_shadowMtx);
        if (!m_shadowMem) {
            uint64_t bytes = m_approxSize;
            ComPtr<ID3D12Device> nativeDev;
            if (SUCCEEDED(m_res->GetDevice(IID_PPV_ARGS(&nativeDev))) && nativeDev) {
                const UINT mips = std::max<UINT>(1u, d.MipLevels);
                const UINT slices = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 1u : std::max<UINT>(1u, d.DepthOrArraySize);
                UINT64 copyBytes = 0;
                nativeDev->GetCopyableFootprints(&d, 0, mips * slices, 0, nullptr, nullptr, nullptr, &copyBytes);
                if (copyBytes != UINT64_MAX && copyBytes > bytes) {
                    bytes = copyBytes;
                }
            }
            const uint64_t gcnBytes = ComputeGcn1DThinLayout(d, nullptr);
            if (gcnBytes > bytes) {
                bytes = gcnBytes;
            }
            if (bytes < 65536ULL) {
                bytes = std::max<uint64_t>(65536ULL, d.Width * std::max<uint64_t>(d.Height, 1ULL) * 16ULL);
            }
            bytes = (bytes + 65535ULL) & ~65535ULL;
            m_shadowMem = VirtualAlloc(nullptr, static_cast<size_t>(bytes), MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
            if (m_shadowMem) {
                m_shadowBytes = bytes;
                ResetWriteWatch(m_shadowMem, static_cast<SIZE_T>(m_shadowBytes));
                RegisterShadow(this);
                g_xboxStats.shadowAllocs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(m_shadowMem);
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(
        UINT DstSubresource, const D3D12_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override {
        return m_res->WriteToSubresource(DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(
        void* pDstData, UINT DstRowPitch, UINT DstDepthPitch, UINT SrcSubresource, const D3D12_BOX* pSrcBox) override {
        return m_res->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, SrcSubresource, pSrcBox);
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS* pHeapFlags) override {
        return m_res->GetHeapProperties(pHeapProperties, pHeapFlags);
    }

private:
    inline static std::atomic<void*> s_vtbl{nullptr};
    std::atomic<ULONG> m_refs{1};
    ComPtr<ID3D12Resource> m_res;
    ID3D12Device* m_parent = nullptr;
    uint64_t m_approxSize = 0;
    std::atomic<bool> m_mappedViaShadow{false};
    void InitCpuBufferTracking() {
        if (!m_res) return;
        D3D12_RESOURCE_DESC d = m_res->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width == 0) return;
        D3D12_HEAP_PROPERTIES hp{};
        if (FAILED(m_res->GetHeapProperties(&hp, nullptr))) return;
        const bool cpuAccess = (hp.Type == D3D12_HEAP_TYPE_UPLOAD) ||
                               (hp.Type == D3D12_HEAP_TYPE_CUSTOM &&
                                hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
                                hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
        if (!cpuAccess) return;
        D3D12_RANGE emptyRead{0, 0};
        void* ptr = nullptr;
        if (SUCCEEDED(m_res->Map(0, &emptyRead, &ptr)) && ptr) {
            m_mappedCpu = ptr;
            m_bufGpuVa = m_res->GetGPUVirtualAddress();
            m_bufSize = d.Width;
            std::lock_guard<std::mutex> lock(s_uploadBufMtx);
            s_uploadBufs.push_back(MappedUploadBuf{m_bufGpuVa, m_bufSize, m_mappedCpu, this});
        }
    }

    void ShutdownCpuBufferTracking() {
        if (!m_mappedCpu) return;
        {
            std::lock_guard<std::mutex> lock(s_uploadBufMtx);
            for (size_t i = 0; i < s_uploadBufs.size(); ++i) {
                if (s_uploadBufs[i].owner == this) {
                    s_uploadBufs[i] = s_uploadBufs.back();
                    s_uploadBufs.pop_back();
                    break;
                }
            }
        }
        if (m_res) {
            m_res->Unmap(0, nullptr);
        }
        m_mappedCpu = nullptr;
    }

    std::atomic<uint32_t> m_currentState{0};
    void* m_mappedCpu = nullptr;
    uint64_t m_bufGpuVa = 0;
    uint64_t m_bufSize = 0;
    std::mutex m_shadowMtx;
    void* m_shadowMem = nullptr;
    uint64_t m_shadowBytes = 0;
    inline static std::mutex s_shadowRegMtx;
    inline static std::vector<XboxD3D12Resource*> s_shadowReg;
    inline static std::mutex s_uploadBufMtx;
    inline static std::vector<MappedUploadBuf> s_uploadBufs;
};

static D3D12_TEXTURE_COPY_LOCATION UnwrapCopyLocation(const D3D12_TEXTURE_COPY_LOCATION* loc) {
    D3D12_TEXTURE_COPY_LOCATION out{};
    if (loc) {
        out = *loc;
        out.pResource = XboxD3D12Resource::Unwrap(out.pResource);
    }
    return out;
}

void ForwardSanitizedResourceBarriers(
    ID3D12GraphicsCommandList* cl, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (!cl || !pBarriers || NumBarriers == 0) {
        if (cl) cl->ResourceBarrier(NumBarriers, pBarriers);
        return;
    }
    constexpr uint32_t kBufferOnlyReadMask =
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDEX_BUFFER);

    std::vector<D3D12_RESOURCE_BARRIER> filtered;
    filtered.reserve(NumBarriers);
    for (UINT i = 0; i < NumBarriers; ++i) {
        D3D12_RESOURCE_BARRIER b = pBarriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            XboxD3D12Resource* xbRes = XboxD3D12Resource::FromInterface(b.Transition.pResource);
            b.Transition.pResource = xbRes ? xbRes->Native() : b.Transition.pResource;
            const D3D12_RESOURCE_DESC desc = b.Transition.pResource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(
                    static_cast<uint32_t>(b.Transition.StateBefore) & ~kBufferOnlyReadMask);
                b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(
                    static_cast<uint32_t>(b.Transition.StateAfter) & ~kBufferOnlyReadMask);
                if (xbRes && (b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || b.Transition.Subresource == 0)) {
                    xbRes->SetTrackedState(b.Transition.StateAfter);
                }
                if (b.Transition.StateBefore == b.Transition.StateAfter) {
                    continue;
                }
            }
        }
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            b.Aliasing.pResourceBefore = XboxD3D12Resource::Unwrap(b.Aliasing.pResourceBefore);
            b.Aliasing.pResourceAfter = XboxD3D12Resource::Unwrap(b.Aliasing.pResourceAfter);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            b.UAV.pResource = XboxD3D12Resource::Unwrap(b.UAV.pResource);
        }
        filtered.push_back(b);
    }
    if (!filtered.empty()) {
        cl->ResourceBarrier(static_cast<UINT>(filtered.size()), filtered.data());
    }
}

ID3D12Resource* UnwrapXboxResourcePtr(ID3D12Resource* res) {
    return XboxD3D12Resource::Unwrap(res);
}

void FlushDirtyXboxShadowTextures(
    ID3D12Device* nativeDevice,
    ID3D12CommandQueue* nativeQueue,
    ComPtr<ID3D12CommandAllocator>& uploadAlloc,
    ComPtr<ID3D12GraphicsCommandList>& uploadCl,
    ComPtr<ID3D12Fence>& uploadFence,
    ScopedWinHandle& uploadFenceEvent,
    uint64_t& uploadFenceVal,
    ComPtr<ID3D12Resource>& stagingBuf,
    uint64_t& stagingBufSize) {
    static std::mutex s_uploadMtx;
    std::lock_guard<std::mutex> upLock(s_uploadMtx);

    std::vector<ComPtr<XboxD3D12Resource>> snap;
    XboxD3D12Resource::SnapshotShadowResources(snap);
    if (snap.empty()) return;

    struct DirtyItem {
        ComPtr<XboxD3D12Resource> res;
        void* shadowPtr = nullptr;
        uint64_t shadowBytes = 0;
        D3D12_RESOURCE_DESC desc{};
        UINT numSubs = 0;
        UINT64 totalCopyBytes = 0;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> numRows;
        std::vector<UINT64> rowSizes;
    };
    std::vector<DirtyItem> dirty;
    uint64_t batchBytes = 0;

    for (auto& r : snap) {
        void* ptr = nullptr;
        uint64_t bytes = 0;
        if (!r->HasDirtyShadowAndReset(ptr, bytes)) continue;
        ID3D12Resource* native = r->Native();
        if (!native) continue;
        D3D12_RESOURCE_DESC d = native->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || d.SampleDesc.Count > 1 ||
            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
            continue;
        }
        const UINT mips = std::max<UINT>(1u, d.MipLevels);
        const UINT slices = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 1u : std::max<UINT>(1u, d.DepthOrArraySize);
        const UINT numSubs = mips * slices;
        DirtyItem item;
        item.res = r;
        item.shadowPtr = ptr;
        item.shadowBytes = bytes;
        item.desc = d;
        item.numSubs = numSubs;
        item.footprints.resize(numSubs);
        item.numRows.resize(numSubs);
        item.rowSizes.resize(numSubs);
        nativeDevice->GetCopyableFootprints(&d, 0, numSubs, 0, item.footprints.data(), item.numRows.data(), item.rowSizes.data(), &item.totalCopyBytes);
        if (item.totalCopyBytes == 0 || item.totalCopyBytes == UINT64_MAX) continue;
        batchBytes = ((batchBytes + 511ULL) & ~511ULL) + item.totalCopyBytes;
        dirty.push_back(std::move(item));
    }
    if (dirty.empty()) return;

    if (!uploadAlloc) {
        if (FAILED(nativeDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAlloc)))) return;
    }
    if (!uploadCl) {
        if (FAILED(nativeDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc.Get(), nullptr, IID_PPV_ARGS(&uploadCl)))) return;
        uploadCl->Close();
    }
    if (!uploadFence) {
        if (FAILED(nativeDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence)))) return;
        uploadFenceEvent.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    const uint64_t needed = std::max<uint64_t>(8ULL * 1024ULL * 1024ULL, (batchBytes + 65535ULL) & ~65535ULL);
    if (!stagingBuf || stagingBufSize < needed) {
        stagingBuf.Reset();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = needed;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(nativeDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&stagingBuf)))) {
            return;
        }
        stagingBufSize = needed;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE emptyRead{0, 0};
    if (FAILED(stagingBuf->Map(0, &emptyRead, reinterpret_cast<void**>(&mapped))) || !mapped) return;

    if (FAILED(uploadAlloc->Reset()) || FAILED(uploadCl->Reset(uploadAlloc.Get(), nullptr))) {
        stagingBuf->Unmap(0, nullptr);
        return;
    }

    uint64_t curOff = 0;
    for (auto& item : dirty) {
        curOff = (curOff + 511ULL) & ~511ULL;
        const uint8_t* srcBase = static_cast<const uint8_t*>(item.shadowPtr);
        uint64_t nonZeroSample = 0;

        ID3D12Resource* dstRes = item.res->Native();
        const D3D12_RESOURCE_STATES prevState = item.res->GetTrackedState();
        if (prevState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER pre{};
            pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre.Transition.pResource = dstRes;
            pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pre.Transition.StateBefore = prevState;
            pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            uploadCl->ResourceBarrier(1, &pre);
        }

        std::vector<Gcn1DThinMipInfo> gcnMips;
        ComputeGcn1DThinLayout(item.desc, &gcnMips);
        const UINT mipCount = std::max<UINT>(1u, item.desc.MipLevels);

        for (UINT s = 0; s < item.numSubs; ++s) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = item.footprints[s];
            const uint64_t dstSubOff = fp.Offset;
            fp.Offset = curOff + dstSubOff;
            const UINT rows = item.numRows[s];
            const UINT64 rowPitch = fp.Footprint.RowPitch;
            const UINT depth = std::max<UINT>(1u, fp.Footprint.Depth);
            const UINT mipIdx = s % mipCount;
            const UINT arraySlice = s / mipCount;
            if (mipIdx < gcnMips.size()) {
                const Gcn1DThinMipInfo& mi = gcnMips[mipIdx];
                for (UINT z = 0; z < depth; ++z) {
                    const uint64_t srcSliceOff = mi.offsetBytes + static_cast<uint64_t>(arraySlice + z) * mi.sliceBytes;
                    const uint64_t dstSliceOff = fp.Offset + static_cast<uint64_t>(z) * rows * rowPitch;
                    if (srcSliceOff < item.shadowBytes && dstSliceOff < stagingBufSize) {
                        DetileGcn1DThinSlice(
                            srcBase + srcSliceOff,
                            item.shadowBytes - srcSliceOff,
                            mapped + dstSliceOff,
                            rowPitch,
                            stagingBufSize - dstSliceOff,
                            mi,
                            nonZeroSample);
                    }
                }
            }
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstRes;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = stagingBuf.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = fp;
            uploadCl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }

        if (prevState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER post{};
            post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post.Transition.pResource = dstRes;
            post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            post.Transition.StateAfter = prevState;
            uploadCl->ResourceBarrier(1, &post);
        }

        curOff += item.totalCopyBytes;
        const uint64_t upIdx = g_xboxStats.shadowUploads.fetch_add(1, std::memory_order_relaxed);
        if (upIdx < 10) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "-> uploaded #%llu: %llux%u fmt=%u subs=%u bytes=%llu prevState=0x%X nonZero=%llu",
                          static_cast<unsigned long long>(upIdx),
                          static_cast<unsigned long long>(item.desc.Width), item.desc.Height,
                          static_cast<unsigned>(item.desc.Format), item.numSubs,
                          static_cast<unsigned long long>(item.totalCopyBytes),
                          static_cast<unsigned>(prevState), static_cast<unsigned long long>(nonZeroSample));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "ShadowTexUpload", msg);
        }
    }

    stagingBuf->Unmap(0, nullptr);
    if (SUCCEEDED(uploadCl->Close())) {
        ID3D12CommandList* lists[] = { uploadCl.Get() };
        nativeQueue->ExecuteCommandLists(1, lists);
        ++uploadFenceVal;
        if (SUCCEEDED(nativeQueue->Signal(uploadFence.Get(), uploadFenceVal)) &&
            uploadFence->GetCompletedValue() < uploadFenceVal && uploadFenceEvent.h) {
            uploadFence->SetEventOnCompletion(uploadFenceVal, uploadFenceEvent.h);
            WaitForSingleObject(uploadFenceEvent.h, 5000);
        }
    }
}

// Private IID used by XboxD3D12CommandAllocator::Unwrap()
static constexpr GUID IID_ScarlettXboxCmdAllocUnwrap = {
    0x6a9d2c41, 0x7e53, 0x4b8f, { 0x9c, 0x11, 0x42, 0x8e, 0x5d, 0x3a, 0x91, 0x07 }
};

class XboxD3D12CommandAllocator final : public ID3D12CommandAllocator {
public:
    XboxD3D12CommandAllocator(ComPtr<ID3D12CommandAllocator> alloc, ID3D12Device* parentDevice)
        : m_alloc(std::move(alloc)), m_parentDevice(parentDevice) {}

    static ID3D12CommandAllocator* Unwrap(ID3D12CommandAllocator* pAlloc) {
        if (!pAlloc) return nullptr;
        void* raw = nullptr;
        if (SUCCEEDED(pAlloc->QueryInterface(IID_ScarlettXboxCmdAllocUnwrap, &raw)) && raw) {
            auto* native = static_cast<ID3D12CommandAllocator*>(raw);
            native->Release();
            return native;
        }
        return pAlloc;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_ScarlettXboxCmdAllocUnwrap)) {
            *ppv = m_alloc.Get();
            m_alloc->AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(riid, __uuidof(ID3D12CommandAllocator))) {
            *ppv = static_cast<ID3D12CommandAllocator*>(this);
            AddRef();
            return S_OK;
        }
        // Accept Xbox extended ID3D12CommandAllocator IIDs via this wrapper
        *ppv = static_cast<ID3D12CommandAllocator*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_alloc->GetPrivateData(guid, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_alloc->SetPrivateData(guid, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_alloc->SetPrivateDataInterface(guid, pData);
    }

    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_alloc->SetName(Name);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (m_parentDevice) {
            return m_parentDevice->QueryInterface(riid, ppvDevice);
        }
        return m_alloc->GetDevice(riid, ppvDevice);
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        return m_alloc->Reset();
    }

    // Slot 9 (+0x48): Xbox extended ID3D12CommandAllocator memory/OOM callback setter
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot9(void* callback, void* context) {
        m_oomCallback = callback;
        m_oomContext = context;
        static std::atomic<uint32_t> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> slot9 (+0x48) callback=%p ctx=%p (accepted)", callback, context);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12CommandAllocator::XboxExtSlot9", msg);
        }
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot10(uint64_t, uint64_t, uint64_t) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot11(uint64_t, uint64_t, uint64_t) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot12(uint64_t, uint64_t, uint64_t) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot13(uint64_t, uint64_t, uint64_t) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot14(uint64_t, uint64_t, uint64_t) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot15(uint64_t, uint64_t, uint64_t) { return S_OK; }

private:
    std::atomic<ULONG> m_refs{1};
    ComPtr<ID3D12CommandAllocator> m_alloc;
    ComPtr<ID3D12Device> m_parentDevice;
    void* m_oomCallback{nullptr};
    void* m_oomContext{nullptr};
};

class XboxD3D12DmaCommandList final : public ID3D12CommandList {
public:
    XboxD3D12DmaCommandList(ComPtr<ID3D12GraphicsCommandList> nativeCl, ID3D12Device* parentDevice)
        : m_cl(std::move(nativeCl)), m_parent(parentDevice) {
        if (m_parent) m_parent->AddRef();
    }

    ~XboxD3D12DmaCommandList() {
        if (m_parent) m_parent->Release();
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(riid, __uuidof(ID3D12CommandList)) ||
            IsEqualGUID(riid, IID_ID3D12XboxDmaCommandList)) {
            *ppvObject = static_cast<ID3D12CommandList*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, IID_ScarlettNativeCommandList)) {
            return m_cl ? m_cl->QueryInterface(__uuidof(ID3D12CommandList), ppvObject) : E_NOINTERFACE;
        }
        return m_cl ? m_cl->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_cl->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_cl->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_cl->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_cl->SetName(Name);
    }

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_parent ? m_parent->QueryInterface(riid, ppvDevice) : m_cl->GetDevice(riid, ppvDevice);
    }

    // ID3D12CommandList (slot 8)
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        return static_cast<D3D12_COMMAND_LIST_TYPE>(0x10);
    }

    // ID3D12XboxDmaCommandList (slots 9..35)
    // Slot 9 (+0x48): Close
    virtual HRESULT STDMETHODCALLTYPE Close() {
        return m_cl->Close();
    }
    // Slot 10 (+0x50): Reset(pAllocator, Flags)
    virtual HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, UINT /*Flags*/) {
        return m_cl->Reset(XboxD3D12CommandAllocator::Unwrap(pAllocator), nullptr);
    }
    // Slot 11 (+0x58): CopyBufferRegion (confirmed @ Terraria.exe+0x7C0D25)
    virtual void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) {
        m_cl->CopyBufferRegion(XboxD3D12Resource::Unwrap(pDstBuffer), DstOffset, XboxD3D12Resource::Unwrap(pSrcBuffer), SrcOffset, NumBytes);
    }
    // Slot 12 (+0x60): CopyTextureRegion
    virtual void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
        D3D12_TEXTURE_COPY_LOCATION dLoc = UnwrapCopyLocation(pDst); D3D12_TEXTURE_COPY_LOCATION sLoc = UnwrapCopyLocation(pSrc); m_cl->CopyTextureRegion(pDst ? &dLoc : nullptr, DstX, DstY, DstZ, pSrc ? &sLoc : nullptr, pSrcBox);
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 13)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 14)
    // Slot 15 (+0x78): ResourceBarrier (confirmed @ Terraria.exe+0x7C0CFE)
    virtual void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
        ForwardSanitizedResourceBarriers(m_cl.Get(), NumBarriers, pBarriers);
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 16)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 17)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 18)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 19)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 20)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 21)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 22)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 23)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 24)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 25)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 26)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 27)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 28)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 29)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 30)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 31)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 32)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 33)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 34)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12DmaCommandList, 35)

private:
    uint64_t m_xboxInlinePad[32]{};
    std::atomic<ULONG> m_refs{1};
    ComPtr<ID3D12GraphicsCommandList> m_cl;
    ID3D12Device* m_parent = nullptr;
};

class XboxD3D12GraphicsCommandList final : public ID3D12CommandList {
public:
    XboxD3D12GraphicsCommandList(ComPtr<ID3D12GraphicsCommandList> nativeCl, ID3D12Device* parentDevice)
        : m_cl(std::move(nativeCl)), m_parent(parentDevice) {
        if (m_parent) m_parent->AddRef();
    }

    ~XboxD3D12GraphicsCommandList() {
        if (m_parent) m_parent->Release();
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        static constexpr GUID kXboxGclIids[] = {
            { 0xc3827890, 0x4024, 0x474c, { 0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74 } }, // Scarlett ID3D12GraphicsCommandList6
            { 0x553103fb, 0x1fe7, 0x4557, { 0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7 } }, // ID3D12GraphicsCommandList1
            { 0x38c3e585, 0xff17, 0x412c, { 0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28 } }, // ID3D12GraphicsCommandList2
            { 0x6fda83a7, 0xb84c, 0x4e38, { 0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d } }, // ID3D12GraphicsCommandList3
            { 0x8754318e, 0xd3a9, 0x4541, { 0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74 } }, // ID3D12GraphicsCommandList4
            { 0x55050859, 0x4024, 0x474c, { 0x87, 0xf5, 0x64, 0x72, 0xea, 0xee, 0x44, 0xea } }, // ID3D12GraphicsCommandList5
            { 0xc3827890, 0xe548, 0x4cfa, { 0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80 } }, // ID3D12GraphicsCommandList6 (PC)
            { 0xdd171223, 0x8b61, 0x4769, { 0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1 } }, // ID3D12GraphicsCommandList7
            { 0xee936ef9, 0x599d, 0x4d28, { 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x50 } }, // ID3D12GraphicsCommandList8
            { 0x50c10c65, 0xd8d7, 0x4686, { 0x88, 0x3b, 0x79, 0x17, 0x1a, 0xa9, 0x16, 0x79 } }, // ID3D12GraphicsCommandList9
        };
        bool isGclIid = IsEqualGUID(riid, __uuidof(IUnknown)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12DeviceChild)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12CommandList)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12GraphicsCommandList));
        if (!isGclIid) {
            for (const auto& g : kXboxGclIids) {
                if (IsEqualGUID(riid, g)) { isGclIid = true; break; }
            }
        }
        if (isGclIid) {
            *ppvObject = static_cast<ID3D12CommandList*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, IID_ScarlettNativeCommandList)) {
            return m_cl ? m_cl->QueryInterface(__uuidof(ID3D12CommandList), ppvObject) : E_NOINTERFACE;
        }
        return m_cl ? m_cl->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) {
            static std::atomic<uint32_t> s_relZero{0};
            if (s_relZero.fetch_add(1, std::memory_order_relaxed) < 8) {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "-> Release() hit 0 on GfxCL=%p (pinned to avoid UAF)", this);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12GfxCL", msg);
            }
            m_refs.store(1, std::memory_order_relaxed);
            return 0;
        }
        return r;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_cl->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_cl->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_cl->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_cl->SetName(Name);
    }

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_parent ? m_parent->QueryInterface(riid, ppvDevice) : m_cl->GetDevice(riid, ppvDevice);
    }

    // ID3D12CommandList (slot 8)
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        return m_cl->GetType();
    }

    // Xbox ID3D12GraphicsCommandList slots 9..26 (match PC 1:1)
    virtual HRESULT STDMETHODCALLTYPE Close() { return m_cl->Close(); }                                                                // 9  (+0x048)
    virtual HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) {                  // 10 (+0x050)
        return m_cl->Reset(XboxD3D12CommandAllocator::Unwrap(pAllocator), pInitialState);
    }
    virtual void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) { m_cl->ClearState(pPipelineState); }               // 11 (+0x058)
    virtual void STDMETHODCALLTYPE DrawInstanced(                                                                                       // 12 (+0x060)
        UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
        g_xboxStats.draws.fetch_add(1, std::memory_order_relaxed);
        m_cl->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    virtual void STDMETHODCALLTYPE DrawIndexedInstanced(                                                                                // 13 (+0x068)
        UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
        const uint64_t dn = g_xboxStats.drawIndexed.fetch_add(1, std::memory_order_relaxed);
        if (dn < 8) {
            char db[160];
            std::snprintf(db, sizeof(db), "-> DrawIndexed #%llu idxCnt=%u instCnt=%u startIdx=%u baseVtx=%d startInst=%u",
                          static_cast<unsigned long long>(dn), IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12GfxCL", db);
        }
        m_cl->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
    virtual void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {                  // 14 (+0x070)
        g_xboxStats.dispatches.fetch_add(1, std::memory_order_relaxed);
        m_cl->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    virtual void STDMETHODCALLTYPE CopyBufferRegion(                                                                                    // 15 (+0x078)
        ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) {
        m_cl->CopyBufferRegion(XboxD3D12Resource::Unwrap(pDstBuffer), DstOffset, XboxD3D12Resource::Unwrap(pSrcBuffer), SrcOffset, NumBytes);
    }
    virtual void STDMETHODCALLTYPE CopyTextureRegion(                                                                                   // 16 (+0x080)
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
        D3D12_TEXTURE_COPY_LOCATION dLoc = UnwrapCopyLocation(pDst); D3D12_TEXTURE_COPY_LOCATION sLoc = UnwrapCopyLocation(pSrc); m_cl->CopyTextureRegion(pDst ? &dLoc : nullptr, DstX, DstY, DstZ, pSrc ? &sLoc : nullptr, pSrcBox);
    }
    virtual void STDMETHODCALLTYPE CopyResource(ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) {                          // 17 (+0x088)
        m_cl->CopyResource(XboxD3D12Resource::Unwrap(pDstResource), XboxD3D12Resource::Unwrap(pSrcResource));
    }
    virtual void STDMETHODCALLTYPE CopyTiles(                                                                                           // 18 (+0x090)
        ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer,
        UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) {
        m_cl->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
    }
    virtual void STDMETHODCALLTYPE ResolveSubresource(                                                                                  // 19 (+0x098)
        ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) {
        m_cl->ResolveSubresource(XboxD3D12Resource::Unwrap(pDstResource), DstSubresource, XboxD3D12Resource::Unwrap(pSrcResource), SrcSubresource, Format);
    }
    virtual void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) {                                 // 20 (+0x0A0)
        m_cl->IASetPrimitiveTopology(PrimitiveTopology);
    }
    virtual void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) {                               // 21 (+0x0A8)
        m_cl->RSSetViewports(NumViewports, pViewports);
    }
    virtual void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) {                                        // 22 (+0x0B0)
        m_cl->RSSetScissorRects(NumRects, pRects);
    }
    virtual void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) {                                                      // 23 (+0x0B8)
        m_cl->OMSetBlendFactor(BlendFactor);
    }
    virtual void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) {                                                                  // 24 (+0x0C0)
        m_cl->OMSetStencilRef(StencilRef);
    }
    virtual void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) {                                             // 25 (+0x0C8)
        if (pPipelineState) g_xboxStats.setPsoValid.fetch_add(1, std::memory_order_relaxed);
        else g_xboxStats.setPsoNull.fetch_add(1, std::memory_order_relaxed);
        m_cl->SetPipelineState(pPipelineState);
    }
    virtual void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {                        // 26 (+0x0D0)
        ForwardSanitizedResourceBarriers(m_cl.Get(), NumBarriers, pBarriers);
    }

    // Slots 27..31 (+0x0D8..+0x0F8): Xbox-inserted slots (4 extra + ExecuteBundle)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 27)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 28)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 29)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 30)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 31)

    // Slots 32..63 (+0x100..+0x1F8): PC slots 28..59 shifted by +4
    virtual void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) {        // 32 (+0x100)
        if (!ppDescriptorHeaps || NumDescriptorHeaps == 0) {
            m_cl->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
            return;
        }
        std::vector<ComPtr<ID3D12DescriptorHeap>> refs;
        std::vector<ID3D12DescriptorHeap*> unwrapped;
        refs.reserve(NumDescriptorHeaps);
        unwrapped.reserve(NumDescriptorHeaps);
        for (UINT i = 0; i < NumDescriptorHeaps; ++i) {
            ID3D12DescriptorHeap* heap = ppDescriptorHeaps[i];
            if (heap) {
                ComPtr<ID3D12DescriptorHeap> nativeHeap;
                if (SUCCEEDED(heap->QueryInterface(IID_ScarlettNativeDescriptorHeap,
                                                   reinterpret_cast<void**>(nativeHeap.ReleaseAndGetAddressOf()))) && nativeHeap) {
                    unwrapped.push_back(nativeHeap.Get());
                    refs.push_back(std::move(nativeHeap));
                    continue;
                }
            }
            unwrapped.push_back(heap);
        }
        m_boundHeapCount = 0;
        for (ID3D12DescriptorHeap* h : unwrapped) {
            if (!h || m_boundHeapCount >= 2) continue;
            D3D12_DESCRIPTOR_HEAP_DESC hd = h->GetDesc();
            if ((hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
                UINT64 start = h->GetGPUDescriptorHandleForHeapStart().ptr;
                UINT inc = 64u;
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(h->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
                    inc = dev->GetDescriptorHandleIncrementSize(hd.Type);
                    dev->Release();
                }
                UINT64 end = start + static_cast<UINT64>(hd.NumDescriptors) * static_cast<UINT64>(inc);
                m_boundHeaps[m_boundHeapCount++] = { start, end };
            }
        }
        m_cl->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped.data());
    }
    virtual void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) {                                       // 33 (+0x108)
        m_cl->SetComputeRootSignature(pRootSignature);
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) {                                      // 34 (+0x110)
        m_cl->SetGraphicsRootSignature(pRootSignature);
    }
    virtual void STDMETHODCALLTYPE SetComputeRootDescriptorTable(                                                                       // 35 (+0x118)
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
        if (!IsValidBoundGpuDescriptor(BaseDescriptor.ptr)) {
            LogInvalidGpuDescriptor("Cmp", RootParameterIndex, BaseDescriptor.ptr);
            return;
        }
        m_cl->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(                                                                      // 36 (+0x120)
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
        if (!IsValidBoundGpuDescriptor(BaseDescriptor.ptr)) {
            LogInvalidGpuDescriptor("Gfx", RootParameterIndex, BaseDescriptor.ptr);
            return;
        }
        m_cl->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    virtual void STDMETHODCALLTYPE SetComputeRoot32BitConstant(                                                                         // 37 (+0x128)
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
        m_cl->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(                                                                        // 38 (+0x130)
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
        m_cl->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    virtual void STDMETHODCALLTYPE SetComputeRoot32BitConstants(                                                                        // 39 (+0x138)
        UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) {
        m_cl->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(                                                                       // 40 (+0x140)
        UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) {
        m_cl->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    virtual void STDMETHODCALLTYPE SetComputeRootConstantBufferView(                                                                    // 41 (+0x148)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        m_cl->SetComputeRootConstantBufferView(RootParameterIndex, BounceRootGuestVa(BufferLocation, false, "CmpCBV"));
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(                                                                   // 42 (+0x150)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        D3D12_GPU_VIRTUAL_ADDRESS loc = BounceRootGuestVa(BufferLocation, false, "GfxCBV");
        if ((loc & 255ULL) != 0ULL) {
            static std::atomic<uint32_t> s_rootCbvWarn{0};
            if (s_rootCbvWarn.fetch_add(1, std::memory_order_relaxed) < 8) {
                char rb[128];
                std::snprintf(rb, sizeof(rb), "-> UNALIGNED RootCBV slot=%u loc=0x%llX", RootParameterIndex, static_cast<unsigned long long>(loc));
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12GfxCL", rb);
            }
        }
        m_cl->SetGraphicsRootConstantBufferView(RootParameterIndex, loc);
    }
    virtual void STDMETHODCALLTYPE SetComputeRootShaderResourceView(                                                                    // 43 (+0x158)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        m_cl->SetComputeRootShaderResourceView(RootParameterIndex, BounceRootGuestVa(BufferLocation, false, "CmpSRV"));
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(                                                                   // 44 (+0x160)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        m_cl->SetGraphicsRootShaderResourceView(RootParameterIndex, BounceRootGuestVa(BufferLocation, false, "GfxSRV"));
    }
    virtual void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(                                                                   // 45 (+0x168)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        m_cl->SetComputeRootUnorderedAccessView(RootParameterIndex, BounceRootGuestVa(BufferLocation, true, "CmpUAV"));
    }
    virtual void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(                                                                  // 46 (+0x170)
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
        m_cl->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BounceRootGuestVa(BufferLocation, true, "GfxUAV"));
    }
    virtual void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {                                             // 47 (+0x178)
        if (!pView || pView->BufferLocation == 0ULL || pView->SizeInBytes == 0u) {
            m_cl->IASetIndexBuffer(pView);
            return;
        }
        D3D12_INDEX_BUFFER_VIEW local = *pView;
        const D3D12_GPU_VIRTUAL_ADDRESS bounced = BounceRootGuestVa(local.BufferLocation, false, "IB", local.SizeInBytes);
        if (bounced != local.BufferLocation) {
            local.BufferLocation = bounced;
            if (local.SizeInBytes > 4u * 1024u * 1024u) local.SizeInBytes = 4u * 1024u * 1024u;
        }
        m_cl->IASetIndexBuffer(&local);
    }
    virtual void STDMETHODCALLTYPE IASetVertexBuffers(                                                                                  // 48 (+0x180)
        UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
        if (!pViews || NumViews == 0u) {
            m_cl->IASetVertexBuffers(StartSlot, NumViews, pViews);
            return;
        }
        D3D12_VERTEX_BUFFER_VIEW local[16]{};
        const UINT count = std::min<UINT>(NumViews, 16u);
        for (UINT i = 0; i < count; ++i) {
            local[i] = pViews[i];
            if (local[i].BufferLocation != 0ULL && local[i].SizeInBytes > 0u) {
                const D3D12_GPU_VIRTUAL_ADDRESS bounced = BounceRootGuestVa(local[i].BufferLocation, false, "VB", local[i].SizeInBytes);
                if (bounced != local[i].BufferLocation) {
                    local[i].BufferLocation = bounced;
                    if (local[i].SizeInBytes > 4u * 1024u * 1024u) local[i].SizeInBytes = 4u * 1024u * 1024u;
                }
            }
        }
        m_cl->IASetVertexBuffers(StartSlot, count, local);
    }
    virtual void STDMETHODCALLTYPE SOSetTargets(                                                                                        // 49 (+0x188)
        UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) {
        m_cl->SOSetTargets(StartSlot, NumViews, pViews);
    }
    virtual void STDMETHODCALLTYPE OMSetRenderTargets(                                                                                  // 50 (+0x190)
        UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
        g_xboxStats.omSetRt.fetch_add(1, std::memory_order_relaxed);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvBuf[8]{};
        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = pRenderTargetDescriptors;
        if (pRenderTargetDescriptors && NumRenderTargetDescriptors > 0) {
            const UINT count = RTsSingleHandleToDescriptorRange ? 1u : std::min<UINT>(NumRenderTargetDescriptors, 8u);
            for (UINT i = 0; i < count; ++i) rtvBuf[i] = TranslateCpuDescriptorHandle(pRenderTargetDescriptors[i]);
            rtvPtr = rtvBuf;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsvLocal{};
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
        if (pDepthStencilDescriptor) {
            dsvLocal = TranslateCpuDescriptorHandle(*pDepthStencilDescriptor);
            dsvPtr = &dsvLocal;
        }
        m_cl->OMSetRenderTargets(NumRenderTargetDescriptors, rtvPtr,
                                 RTsSingleHandleToDescriptorRange, dsvPtr);
    }
    virtual void STDMETHODCALLTYPE ClearDepthStencilView(                                                                               // 51 (+0x198)
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
        FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
        m_cl->ClearDepthStencilView(TranslateCpuDescriptorHandle(DepthStencilView), ClearFlags, Depth, Stencil, NumRects, pRects);
    }
    virtual void STDMETHODCALLTYPE ClearRenderTargetView(                                                                               // 52 (+0x1A0)
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) {
        const uint64_t clrN = g_xboxStats.clears.fetch_add(1, std::memory_order_relaxed);
        if (clrN < 5 && ColorRGBA) {
            char cb[128];
            std::snprintf(cb, sizeof(cb), "-> ClearRTV #%llu rgba=(%.3f, %.3f, %.3f, %.3f) rects=%u",
                          static_cast<unsigned long long>(clrN), ColorRGBA[0], ColorRGBA[1], ColorRGBA[2], ColorRGBA[3], NumRects);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12GfxCL", cb);
        }
        m_cl->ClearRenderTargetView(TranslateCpuDescriptorHandle(RenderTargetView), ColorRGBA, NumRects, pRects);
    }
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(                                                                        // 53 (+0x1A8)
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
        m_cl->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, TranslateCpuDescriptorHandle(ViewCPUHandle), XboxD3D12Resource::Unwrap(pResource), Values, NumRects, pRects);
    }
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(                                                                       // 54 (+0x1B0)
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
        m_cl->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, TranslateCpuDescriptorHandle(ViewCPUHandle), XboxD3D12Resource::Unwrap(pResource), Values, NumRects, pRects);
    }
    virtual void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) {                   // 55 (+0x1B8)
        m_cl->DiscardResource(XboxD3D12Resource::Unwrap(pResource), pRegion);
    }
    virtual void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {                        // 56 (+0x1C0)
        m_cl->BeginQuery(pQueryHeap, Type, Index);
    }
    virtual void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {                          // 57 (+0x1C8)
        m_cl->EndQuery(pQueryHeap, Type, Index);
    }
    virtual void STDMETHODCALLTYPE ResolveQueryData(                                                                                    // 58 (+0x1D0)
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
        ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) {
        m_cl->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, XboxD3D12Resource::Unwrap(pDestinationBuffer), AlignedDestinationBufferOffset);
    }
    virtual void STDMETHODCALLTYPE SetPredication(                                                                                      // 59 (+0x1D8)
        ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) {
        m_cl->SetPredication(pBuffer, AlignedBufferOffset, Operation);
    }
    virtual void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) {                                            // 60 (+0x1E0)
        m_cl->SetMarker(Metadata, pData, Size);
    }
    virtual void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) {                                           // 61 (+0x1E8)
        m_cl->BeginEvent(Metadata, pData, Size);
    }
    virtual void STDMETHODCALLTYPE EndEvent() {                                                                                         // 62 (+0x1F0)
        m_cl->EndEvent();
    }
    virtual void STDMETHODCALLTYPE ExecuteIndirect(                                                                                     // 63 (+0x1F8)
        ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
        UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) {
        m_cl->ExecuteIndirect(pCommandSignature, MaxCommandCount, XboxD3D12Resource::Unwrap(pArgumentBuffer), ArgumentBufferOffset, XboxD3D12Resource::Unwrap(pCountBuffer), CountBufferOffset);
    }

    // Xbox ID3D12GraphicsCommandList extension slots (64..119)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 64)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 65)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 66)
    // Slot 67 (+0x218): Xbox FlushGpuCacheRange (coherency hint; no-op on PC)
    virtual void STDMETHODCALLTYPE FlushGpuCacheRange(UINT32 /*Flags*/, void* /*pBaseAddress*/, SIZE_T /*SizeInBytes*/) {}
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 68)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 69)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 70)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 71)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 72)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 73)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 74)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 75)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 76)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 77)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 78)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 79)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 80)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 81)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 82)
    // Slot 83 (+0x298): Xbox CopyTextureRegionX (trailing flags ignored on PC)
    virtual void STDMETHODCALLTYPE CopyTextureRegionX(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox, UINT /*Flags*/) {
        D3D12_TEXTURE_COPY_LOCATION dLoc = UnwrapCopyLocation(pDst); D3D12_TEXTURE_COPY_LOCATION sLoc = UnwrapCopyLocation(pSrc); m_cl->CopyTextureRegion(pDst ? &dLoc : nullptr, DstX, DstY, DstZ, pSrc ? &sLoc : nullptr, pSrcBox);
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 84)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 85)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 86)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 87)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 88)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 89)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 90)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 91)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 92)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 93)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 94)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 95)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 96)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 97)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 98)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 99)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 100)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 101)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 102)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 103)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 104)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 105)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 106)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 107)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 108)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 109)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 110)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 111)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 112)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 113)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 114)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 115)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 116)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 117)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 118)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 119)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 120)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 121)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 122)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 123)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 124)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 125)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 126)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 127)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 128)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 129)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 130)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 131)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 132)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 133)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 134)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 135)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 136)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 137)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 138)
    // Slot 139 (+0x458): Xbox ResetX(pAllocator, pInitialState, Flags)
    virtual HRESULT STDMETHODCALLTYPE ResetX(
        ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState, UINT /*Flags*/) {
        return m_cl->Reset(XboxD3D12CommandAllocator::Unwrap(pAllocator), pInitialState);
    }
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 140)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 141)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 142)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 143)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 144)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 145)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 146)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 147)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 148)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 149)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 150)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 151)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 152)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 153)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 154)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 155)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 156)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 157)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 158)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 159)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 160)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 161)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 162)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 163)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 164)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 165)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 166)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 167)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 168)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 169)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 170)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 171)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 172)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 173)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 174)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 175)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 176)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 177)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 178)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 179)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 180)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 181)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 182)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 183)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 184)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 185)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 186)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 187)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 188)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 189)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 190)
    SCARLETT_XBOX_SUBOBJ_EXT_STUB(XboxD3D12GraphicsCommandList, 191)

private:
    // Zero-initialized header pad (+0x08..+0x107) so Xbox inline PM4 fastpaths
    // ([this+0x38] < [this+0x50]) always take the virtual fallback branch.
    D3D12_GPU_VIRTUAL_ADDRESS BounceRootGuestVa(D3D12_GPU_VIRTUAL_ADDRESS loc, bool isUav, const char* tag, uint64_t reqBytes = 4096ULL) {
        if (loc == 0ULL) return 0ULL;
        constexpr uint64_t kRingSize = 4ULL * 1024ULL * 1024ULL;
        const uint64_t kSliceBytes = std::min<uint64_t>(kRingSize, std::max<uint64_t>(256ULL, (reqBytes + 255ULL) & ~255ULL));
        if (m_rootBounceGpu != 0ULL && loc >= m_rootBounceGpu && loc < m_rootBounceGpu + kRingSize) {
            return loc;
        }
        if (m_rootUavGpu != 0ULL && loc >= m_rootUavGpu && loc < m_rootUavGpu + 65536ULL) {
            return loc;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(loc), &mbi, sizeof(mbi)) == 0) {
            return loc;
        }
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool readable =
            (mbi.State == MEM_COMMIT) &&
            ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
             prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) {
            return loc;
        }
        ComPtr<ID3D12Device> nativeDev;
        if (FAILED(m_cl->GetDevice(IID_PPV_ARGS(&nativeDev))) || !nativeDev) {
            return loc;
        }
        if (isUav) {
            if (!m_rootUavBuf) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 65536ULL;
                bd.Height = 1;
                bd.DepthOrArraySize = 1;
                bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (SUCCEEDED(nativeDev->CreateCommittedResource(
                        &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        nullptr, IID_PPV_ARGS(&m_rootUavBuf))) && m_rootUavBuf) {
                    m_rootUavGpu = m_rootUavBuf->GetGPUVirtualAddress();
                }
            }
            return m_rootUavGpu ? m_rootUavGpu : loc;
        }
        if (!m_rootBounceBuf) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = kRingSize;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(nativeDev->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&m_rootBounceBuf))) && m_rootBounceBuf) {
                D3D12_RANGE emptyRead{0, 0};
                if (SUCCEEDED(m_rootBounceBuf->Map(0, &emptyRead, reinterpret_cast<void**>(&m_rootBounceCpu))) && m_rootBounceCpu) {
                    m_rootBounceGpu = m_rootBounceBuf->GetGPUVirtualAddress();
                } else {
                    m_rootBounceBuf.Reset();
                }
            }
        }
        if (!m_rootBounceCpu || !m_rootBounceGpu) return loc;
        if (m_rootBounceCursor + kSliceBytes > kRingSize) m_rootBounceCursor = 0;
        const uint64_t off = m_rootBounceCursor;
        m_rootBounceCursor += kSliceBytes;
        const uint64_t regionEnd = reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
        const uint64_t copyN = std::min<uint64_t>(kSliceBytes, (regionEnd > loc) ? (regionEnd - loc) : 0ULL);
        if (copyN > 0) {
            std::memcpy(m_rootBounceCpu + off, reinterpret_cast<const void*>(loc), static_cast<size_t>(copyN));
        }
        if (copyN < kSliceBytes) {
            std::memset(m_rootBounceCpu + off + copyN, 0, static_cast<size_t>(kSliceBytes - copyN));
        }
        const D3D12_GPU_VIRTUAL_ADDRESS bounced = m_rootBounceGpu + off;
        static std::atomic<uint32_t> s_bounceLogs{0};
        if (s_bounceLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> bounced %s guestVA=0x%llX -> hostGPU=0x%llX (%llu B)",
                          tag, static_cast<unsigned long long>(loc), static_cast<unsigned long long>(bounced),
                          static_cast<unsigned long long>(copyN));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12GfxCL::BounceRootGuestVa", msg);
        }
        return bounced;
    }
    uint64_t m_xboxInlinePad[32]{};
    std::atomic<ULONG> m_refs{1};
    struct BoundHeapRange { UINT64 start; UINT64 end; };
    BoundHeapRange m_boundHeaps[2]{};
    uint32_t m_boundHeapCount = 0;

    bool IsValidBoundGpuDescriptor(UINT64 ptr) const {
        if (ptr == 0) return false;
        for (uint32_t i = 0; i < m_boundHeapCount; ++i) {
            if (ptr >= m_boundHeaps[i].start && ptr < m_boundHeaps[i].end) return true;
        }
        return false;
    }

    void LogInvalidGpuDescriptor(const char* kind, UINT slot, UINT64 ptr) const {
        static std::atomic<uint32_t> s_badDesc{0};
        if (s_badDesc.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "-> SKIPPED invalid %s RootDescriptorTable slot=%u ptr=0x%llX (boundHeaps=%u)",
                          kind, slot, static_cast<unsigned long long>(ptr), m_boundHeapCount);
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12GfxCL", msg);
        }
    }

    ComPtr<ID3D12GraphicsCommandList> m_cl;
    ID3D12Device* m_parent = nullptr;
    ComPtr<ID3D12Resource> m_rootBounceBuf;
    uint8_t* m_rootBounceCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_rootBounceGpu = 0;
    uint64_t m_rootBounceCursor = 0;
    ComPtr<ID3D12Resource> m_rootUavBuf;
    D3D12_GPU_VIRTUAL_ADDRESS m_rootUavGpu = 0;
};

#undef SCARLETT_XBOX_SUBOBJ_EXT_STUB

#define SCARLETT_XBOX_DEV_EXT_STUB(SLOT) \
    virtual HRESULT STDMETHODCALLTYPE XboxExtSlot##SLOT(uint64_t a1, uint64_t a2, uint64_t a3) { \
        char msg[256]; \
        std::snprintf(msg, sizeof(msg), \
                      "UNIMPLEMENTED slot=%d (+0x%03X) a1=0x%llX a2=0x%llX a3=0x%llX ret=%s", \
                      SLOT, (SLOT) * 8, \
                      static_cast<unsigned long long>(a1), \
                      static_cast<unsigned long long>(a2), \
                      static_cast<unsigned long long>(a3), \
                      FormatCallerAddr(_ReturnAddress()).c_str()); \
        sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::Ext", msg); \
        return S_OK; \
    }

class XboxD3D12Device final : public ID3D12Device {
public:
    XboxD3D12Device(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter1> adapter, ComPtr<ID3D12Device> device)
        : m_factory(std::move(factory)), m_adapter(std::move(adapter)), m_device(std::move(device)) {
                InstallXboxCAbiVtable();
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device", "-> created Xbox ID3D12Device wrapper");
    }

    ~XboxD3D12Device() {
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "~XboxD3D12Device", "-> destroyed Xbox ID3D12Device wrapper");
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        static constexpr GUID kXboxDevIids[] = {
            { 0x77acce80, 0x638e, 0x4e65, { 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e } }, // ID3D12Device1
            { 0x30baa41e, 0xb15b, 0x475c, { 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28 } }, // ID3D12Device2
            { 0x81dadc15, 0x2bad, 0x4392, { 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98 } }, // ID3D12Device3
            { 0xe865df17, 0xa9ee, 0x46f9, { 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5 } }, // ID3D12Device4
            { 0x8b4f173b, 0x2fea, 0x4b80, { 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d } }, // ID3D12Device5
            { 0xc70b221b, 0x40e4, 0x4a17, { 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc } }, // ID3D12Device6
            { 0x5c014b53, 0x68a1, 0x4b9b, { 0x8b, 0x1d, 0x62, 0xfe, 0x68, 0xc6, 0x09, 0x66 } }, // ID3D12Device7
            { 0x9218e6bb, 0xf944, 0x4f7e, { 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3 } }, // ID3D12Device8
            { 0x4c80e962, 0xf032, 0x4f60, { 0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c } }, // ID3D12Device9
            { 0x517f8718, 0xaa66, 0x49f9, { 0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31 } }, // ID3D12Device10
            { 0x5405c344, 0xd457, 0x444e, { 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39 } }, // ID3D12Device11
        };
        bool isDevIid = IsEqualGUID(riid, __uuidof(IUnknown)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12Object)) ||
                        IsEqualGUID(riid, __uuidof(ID3D12Device));
        if (!isDevIid) {
            for (const auto& g : kXboxDevIids) {
                if (IsEqualGUID(riid, g)) { isDevIid = true; break; }
            }
        }
        if (isDevIid) {
            *ppvObject = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, __uuidof(IDXGIDevice))) {
            *ppvObject = new XboxDXGIDevice(m_adapter, this);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::QueryInterface", "-> returned XboxDXGIDevice");
            return S_OK;
        }
        HRESULT hr = m_device->QueryInterface(riid, ppvObject);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "-> forwarded riid=%s hr=0x%08lX", FormatGuidSimple(riid).c_str(), hr);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::QueryInterface", msg);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_device->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_device->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_device->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_device->SetName(Name);
    }

    // ID3D12Device (slots 7..43)
    UINT STDMETHODCALLTYPE GetNodeCount() override {
        return m_device->GetNodeCount();
    }

    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override {
        if (!pDesc || !ppCommandQueue) return E_INVALIDARG;
        *ppCommandQueue = nullptr;
        D3D12_COMMAND_QUEUE_DESC clean = SanitizeQueueDesc(*pDesc);
        ComPtr<ID3D12CommandQueue> nativeQueue;
        HRESULT hr = m_device->CreateCommandQueue(&clean, IID_PPV_ARGS(&nativeQueue));
        if (SUCCEEDED(hr)) {
            ComPtr<XboxD3D12CommandQueue> wrapped;
            wrapped.Attach(new XboxD3D12CommandQueue(m_factory, m_device, std::move(nativeQueue), this, pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT));
            hr = wrapped->QueryInterface(riid, ppCommandQueue);
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg), "-> rawType=%d cleanType=%d flags=0x%X hr=0x%08lX",
                      static_cast<int>(pDesc->Type), static_cast<int>(clean.Type),
                      static_cast<unsigned>(clean.Flags), hr);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateCommandQueue", msg);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override {
        if (!ppCommandAllocator) return E_POINTER;
        *ppCommandAllocator = nullptr;
        D3D12_COMMAND_LIST_TYPE cleanType = SanitizeCommandListType(type);
        ComPtr<ID3D12CommandAllocator> nativeAlloc;
        HRESULT hr = m_device->CreateCommandAllocator(cleanType, IID_PPV_ARGS(&nativeAlloc));
        if (FAILED(hr)) return hr;
        ComPtr<XboxD3D12CommandAllocator> wrapped;
        wrapped.Attach(new XboxD3D12CommandAllocator(std::move(nativeAlloc), this));
        return wrapped->QueryInterface(riid, ppCommandAllocator);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        if (!pDesc || !ppPipelineState) return E_INVALIDARG;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC clean = *pDesc;
        sc::d3d12::DxbcStripResult vsS, psS, dsS, hsS, gsS;
        auto tryStrip = [](D3D12_SHADER_BYTECODE& bc, auto& res) {
            if (bc.pShaderBytecode && bc.BytecodeLength > 0 &&
                sc::d3d12::StripXboxDxbcChunks(bc.pShaderBytecode, bc.BytecodeLength, res) &&
                res.wasStripped && res.hasExecutableShader && !res.bytecode.empty()) {
                bc.pShaderBytecode = res.bytecode.data();
                bc.BytecodeLength = res.bytecode.size();
            }
        };
        tryStrip(clean.VS, vsS);
        tryStrip(clean.PS, psS);
        tryStrip(clean.DS, dsS);
        tryStrip(clean.HS, hsS);
        tryStrip(clean.GS, gsS);
        HRESULT hr = clean.pRootSignature
            ? m_device->CreateGraphicsPipelineState(&clean, riid, ppPipelineState)
            : E_INVALIDARG;
        if (FAILED(hr) || !*ppPipelineState) {
            HRESULT fbHr = ReturnNoopPso(false, riid, ppPipelineState);
            static std::atomic<uint32_t> s_gfxFbLogs{0};
            if (FAILED(fbHr) || s_gfxFbLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::string vsInfo = sc::d3d12::InspectShaderBytecode(clean.VS);
                std::string psInfo = sc::d3d12::InspectShaderBytecode(clean.PS);
                char msg[384];
                std::snprintf(msg, sizeof(msg),
                              "-> GfxPSO origHr=0x%08lX -> NOOP FALLBACK hr=0x%08lX pso=%p VS=[%s] PS=[%s]",
                              static_cast<unsigned long>(hr), static_cast<unsigned long>(fbHr),
                              *ppPipelineState, vsInfo.c_str(), psInfo.c_str());
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateGfxPSO", msg);
            }
            if (SUCCEEDED(fbHr)) {
                g_xboxStats.psoGfxOk.fetch_add(1, std::memory_order_relaxed);
                return fbHr;
            }
        }
        const uint64_t n = SUCCEEDED(hr)
            ? g_xboxStats.psoGfxOk.fetch_add(1, std::memory_order_relaxed)
            : g_xboxStats.psoGfxFail.fetch_add(1, std::memory_order_relaxed);
        if (n < 5) {
            std::string vsInfo = sc::d3d12::InspectShaderBytecode(clean.VS);
            std::string psInfo = sc::d3d12::InspectShaderBytecode(clean.PS);
            char msg[384];
            std::snprintf(msg, sizeof(msg), "-> GfxPSO hr=0x%08lX (vsStrip=%d psStrip=%d rtvs=%u rtv0=%u) VS=[%s] PS=[%s]",
                          static_cast<unsigned long>(hr), vsS.wasStripped ? 1 : 0, psS.wasStripped ? 1 : 0,
                          clean.NumRenderTargets, clean.NumRenderTargets > 0 ? static_cast<unsigned>(clean.RTVFormats[0]) : 0u,
                          vsInfo.c_str(), psInfo.c_str());
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateGfxPSO", msg);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        if (!pDesc || !ppPipelineState) return E_INVALIDARG;
        bool forceNoopFallback = false;
        if (pDesc->CS.pShaderBytecode && pDesc->CS.BytecodeLength > 32) {
            const auto* bytes = static_cast<const char*>(pDesc->CS.pShaderBytecode);
            const size_t len = pDesc->CS.BytecodeLength;
            constexpr const char kNeedle[] = "LeafRehydration.pdb";
            constexpr size_t kNeedleLen = sizeof(kNeedle) - 1;
            if (len >= kNeedleLen) {
                for (size_t i = 0; i + kNeedleLen <= len; ++i) {
                    if (bytes[i] == kNeedle[0] && std::memcmp(bytes + i, kNeedle, kNeedleLen) == 0) {
                        forceNoopFallback = true;
                        break;
                    }
                }
            }
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC clean = *pDesc;
        sc::d3d12::DxbcStripResult csS;
        if (!forceNoopFallback && clean.CS.pShaderBytecode && clean.CS.BytecodeLength > 0 &&
            sc::d3d12::StripXboxDxbcChunks(clean.CS.pShaderBytecode, clean.CS.BytecodeLength, csS) &&
            csS.wasStripped && csS.hasExecutableShader && !csS.bytecode.empty()) {
            clean.CS.pShaderBytecode = csS.bytecode.data();
            clean.CS.BytecodeLength = csS.bytecode.size();
        }
        HRESULT hr = forceNoopFallback
            ? E_NOTIMPL
            : m_device->CreateComputePipelineState(&clean, riid, ppPipelineState);
        if (FAILED(hr)) {
            {
                char preFb[128];
                std::snprintf(preFb, sizeof(preFb), "-> CmpPSO origHr=0x%08lX, invoking cs_6_5 NOOP fallback",
                    static_cast<unsigned long>(hr));
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateCmpPSO", preFb);
            }
            static const uint8_t kNoopComputeDxbc[] = {
                0x44, 0x58, 0x42, 0x43, 0x78, 0x1C, 0xB8, 0xF2, 0x49, 0x73, 0x23, 0x71, 0xD4, 0x95, 0x0A, 0x39,
                0x2A, 0x63, 0xA3, 0xA2, 0x01, 0x00, 0x00, 0x00, 0xA0, 0x08, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
                0x3C, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x00, 0x6C, 0x00, 0x00, 0x00,
                0xC0, 0x00, 0x00, 0x00, 0xB4, 0x04, 0x00, 0x00, 0xD0, 0x04, 0x00, 0x00, 0x53, 0x46, 0x49, 0x30,
                0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x53, 0x47, 0x31,
                0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x4F, 0x53, 0x47, 0x31,
                0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x50, 0x53, 0x56, 0x30,
                0x4C, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x08, 0x00, 0x00, 0x00, 0x00, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x53, 0x54, 0x41, 0x54, 0xEC, 0x03, 0x00, 0x00, 0x65, 0x00, 0x05, 0x00, 0xFB, 0x00, 0x00, 0x00,
                0x44, 0x58, 0x49, 0x4C, 0x05, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xD4, 0x03, 0x00, 0x00,
                0x42, 0x43, 0xC0, 0xDE, 0x21, 0x0C, 0x00, 0x00, 0xF2, 0x00, 0x00, 0x00, 0x0B, 0x82, 0x20, 0x00,
                0x02, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x07, 0x81, 0x23, 0x91, 0x41, 0xC8, 0x04, 0x49,
                0x06, 0x10, 0x32, 0x39, 0x92, 0x01, 0x84, 0x0C, 0x25, 0x05, 0x08, 0x19, 0x1E, 0x04, 0x8B, 0x62,
                0x80, 0x0C, 0x45, 0x02, 0x42, 0x92, 0x0B, 0x42, 0x64, 0x10, 0x32, 0x14, 0x38, 0x08, 0x18, 0x4B,
                0x0A, 0x32, 0x32, 0x88, 0x48, 0x90, 0x14, 0x20, 0x43, 0x46, 0x88, 0xA5, 0x00, 0x19, 0x32, 0x42,
                0xE4, 0x48, 0x0E, 0x90, 0x91, 0x21, 0xC4, 0x50, 0x41, 0x51, 0x81, 0x8C, 0xE1, 0x83, 0xE5, 0x8A,
                0x04, 0x19, 0x46, 0x06, 0x51, 0x18, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x1B, 0x8C, 0xE0, 0xFF,
                0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x01, 0x24, 0x00, 0x49, 0x18, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x13, 0x82, 0x00, 0x00, 0x89, 0x20, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x32, 0x22, 0xC8, 0x08,
                0x20, 0x64, 0x85, 0x04, 0x93, 0x21, 0xA4, 0x84, 0x04, 0x93, 0x21, 0xE3, 0x84, 0xA1, 0x90, 0x14,
                0x12, 0x4C, 0x86, 0x8C, 0x0B, 0x84, 0x64, 0x4C, 0x10, 0x1C, 0x23, 0x00, 0x25, 0x00, 0x65, 0x8E,
                0x00, 0x0C, 0xCA, 0x80, 0x8D, 0x38, 0x10, 0x00, 0x13, 0x14, 0x72, 0xC0, 0x87, 0x74, 0x60, 0x87,
                0x36, 0x68, 0x87, 0x79, 0x68, 0x03, 0x72, 0xC0, 0x87, 0x0D, 0xAF, 0x50, 0x0E, 0x6D, 0xD0, 0x0E,
                0x7A, 0x50, 0x0E, 0x6D, 0x00, 0x0F, 0x7A, 0x30, 0x07, 0x72, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D,
                0x90, 0x0E, 0x71, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x90, 0x0E, 0x78, 0xA0, 0x07, 0x73, 0x20,
                0x07, 0x6D, 0x90, 0x0E, 0x71, 0x60, 0x07, 0x7A, 0x30, 0x07, 0x72, 0xD0, 0x06, 0xE9, 0x30, 0x07,
                0x72, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x90, 0x0E, 0x76, 0x40, 0x07, 0x7A, 0x60, 0x07, 0x74,
                0xD0, 0x06, 0xE6, 0x10, 0x07, 0x76, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x60, 0x0E, 0x73, 0x20,
                0x07, 0x7A, 0x30, 0x07, 0x72, 0xD0, 0x06, 0xE6, 0x60, 0x07, 0x74, 0xA0, 0x07, 0x76, 0x40, 0x07,
                0x6D, 0xE0, 0x0E, 0x78, 0xA0, 0x07, 0x71, 0x60, 0x07, 0x7A, 0x30, 0x07, 0x72, 0xA0, 0x07, 0x76,
                0x40, 0x07, 0x43, 0x9E, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86,
                0x3C, 0x04, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x81, 0x00, 0x00,
                0x07, 0x00, 0x00, 0x00, 0x32, 0x1E, 0x98, 0x10, 0x19, 0x11, 0x4C, 0x90, 0x8C, 0x09, 0x26, 0x47,
                0xC6, 0x04, 0x43, 0x5A, 0x09, 0x94, 0xC2, 0x08, 0x40, 0x31, 0x14, 0x42, 0x11, 0x00, 0x00, 0x00,
                0x79, 0x18, 0x00, 0x00, 0x3D, 0x00, 0x00, 0x00, 0x1A, 0x03, 0x4C, 0x90, 0x46, 0x02, 0x13, 0x44,
                0x8F, 0x0C, 0x6F, 0xEC, 0xED, 0x4D, 0x0C, 0x24, 0xC6, 0x25, 0xC7, 0x45, 0xC6, 0x06, 0x46, 0xC6,
                0x25, 0xE6, 0x06, 0x04, 0x45, 0x26, 0x86, 0x4C, 0x06, 0xC7, 0xEC, 0x46, 0xE6, 0x26, 0x65, 0x43,
                0x10, 0x4C, 0x10, 0x06, 0x61, 0x82, 0x30, 0x0C, 0x1B, 0x84, 0x81, 0x98, 0x20, 0x0C, 0xC4, 0x06,
                0xC1, 0x30, 0x28, 0x8C, 0xCD, 0x4D, 0x10, 0x86, 0x62, 0xC3, 0x80, 0x24, 0xC4, 0x04, 0x41, 0x00,
                0x36, 0x00, 0x1B, 0x06, 0x83, 0x61, 0x36, 0x04, 0xCD, 0x86, 0x61, 0x58, 0x1C, 0x12, 0x6D, 0x61,
                0x69, 0x6E, 0x13, 0x84, 0xC1, 0xD8, 0x30, 0x0C, 0xC3, 0xB0, 0x41, 0x88, 0xA4, 0x0D, 0xC5, 0x02,
                0x01, 0xC0, 0xC4, 0x43, 0x2C, 0x4C, 0x4E, 0x2E, 0xAD, 0x4C, 0x8E, 0x45, 0x9A, 0xDB, 0x1C, 0xDD,
                0xDC, 0x04, 0x61, 0x38, 0x36, 0x10, 0xD5, 0x60, 0x5D, 0x55, 0xD8, 0xD8, 0xEC, 0xDA, 0x5C, 0xD2,
                0xC8, 0xCA, 0xDC, 0xE8, 0xA6, 0x04, 0x41, 0x15, 0x32, 0x3C, 0x17, 0xBB, 0x32, 0xB9, 0xB9, 0xB4,
                0x37, 0xB7, 0x29, 0x01, 0xD1, 0x84, 0x0C, 0xCF, 0xC5, 0x2E, 0x8C, 0xCD, 0xAE, 0x4C, 0x6E, 0x4A,
                0x60, 0xD4, 0x21, 0xC3, 0x73, 0x99, 0x43, 0x0B, 0x23, 0x2B, 0x93, 0x6B, 0x7A, 0x23, 0x2B, 0x63,
                0x9B, 0x12, 0x24, 0x95, 0xC8, 0xF0, 0x5C, 0xE8, 0xF2, 0xE0, 0xCA, 0x82, 0xDC, 0xDC, 0xDE, 0xE8,
                0xC2, 0xE8, 0xD2, 0xDE, 0xDC, 0xE6, 0xA6, 0x04, 0x4E, 0x1D, 0x32, 0x3C, 0x97, 0x32, 0x37, 0x3A,
                0xB9, 0x3C, 0xA8, 0xB7, 0x34, 0x37, 0xBA, 0xB9, 0x29, 0xC1, 0xD4, 0x85, 0x0C, 0xCF, 0x65, 0xEC,
                0xAD, 0xCE, 0x8D, 0xAE, 0x4C, 0x6E, 0x6E, 0x4A, 0x70, 0x01, 0x00, 0x00, 0x79, 0x18, 0x00, 0x00,
                0x4C, 0x00, 0x00, 0x00, 0x33, 0x08, 0x80, 0x1C, 0xC4, 0xE1, 0x1C, 0x66, 0x14, 0x01, 0x3D, 0x88,
                0x43, 0x38, 0x84, 0xC3, 0x8C, 0x42, 0x80, 0x07, 0x79, 0x78, 0x07, 0x73, 0x98, 0x71, 0x0C, 0xE6,
                0x00, 0x0F, 0xED, 0x10, 0x0E, 0xF4, 0x80, 0x0E, 0x33, 0x0C, 0x42, 0x1E, 0xC2, 0xC1, 0x1D, 0xCE,
                0xA1, 0x1C, 0x66, 0x30, 0x05, 0x3D, 0x88, 0x43, 0x38, 0x84, 0x83, 0x1B, 0xCC, 0x03, 0x3D, 0xC8,
                0x43, 0x3D, 0x8C, 0x03, 0x3D, 0xCC, 0x78, 0x8C, 0x74, 0x70, 0x07, 0x7B, 0x08, 0x07, 0x79, 0x48,
                0x87, 0x70, 0x70, 0x07, 0x7A, 0x70, 0x03, 0x76, 0x78, 0x87, 0x70, 0x20, 0x87, 0x19, 0xCC, 0x11,
                0x0E, 0xEC, 0x90, 0x0E, 0xE1, 0x30, 0x0F, 0x6E, 0x30, 0x0F, 0xE3, 0xF0, 0x0E, 0xF0, 0x50, 0x0E,
                0x33, 0x10, 0xC4, 0x1D, 0xDE, 0x21, 0x1C, 0xD8, 0x21, 0x1D, 0xC2, 0x61, 0x1E, 0x66, 0x30, 0x89,
                0x3B, 0xBC, 0x83, 0x3B, 0xD0, 0x43, 0x39, 0xB4, 0x03, 0x3C, 0xBC, 0x83, 0x3C, 0x84, 0x03, 0x3B,
                0xCC, 0xF0, 0x14, 0x76, 0x60, 0x07, 0x7B, 0x68, 0x07, 0x37, 0x68, 0x87, 0x72, 0x68, 0x07, 0x37,
                0x80, 0x87, 0x70, 0x90, 0x87, 0x70, 0x60, 0x07, 0x76, 0x28, 0x07, 0x76, 0xF8, 0x05, 0x76, 0x78,
                0x87, 0x77, 0x80, 0x87, 0x5F, 0x08, 0x87, 0x71, 0x18, 0x87, 0x72, 0x98, 0x87, 0x79, 0x98, 0x81,
                0x2C, 0xEE, 0xF0, 0x0E, 0xEE, 0xE0, 0x0E, 0xF5, 0xC0, 0x0E, 0xEC, 0x30, 0x03, 0x62, 0xC8, 0xA1,
                0x1C, 0xE4, 0xA1, 0x1C, 0xCC, 0xA1, 0x1C, 0xE4, 0xA1, 0x1C, 0xDC, 0x61, 0x1C, 0xCA, 0x21, 0x1C,
                0xC4, 0x81, 0x1D, 0xCA, 0x61, 0x06, 0xD6, 0x90, 0x43, 0x39, 0xC8, 0x43, 0x39, 0x98, 0x43, 0x39,
                0xC8, 0x43, 0x39, 0xB8, 0xC3, 0x38, 0x94, 0x43, 0x38, 0x88, 0x03, 0x3B, 0x94, 0xC3, 0x2F, 0xBC,
                0x83, 0x3C, 0xFC, 0x82, 0x3B, 0xD4, 0x03, 0x3B, 0xB0, 0xC3, 0x0C, 0xC4, 0x21, 0x07, 0x7C, 0x70,
                0x03, 0x7A, 0x28, 0x87, 0x76, 0x80, 0x87, 0x19, 0xD1, 0x43, 0x0E, 0xF8, 0xE0, 0x06, 0xE4, 0x20,
                0x0E, 0xE7, 0xE0, 0x06, 0xF6, 0x10, 0x0E, 0xF2, 0xC0, 0x0E, 0xE1, 0x90, 0x0F, 0xEF, 0x50, 0x0F,
                0xF4, 0x00, 0x00, 0x00, 0x71, 0x20, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x16, 0xD0, 0x0C, 0x97,
                0xEF, 0x3C, 0x7E, 0x00, 0x44, 0x11, 0x42, 0x44, 0x06, 0x40, 0x30, 0x00, 0xD2, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x48, 0x41, 0x53, 0x48, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x5A, 0xA8, 0x24, 0x16, 0x32, 0xD1, 0xB8, 0x2E, 0x58, 0xE4, 0x91, 0xCE, 0x31, 0xCF, 0x49, 0x8C,
                0x44, 0x58, 0x49, 0x4C, 0xC8, 0x03, 0x00, 0x00, 0x65, 0x00, 0x05, 0x00, 0xF2, 0x00, 0x00, 0x00,
                0x44, 0x58, 0x49, 0x4C, 0x05, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xB0, 0x03, 0x00, 0x00,
                0x42, 0x43, 0xC0, 0xDE, 0x21, 0x0C, 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x0B, 0x82, 0x20, 0x00,
                0x02, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x07, 0x81, 0x23, 0x91, 0x41, 0xC8, 0x04, 0x49,
                0x06, 0x10, 0x32, 0x39, 0x92, 0x01, 0x84, 0x0C, 0x25, 0x05, 0x08, 0x19, 0x1E, 0x04, 0x8B, 0x62,
                0x80, 0x0C, 0x45, 0x02, 0x42, 0x92, 0x0B, 0x42, 0x64, 0x10, 0x32, 0x14, 0x38, 0x08, 0x18, 0x4B,
                0x0A, 0x32, 0x32, 0x88, 0x48, 0x90, 0x14, 0x20, 0x43, 0x46, 0x88, 0xA5, 0x00, 0x19, 0x32, 0x42,
                0xE4, 0x48, 0x0E, 0x90, 0x91, 0x21, 0xC4, 0x50, 0x41, 0x51, 0x81, 0x8C, 0xE1, 0x83, 0xE5, 0x8A,
                0x04, 0x19, 0x46, 0x06, 0x51, 0x18, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x1B, 0x8C, 0xE0, 0xFF,
                0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x01, 0x24, 0x00, 0x49, 0x18, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x13, 0x82, 0x00, 0x00, 0x89, 0x20, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x32, 0x22, 0xC8, 0x08,
                0x20, 0x64, 0x85, 0x04, 0x93, 0x21, 0xA4, 0x84, 0x04, 0x93, 0x21, 0xE3, 0x84, 0xA1, 0x90, 0x14,
                0x12, 0x4C, 0x86, 0x8C, 0x0B, 0x84, 0x64, 0x4C, 0x10, 0x1C, 0x23, 0x00, 0x25, 0x00, 0x65, 0x8E,
                0x00, 0x0C, 0xCA, 0x80, 0x8D, 0x38, 0x10, 0x00, 0x13, 0x14, 0x72, 0xC0, 0x87, 0x74, 0x60, 0x87,
                0x36, 0x68, 0x87, 0x79, 0x68, 0x03, 0x72, 0xC0, 0x87, 0x0D, 0xAF, 0x50, 0x0E, 0x6D, 0xD0, 0x0E,
                0x7A, 0x50, 0x0E, 0x6D, 0x00, 0x0F, 0x7A, 0x30, 0x07, 0x72, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D,
                0x90, 0x0E, 0x71, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x90, 0x0E, 0x78, 0xA0, 0x07, 0x73, 0x20,
                0x07, 0x6D, 0x90, 0x0E, 0x71, 0x60, 0x07, 0x7A, 0x30, 0x07, 0x72, 0xD0, 0x06, 0xE9, 0x30, 0x07,
                0x72, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x90, 0x0E, 0x76, 0x40, 0x07, 0x7A, 0x60, 0x07, 0x74,
                0xD0, 0x06, 0xE6, 0x10, 0x07, 0x76, 0xA0, 0x07, 0x73, 0x20, 0x07, 0x6D, 0x60, 0x0E, 0x73, 0x20,
                0x07, 0x7A, 0x30, 0x07, 0x72, 0xD0, 0x06, 0xE6, 0x60, 0x07, 0x74, 0xA0, 0x07, 0x76, 0x40, 0x07,
                0x6D, 0xE0, 0x0E, 0x78, 0xA0, 0x07, 0x71, 0x60, 0x07, 0x7A, 0x30, 0x07, 0x72, 0xA0, 0x07, 0x76,
                0x40, 0x07, 0x43, 0x9E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86,
                0x3C, 0x04, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x81, 0x00, 0x00,
                0x06, 0x00, 0x00, 0x00, 0x32, 0x1E, 0x98, 0x0C, 0x19, 0x11, 0x4C, 0x90, 0x8C, 0x09, 0x26, 0x47,
                0xC6, 0x04, 0x43, 0x5A, 0x09, 0x94, 0x42, 0x49, 0x14, 0x43, 0x21, 0x00, 0x79, 0x18, 0x00, 0x00,
                0x2B, 0x00, 0x00, 0x00, 0x1A, 0x03, 0x4C, 0x90, 0x46, 0x02, 0x13, 0x44, 0x8F, 0x0C, 0x6F, 0xEC,
                0xED, 0x4D, 0x0C, 0x24, 0xC6, 0x25, 0xC7, 0x45, 0xC6, 0x06, 0x46, 0xC6, 0x25, 0xE6, 0x06, 0x04,
                0x45, 0x26, 0x86, 0x4C, 0x06, 0xC7, 0xEC, 0x46, 0xE6, 0x26, 0x65, 0x43, 0x10, 0x4C, 0x10, 0x06,
                0x61, 0x82, 0x30, 0x0C, 0x1B, 0x84, 0x81, 0x98, 0x20, 0x0C, 0xC4, 0x06, 0x61, 0x30, 0x28, 0x8C,
                0xCD, 0x4D, 0x10, 0x86, 0x62, 0xC3, 0x80, 0x24, 0xC4, 0x04, 0x41, 0x00, 0x48, 0xB4, 0x85, 0xA5,
                0xB9, 0x4D, 0x10, 0x06, 0x63, 0xC3, 0x30, 0x0C, 0xC3, 0x06, 0xA1, 0x71, 0x36, 0x14, 0x0B, 0x03,
                0x00, 0x4F, 0x15, 0x36, 0x36, 0xBB, 0x36, 0x97, 0x34, 0xB2, 0x32, 0x37, 0xBA, 0x29, 0x41, 0x50,
                0x85, 0x0C, 0xCF, 0xC5, 0xAE, 0x4C, 0x6E, 0x2E, 0xED, 0xCD, 0x6D, 0x4A, 0x40, 0x34, 0x21, 0xC3,
                0x73, 0xB1, 0x0B, 0x63, 0xB3, 0x2B, 0x93, 0x9B, 0x12, 0x18, 0x75, 0xC8, 0xF0, 0x5C, 0xE6, 0xD0,
                0xC2, 0xC8, 0xCA, 0xE4, 0x9A, 0xDE, 0xC8, 0xCA, 0xD8, 0xA6, 0x04, 0x49, 0x1D, 0x32, 0x3C, 0x97,
                0x32, 0x37, 0x3A, 0xB9, 0x3C, 0xA8, 0xB7, 0x34, 0x37, 0xBA, 0xB9, 0x29, 0xC1, 0x03, 0x00, 0x00,
                0x79, 0x18, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x33, 0x08, 0x80, 0x1C, 0xC4, 0xE1, 0x1C, 0x66,
                0x14, 0x01, 0x3D, 0x88, 0x43, 0x38, 0x84, 0xC3, 0x8C, 0x42, 0x80, 0x07, 0x79, 0x78, 0x07, 0x73,
                0x98, 0x71, 0x0C, 0xE6, 0x00, 0x0F, 0xED, 0x10, 0x0E, 0xF4, 0x80, 0x0E, 0x33, 0x0C, 0x42, 0x1E,
                0xC2, 0xC1, 0x1D, 0xCE, 0xA1, 0x1C, 0x66, 0x30, 0x05, 0x3D, 0x88, 0x43, 0x38, 0x84, 0x83, 0x1B,
                0xCC, 0x03, 0x3D, 0xC8, 0x43, 0x3D, 0x8C, 0x03, 0x3D, 0xCC, 0x78, 0x8C, 0x74, 0x70, 0x07, 0x7B,
                0x08, 0x07, 0x79, 0x48, 0x87, 0x70, 0x70, 0x07, 0x7A, 0x70, 0x03, 0x76, 0x78, 0x87, 0x70, 0x20,
                0x87, 0x19, 0xCC, 0x11, 0x0E, 0xEC, 0x90, 0x0E, 0xE1, 0x30, 0x0F, 0x6E, 0x30, 0x0F, 0xE3, 0xF0,
                0x0E, 0xF0, 0x50, 0x0E, 0x33, 0x10, 0xC4, 0x1D, 0xDE, 0x21, 0x1C, 0xD8, 0x21, 0x1D, 0xC2, 0x61,
                0x1E, 0x66, 0x30, 0x89, 0x3B, 0xBC, 0x83, 0x3B, 0xD0, 0x43, 0x39, 0xB4, 0x03, 0x3C, 0xBC, 0x83,
                0x3C, 0x84, 0x03, 0x3B, 0xCC, 0xF0, 0x14, 0x76, 0x60, 0x07, 0x7B, 0x68, 0x07, 0x37, 0x68, 0x87,
                0x72, 0x68, 0x07, 0x37, 0x80, 0x87, 0x70, 0x90, 0x87, 0x70, 0x60, 0x07, 0x76, 0x28, 0x07, 0x76,
                0xF8, 0x05, 0x76, 0x78, 0x87, 0x77, 0x80, 0x87, 0x5F, 0x08, 0x87, 0x71, 0x18, 0x87, 0x72, 0x98,
                0x87, 0x79, 0x98, 0x81, 0x2C, 0xEE, 0xF0, 0x0E, 0xEE, 0xE0, 0x0E, 0xF5, 0xC0, 0x0E, 0xEC, 0x30,
                0x03, 0x62, 0xC8, 0xA1, 0x1C, 0xE4, 0xA1, 0x1C, 0xCC, 0xA1, 0x1C, 0xE4, 0xA1, 0x1C, 0xDC, 0x61,
                0x1C, 0xCA, 0x21, 0x1C, 0xC4, 0x81, 0x1D, 0xCA, 0x61, 0x06, 0xD6, 0x90, 0x43, 0x39, 0xC8, 0x43,
                0x39, 0x98, 0x43, 0x39, 0xC8, 0x43, 0x39, 0xB8, 0xC3, 0x38, 0x94, 0x43, 0x38, 0x88, 0x03, 0x3B,
                0x94, 0xC3, 0x2F, 0xBC, 0x83, 0x3C, 0xFC, 0x82, 0x3B, 0xD4, 0x03, 0x3B, 0xB0, 0xC3, 0x0C, 0xC4,
                0x21, 0x07, 0x7C, 0x70, 0x03, 0x7A, 0x28, 0x87, 0x76, 0x80, 0x87, 0x19, 0xD1, 0x43, 0x0E, 0xF8,
                0xE0, 0x06, 0xE4, 0x20, 0x0E, 0xE7, 0xE0, 0x06, 0xF6, 0x10, 0x0E, 0xF2, 0xC0, 0x0E, 0xE1, 0x90,
                0x0F, 0xEF, 0x50, 0x0F, 0xF4, 0x00, 0x00, 0x00, 0x71, 0x20, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
                0x16, 0xD0, 0x0C, 0x97, 0xEF, 0x3C, 0x7E, 0x00, 0x44, 0x11, 0x42, 0x44, 0x06, 0x40, 0x30, 0x00,
                0xD2, 0x00, 0x00, 0x00, 0x61, 0x20, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x13, 0x04, 0x41, 0x2C,
                0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xB4, 0x02, 0x0D, 0x28, 0x08, 0x00, 0x00, 0x00,
                0x23, 0x06, 0x06, 0x00, 0x82, 0x60, 0x40, 0x20, 0x42, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            HRESULT fbHr = E_FAIL;
            if (pDesc->pRootSignature) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC fbDesc{};
                fbDesc.pRootSignature = pDesc->pRootSignature;
                fbDesc.CS.pShaderBytecode = kNoopComputeDxbc;
                fbDesc.CS.BytecodeLength = sizeof(kNoopComputeDxbc);
                fbHr = m_device->CreateComputePipelineState(&fbDesc, riid, ppPipelineState);
            }
            if (FAILED(fbHr) || !*ppPipelineState) {
                fbHr = ReturnNoopPso(true, riid, ppPipelineState);
            }
            static std::atomic<uint32_t> s_cmpFbLogs{0};
            if (FAILED(fbHr) || s_cmpFbLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::string csInfo = sc::d3d12::InspectShaderBytecode(pDesc->CS);
                char msg[384];
                std::snprintf(msg, sizeof(msg),
                    "-> CmpPSO origHr=0x%08lX -> NOOP FALLBACK hr=0x%08lX pso=%p (csStrip=%d) CS=[%s]",
                    static_cast<unsigned long>(hr), static_cast<unsigned long>(fbHr),
                    *ppPipelineState, csS.wasStripped ? 1 : 0, csInfo.c_str());
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateCmpPSO", msg);
            }
            if (SUCCEEDED(fbHr)) {
                g_xboxStats.psoCmpOk.fetch_add(1, std::memory_order_relaxed);
                return fbHr;
            }
        }
        const uint64_t n = SUCCEEDED(hr)
            ? g_xboxStats.psoCmpOk.fetch_add(1, std::memory_order_relaxed)
            : g_xboxStats.psoCmpFail.fetch_add(1, std::memory_order_relaxed);
        if (n < 5) {
            std::string csInfo = sc::d3d12::InspectShaderBytecode(clean.CS);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "-> CmpPSO hr=0x%08lX (csStrip=%d) CS=[%s]",
                          static_cast<unsigned long>(hr), csS.wasStripped ? 1 : 0, csInfo.c_str());
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateCmpPSO", msg);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;
        ComPtr<ID3D12GraphicsCommandList> nativeCl;
        HRESULT hr = m_device->CreateCommandList(nodeMask, SanitizeCommandListType(type),
                                                 XboxD3D12CommandAllocator::Unwrap(pCommandAllocator), pInitialState, IID_PPV_ARGS(&nativeCl));
        if (FAILED(hr)) return hr;
        if (IsEqualGUID(riid, IID_ID3D12XboxDmaCommandList)) {
            ComPtr<XboxD3D12DmaCommandList> wrapped;
            wrapped.Attach(new XboxD3D12DmaCommandList(std::move(nativeCl), this));
            return wrapped->QueryInterface(riid, ppCommandList);
        }
        ComPtr<XboxD3D12GraphicsCommandList> wrapped;
        wrapped.Attach(new XboxD3D12GraphicsCommandList(std::move(nativeCl), this));
        return wrapped->QueryInterface(riid, ppCommandList);
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override {
        if (!pFeatureSupportData) return E_INVALIDARG;
        if (Feature == D3D12_FEATURE_D3D12_OPTIONS &&
            FeatureSupportDataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS pcOpts{};
            HRESULT hr = m_device->CheckFeatureSupport(Feature, &pcOpts, sizeof(pcOpts));
            if (SUCCEEDED(hr)) {
                std::memset(pFeatureSupportData, 0, FeatureSupportDataSize);
                std::memcpy(pFeatureSupportData, &pcOpts,
                            std::min<size_t>(sizeof(pcOpts), FeatureSupportDataSize));
            }
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> D3D12_OPTIONS adapted guestSize=%u pcSize=%zu hr=0x%08lX",
                          FeatureSupportDataSize, sizeof(pcOpts), hr);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CheckFeatureSupport", msg);
            return hr;
        }
        HRESULT hr = m_device->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
        if (FAILED(hr)) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> FAILED feature=%d size=%u hr=0x%08lX",
                          static_cast<int>(Feature), FeatureSupportDataSize, hr);
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CheckFeatureSupport", msg);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override {
        if (!pDescriptorHeapDesc || !ppvHeap) return E_INVALIDARG;
        *ppvHeap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC clean = *pDescriptorHeapDesc;
        clean.NodeMask = 0;
        if (static_cast<UINT>(clean.Type) >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "-> remapped Xbox heap type %d -> CBV_SRV_UAV (0) (num=%u flags=0x%X)",
                          static_cast<int>(clean.Type), clean.NumDescriptors,
                          static_cast<unsigned>(clean.Flags));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateDescriptorHeap", msg);
            clean.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        }
        clean.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(
            static_cast<UINT>(clean.Flags) & static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE));
        if ((clean.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
            constexpr UINT kMaxShaderVisibleViews = 1000000;
            constexpr UINT kMaxShaderVisibleSamplers = 2048;
            if (clean.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && clean.NumDescriptors > kMaxShaderVisibleViews) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "-> clamped shader-visible CBV_SRV_UAV heap %u -> %u",
                              clean.NumDescriptors, kMaxShaderVisibleViews);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateDescriptorHeap", msg);
                clean.NumDescriptors = kMaxShaderVisibleViews;
            } else if (clean.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && clean.NumDescriptors > kMaxShaderVisibleSamplers) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "-> clamped shader-visible SAMPLER heap %u -> %u",
                              clean.NumDescriptors, kMaxShaderVisibleSamplers);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateDescriptorHeap", msg);
                clean.NumDescriptors = kMaxShaderVisibleSamplers;
            }
        }
        ComPtr<ID3D12DescriptorHeap> nativeHeap;
        HRESULT hr = m_device->CreateDescriptorHeap(&clean, IID_PPV_ARGS(&nativeHeap));
        if (FAILED(hr)) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> FAILED type=%d num=%u flags=0x%X hr=0x%08lX",
                          static_cast<int>(clean.Type), clean.NumDescriptors,
                          static_cast<unsigned>(clean.Flags), hr);
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateDescriptorHeap", msg);
            return hr;
        }
        const UINT inc = GetDescriptorHandleIncrementSize(clean.Type);
        const SIZE_T origBytes = static_cast<SIZE_T>(pDescriptorHeapDesc->NumDescriptors) * static_cast<SIZE_T>(inc);
        SIZE_T guestCpuStart = 0;
        if (const auto* hooks = GetTitleXMemAllocationHooks(); hooks && hooks->pAlloc && origBytes > 0) {
            void* guestPtr = hooks->pAlloc(origBytes, kXboxD3DDescriptorHeapAllocAttrs);
            if (guestPtr) {
                guestCpuStart = reinterpret_cast<SIZE_T>(guestPtr);
                char msg[224];
                std::snprintf(msg, sizeof(msg),
                              "-> placed heap type=%d origNum=%u clampedNum=%u inc=%u bytes=0x%llX guestVA=0x%llX nativeCPU=0x%llX",
                              static_cast<int>(clean.Type), pDescriptorHeapDesc->NumDescriptors,
                              clean.NumDescriptors, inc,
                              static_cast<unsigned long long>(origBytes),
                              static_cast<unsigned long long>(guestCpuStart),
                              static_cast<unsigned long long>(nativeHeap->GetCPUDescriptorHandleForHeapStart().ptr));
                sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateDescriptorHeap", msg);
            }
        }
        const bool shaderVisible = (clean.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
        ComPtr<XboxD3D12DescriptorHeap> wrapped;
        wrapped.Attach(new XboxD3D12DescriptorHeap(
            std::move(nativeHeap), this, shaderVisible, *pDescriptorHeapDesc, guestCpuStart, origBytes, inc));
        return wrapped->QueryInterface(riid, ppvHeap);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override {
        if (static_cast<UINT>(DescriptorHeapType) >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
            return m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return m_device->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
        REFIID riid, void** ppvRootSignature) override {
        HRESULT hr = m_device->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
        const uint64_t n = SUCCEEDED(hr)
            ? g_xboxStats.rsOk.fetch_add(1, std::memory_order_relaxed)
            : g_xboxStats.rsFail.fetch_add(1, std::memory_order_relaxed);
        if (FAILED(hr) && n < 5) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "-> CreateRootSignature FAILED hr=0x%08lX bytes=%zu",
                          static_cast<unsigned long>(hr), blobLengthInBytes);
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateRootSig", msg);
        }
        return hr;
    }

    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        if (!pDesc) {
            m_device->CreateConstantBufferView(nullptr, TranslateCpuDescriptorHandle(DestDescriptor));
            return;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC clean = *pDesc;
        const UINT64 origLoc = clean.BufferLocation;
        const UINT origSize = clean.SizeInBytes;
        const UINT64 delta = origLoc & 255ULL;
        const UINT alignedSize = static_cast<UINT>(std::max<UINT64>(256ULL, (static_cast<UINT64>(origSize) + 255ULL) & ~255ULL));
        if (delta != 0ULL && origLoc != 0ULL && origSize > 0u) {
            D3D12_GPU_VIRTUAL_ADDRESS bouncedGpu = 0;
            void* bouncedCpu = AllocCbvBounceSlot(alignedSize, bouncedGpu);
            bool nonZero = false;
            float f[4]{};
            if (bouncedCpu && bouncedGpu != 0 &&
                XboxD3D12Resource::CopyFromMappedUploadBuffer(origLoc, origSize, bouncedCpu, alignedSize, nonZero, f)) {
                clean.BufferLocation = bouncedGpu;
                clean.SizeInBytes = alignedSize;
                const uint64_t n = g_xboxStats.cbvBounced.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                                  "-> CBV bounced #%llu 0x%llX(+%llu,%uB) -> 0x%llX(%uB) nz=%d f=(%.2f,%.2f,%.2f,%.2f)",
                                  static_cast<unsigned long long>(n),
                                  static_cast<unsigned long long>(origLoc),
                                  static_cast<unsigned long long>(delta), origSize,
                                  static_cast<unsigned long long>(bouncedGpu), alignedSize,
                                  nonZero ? 1 : 0, f[0], f[1], f[2], f[3]);
                    sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CBV", msg);
                }
            } else {
                bool bouncedGuestVa = false;
                if (bouncedCpu && bouncedGpu != 0) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (::VirtualQuery(reinterpret_cast<LPCVOID>(origLoc), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                        mbi.State == MEM_COMMIT &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
                        const uint64_t regionEnd = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
                        const uint64_t copyN = std::min<uint64_t>(origSize, (regionEnd > origLoc) ? (regionEnd - origLoc) : 0ULL);
                        if (copyN > 0) {
                            std::memcpy(bouncedCpu, reinterpret_cast<const void*>(origLoc), static_cast<size_t>(copyN));
                        }
                        if (copyN < alignedSize) {
                            std::memset(static_cast<uint8_t*>(bouncedCpu) + copyN, 0, static_cast<size_t>(alignedSize - copyN));
                        }
                        clean.BufferLocation = bouncedGpu;
                        clean.SizeInBytes = alignedSize;
                        bouncedGuestVa = true;
                        const uint64_t n = g_xboxStats.cbvBounced.fetch_add(1, std::memory_order_relaxed);
                        if (n < 8) {
                            char msg[200];
                            std::snprintf(msg, sizeof(msg),
                                          "-> CBV guestVA bounced #%llu 0x%llX(+%llu,%uB) -> 0x%llX(%uB)",
                                          static_cast<unsigned long long>(n),
                                          static_cast<unsigned long long>(origLoc),
                                          static_cast<unsigned long long>(delta), origSize,
                                          static_cast<unsigned long long>(bouncedGpu), alignedSize);
                            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CBV", msg);
                        }
                    }
                }
                if (!bouncedGuestVa) {
                    const uint64_t miss = g_xboxStats.cbvBounceMiss.fetch_add(1, std::memory_order_relaxed);
                    if (miss < 8) {
                        char msg[160];
                        std::snprintf(msg, sizeof(msg), "-> CBV bounce MISS #%llu loc=0x%llX delta=%llu size=%u",
                                      static_cast<unsigned long long>(miss),
                                      static_cast<unsigned long long>(origLoc),
                                      static_cast<unsigned long long>(delta), origSize);
                        sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CBV", msg);
                    }
                    clean.BufferLocation = origLoc & ~255ULL;
                    const UINT64 totalSize = delta + static_cast<UINT64>(origSize);
                    clean.SizeInBytes = static_cast<UINT>(std::max<UINT64>(256ULL, (totalSize + 255ULL) & ~255ULL));
                }
            }
        } else if (origLoc != 0ULL && origSize > 0u) {
            clean.BufferLocation = origLoc & ~255ULL;
            clean.SizeInBytes = alignedSize;

            // On Xbox unified memory, titles pass guest CPU VAs (from XMemVirtualAlloc) directly as CBV BufferLocation.
            // If origLoc is a committed readable CPU VA (and not inside our host CBV bounce ring), copy into a host upload bounce slot.
            const bool inBounceRing = (m_cbvBounceGpu != 0ULL &&
                origLoc >= m_cbvBounceGpu && origLoc < m_cbvBounceGpu + 16ULL * 1024ULL * 1024ULL);
            if (!inBounceRing) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (::VirtualQuery(reinterpret_cast<LPCVOID>(origLoc), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
                    const uint64_t regionEnd = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
                    const uint64_t avail = (regionEnd > origLoc) ? (regionEnd - origLoc) : 0ULL;
                    D3D12_GPU_VIRTUAL_ADDRESS bouncedGpu = 0;
                    void* bouncedCpu = AllocCbvBounceSlot(alignedSize, bouncedGpu);
                    if (bouncedCpu && bouncedGpu != 0 && avail > 0) {
                        const uint64_t copyN = std::min<uint64_t>(origSize, std::min<uint64_t>(avail, alignedSize));
                        std::memcpy(bouncedCpu, reinterpret_cast<const void*>(origLoc), static_cast<size_t>(copyN));
                        if (copyN < alignedSize) {
                            std::memset(static_cast<uint8_t*>(bouncedCpu) + copyN, 0, static_cast<size_t>(alignedSize - copyN));
                        }
                        clean.BufferLocation = bouncedGpu;
                        clean.SizeInBytes = alignedSize;
                        static std::atomic<uint64_t> s_cpuCbvBounced{0};
                        const uint64_t bn = s_cpuCbvBounced.fetch_add(1, std::memory_order_relaxed);
                        if (bn < 8) {
                            char msg[200];
                            std::snprintf(msg, sizeof(msg),
                                "-> CBV guest-CPU-VA bounced #%llu 0x%llX(%uB) -> 0x%llX(%uB)",
                                static_cast<unsigned long long>(bn),
                                static_cast<unsigned long long>(origLoc), origSize,
                                static_cast<unsigned long long>(bouncedGpu), alignedSize);
                            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CBV", msg);
                        }
                    }
                }
            }
        } else {
            clean.BufferLocation = origLoc & ~255ULL;
            clean.SizeInBytes = alignedSize;
        }
        m_device->CreateConstantBufferView(&clean, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateShaderResourceView(XboxD3D12Resource::Unwrap(pResource), pDesc, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateUnorderedAccessView(XboxD3D12Resource::Unwrap(pResource), XboxD3D12Resource::Unwrap(pCounterResource), pDesc, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateRenderTargetView(XboxD3D12Resource::Unwrap(pResource), pDesc, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateDepthStencilView(XboxD3D12Resource::Unwrap(pResource), pDesc, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        m_device->CreateSampler(pDesc, TranslateCpuDescriptorHandle(DestDescriptor));
    }
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dstStarts;
        const D3D12_CPU_DESCRIPTOR_HANDLE* dstPtr = pDestDescriptorRangeStarts;
        if (pDestDescriptorRangeStarts && NumDestDescriptorRanges > 0) {
            dstStarts.resize(NumDestDescriptorRanges);
            for (UINT i = 0; i < NumDestDescriptorRanges; ++i)
                dstStarts[i] = TranslateCpuDescriptorHandle(pDestDescriptorRangeStarts[i]);
            dstPtr = dstStarts.data();
        }
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srcStarts;
        const D3D12_CPU_DESCRIPTOR_HANDLE* srcPtr = pSrcDescriptorRangeStarts;
        if (pSrcDescriptorRangeStarts && NumSrcDescriptorRanges > 0) {
            srcStarts.resize(NumSrcDescriptorRanges);
            for (UINT i = 0; i < NumSrcDescriptorRanges; ++i)
                srcStarts[i] = TranslateCpuDescriptorHandle(pSrcDescriptorRangeStarts[i]);
            srcPtr = srcStarts.data();
        }
        m_device->CopyDescriptors(NumDestDescriptorRanges, dstPtr, pDestDescriptorRangeSizes,
                                  NumSrcDescriptorRanges, srcPtr, pSrcDescriptorRangeSizes, DescriptorHeapsType);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        m_device->CopyDescriptorsSimple(NumDescriptors, TranslateCpuDescriptorHandle(DestDescriptorRangeStart), TranslateCpuDescriptorHandle(SrcDescriptorRangeStart), DescriptorHeapsType);
    }

    static constexpr size_t kXboxDeviceVtblSlots = 144;
    inline static std::once_flag s_vtblOnce{};
    // Index 0 holds MSVC RTTI complete object locator (origVtbl[-1]); slots start at index 1.
    inline static void* s_patchedVtblBuf[kXboxDeviceVtblSlots + 1]{};

    static void* const* PatchedVtbl() { return &s_patchedVtblBuf[1]; }

    void InstallXboxCAbiVtable() {
        void* const* origVtbl = *reinterpret_cast<void* const**>(this);
        std::call_once(s_vtblOnce, [origVtbl]() {
            s_patchedVtblBuf[0] = origVtbl[-1];
            std::memcpy(&s_patchedVtblBuf[1], origVtbl, kXboxDeviceVtblSlots * sizeof(void*));
            s_patchedVtblBuf[1 + 25] = reinterpret_cast<void*>(&ThunkGetResourceAllocationInfo);
            s_patchedVtblBuf[1 + 26] = reinterpret_cast<void*>(&ThunkGetCustomHeapProperties);
            s_patchedVtblBuf[1 + 43] = reinterpret_cast<void*>(&ThunkGetAdapterLuid);
            static_assert(kXboxDeviceVtblSlots > 108, "vtable must cover slot 108");
            s_patchedVtblBuf[1 + 108] = reinterpret_cast<void*>(&ThunkGetResourceAllocationInfo1);
        });
        *reinterpret_cast<void***>(this) = &s_patchedVtblBuf[1];
    }

    D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfoImpl(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
        if (!pResourceDescs || numResourceDescs == 0) {
            return m_device->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
        }
        std::vector<D3D12_RESOURCE_DESC> clean;
        clean.reserve(numResourceDescs);
        for (UINT i = 0; i < numResourceDescs; ++i) {
            clean.push_back(SanitizeXboxResourceDesc(pResourceDescs[i], "XboxD3D12Device::GetResourceAllocationInfo"));
        }
        D3D12_RESOURCE_ALLOCATION_INFO info = m_device->GetResourceAllocationInfo(visibleMask, numResourceDescs, clean.data());
        if (info.SizeInBytes == UINT64_MAX) {
            for (auto& d : clean) {
                d.Alignment = 0;
                d.Layout = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                    ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                    : D3D12_TEXTURE_LAYOUT_UNKNOWN;
            }
            info = m_device->GetResourceAllocationInfo(visibleMask, numResourceDescs, clean.data());
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "-> retried with Alignment=0 (count=%u) -> size=%llu align=%llu",
                          numResourceDescs,
                          static_cast<unsigned long long>(info.SizeInBytes),
                          static_cast<unsigned long long>(info.Alignment));
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::GetResourceAllocationInfo", msg);
        }
        return info;
    }

    static D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE ThunkGetResourceAllocationInfo(
        void* arg0, void* arg1, UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
        XboxD3D12Device* self = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO* out = nullptr;
        if (arg0 && *reinterpret_cast<void* const**>(arg0) == PatchedVtbl()) {
            self = static_cast<XboxD3D12Device*>(arg0);
            out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg1);
        } else {
            out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg0);
            self = static_cast<XboxD3D12Device*>(arg1);
        }
        D3D12_RESOURCE_ALLOCATION_INFO res{};
        if (self) {
            res = self->GetResourceAllocationInfoImpl(visibleMask, numResourceDescs, pResourceDescs);
        }
        if (out) {
            *out = res;
        }
        return out;
    }

    D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo1Impl(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        if (!pResourceDescs || numResourceDescs == 0) {
            return D3D12_RESOURCE_ALLOCATION_INFO{0, 0};
        }
        std::vector<D3D12_RESOURCE_DESC> clean;
        clean.reserve(numResourceDescs);
        for (UINT i = 0; i < numResourceDescs; ++i) {
            clean.push_back(SanitizeXboxResourceDesc(pResourceDescs[i], "XboxD3D12Device::GetResourceAllocationInfo1"));
        }

        ComPtr<ID3D12Device4> dev4;
        D3D12_RESOURCE_ALLOCATION_INFO info{UINT64_MAX, 0};
        if (SUCCEEDED(m_device.As(&dev4)) && dev4) {
            info = dev4->GetResourceAllocationInfo1(visibleMask, numResourceDescs, clean.data(), pResourceAllocationInfo1);
            if (info.SizeInBytes == UINT64_MAX) {
                for (auto& d : clean) {
                    d.Alignment = 0;
                    d.Layout = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                        ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                        : D3D12_TEXTURE_LAYOUT_UNKNOWN;
                    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                            static_cast<uint32_t>(d.Flags) &
                            ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
                    }
                }
                info = dev4->GetResourceAllocationInfo1(visibleMask, numResourceDescs, clean.data(), pResourceAllocationInfo1);
            }
        } else {
            info = GetResourceAllocationInfoImpl(visibleMask, numResourceDescs, clean.data());
            if (pResourceAllocationInfo1) {
                uint64_t off = 0;
                for (UINT i = 0; i < numResourceDescs; ++i) {
                    D3D12_RESOURCE_ALLOCATION_INFO single = GetResourceAllocationInfoImpl(visibleMask, 1, &clean[i]);
                    const uint64_t a = single.Alignment ? single.Alignment : 65536ULL;
                    off = (off + a - 1ULL) & ~(a - 1ULL);
                    pResourceAllocationInfo1[i].Offset = off;
                    pResourceAllocationInfo1[i].Alignment = single.Alignment;
                    pResourceAllocationInfo1[i].SizeInBytes = single.SizeInBytes;
                    off += single.SizeInBytes;
                }
            }
        }

        static std::atomic<uint32_t> s_slot108Log{0};
        if (info.SizeInBytes == UINT64_MAX || s_slot108Log.fetch_add(1, std::memory_order_relaxed) < 6) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "-> slot108 count=%u dim=%u w=%llu h=%u fmt=%u -> size=%llu align=%llu",
                          numResourceDescs,
                          static_cast<unsigned>(clean[0].Dimension),
                          static_cast<unsigned long long>(clean[0].Width),
                          clean[0].Height,
                          static_cast<unsigned>(clean[0].Format),
                          static_cast<unsigned long long>(info.SizeInBytes),
                          static_cast<unsigned long long>(info.Alignment));
            sc::log::Write(info.SizeInBytes == UINT64_MAX ? sc::log::Level::Warn : sc::log::Level::Info,
                           "d3d12_xs", "XboxD3D12Device::GetResourceAllocationInfo1", msg);
        }
        return info;
    }

    static D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE ThunkGetResourceAllocationInfo1(
        void* arg0, void* arg1, UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        XboxD3D12Device* self = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO* out = nullptr;
        if (arg0 && *reinterpret_cast<void* const**>(arg0) == PatchedVtbl()) {
            self = static_cast<XboxD3D12Device*>(arg0);
            out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg1);
        } else {
            out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg0);
            self = static_cast<XboxD3D12Device*>(arg1);
        }
        D3D12_RESOURCE_ALLOCATION_INFO res{};
        if (self) {
            res = self->GetResourceAllocationInfo1Impl(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        }
        if (out) {
            *out = res;
        }
        return out;
    }

    static D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE ThunkGetCustomHeapProperties(
        void* arg0, void* arg1, UINT nodeMask, D3D12_HEAP_TYPE heapType) {
        XboxD3D12Device* self = nullptr;
        D3D12_HEAP_PROPERTIES* out = nullptr;
        if (arg0 && *reinterpret_cast<void* const**>(arg0) == PatchedVtbl()) {
            self = static_cast<XboxD3D12Device*>(arg0);
            out = static_cast<D3D12_HEAP_PROPERTIES*>(arg1);
        } else {
            out = static_cast<D3D12_HEAP_PROPERTIES*>(arg0);
            self = static_cast<XboxD3D12Device*>(arg1);
        }
        D3D12_HEAP_PROPERTIES res{};
        if (self) {
            res = self->m_device->GetCustomHeapProperties(nodeMask, heapType);
        }
        if (out) {
            *out = res;
        }
        return out;
    }

    static uint64_t STDMETHODCALLTYPE ThunkGetAdapterLuid(void* selfPtr) {
        auto* self = static_cast<XboxD3D12Device*>(selfPtr);
        LUID luid = self ? self->m_device->GetAdapterLuid() : LUID{0, 0};
        uint64_t packed = 0;
        std::memcpy(&packed, &luid, sizeof(packed));
        return packed;
    }

    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override {
        return GetResourceAllocationInfoImpl(visibleMask, numResourceDescs, pResourceDescs);
    }

    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override {
        return m_device->GetCustomHeapProperties(nodeMask, heapType);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override {
        D3D12_RESOURCE_DESC clean{};
        const D3D12_RESOURCE_DESC* descPtr = pDesc;
        if (pDesc) {
            clean = SanitizeXboxResourceDesc(*pDesc, "XboxD3D12Device::CreateCommittedResource");
            descPtr = &clean;
        }
        const bool isCpuBuffer =
            descPtr && descPtr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pHeapProperties &&
            (pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD ||
             (pHeapProperties->Type == D3D12_HEAP_TYPE_CUSTOM &&
              pHeapProperties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
              pHeapProperties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE));
        if (descPtr && (descPtr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || isCpuBuffer) && ppvResource &&
            (IsEqualGUID(riidResource, __uuidof(ID3D12Resource)) || IsEqualGUID(riidResource, __uuidof(IUnknown)))) {
            *ppvResource = nullptr;
            ComPtr<ID3D12Resource> nativeRes;
            HRESULT hr = m_device->CreateCommittedResource(pHeapProperties, HeapFlags, descPtr,
                                                           InitialResourceState, pOptimizedClearValue,
                                                           IID_PPV_ARGS(&nativeRes));
            if (SUCCEEDED(hr) && nativeRes) {
                D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, descPtr);
                uint64_t sz = (ai.SizeInBytes != UINT64_MAX) ? ai.SizeInBytes : 0;
                *ppvResource = static_cast<ID3D12Resource*>(new XboxD3D12Resource(std::move(nativeRes), this, sz, InitialResourceState));
            }
            return hr;
        }
        return m_device->CreateCommittedResource(pHeapProperties, HeapFlags, descPtr,
                                                 InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
    }

    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        return m_device->CreateHeap(pDesc, riid, ppvHeap);
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override {
        D3D12_RESOURCE_DESC clean{};
        const D3D12_RESOURCE_DESC* descPtr = pDesc;
        if (pDesc) {
            clean = SanitizeXboxResourceDesc(*pDesc, "XboxD3D12Device::CreatePlacedResource");
            descPtr = &clean;
        }
        bool isCpuPlacedBuf = false;
        if (descPtr && descPtr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pHeap) {
            const D3D12_HEAP_DESC hd = pHeap->GetDesc();
            isCpuPlacedBuf = (hd.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) ||
                             (hd.Properties.Type == D3D12_HEAP_TYPE_CUSTOM &&
                              hd.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
                              hd.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
        }
        if (descPtr && (descPtr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || isCpuPlacedBuf) && ppvResource &&
            (IsEqualGUID(riid, __uuidof(ID3D12Resource)) || IsEqualGUID(riid, __uuidof(IUnknown)))) {
            *ppvResource = nullptr;
            ComPtr<ID3D12Resource> nativeRes;
            HRESULT hr = m_device->CreatePlacedResource(pHeap, HeapOffset, descPtr,
                                                        InitialState, pOptimizedClearValue,
                                                        IID_PPV_ARGS(&nativeRes));
            if (SUCCEEDED(hr) && nativeRes) {
                D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, descPtr);
                uint64_t sz = (ai.SizeInBytes != UINT64_MAX) ? ai.SizeInBytes : 0;
                *ppvResource = static_cast<ID3D12Resource*>(new XboxD3D12Resource(std::move(nativeRes), this, sz, InitialState));
            }
            return hr;
        }
        return m_device->CreatePlacedResource(pHeap, HeapOffset, descPtr,
                                              InitialState, pOptimizedClearValue, riid, ppvResource);
    }

    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override {
        D3D12_RESOURCE_DESC clean{};
        const D3D12_RESOURCE_DESC* descPtr = pDesc;
        if (pDesc) {
            clean = SanitizeXboxResourceDesc(*pDesc, "XboxD3D12Device::CreateReservedResource");
            descPtr = &clean;
        }
        if (descPtr && descPtr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && ppvResource &&
            (IsEqualGUID(riid, __uuidof(ID3D12Resource)) || IsEqualGUID(riid, __uuidof(IUnknown)))) {
            *ppvResource = nullptr;
            ComPtr<ID3D12Resource> nativeRes;
            HRESULT hr = m_device->CreateReservedResource(descPtr, InitialState, pOptimizedClearValue,
                                                          IID_PPV_ARGS(&nativeRes));
            if (SUCCEEDED(hr) && nativeRes) {
                D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, descPtr);
                uint64_t sz = (ai.SizeInBytes != UINT64_MAX) ? ai.SizeInBytes : 0;
                *ppvResource = static_cast<ID3D12Resource*>(new XboxD3D12Resource(std::move(nativeRes), this, sz));
            }
            return hr;
        }
        return m_device->CreateReservedResource(descPtr, InitialState, pOptimizedClearValue, riid, ppvResource);
    }

    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
        DWORD Access, LPCWSTR Name, HANDLE* pHandle) override {
        return m_device->CreateSharedHandle(XboxD3D12Resource::UnwrapDeviceChild(pObject), pAttributes, Access, Name, pHandle);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) override {
        return m_device->OpenSharedHandle(NTHandle, riid, ppvObj);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override {
        return m_device->OpenSharedHandleByName(Name, Access, pNTHandle);
    }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        if (!ppObjects || NumObjects == 0) return m_device->MakeResident(NumObjects, ppObjects);
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = XboxD3D12Resource::UnwrapPageable(ppObjects[i]);
        return m_device->MakeResident(NumObjects, unwrapped.data());
    }
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        if (!ppObjects || NumObjects == 0) return m_device->Evict(NumObjects, ppObjects);
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = XboxD3D12Resource::UnwrapPageable(ppObjects[i]);
        return m_device->Evict(NumObjects, unwrapped.data());
    }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override {
        if (!ppFence) return E_POINTER;
        *ppFence = nullptr;
        const D3D12_FENCE_FLAGS cleanFlags = static_cast<D3D12_FENCE_FLAGS>(
            static_cast<uint32_t>(Flags) &
            static_cast<uint32_t>(D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D12_FENCE_FLAG_NON_MONITORED));
        ComPtr<ID3D12Fence> nativeFence;
        HRESULT hr = m_device->CreateFence(InitialValue, cleanFlags, IID_PPV_ARGS(&nativeFence));
        if (FAILED(hr) || !nativeFence) return hr;
        ComPtr<XboxD3D12Fence> wrapped;
        wrapped.Attach(new XboxD3D12Fence(std::move(nativeFence), InitialValue, cleanFlags, this));
        return wrapped->QueryInterface(riid, ppFence);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        return m_device->GetDeviceRemovedReason();
    }
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override {
        D3D12_RESOURCE_DESC clean{};
        const D3D12_RESOURCE_DESC* descPtr = pResourceDesc;
        if (pResourceDesc) {
            clean = SanitizeXboxResourceDesc(*pResourceDesc, "XboxD3D12Device::GetCopyableFootprints");
            descPtr = &clean;
        }
        m_device->GetCopyableFootprints(descPtr, FirstSubresource, NumSubresources, BaseOffset,
                                        pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        return m_device->CreateQueryHeap(pDesc, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override {
        return m_device->SetStablePowerState(Enable);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override {
        if (!pDesc || !ppvCommandSignature) return E_INVALIDARG;
        HRESULT hr = m_device->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature);
        if (SUCCEEDED(hr)) {
            return hr;
        }

        // Dump the exact Xbox descriptor that desktop D3D12 rejected
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
            "-> FAILED hr=0x%08lX stride=%u numArgs=%u nodeMask=0x%X rs=%p",
            static_cast<unsigned long>(hr), pDesc->ByteStride, pDesc->NumArgumentDescs,
            pDesc->NodeMask, static_cast<const void*>(pRootSignature));
        sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateCommandSignature", hdr);

        if (pDesc->pArgumentDescs && pDesc->NumArgumentDescs <= 32) {
            for (UINT i = 0; i < pDesc->NumArgumentDescs; ++i) {
                const auto& a = pDesc->pArgumentDescs[i];
                uint32_t rawWords[3]{};
                static_assert(sizeof(a) >= 16, "D3D12_INDIRECT_ARGUMENT_DESC layout");
                std::memcpy(rawWords, reinterpret_cast<const uint8_t*>(&a) + 4, sizeof(rawWords));
                char abuf[256];
                std::snprintf(abuf, sizeof(abuf),
                    "->   arg[%u] type=%u (0x%X) w0=0x%08X w1=0x%08X w2=0x%08X",
                    i, static_cast<unsigned>(a.Type), static_cast<unsigned>(a.Type),
                    rawWords[0], rawWords[1], rawWords[2]);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreateCommandSignature", abuf);
            }
        }

        // Fallback: preserve valid desktop args where possible, or create a valid Dispatch signature
        D3D12_INDIRECT_ARGUMENT_DESC fbArg{};
        fbArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC fbDesc{};
        fbDesc.ByteStride = (pDesc->ByteStride >= 12) ? pDesc->ByteStride : 12;
        fbDesc.NumArgumentDescs = 1;
        fbDesc.pArgumentDescs = &fbArg;
        fbDesc.NodeMask = pDesc->NodeMask;

        HRESULT fbHr = m_device->CreateCommandSignature(&fbDesc, nullptr, riid, ppvCommandSignature);
        char fbuf[160];
        std::snprintf(fbuf, sizeof(fbuf),
            "-> FALLBACK Dispatch cmdSig stride=%u hr=0x%08lX ptr=%p",
            fbDesc.ByteStride, static_cast<unsigned long>(fbHr), *ppvCommandSignature);
        sc::log::Write(SUCCEEDED(fbHr) ? sc::log::Level::Warn : sc::log::Level::Error,
            "d3d12_xs", "XboxD3D12Device::CreateCommandSignature", fbuf);
        return SUCCEEDED(fbHr) ? fbHr : hr;
    }
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonOpaqueMip,
        UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonOpaqueMip) override {
        m_device->GetResourceTiling(XboxD3D12Resource::Unwrap(pTiledResource), pNumTilesForEntireResource, pPackedMipDesc,
                                    pStandardTileShapeForNonOpaqueMip, pNumSubresourceTilings,
                                    FirstSubresourceTilingToGet, pSubresourceTilingsForNonOpaqueMip);
    }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override {
        return m_device->GetAdapterLuid();
    }

    // Xbox ID3D12Device extension slots (44..85, offsets +0x160..+0x2A8)
    SCARLETT_XBOX_DEV_EXT_STUB(44)
    SCARLETT_XBOX_DEV_EXT_STUB(45)
    SCARLETT_XBOX_DEV_EXT_STUB(46)
    SCARLETT_XBOX_DEV_EXT_STUB(47)

    // Slot 48 (+0x180): Xbox extended CreateCommandQueue
    virtual HRESULT STDMETHODCALLTYPE CreateCommandQueueXbox(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
        if (!pDesc || !ppCommandQueue) return E_INVALIDARG;
        *ppCommandQueue = nullptr;
        D3D12_COMMAND_QUEUE_DESC clean = SanitizeQueueDesc(*pDesc);
        ComPtr<ID3D12CommandQueue> nativeQueue;
        HRESULT hr = m_device->CreateCommandQueue(&clean, IID_PPV_ARGS(&nativeQueue));
        if (SUCCEEDED(hr)) {
            ComPtr<XboxD3D12CommandQueue> wrapped;
            wrapped.Attach(new XboxD3D12CommandQueue(m_factory, m_device, std::move(nativeQueue), this, pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT));
            hr = wrapped->QueryInterface(riid, ppCommandQueue);
        }
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "-> slot48 rawType=0x%X rawPri=%d rawFlags=0x%X -> cleanType=%d hr=0x%08lX",
                      static_cast<unsigned>(pDesc->Type), pDesc->Priority,
                      static_cast<unsigned>(pDesc->Flags), static_cast<int>(clean.Type), hr);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateCommandQueueXbox", msg);
        return hr;
    }

    SCARLETT_XBOX_DEV_EXT_STUB(49)
    SCARLETT_XBOX_DEV_EXT_STUB(50)
    SCARLETT_XBOX_DEV_EXT_STUB(51)
    SCARLETT_XBOX_DEV_EXT_STUB(52)
    SCARLETT_XBOX_DEV_EXT_STUB(53)
    SCARLETT_XBOX_DEV_EXT_STUB(54)
    // Slot 55 (+0x1B8): Derived/specialized pipeline state creation (pBaseObj, count, pDesc, riid, ppOut)
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineStateSlot55(
        void* pBaseObj,
        UINT count,
        const void* pDesc,
        REFIID riid,
        void** ppOut) {
        if (!ppOut) return E_POINTER;
        *ppOut = nullptr;
        HRESULT hr = ReturnNoopPso(false, riid, ppOut);
        static std::atomic<uint32_t> s_slot55Logs{0};
        if (FAILED(hr) || s_slot55Logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "-> slot55 base=%p count=%u desc=%p riid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} hr=0x%08lX out=%p",
                          pBaseObj, count, pDesc,
                          riid.Data1, riid.Data2, riid.Data3,
                          riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                          riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7],
                          static_cast<unsigned long>(hr), *ppOut);
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn,
                           "d3d12_xs", "XboxD3D12Device::CreatePipelineStateSlot55", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(56)
    // Slot 57 (+0x1C8): Base pipeline/shader state creation (pDesc, riid, ppOut)
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineStateSlot57(
        const void* pDesc,
        REFIID riid,
        void** ppOut) {
        if (!ppOut) return E_POINTER;
        *ppOut = nullptr;
        HRESULT hr = ReturnNoopPso(false, riid, ppOut);
        static std::atomic<uint32_t> s_slot57Logs{0};
        if (FAILED(hr) || s_slot57Logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "-> slot57 desc=%p riid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} hr=0x%08lX out=%p",
                          pDesc,
                          riid.Data1, riid.Data2, riid.Data3,
                          riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                          riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7],
                          static_cast<unsigned long>(hr), *ppOut);
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn,
                           "d3d12_xs", "XboxD3D12Device::CreatePipelineStateSlot57", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(58)
    SCARLETT_XBOX_DEV_EXT_STUB(59)
    SCARLETT_XBOX_DEV_EXT_STUB(60)
    SCARLETT_XBOX_DEV_EXT_STUB(61)
    // Slot 62 (+0x1F0): CreateComputePipelineStateX
    virtual HRESULT STDMETHODCALLTYPE CreateComputePipelineStateX(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
        UINT64 /*a2*/,
        UINT64 /*a3*/,
        REFIID riid,
        void** ppPipelineState) {
        if (!pDesc || !ppPipelineState) return E_INVALIDARG;
        *ppPipelineState = nullptr;
        static std::atomic<uint32_t> s_slot62Count{0};
        const uint32_t cnt = s_slot62Count.fetch_add(1, std::memory_order_relaxed);
        {
            char pre[128];
            std::snprintf(pre, sizeof(pre), "-> slot62 #%u enter rs=%p csLen=%zu",
                cnt + 1, static_cast<const void*>(pDesc->pRootSignature),
                static_cast<size_t>(pDesc->CS.BytecodeLength));
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateComputePipelineStateX", pre);
        }
        HRESULT hr = this->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        if (FAILED(hr) || cnt < 5) {
            std::string csInfo = sc::d3d12::InspectShaderBytecode(pDesc->CS);
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "-> slot62 #%u rs=%p csLen=%zu hr=0x%08lX pso=%p CS=[%s]",
                cnt + 1,
                static_cast<const void*>(pDesc->pRootSignature),
                static_cast<size_t>(pDesc->CS.BytecodeLength),
                static_cast<unsigned long>(hr),
                *ppPipelineState,
                csInfo.c_str());
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn,
                "d3d12_xs", "XboxD3D12Device::CreateComputePipelineStateX", buf);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(63)
    SCARLETT_XBOX_DEV_EXT_STUB(64)
    SCARLETT_XBOX_DEV_EXT_STUB(65)
    SCARLETT_XBOX_DEV_EXT_STUB(66)
    // Slot 67 (+0x218): Pipeline state creation (pDesc, riid, ppOut)
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineStateSlot67(
        const void* pDesc,
        REFIID riid,
        void** ppOut) {
        if (!ppOut) return E_POINTER;
        *ppOut = nullptr;
        HRESULT hr = ReturnNoopPso(false, riid, ppOut);
        static std::atomic<uint32_t> s_slot67Logs{0};
        if (FAILED(hr) || s_slot67Logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "-> slot67 desc=%p riid={%08lX-%04X-%04X} hr=0x%08lX out=%p",
                          pDesc, riid.Data1, riid.Data2, riid.Data3,
                          static_cast<unsigned long>(hr), *ppOut);
            sc::log::Write(SUCCEEDED(hr) ? sc::log::Level::Info : sc::log::Level::Warn,
                           "d3d12_xs", "XboxD3D12Device::CreatePipelineStateSlot67", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(68)

    // Slot 69 (+0x228): Xbox driver hint / quirk toggle
    virtual HRESULT STDMETHODCALLTYPE XboxSetDriverHint69(uint32_t id, uint32_t value) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "-> slot69 id=0x%08X value=%u", id, value);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::XboxSetDriverHint69", msg);
        return S_OK;
    }

    SCARLETT_XBOX_DEV_EXT_STUB(70)
    SCARLETT_XBOX_DEV_EXT_STUB(71)
    SCARLETT_XBOX_DEV_EXT_STUB(72)
    // Slot 73 (+0x248): Xbox CreateCommandListX(pDesc, pAllocator, pInitialState, riid, ppCommandList)
    virtual HRESULT STDMETHODCALLTYPE CreateCommandListX(
        const D3D12XBOX_COMMAND_LIST_DESC* pDesc,
        ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState,
        REFIID riid,
        void** ppCommandList) {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;
        const D3D12_COMMAND_LIST_TYPE rawType = pDesc ? pDesc->Type : D3D12_COMMAND_LIST_TYPE_DIRECT;
        const D3D12_COMMAND_LIST_TYPE cleanType = SanitizeCommandListType(rawType);
        ComPtr<ID3D12GraphicsCommandList> nativeCl;
        HRESULT hr = m_device->CreateCommandList(0, cleanType, XboxD3D12CommandAllocator::Unwrap(pCommandAllocator), pInitialState, IID_PPV_ARGS(&nativeCl));
        if (SUCCEEDED(hr)) {
            if (IsEqualGUID(riid, IID_ID3D12XboxDmaCommandList)) {
                ComPtr<XboxD3D12DmaCommandList> wrapped;
                wrapped.Attach(new XboxD3D12DmaCommandList(std::move(nativeCl), this));
                hr = wrapped->QueryInterface(riid, ppCommandList);
            } else {
                ComPtr<XboxD3D12GraphicsCommandList> wrapped;
                wrapped.Attach(new XboxD3D12GraphicsCommandList(std::move(nativeCl), this));
                hr = wrapped->QueryInterface(riid, ppCommandList);
            }
        }
        char msg[224];
        std::snprintf(msg, sizeof(msg),
                      "-> slot73 rawType=0x%X cleanType=%d riid=%s ptr=%p hr=0x%08lX",
                      static_cast<unsigned>(rawType), static_cast<int>(cleanType),
                      FormatGuidSimple(riid).c_str(), *ppCommandList, hr);
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreateCommandListX", msg);
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(74)
    SCARLETT_XBOX_DEV_EXT_STUB(75)
    SCARLETT_XBOX_DEV_EXT_STUB(76)
    SCARLETT_XBOX_DEV_EXT_STUB(77)
    // Slot 78 (+0x270): SetFrameIntervalX(pOutput, IntervalUs, Period, Flags)
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        void* /*pOutput*/, UINT32 IntervalUs, UINT32 /*Period*/, UINT32 /*Flags*/) {
        if (IntervalUs > 0 && IntervalUs <= 100000) {
            m_frameIntervalUs.store(IntervalUs, std::memory_order_relaxed);
        }
        return S_OK;
    }
    // Slot 79 (+0x278): ScheduleFrameEventX(Event, OffsetUs, hEventOrParam, Flags)
    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        UINT32 /*Event*/, UINT64 /*OffsetUs*/, void* /*pParam*/, UINT32 /*Flags*/) {
        return S_OK;
    }
    // Slot 80 (+0x280): WaitFrameEventX(Event, TimeoutMs, pReserved, Flags, pOutToken)
    virtual HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        UINT32 /*Event*/, UINT32 /*TimeoutMs*/, void* /*pReserved*/, UINT32 /*Flags*/, UINT64* pOutToken) {
        const uint32_t intervalUs = m_frameIntervalUs.load(std::memory_order_relaxed);
        const uint64_t intervalMs = (intervalUs >= 1000) ? (intervalUs / 1000) : 16;
        const uint64_t nowMs = GetTickCount64();
        const uint64_t prevMs = m_lastFrameTickMs.exchange(nowMs, std::memory_order_relaxed);
        if (prevMs != 0 && nowMs >= prevMs && (nowMs - prevMs) < intervalMs) {
            Sleep(static_cast<DWORD>(intervalMs - (nowMs - prevMs)));
            m_lastFrameTickMs.store(GetTickCount64(), std::memory_order_relaxed);
        }
        const uint64_t tok = m_frameToken.fetch_add(1, std::memory_order_relaxed);
        if (pOutToken) {
            *pOutToken = tok;
        }
        if (tok == 1) {
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::WaitFrameEventX", "-> issued first frame pipeline token");
        }
        return S_OK;
    }
    // Slot 81 (+0x288): GetFrameStatisticsX(Token, Flags, pNumStats, pStats)
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatisticsX(
        UINT64 /*Token*/, UINT32 /*Flags*/, UINT32* pNumStats, void* /*pStats*/) {
        if (pNumStats) {
            *pNumStats = 0;
        }
        return S_OK;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(82)
    SCARLETT_XBOX_DEV_EXT_STUB(83)
    SCARLETT_XBOX_DEV_EXT_STUB(84)
    SCARLETT_XBOX_DEV_EXT_STUB(85)
    // Slot 86 (+0x2B0): ID3D12Device2::CreatePipelineState
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
        REFIID riid,
        void** ppPipelineState) {
        if (!pDesc || !ppPipelineState) return E_INVALIDARG;
        *ppPipelineState = nullptr;

        static std::atomic<uint32_t> s_slot86Count{0};
        const uint32_t idx = s_slot86Count.fetch_add(1, std::memory_order_relaxed) + 1;

        // Log stream size and first 4 DWORDs before invoking host Device2
        uint32_t head[8]{};
        if (pDesc->pPipelineStateSubobjectStream && pDesc->SizeInBytes >= 4) {
            const size_t n = std::min<size_t>(sizeof(head), pDesc->SizeInBytes);
            std::memcpy(head, pDesc->pPipelineStateSubobjectStream, n);
        }

        ComPtr<ID3D12Device2> dev2;
        HRESULT hr = m_device.As(&dev2);
        if (SUCCEEDED(hr) && dev2) {
            hr = dev2->CreatePipelineState(pDesc, riid, ppPipelineState);
        }

        if (FAILED(hr) || !*ppPipelineState) {
            const bool isComputeStream = (pDesc->SizeInBytes <= 0x60);
            HRESULT fbHr = ReturnNoopPso(isComputeStream, riid, ppPipelineState);
            static std::atomic<uint32_t> s_slot86FbLogs{0};
            if (FAILED(fbHr) || s_slot86FbLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                    "-> slot86 #%u sz=0x%zX head=[0x%X,0x%X,0x%X,0x%X] hostHr=0x%08lX -> NOOP(%s) hr=0x%08lX pso=%p",
                    idx, static_cast<size_t>(pDesc->SizeInBytes),
                    head[0], head[1], head[2], head[3],
                    static_cast<unsigned long>(hr), isComputeStream ? "cmp" : "gfx",
                    static_cast<unsigned long>(fbHr), *ppPipelineState);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::CreatePipelineState", msg);
            }
            return SUCCEEDED(fbHr) ? fbHr : hr;
        }

        if (idx <= 8) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "-> slot86 #%u sz=0x%zX head=[0x%X,0x%X,0x%X,0x%X] hr=0x%08lX pso=%p",
                idx, static_cast<size_t>(pDesc->SizeInBytes),
                head[0], head[1], head[2], head[3],
                static_cast<unsigned long>(hr), *ppPipelineState);
            sc::log::Write(sc::log::Level::Info, "d3d12_xs", "XboxD3D12Device::CreatePipelineState", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(87)
    SCARLETT_XBOX_DEV_EXT_STUB(88)
    SCARLETT_XBOX_DEV_EXT_STUB(89)
    SCARLETT_XBOX_DEV_EXT_STUB(90)
    SCARLETT_XBOX_DEV_EXT_STUB(91)
    SCARLETT_XBOX_DEV_EXT_STUB(92)
    SCARLETT_XBOX_DEV_EXT_STUB(93)
    SCARLETT_XBOX_DEV_EXT_STUB(94)
    SCARLETT_XBOX_DEV_EXT_STUB(95)
    SCARLETT_XBOX_DEV_EXT_STUB(96)
    SCARLETT_XBOX_DEV_EXT_STUB(97)
    SCARLETT_XBOX_DEV_EXT_STUB(98)
    SCARLETT_XBOX_DEV_EXT_STUB(99)
    SCARLETT_XBOX_DEV_EXT_STUB(100)
    SCARLETT_XBOX_DEV_EXT_STUB(101)
    SCARLETT_XBOX_DEV_EXT_STUB(102)
    SCARLETT_XBOX_DEV_EXT_STUB(103)
    SCARLETT_XBOX_DEV_EXT_STUB(104)
    SCARLETT_XBOX_DEV_EXT_STUB(105)
    SCARLETT_XBOX_DEV_EXT_STUB(106)
    SCARLETT_XBOX_DEV_EXT_STUB(107)
    SCARLETT_XBOX_DEV_EXT_STUB(108)
    SCARLETT_XBOX_DEV_EXT_STUB(109)
    SCARLETT_XBOX_DEV_EXT_STUB(110)
    SCARLETT_XBOX_DEV_EXT_STUB(111)
    SCARLETT_XBOX_DEV_EXT_STUB(112)
    SCARLETT_XBOX_DEV_EXT_STUB(113)
    SCARLETT_XBOX_DEV_EXT_STUB(114)
    SCARLETT_XBOX_DEV_EXT_STUB(115)
    SCARLETT_XBOX_DEV_EXT_STUB(116)
    SCARLETT_XBOX_DEV_EXT_STUB(117)
    SCARLETT_XBOX_DEV_EXT_STUB(118)
    SCARLETT_XBOX_DEV_EXT_STUB(119)
    SCARLETT_XBOX_DEV_EXT_STUB(120)
    SCARLETT_XBOX_DEV_EXT_STUB(121)
    SCARLETT_XBOX_DEV_EXT_STUB(122)
    SCARLETT_XBOX_DEV_EXT_STUB(123)
    SCARLETT_XBOX_DEV_EXT_STUB(124)
    SCARLETT_XBOX_DEV_EXT_STUB(125)
    SCARLETT_XBOX_DEV_EXT_STUB(126)
    // Slot 127 (+0x3F8): CreateCommittedResource3
    virtual HRESULT STDMETHODCALLTYPE CreateCommittedResource3(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_BARRIER_LAYOUT /*InitialLayout*/,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        IUnknown* /*pProtectedResourceSession*/,
        UINT32 /*NumCastableFormats*/,
        const DXGI_FORMAT* /*pCastableFormats*/,
        REFIID riidResource,
        void** ppvResource) {
        if (!pDesc || !ppvResource) return E_INVALIDARG;
        *ppvResource = nullptr;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = pDesc->Dimension;
        desc.Alignment        = pDesc->Alignment;
        desc.Width            = pDesc->Width;
        desc.Height           = pDesc->Height;
        desc.DepthOrArraySize = pDesc->DepthOrArraySize;
        desc.MipLevels        = pDesc->MipLevels;
        desc.Format           = pDesc->Format;
        desc.SampleDesc       = pDesc->SampleDesc;
        desc.Layout           = pDesc->Layout;
        desc.Flags            = pDesc->Flags;

        D3D12_RESOURCE_DESC clean = SanitizeXboxResourceDesc(desc, "XboxD3D12Device::CreateCommittedResource3");

        D3D12_HEAP_PROPERTIES heapProps{};
        if (pHeapProperties) {
            heapProps = *pHeapProperties;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;
        } else {
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;
        }

        const D3D12_HEAP_FLAGS cleanHeapFlags = static_cast<D3D12_HEAP_FLAGS>(
            static_cast<UINT>(HeapFlags) & ~0x00000008u); // strip D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT if problematic

        const bool allowClear =
            clean.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            ((clean.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0);
        const D3D12_CLEAR_VALUE* clearPtr = allowClear ? pOptimizedClearValue : nullptr;

        ComPtr<ID3D12Resource> nativeRes;
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, cleanHeapFlags, &clean, D3D12_RESOURCE_STATE_COMMON, clearPtr, IID_PPV_ARGS(&nativeRes));
        if (FAILED(hr) && clean.Alignment != 0) {
            clean.Alignment = 0;
            hr = m_device->CreateCommittedResource(
                &heapProps, cleanHeapFlags, &clean, D3D12_RESOURCE_STATE_COMMON, clearPtr, IID_PPV_ARGS(&nativeRes));
        }

        if (SUCCEEDED(hr) && nativeRes) {
            D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &clean);
            const uint64_t sz = (ai.SizeInBytes != UINT64_MAX && ai.SizeInBytes != 0) ? ai.SizeInBytes : clean.Width;
            if (const auto* hooks = GetTitleXMemAllocationHooks(); hooks && hooks->pAlloc && sz > 0) {
                (void)hooks->pAlloc(static_cast<SIZE_T>(sz), (0x80ULL << 16) | 0x8000ULL);
            }
            if (IsEqualGUID(riidResource, __uuidof(ID3D12Resource)) || IsEqualGUID(riidResource, __uuidof(IUnknown))) {
                *ppvResource = static_cast<ID3D12Resource*>(
                    new XboxD3D12Resource(std::move(nativeRes), this, sz, D3D12_RESOURCE_STATE_COMMON));
            } else {
                hr = nativeRes->QueryInterface(riidResource, ppvResource);
            }
        }

        static std::atomic<uint32_t> s_logs{0};
        if (FAILED(hr) || s_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "-> slot127 dim=%d w=%llu h=%u sz=0x%llX hr=0x%08lX",
                          static_cast<int>(clean.Dimension),
                          static_cast<unsigned long long>(clean.Width),
                          clean.Height,
                          static_cast<unsigned long long>(nativeRes ? clean.Width : 0),
                          hr);
            sc::log::Write(FAILED(hr) ? sc::log::Level::Error : sc::log::Level::Info,
                           "d3d12_xs", "XboxD3D12Device::CreateCommittedResource3", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(128)
    SCARLETT_XBOX_DEV_EXT_STUB(129)
    SCARLETT_XBOX_DEV_EXT_STUB(130)
    SCARLETT_XBOX_DEV_EXT_STUB(131)
    SCARLETT_XBOX_DEV_EXT_STUB(132)
    // Slot 133 (+0x428): CreatePlacedResourceX(Address, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource)
    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) {
        if (!pDesc || !ppvResource) return E_INVALIDARG;
        *ppvResource = nullptr;

        D3D12_RESOURCE_DESC clean = SanitizeXboxResourceDesc(*pDesc, "XboxD3D12Device::CreatePlacedResourceX");
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_STATES cleanState = D3D12_RESOURCE_STATE_COMMON;
        if (static_cast<uint32_t>(InitialState) != 0xFFFFFFFFu &&
            (static_cast<uint32_t>(InitialState) & ~0x0040FFFFu) == 0u) {
            uint32_t st = static_cast<uint32_t>(InitialState);
            if (clean.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                st &= ~static_cast<uint32_t>(
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                    D3D12_RESOURCE_STATE_INDEX_BUFFER |
                    D3D12_RESOURCE_STATE_STREAM_OUT |
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            }
            constexpr uint32_t kExclusiveWriteMask =
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_STREAM_OUT) |
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST) |
                static_cast<uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_DEST);
            const uint32_t writeBits = st & kExclusiveWriteMask;
            if (writeBits != 0u && st != writeBits) {
                // Keep a single write bit if unambiguous; otherwise fall back to COMMON
                st = ((writeBits & (writeBits - 1u)) == 0u) ? writeBits : 0u;
            }
            const uint32_t rFlags = static_cast<uint32_t>(clean.Flags);
            if (((st & static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET)) != 0u &&
                 (rFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) == 0u) ||
                ((st & static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) != 0u &&
                 (rFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0u) ||
                ((st & static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != 0u &&
                 (rFlags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) == 0u)) {
                st = 0u;
            }
            cleanState = static_cast<D3D12_RESOURCE_STATES>(st);
        }

        const bool allowClear =
            clean.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            ((clean.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0);
        const D3D12_CLEAR_VALUE* clearPtr = allowClear ? pOptimizedClearValue : nullptr;

        ComPtr<ID3D12Resource> nativeRes;
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &clean, cleanState, clearPtr, IID_PPV_ARGS(&nativeRes));
        if (FAILED(hr) && cleanState != D3D12_RESOURCE_STATE_COMMON) {
            cleanState = D3D12_RESOURCE_STATE_COMMON;
            hr = m_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &clean, cleanState, clearPtr, IID_PPV_ARGS(&nativeRes));
        }
        if (FAILED(hr) && clean.Alignment != 0) {
            clean.Alignment = 0;
            hr = m_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &clean, cleanState, clearPtr, IID_PPV_ARGS(&nativeRes));
        }

        if (SUCCEEDED(hr) && nativeRes) {
            if (IsEqualGUID(riid, __uuidof(ID3D12Resource)) || IsEqualGUID(riid, __uuidof(IUnknown))) {
                D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &clean);
                uint64_t sz = (ai.SizeInBytes != UINT64_MAX) ? ai.SizeInBytes : clean.Width;
                *ppvResource = static_cast<ID3D12Resource*>(
                    new XboxD3D12Resource(std::move(nativeRes), this, sz, cleanState));
            } else {
                hr = nativeRes->QueryInterface(riid, ppvResource);
            }
        }

        static std::atomic<uint32_t> s_logs{0};
        if (FAILED(hr) || s_logs.fetch_add(1, std::memory_order_relaxed) < 32) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "-> slot133 addr=0x%llX dim=%d w=%llu h=%u flags=0x%X rawState=0x%X riid=%08lX-%04X-%04X hr=0x%08lX",
                          static_cast<unsigned long long>(Address), static_cast<int>(clean.Dimension),
                          static_cast<unsigned long long>(clean.Width), clean.Height,
                          static_cast<unsigned>(clean.Flags), static_cast<unsigned>(InitialState),
                          riid.Data1, riid.Data2, riid.Data3, hr);
            sc::log::Write(FAILED(hr) ? sc::log::Level::Error : sc::log::Level::Info,
                           "d3d12_xs", "XboxD3D12Device::CreatePlacedResourceX", msg);
        }
        return hr;
    }
    SCARLETT_XBOX_DEV_EXT_STUB(134)
    SCARLETT_XBOX_DEV_EXT_STUB(135)
    SCARLETT_XBOX_DEV_EXT_STUB(136)
    SCARLETT_XBOX_DEV_EXT_STUB(137)
    SCARLETT_XBOX_DEV_EXT_STUB(138)
    SCARLETT_XBOX_DEV_EXT_STUB(139)
    SCARLETT_XBOX_DEV_EXT_STUB(140)
    SCARLETT_XBOX_DEV_EXT_STUB(141)
    SCARLETT_XBOX_DEV_EXT_STUB(142)
    SCARLETT_XBOX_DEV_EXT_STUB(143)

private:
    static D3D12_COMMAND_LIST_TYPE SanitizeCommandListType(D3D12_COMMAND_LIST_TYPE type) {
        const uint32_t raw = static_cast<uint32_t>(type);
        if (raw == 0x10u) return D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (type == D3D12_COMMAND_LIST_TYPE_COPY) return D3D12_COMMAND_LIST_TYPE_COPY;
        if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE) return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (type == D3D12_COMMAND_LIST_TYPE_BUNDLE) return D3D12_COMMAND_LIST_TYPE_BUNDLE;
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    static D3D12_COMMAND_QUEUE_DESC SanitizeQueueDesc(const D3D12_COMMAND_QUEUE_DESC& raw) {
        D3D12_COMMAND_QUEUE_DESC clean{};
        clean.Type = SanitizeCommandListType(raw.Type);
        clean.Priority = (raw.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ||
                          raw.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
                         ? raw.Priority
                         : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        clean.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(
            static_cast<uint32_t>(raw.Flags) & static_cast<uint32_t>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT));
        clean.NodeMask = 0;
        return clean;
    }

    std::atomic<ULONG> m_refs{1};
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    std::atomic<uint32_t> m_frameIntervalUs{16667};
    std::atomic<uint64_t> m_frameToken{1};
    std::atomic<uint64_t> m_lastFrameTickMs{0};

    void* AllocCbvBounceSlot(UINT alignedSize, D3D12_GPU_VIRTUAL_ADDRESS& outGpu) {
        outGpu = 0;
        constexpr uint64_t kRingSize = 16ULL * 1024ULL * 1024ULL;
        if (alignedSize == 0 || alignedSize > kRingSize) return nullptr;
        std::lock_guard<std::mutex> lock(m_cbvBounceMtx);
        if (!m_cbvBounceBuf) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = kRingSize;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_cbvBounceBuf)))) {
                return nullptr;
            }
            D3D12_RANGE emptyRead{0, 0};
            if (FAILED(m_cbvBounceBuf->Map(0, &emptyRead, reinterpret_cast<void**>(&m_cbvBounceCpu))) || !m_cbvBounceCpu) {
                m_cbvBounceBuf.Reset();
                return nullptr;
            }
            m_cbvBounceGpu = m_cbvBounceBuf->GetGPUVirtualAddress();
        }
        if (m_cbvBounceCursor + alignedSize > kRingSize) {
            m_cbvBounceCursor = 0;
        }
        const uint64_t off = m_cbvBounceCursor;
        m_cbvBounceCursor += alignedSize;
        outGpu = m_cbvBounceGpu + off;
        return m_cbvBounceCpu + off;
    }

    void EnsureNoopPsosInitialized() {
        std::call_once(m_noopPsoOnce, [this]() {
            HMODULE hD3d12 = GetModuleHandleA("d3d12.dll");
            if (!hD3d12) hD3d12 = LoadLibraryA("d3d12.dll");
            auto pSerialize = hD3d12
                ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                      GetProcAddress(hD3d12, "D3D12SerializeRootSignature"))
                : nullptr;
            if (pSerialize) {
                D3D12_ROOT_SIGNATURE_DESC rsDesc{};
                rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
                ComPtr<ID3DBlob> sigBlob, errBlob;
                if (SUCCEEDED(pSerialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) && sigBlob) {
                    m_device->CreateRootSignature(
                        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                        IID_PPV_ARGS(&m_noopRootSig));
                }
            }
            if (!m_noopRootSig) {
                sc::log::Write(sc::log::Level::Error, "d3d12_xs", "XboxD3D12Device::EnsureNoopPsosInitialized",
                               "-> failed to create cached empty NOOP root signature");
                return;
            }

            using PFN_D3DCompile = HRESULT(WINAPI*)(
                LPCVOID, SIZE_T, LPCSTR, const void*, void*,
                LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            HMODULE hCompiler = LoadLibraryA("d3dcompiler_47.dll");
            auto pD3DCompile = hCompiler
                ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(hCompiler, "D3DCompile"))
                : nullptr;

            auto compileHlsl = [&](const char* src, const char* target) -> ComPtr<ID3DBlob> {
                if (!pD3DCompile) return {};
                ComPtr<ID3DBlob> code, err;
                HRESULT chr = pD3DCompile(
                    src, std::strlen(src), nullptr, nullptr, nullptr,
                    "main", target, 0, 0, &code, &err);
                return SUCCEEDED(chr) ? code : ComPtr<ID3DBlob>{};
            };

            if (auto cs = compileHlsl("[numthreads(1,1,1)] void main() {}", "cs_5_0")) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC cDesc{};
                cDesc.pRootSignature = m_noopRootSig.Get();
                cDesc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
                m_device->CreateComputePipelineState(&cDesc, IID_PPV_ARGS(&m_noopComputePso));
            }

            auto vs = compileHlsl("float4 main() : SV_Position { return float4(0,0,0,1); }", "vs_5_0");
            auto ps = compileHlsl("float4 main() : SV_Target { return float4(0,0,0,0); }", "ps_5_0");
            if (vs && ps) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC gDesc{};
                gDesc.pRootSignature = m_noopRootSig.Get();
                gDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
                gDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
                gDesc.BlendState.AlphaToCoverageEnable = FALSE;
                gDesc.BlendState.IndependentBlendEnable = FALSE;
                auto& rt0 = gDesc.BlendState.RenderTarget[0];
                rt0.BlendEnable = FALSE;
                rt0.LogicOpEnable = FALSE;
                rt0.SrcBlend = D3D12_BLEND_ONE;
                rt0.DestBlend = D3D12_BLEND_ZERO;
                rt0.BlendOp = D3D12_BLEND_OP_ADD;
                rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt0.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                gDesc.SampleMask = UINT_MAX;
                gDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                gDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                gDesc.RasterizerState.DepthClipEnable = TRUE;
                gDesc.DepthStencilState.DepthEnable = FALSE;
                gDesc.DepthStencilState.StencilEnable = FALSE;
                gDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
                gDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                gDesc.NumRenderTargets = 1;
                gDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                gDesc.SampleDesc.Count = 1;
                gDesc.SampleDesc.Quality = 0;
                m_device->CreateGraphicsPipelineState(&gDesc, IID_PPV_ARGS(&m_noopGraphicsPso));
            }

            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> initialized NOOP cache: rs=%p cmp=%p gfx=%p",
                          static_cast<void*>(m_noopRootSig.Get()),
                          static_cast<void*>(m_noopComputePso.Get()),
                          static_cast<void*>(m_noopGraphicsPso.Get()));
            sc::log::Write((m_noopComputePso && m_noopGraphicsPso) ? sc::log::Level::Info : sc::log::Level::Error,
                           "d3d12_xs", "XboxD3D12Device::EnsureNoopPsosInitialized", msg);
        });
    }

    ID3D12RootSignature* GetOrCreateNoopRootSignature() {
        EnsureNoopPsosInitialized();
        return m_noopRootSig.Get();
    }

    HRESULT ReturnNoopPso(bool isCompute, REFIID riid, void** ppPipelineState) {
        if (!ppPipelineState) return E_POINTER;
        *ppPipelineState = nullptr;
        EnsureNoopPsosInitialized();
        ID3D12PipelineState* pso = isCompute ? m_noopComputePso.Get() : m_noopGraphicsPso.Get();
        if (!pso) pso = m_noopComputePso ? m_noopComputePso.Get() : m_noopGraphicsPso.Get();
        if (!pso) return E_FAIL;
        HRESULT hr = pso->QueryInterface(riid, ppPipelineState);
        if (hr == E_NOINTERFACE) {
            static std::atomic<uint32_t> s_iidLogs{0};
            if (s_iidLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "-> non-desktop PSO IID {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}; returning AddRef'd NOOP PSO",
                              riid.Data1, riid.Data2, riid.Data3,
                              riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                              riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
                sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "XboxD3D12Device::ReturnNoopPso", msg);
            }
            pso->AddRef();
            *ppPipelineState = pso;
            return S_OK;
        }
        return hr;
    }

    std::once_flag m_noopPsoOnce;
    ComPtr<ID3D12RootSignature> m_noopRootSig;
    ComPtr<ID3D12PipelineState> m_noopComputePso;
    ComPtr<ID3D12PipelineState> m_noopGraphicsPso;

    std::mutex m_cbvBounceMtx;
    ComPtr<ID3D12Resource> m_cbvBounceBuf;
    uint8_t* m_cbvBounceCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_cbvBounceGpu = 0;
    uint64_t m_cbvBounceCursor = 0;
};

#undef SCARLETT_XBOX_DEV_EXT_STUB

} // namespace

namespace sc::d3d12xs {

Microsoft::WRL::ComPtr<ID3D12Device> CreateXboxD3D12Device(
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory,
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter,
    Microsoft::WRL::ComPtr<ID3D12Device> device) {
    Microsoft::WRL::ComPtr<ID3D12Device> out;
    out.Attach(new XboxD3D12Device(std::move(factory), std::move(adapter), std::move(device)));
    return out;
}

} // namespace sc::d3d12xs
