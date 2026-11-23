#include "d3d12_xs_internal.hpp"
#include "common/config.hpp"
#include "common/stub.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
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
        DWORD cookie = 0;
        infoQueue1->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID, LPCSTR desc, void*) {
                if (sev <= D3D12_MESSAGE_SEVERITY_WARNING && desc) {
                    if (sev == D3D12_MESSAGE_SEVERITY_WARNING && std::strstr(desc, "is not a comparison filter")) {
                        static int s_samplerWarnCount = 0;
                        if (++s_samplerWarnCount > 1) return;
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

    if (IsEqualGUID(riid, __uuidof(ID3D12Device))) {
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
    return ::D3D12SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
}


void WINAPI D3D12XboxSetProcessDebugFlags(uint32_t /*flags*/) {
}

HRESULT WINAPI D3DMapEsramMemory(
    uint32_t /*flags*/, void* /*virtualAddress*/, uint32_t /*numPages*/, const uint32_t* /*pageList*/) {
    return S_OK;
}

} // extern "C"