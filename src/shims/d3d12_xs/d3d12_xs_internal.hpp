#pragma once

#include "d3d12_xs.hpp"

#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace sc::d3d12xs {

template <typename Interface, typename Impl, typename... Args>
Microsoft::WRL::ComPtr<Interface> MakeCom(Args&&... args) {
    Microsoft::WRL::ComPtr<Interface> ptr;
    ptr.Attach(new Impl(std::forward<Args>(args)...));
    return ptr;
}

class Fnv1a64 {
public:
    void Update(const void* data, size_t len) {
        if (!data || len == 0) {
            return;
        }
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            m_hash ^= bytes[i];
            m_hash *= 1099511628211ULL;
        }
    }

    template <typename T>
    void UpdatePod(const T& val) {
        Update(&val, sizeof(T));
    }

    void UpdateStr(const char* str) {
        if (str) {
            Update(str, std::strlen(str) + 1);
        } else {
            uint8_t zero = 0;
            Update(&zero, 1);
        }
    }

    uint64_t Digest() const { return m_hash; }

private:
    uint64_t m_hash{14695981039346656037ULL};
};

std::string WideToUtf8(const wchar_t* wstr);
std::filesystem::path ResolvePsoCachePath(UINT vendorId, UINT deviceId);

uint64_t HashGraphicsPsoDesc(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
    uint64_t rootSigHash);

uint64_t HashComputePsoDesc(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
    uint64_t rootSigHash);

Microsoft::WRL::ComPtr<ID3D12CommandQueueX> CreateScarlettCommandQueueX(
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory,
    Microsoft::WRL::ComPtr<ID3D12Device> device,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue);

Microsoft::WRL::ComPtr<ID3D12GraphicsCommandListX> CreateScarlettGraphicsCommandListX(
    ID3D12GraphicsCommandList* nativeCl);

Microsoft::WRL::ComPtr<ID3D12DeviceX> CreateScarlettDeviceX(
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory,
    Microsoft::WRL::ComPtr<ID3D12Device> device,
    UINT vendorId,
    UINT deviceId);

Microsoft::WRL::ComPtr<ID3D12Device> CreateXboxD3D12Device(
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory,
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter,
    Microsoft::WRL::ComPtr<ID3D12Device> device);

} // namespace sc::d3d12xs