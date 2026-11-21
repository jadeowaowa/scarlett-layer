#include "d3d12_xs_internal.hpp"
#include "common/config.hpp"

#include <cstdio>
#include <cstdlib>

namespace sc::d3d12xs {

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

std::filesystem::path ResolvePsoCachePath(UINT vendorId, UINT deviceId) {
    wchar_t fileName[64];
    std::swprintf(fileName, _countof(fileName), L"pso_cache_%04X_%04X.bin", vendorId, deviceId);
    return sc::config::ResolveDataDir(L"cache") / fileName;
}

uint64_t HashGraphicsPsoDesc(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
    uint64_t rootSigHash) {
    Fnv1a64 h;
    h.UpdatePod(rootSigHash);

    auto hashShader = [&h](const D3D12_SHADER_BYTECODE& bc) {
        h.UpdatePod(bc.BytecodeLength);
        h.Update(bc.pShaderBytecode, bc.BytecodeLength);
    };
    hashShader(desc.VS);
    hashShader(desc.PS);
    hashShader(desc.DS);
    hashShader(desc.HS);
    hashShader(desc.GS);

    h.UpdatePod(desc.InputLayout.NumElements);
    if (desc.InputLayout.pInputElementDescs) {
        for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
            const auto& el = desc.InputLayout.pInputElementDescs[i];
            h.UpdateStr(el.SemanticName);
            h.UpdatePod(el.SemanticIndex);
            h.UpdatePod(el.Format);
            h.UpdatePod(el.InputSlot);
            h.UpdatePod(el.AlignedByteOffset);
            h.UpdatePod(el.InputSlotClass);
            h.UpdatePod(el.InstanceDataStepRate);
        }
    }

    h.UpdatePod(desc.BlendState);
    h.UpdatePod(desc.SampleMask);
    h.UpdatePod(desc.RasterizerState);
    h.UpdatePod(desc.DepthStencilState);
    h.UpdatePod(desc.IBStripCutValue);
    h.UpdatePod(desc.PrimitiveTopologyType);
    h.UpdatePod(desc.NumRenderTargets);
    h.Update(desc.RTVFormats, sizeof(desc.RTVFormats));
    h.UpdatePod(desc.DSVFormat);
    h.UpdatePod(desc.SampleDesc);
    h.UpdatePod(desc.NodeMask);
    h.UpdatePod(desc.Flags);
    return h.Digest();
}

uint64_t HashComputePsoDesc(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
    uint64_t rootSigHash) {
    Fnv1a64 h;
    h.UpdatePod(rootSigHash);
    h.UpdatePod(desc.CS.BytecodeLength);
    h.Update(desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
    h.UpdatePod(desc.NodeMask);
    h.UpdatePod(desc.Flags);
    return h.Digest();
}

} // namespace sc::d3d12xs