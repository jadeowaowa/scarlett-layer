#include "d3d12_xs_internal.hpp"
#include "shader_inspector.hpp"
#include "shader_translator.hpp"
#include "common/stub.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using sc::d3d12xs::Fnv1a64;
using sc::d3d12xs::HashComputePsoDesc;
using sc::d3d12xs::HashGraphicsPsoDesc;
using sc::d3d12xs::ResolvePsoCachePath;

namespace {
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
        *ppCommandQueue = sc::d3d12xs::CreateScarlettCommandQueueX(m_factory, m_device, std::move(nativeQueue));
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
            auto* wrapped = sc::d3d12xs::CreateScarlettGraphicsCommandListX(nativeCl.Get());
            hr = wrapped->QueryInterface(riid, ppCommandList);
            wrapped->Release();
            return hr;
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
        return m_device->CreateCommittedResource(
            pHeapProperties, HeapFlags, pDesc, InitialResourceState,
            pOptimizedClearValue, riidResource, ppvResource);
    }

    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override {
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
        HRESULT hr = m_device->CreatePlacedResource(
            pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
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
        HANDLE hEvent, D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken) override {
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

        // Fast-path passthrough for native DXBC/DXIL without hashing/caching overhead
        if (stageBc.BytecodeLength >= 4 && std::memcmp(stageBc.pShaderBytecode, "DXBC", 4) == 0) {
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
        HRESULT hr = m_translators.Translate(stageBc, *result, outError);
        if (FAILED(hr)) {
            return hr;
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

ID3D12DeviceX* CreateScarlettDeviceX(
    ComPtr<IDXGIFactory6> factory,
    ComPtr<ID3D12Device> device,
    UINT vendorId,
    UINT deviceId) {
    return new ScarlettDeviceX(std::move(factory), std::move(device), vendorId, deviceId);
}

} // namespace sc::d3d12xs