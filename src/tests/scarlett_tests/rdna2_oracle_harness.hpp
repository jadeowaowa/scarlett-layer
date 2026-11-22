#pragma once

#include "test_harness.hpp"

namespace sc::test::oracle {

using namespace sc::d3d12;
using namespace sc::d3d12::rdna2;

struct ComputeOracle {
    Microsoft::WRL::ComPtr<ID3D12DeviceX> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueueX> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
    Microsoft::WRL::ComPtr<ID3D12Resource> cbvBuf, srvBuf, uavO, uavL, rbO, rbL;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};

    ~ComputeOracle() { if (fenceEvent) CloseHandle(fenceEvent); }

    bool MakeBuf(uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                 D3D12_RESOURCE_STATES state, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out)));
    }

    bool Init() {
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{}; params.Version = 1;
        if (FAILED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.GetAddressOf())))) return false;
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueueX(&qd, &queue))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) return false;

        D3D12_ROOT_PARAMETER rps[3]{};
        rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rps[0].Descriptor.ShaderRegister = 0; rps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rps[1].Descriptor.ShaderRegister = 0; rps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rps[2].Descriptor.ShaderRegister = 0; rps[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = rps;
        Microsoft::WRL::ComPtr<ID3DBlob> rsb, rse;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse))) return false;
        if (FAILED(device->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)))) return false;

        if (!MakeBuf(256,  D3D12_HEAP_TYPE_UPLOAD,   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, cbvBuf)) return false;
        if (!MakeBuf(1024, D3D12_HEAP_TYPE_UPLOAD,   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, srvBuf)) return false;
        if (!MakeBuf(1024, D3D12_HEAP_TYPE_DEFAULT,  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, uavO)) return false;
        if (!MakeBuf(1024, D3D12_HEAP_TYPE_DEFAULT,  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, uavL)) return false;
        if (!MakeBuf(1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rbO)) return false;
        if (!MakeBuf(1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rbL)) return false;

        void* pCb = nullptr;
        if (FAILED(cbvBuf->Map(0, nullptr, &pCb))) return false;
        auto* cbF = static_cast<float*>(pCb);
        for (int i = 0; i < 64; ++i) cbF[i] = 0.25f * static_cast<float>(i + 1);
        auto* cbU = static_cast<uint32_t*>(pCb);
        cbU[16] = 0x13572468u; cbU[17] = 0x00000019u; cbU[18] = 0x00000007u; cbU[19] = 0x00000003u;
        cbvBuf->Unmap(0, nullptr);

        void* pIn = nullptr;
        if (FAILED(srvBuf->Map(0, nullptr, &pIn))) return false;
        auto* inU = static_cast<uint32_t*>(pIn);
        for (uint32_t i = 0; i < 256u; ++i) inU[i] = (i * 2654435761u) ^ (i + 17u);
        srvBuf->Unmap(0, nullptr);
        return true;
    }

    bool Run(const char* name, const char* oracleHlsl, const std::vector<uint32_t>& isaWords,
             uint32_t scxsFlags = 0u, bool floatCmp = false, float tol = 1e-4f, uint32_t count = 64u) {
        Microsoft::WRL::ComPtr<ID3DBlob> oBlob, oErr;
        if (FAILED(D3DCompile(oracleHlsl, std::strlen(oracleHlsl), name, nullptr, nullptr,
                              "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &oBlob, &oErr))) {
            std::printf("  [ORACLE COMPILE FAIL] %s: %s\n", name, oErr ? static_cast<const char*>(oErr->GetBufferPointer()) : "err");
            return false;
        }
        const auto scxs = BuildScxsContainer(
            ScxsShaderStage::Compute, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa,
            isaWords.data(), isaWords.size() * sizeof(uint32_t), 6, 6, scxsFlags);

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs.Get();
        Microsoft::WRL::ComPtr<ID3D12PipelineState> psoO, psoL;
        pd.CS = { oBlob->GetBufferPointer(), oBlob->GetBufferSize() };
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoO)))) { std::printf("  [ORACLE PSO FAIL] %s\n", name); return false; }
        pd.CS = { scxs.data(), scxs.size() };
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoL)))) { std::printf("  [LIFTED PSO FAIL] %s\n", name); return false; }

        if (FAILED(alloc->Reset())) return false;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd)))) return false;

        auto execOne = [&](ID3D12PipelineState* pso, ID3D12Resource* uav, ID3D12Resource* rb) {
            D3D12_RESOURCE_BARRIER b0{}; b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b0.Transition = { uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            cmd->ResourceBarrier(1, &b0);
            cmd->SetPipelineState(pso);
            cmd->SetComputeRootSignature(rs.Get());
            cmd->SetComputeRootConstantBufferView(0, cbvBuf->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(1, srvBuf->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(2, uav->GetGPUVirtualAddress());
            cmd->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER b1{}; b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b1.Transition = { uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
            cmd->ResourceBarrier(1, &b1);
            cmd->CopyResource(rb, uav);
            D3D12_RESOURCE_BARRIER b2{}; b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2.Transition = { uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON };
            cmd->ResourceBarrier(1, &b2);
        };
        execOne(psoO.Get(), uavO.Get(), rbO.Get());
        execOne(psoL.Get(), uavL.Get(), rbL.Get());
        if (FAILED(cmd->Close())) return false;

        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), fenceValue))) return false;
        if (fence->GetCompletedValue() < fenceValue) {
            if (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent))) return false;
            WaitForSingleObject(fenceEvent, 5000);
        }

        void *pO = nullptr, *pL = nullptr;
        D3D12_RANGE rr{ 0, 1024 };
        if (FAILED(rbO->Map(0, &rr, &pO)) || FAILED(rbL->Map(0, &rr, &pL))) return false;
        const auto* uO = static_cast<const uint32_t*>(pO);
        const auto* uL = static_cast<const uint32_t*>(pL);
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i) {
            if (!floatCmp) {
                if (uO[i] != uL[i]) {
                    std::printf("  [DIFF FAIL] %s lane=%u oracle=0x%08X lifted=0x%08X\n", name, i, uO[i], uL[i]);
                    ok = false; break;
                }
            } else {
                const float fO = std::bit_cast<float>(uO[i]);
                const float fL = std::bit_cast<float>(uL[i]);
                if (std::isnan(fO) != std::isnan(fL) || std::fabs(fO - fL) > tol) {
                    std::printf("  [DIFF FAIL] %s lane=%u oracle=%.6f lifted=%.6f\n", name, i, fO, fL);
                    ok = false; break;
                }
            }
        }
        D3D12_RANGE wr{ 0, 0 };
        rbO->Unmap(0, &wr);
        rbL->Unmap(0, &wr);
        return ok;
    }
};

} // namespace sc::test::oracle
