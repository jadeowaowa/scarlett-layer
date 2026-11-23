#include "d3d12_xs_internal.hpp"
#include <vector>
#include <cstdint>
#include <cstdio>
#include "common/config.hpp"
#include "common/stub.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
bool IsXboxStandardD3D12DeviceIid(REFIID riid) {
    static constexpr GUID kDeviceIids[] = {
        { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } }, // ID3D12Device
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
    for (const auto& g : kDeviceIids) {
        if (IsEqualGUID(riid, g)) return true;
    }
    return false;
}
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
    bool usingWarp = sc::config::UseWarp();
    if (usingWarp) {
        hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        if (FAILED(hr)) {
            sc::log::Write(sc::log::Level::Error, "d3d12_xs", __FUNCTION__,
                           "-> SCARLETT_USE_WARP=1 requested but EnumWarpAdapter failed");
            return hr;
        }
    } else {
        hr = factory->EnumAdapterByGpuPreference(
            0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (FAILED(hr)) {
            hr = factory->EnumAdapters1(0, &adapter);
        }
        if (FAILED(hr)) {
            hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            if (FAILED(hr)) {
                return hr;
            }
            usingWarp = true;
        }
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::string gpuMsg = std::string(usingWarp ? "-> selected WARP adapter: " : "-> selected host GPU: ") +
                         sc::d3d12xs::WideToUtf8(desc.Description);
    sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, gpuMsg.c_str());

    ComPtr<ID3D12Device> nativeDevice;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&nativeDevice));
    if (FAILED(hr)) {
        sc::log::Write(sc::log::Level::Error, "d3d12_xs", __FUNCTION__, "-> D3D12CreateDevice failed");
        return hr;
    }

#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue1> infoQueue1;
    if (SUCCEEDED(nativeDevice.As(&infoQueue1))) {
        infoQueue1->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue1->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        DWORD cookie = 0;
        infoQueue1->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID, LPCSTR desc, void*) {
                if (sev <= D3D12_MESSAGE_SEVERITY_WARNING && desc) {
                    if (sev == D3D12_MESSAGE_SEVERITY_WARNING && std::strstr(desc, "is not a comparison filter")) {
                        static int s_samplerWarnCount = 0;
                        if (++s_samplerWarnCount > 1) return;
                    }
                    if (std::strstr(desc, "Root Signature doesn't match")) {
                        static std::atomic<int> s_rsMismatchCount{0};
                        if (s_rsMismatchCount.fetch_add(1, std::memory_order_relaxed) >= 3) return;
                    }
                    sc::log::Write(
                        sev == D3D12_MESSAGE_SEVERITY_WARNING ? sc::log::Level::Warn : sc::log::Level::Error,
                        "d3d12_debug", "Validation", desc);
                    if (sev <= D3D12_MESSAGE_SEVERITY_ERROR && sc::config::IsStrictD3D12()) {
                        std::abort();
                    }
                }
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }
#endif

    if (IsXboxStandardD3D12DeviceIid(riid)) {
        auto xboxDev = sc::d3d12xs::CreateXboxD3D12Device(
            std::move(factory), std::move(adapter), std::move(nativeDevice));
        return xboxDev->QueryInterface(riid, ppDevice);
    }

    auto wrapper = sc::d3d12xs::CreateScarlettDeviceX(
        std::move(factory), std::move(nativeDevice), desc.VendorId, desc.DeviceId);
    return wrapper->QueryInterface(riid, ppDevice);
}

HRESULT WINAPI ScarlettSerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) return E_INVALIDARG;

    auto logMsg = [](const char* fn, const char* msg) {
        char safe[1024];
        size_t i = 0;
        for (; msg[i] != '\0' && i + 1 < sizeof(safe); ++i) {
            safe[i] = (msg[i] == '%') ? ' ' : msg[i];
        }
        safe[i] = '\0';
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", fn, safe);
    };

    constexpr UINT kDesktopRootSigFlagsMask = 0x0FFF;
    D3D12_ROOT_SIGNATURE_DESC clean = *pRootSignature;
    UINT rawFlags = static_cast<UINT>(clean.Flags);
    if (rawFlags & ~kDesktopRootSigFlagsMask) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "-> stripped console-only Flags 0x%08X (kept 0x%08X)",
            rawFlags & ~kDesktopRootSigFlagsMask, rawFlags & kDesktopRootSigFlagsMask);
        logMsg(__FUNCTION__, buf);
        clean.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(rawFlags & kDesktopRootSigFlagsMask);
    }

    ID3DBlob* localErr = nullptr;
    HRESULT hr = ::D3D12SerializeRootSignature(&clean, Version, ppBlob, &localErr);
    if (FAILED(hr)) {
        const char* errStr = (localErr && localErr->GetBufferPointer())
            ? static_cast<const char*>(localErr->GetBufferPointer()) : "<none>";
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "-> FAILED hr=0x%08X ver=%u params=%u samplers=%u rawFlags=0x%08X err=%s",
            static_cast<unsigned>(hr), static_cast<unsigned>(Version),
            clean.NumParameters, clean.NumStaticSamplers, rawFlags, errStr);
        logMsg(__FUNCTION__, buf);
    }

    if (ppErrorBlob) {
        *ppErrorBlob = localErr;
    } else if (localErr) {
        localErr->Release();
    }
    return hr;
}


void WINAPI D3D12XboxSetProcessDebugFlags(uint32_t /*flags*/) {
}

HRESULT WINAPI D3DMapEsramMemory(
    uint32_t /*flags*/, void* /*virtualAddress*/, uint32_t /*numPages*/, const uint32_t* /*pageList*/) {
    return S_OK;
}

HRESULT WINAPI D3D12ConfigureVirtualMemoryX(const void* /*pParameters*/) {
    sc::log::Write(sc::log::Level::Info, "d3d12_xs", __FUNCTION__, "-> S_OK (stub)");
    return S_OK;
}

HRESULT WINAPI ScarlettSerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) return E_INVALIDARG;

    auto logMsg = [](const char* fn, const char* msg) {
        char safe[1024];
        size_t i = 0;
        for (; msg[i] != '\0' && i + 1 < sizeof(safe); ++i) {
            safe[i] = (msg[i] == '%') ? ' ' : msg[i];
        }
        safe[i] = '\0';
        sc::log::Write(sc::log::Level::Info, "d3d12_xs", fn, safe);
    };

    constexpr UINT kDesktopRootSigFlagsMask   = 0x0FFF;
    constexpr UINT kDesktopDescRangeFlagsMask = 0x1000F;
    constexpr UINT kDesktopRootDescFlagsMask  = 0x000E;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC clean = *pRootSignature;
    std::vector<D3D12_ROOT_PARAMETER1> params1;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges1;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers0;

    const UINT ver = static_cast<UINT>(clean.Version);
    const UINT rawFlags = (ver == 1)
        ? static_cast<UINT>(clean.Desc_1_0.Flags)
        : static_cast<UINT>(clean.Desc_1_1.Flags);

    auto sanitizeFlags = [&](UINT& flags) {
        UINT stripped = flags & ~kDesktopRootSigFlagsMask;
        if (stripped) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "-> stripped console-only Flags 0x%08X (kept 0x%08X)",
                stripped, flags & kDesktopRootSigFlagsMask);
            logMsg(__FUNCTION__, buf);
            flags &= kDesktopRootSigFlagsMask;
        }
    };

    if (ver == 1) {
        UINT f = static_cast<UINT>(clean.Desc_1_0.Flags);
        sanitizeFlags(f);
        clean.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(f);
    } else if (ver == 2 || ver == 3) {
        UINT f = static_cast<UINT>(clean.Desc_1_1.Flags);
        sanitizeFlags(f);
        clean.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(f);

        UINT numParams = clean.Desc_1_1.NumParameters;
        if (numParams > 0 && numParams <= 128 && clean.Desc_1_1.pParameters) {
            params1.assign(clean.Desc_1_1.pParameters, clean.Desc_1_1.pParameters + numParams);
            ranges1.resize(numParams);
            for (UINT i = 0; i < numParams; ++i) {
                if (params1[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    auto& tbl = params1[i].DescriptorTable;
                    if (tbl.NumDescriptorRanges > 0 && tbl.NumDescriptorRanges <= 256 && tbl.pDescriptorRanges) {
                        ranges1[i].assign(tbl.pDescriptorRanges, tbl.pDescriptorRanges + tbl.NumDescriptorRanges);
                        for (auto& r : ranges1[i]) {
                            UINT rf = static_cast<UINT>(r.Flags);
                            if (rf & ~kDesktopDescRangeFlagsMask) {
                                char buf[256];
                                std::snprintf(buf, sizeof(buf),
                                    "-> param[%u] stripped range flags 0x%08X",
                                    i, rf & ~kDesktopDescRangeFlagsMask);
                                logMsg(__FUNCTION__, buf);
                                r.Flags = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(rf & kDesktopDescRangeFlagsMask);
                            }
                        }
                        tbl.pDescriptorRanges = ranges1[i].data();
                    }
                } else if (params1[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                           params1[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                           params1[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) {
                    UINT df = static_cast<UINT>(params1[i].Descriptor.Flags);
                    if (df & ~kDesktopRootDescFlagsMask) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "-> param[%u] stripped root desc flags 0x%08X",
                            i, df & ~kDesktopRootDescFlagsMask);
                        logMsg(__FUNCTION__, buf);
                        params1[i].Descriptor.Flags = static_cast<D3D12_ROOT_DESCRIPTOR_FLAGS>(df & kDesktopRootDescFlagsMask);
                    }
                }
            }
            clean.Desc_1_1.pParameters = params1.data();
        }
    }

    ID3DBlob* localErr = nullptr;
    HRESULT hr = ::D3D12SerializeVersionedRootSignature(&clean, ppBlob, &localErr);

    if (FAILED(hr) && ver == 3) {
        if (localErr) {
            localErr->Release();
            localErr = nullptr;
        }
        UINT numSamplers = clean.Desc_1_1.NumStaticSamplers;
        if (numSamplers > 0 && numSamplers <= 256 && clean.Desc_1_1.pStaticSamplers) {
            samplers0.resize(numSamplers);
            const auto* srcBytes = reinterpret_cast<const uint8_t*>(clean.Desc_1_1.pStaticSamplers);
            constexpr size_t kSampler1Stride = sizeof(D3D12_STATIC_SAMPLER_DESC) + sizeof(UINT);
            for (UINT i = 0; i < numSamplers; ++i) {
                std::memcpy(&samplers0[i], srcBytes + i * kSampler1Stride, sizeof(D3D12_STATIC_SAMPLER_DESC));
                if (static_cast<UINT>(samplers0[i].BorderColor) == 3) {
                    samplers0[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
                } else if (static_cast<UINT>(samplers0[i].BorderColor) == 4) {
                    samplers0[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
                }
            }
            clean.Desc_1_1.pStaticSamplers = samplers0.data();
        }
        clean.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        hr = ::D3D12SerializeVersionedRootSignature(&clean, ppBlob, &localErr);
        if (SUCCEEDED(hr)) {
            logMsg(__FUNCTION__, "-> downgraded Version 1_2 to 1_1 (S_OK)");
        }
    }

    if (FAILED(hr)) {
        const char* errStr = (localErr && localErr->GetBufferPointer())
            ? static_cast<const char*>(localErr->GetBufferPointer()) : "<none>";
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "-> FAILED hr=0x%08X ver=%u params=%u samplers=%u rawFlags=0x%08X err=%s",
            static_cast<unsigned>(hr), ver,
            clean.Desc_1_1.NumParameters, clean.Desc_1_1.NumStaticSamplers,
            rawFlags, errStr);
        logMsg(__FUNCTION__, buf);

        if ((ver == 2 || ver == 3) && clean.Desc_1_1.pParameters && clean.Desc_1_1.NumParameters <= 32) {
            for (UINT i = 0; i < clean.Desc_1_1.NumParameters; ++i) {
                const auto& rp = clean.Desc_1_1.pParameters[i];
                std::snprintf(buf, sizeof(buf),
                    "->   param[%u] type=%u vis=%u u0=0x%08X u1=0x%08X u2=0x%08X",
                    i, static_cast<unsigned>(rp.ParameterType), static_cast<unsigned>(rp.ShaderVisibility),
                    rp.Constants.ShaderRegister, rp.Constants.RegisterSpace, rp.Constants.Num32BitValues);
                logMsg(__FUNCTION__, buf);
            }
        }
    }

    if (ppErrorBlob) {
        *ppErrorBlob = localErr;
    } else if (localErr) {
        localErr->Release();
    }
    return hr;
}

HRESULT WINAPI ScarlettCreateRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return ::D3D12CreateRootSignatureDeserializer(
        pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

} // extern "C"