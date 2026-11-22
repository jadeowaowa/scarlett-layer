#include "test_harness.hpp"
#include "../../shims/d3d12_xs/shader_stripper.hpp"

using Microsoft::WRL::ComPtr;

bool TestRdna2IsaDecoderAndGpuExecution() {
    using namespace sc::d3d12::rdna2;

    // Assemble a real 12-DWORD RDNA2 (GFX10.3) binary pixel shader:
    //   v0 = 0.25f                                  -> R = 0.2500 (64/255)
    //   v1 = 0.50f; v1 = v0 + v1                    -> G = 0.7500 (191/255)
    //   v2 = 0.50f; v2 = 0.5*v0 + v2 = 0.625
    //   v2 = saturate(mad(v2, 0.5, v0)) = 0.5625    -> B = 0.5625 (143/255)
    //   v3 = 1.00f                                  -> A = 1.0000 (255/255)
    //   exp mrt0, v0, v1, v2, v3 done vm
    //   s_endpgm
    std::vector<uint32_t> rdna2Words;
    AppendVop1LiteralF32(rdna2Words, kVop1MovB32, 0, 0.25f);
    AppendVop1(rdna2Words, kVop1MovB32, 1, kSrcInlineHalf);
    AppendVop2(rdna2Words, kVop2AddF32, 1, VgprSrc(0), 1);
    AppendVop1(rdna2Words, kVop1MovB32, 2, kSrcInlineHalf);
    AppendVop2(rdna2Words, kVop2FmacF32, 2, kSrcInlineHalf, 0);
    AppendVop3(rdna2Words, kVop3MadF32, 2, VgprSrc(2), kSrcInlineHalf, VgprSrc(0), true);
    AppendVop1(rdna2Words, kVop1MovB32, 3, kSrcInlineOne);
    AppendExpMrt0(rdna2Words, 0, 1, 2, 3, 0xFu, true, true);
    AppendSopp(rdna2Words, kSoppEndpgm);

    DecodedProgram decoded{};
    std::string decErr;
    SC_EXPECT(DecodeProgram(rdna2Words.data(), rdna2Words.size() * sizeof(uint32_t), decoded, decErr));
    SC_EXPECT(decoded.insts.size() == 9u);

    const std::string asmText = Disassemble(decoded);
    SC_EXPECT(asmText.find("v_fmac_f32") != std::string::npos);
    SC_EXPECT(asmText.find("v_mad_f32") != std::string::npos);
    SC_EXPECT(asmText.find("exp mrt0") != std::string::npos);
    SC_EXPECT(asmText.find("s_endpgm") != std::string::npos);

    auto psRdna2Scxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        rdna2Words.data(), rdna2Words.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue)));

    const char* vsHlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION {\n"
        "    float2 p = float2((id == 2) ? 3.0f : -1.0f, (id == 1) ? 3.0f : -1.0f);\n"
        "    return float4(p, 0.0f, 1.0f);\n"
        "}\n";
    ComPtr<ID3DBlob> vsBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(vsHlsl, std::strlen(vsHlsl), "rdna2_fs_vs.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psRdna2Scxs.data(), psRdna2Scxs.size() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Pass 1 & Pass 2: RDNA2 binary lift + PSO cache hit
    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso1 != nullptr && pso2 != nullptr);

    // Corrupted RDNA2 SCXS payload -> E_INVALIDARG
    auto corruptPs = psRdna2Scxs;
    corruptPs.back() ^= 0xFFu;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC corruptDesc = psoDesc;
    corruptDesc.PS = { corruptPs.data(), corruptPs.size() };
    ComPtr<ID3D12PipelineState> badPso;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&corruptDesc, IID_PPV_ARGS(&badPso)) == E_INVALIDARG);

    // Render fullscreen triangle into 16x16 RT and read back RGBA8 pixels from GPU
    constexpr UINT kRtSize = 16;
    constexpr UINT kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; // 256 B
    constexpr UINT kReadbackBytes = kRowPitch * kRtSize;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = rtDesc.Format;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kReadbackBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kRtSize), static_cast<float>(kRtSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kRtSize), static_cast<LONG>(kRtSize) };
    cmdList->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rtTex.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rtTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = kRtSize;
    dstLoc.PlacedFootprint.Footprint.Height = kRtSize;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);

    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvt != nullptr);
    SC_EXPECT(SUCCEEDED(cmdQueue->Signal(fence.Get(), 1)));
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
    CloseHandle(fenceEvt);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kReadbackBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    const uint8_t* centerPx = pixels + (8u * kRowPitch) + (8u * 4u);
    const bool rOk = std::abs(static_cast<int>(centerPx[0]) - 64)  <= 1;
    const bool gOk = std::abs(static_cast<int>(centerPx[1]) - 191) <= 1;
    const bool bOk = std::abs(static_cast<int>(centerPx[2]) - 143) <= 1;
    const bool aOk = (centerPx[3] == 255);
    D3D12_RANGE emptyWrite{ 0, 0 };
    rbBuf->Unmap(0, &emptyWrite);

    SC_EXPECT(rOk && gOk && bOk && aOk);
    return true;
}

// 18. d3d12_xs: Dedicated COMPUTE queue -> GPU-side fence Wait() on DIRECT queue -> readback

bool TestRdna2ComputeIsaExecution() {
    using namespace sc::d3d12::rdna2;

    // Binary RDNA2 compute kernel: gOut[dtid.x] = dtid.x * 17u + 0x52440000u
    std::vector<uint32_t> csWords;
    AppendVop3(csWords, kVop3MulLoU32, 1, VgprSrc(0), 128u + 17u, 128u); // v1 = v0 * 17
    AppendVop2LiteralU32(csWords, kVop2AddNcU32, 1, 0x52440000u, 1);     // v1 = v1 + 0x52440000
    AppendMubufStoreDword(csWords, 1, 0, 0);                             // gOut[v0] = v1
    AppendSopp(csWords, kSoppEndpgm);

    DecodedProgram decoded{};
    std::string err;
    SC_EXPECT(DecodeProgram(csWords.data(), csWords.size() * sizeof(uint32_t), decoded, err));
    SC_EXPECT(decoded.insts.size() == 4u);
    const std::string asmText = Disassemble(decoded);
    SC_EXPECT(asmText.find("v_mul_lo_u32") != std::string::npos);
    SC_EXPECT(asmText.find("v_add_nc_u32") != std::string::npos);
    SC_EXPECT(asmText.find("buffer_store_dword") != std::string::npos);

    auto csRdna2Scxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        csWords.data(), csWords.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{ D3D12_COMMAND_LIST_TYPE_COMPUTE };
    ComPtr<ID3D12CommandQueueX> compQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &compQueue)));

    D3D12_DESCRIPTOR_RANGE uavRange{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 1, &uavRange };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    ComPtr<ID3DBlob> rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csRdna2Scxs.data(), csRdna2Scxs.size() };

    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));

    constexpr UINT kElems = 64;
    constexpr UINT kBytes = kElems * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf, rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uavHeap))));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer = { 0, kElems, sizeof(uint32_t), 0, D3D12_BUFFER_UAV_FLAG_NONE };
    deviceX->CreateUnorderedAccessView(uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(rootSig.Get());
    cmdList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = uavBuf.Get();
    D3D12_RESOURCE_BARRIER cb[2]{}; cb[0]=uavBar; cb[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; cb[1].Transition={uavBuf.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE}; cmdList->ResourceBarrier(2, cb);
    cmdList->CopyResource(rbBuf.Get(), uavBuf.Get());
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    compQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(compQueue->Signal(fence.Get(), 1)));

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, kBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &rr, &mapped)));
    const auto* out = static_cast<const uint32_t*>(mapped);
    bool exact = true;
    for (UINT i = 0; i < kElems; ++i) {
        if (out[i] != (i * 17u + 0x52440000u)) { exact = false; break; }
    }
    D3D12_RANGE ew{ 0, 0 };
    rbBuf->Unmap(0, &ew);
    SC_EXPECT(exact);
    return true;
}

// 20. d3d12_xs: Full binary RDNA2 VS + PS pipeline with SMEM CBV loads, VINTRP handoff & GPU readback

bool TestRdna2VertexAndPixelPipelineWithCbv() {
    using namespace sc::d3d12::rdna2;

    // Binary RDNA2 Vertex Shader:
    //   s_load_dwordx4 s[4:7], s[0:1], 0x00   -> scale=(8,8), offset=(7,7)
    //   s_waitcnt
    //   v0 = s4 * v0 + s6; v1 = s5 * v1 + s7  -> maps [-1,-0.5] triangle onto [-1,3] full viewport
    //   exp pos0, v0, v1, v2, v3
    //   exp param0, v4, v5, v6, v7 done       -> forwards vertex color (0.8, 0.5, 0.8, 1.0)
    //   s_endpgm
    std::vector<uint32_t> vsWords;
    AppendSmemLoad(vsWords, kSmemLoadDwordX4, 4, 0, 0x00u);
    AppendSopp(vsWords, kSoppWaitcnt, 0);
    AppendVop2(vsWords, kVop2MulF32, 0, SgprSrc(4), 0);
    AppendVop2(vsWords, kVop2AddF32, 0, SgprSrc(6), 0);
    AppendVop2(vsWords, kVop2MulF32, 1, SgprSrc(5), 1);
    AppendVop2(vsWords, kVop2AddF32, 1, SgprSrc(7), 1);
    AppendExp(vsWords, kExpTargetPos0,   0, 1, 2, 3, 0xFu, false, false);
    AppendExp(vsWords, kExpTargetParam0, 4, 5, 6, 7, 0xFu, true,  false);
    AppendSopp(vsWords, kSoppEndpgm);

    // Binary RDNA2 Pixel Shader:
    //   s_load_dwordx4 s[8:11], s[0:1], 0x10  -> tint=(0.5, 1.0, 0.25, 1.0)
    //   s_waitcnt
    //   v0..v2 = param0.xyz via VINTRP
    //   v0 *= s8 (0.40); v1 *= s9 (0.50); v2 *= s10 (0.20); v3 = s11 (1.0)
    //   exp mrt0, v0, v1, v2, v3 done vm
    //   s_endpgm
    std::vector<uint32_t> psWords;
    AppendSmemLoad(psWords, kSmemLoadDwordX4, 8, 0, 0x10u);
    AppendSopp(psWords, kSoppWaitcnt, 0);
    AppendVintrpMov(psWords, 0, 0, 0);
    AppendVintrpMov(psWords, 1, 0, 1);
    AppendVintrpMov(psWords, 2, 0, 2);
    AppendVop2(psWords, kVop2MulF32, 0, SgprSrc(8),  0);
    AppendVop2(psWords, kVop2MulF32, 1, SgprSrc(9),  1);
    AppendVop2(psWords, kVop2MulF32, 2, SgprSrc(10), 2);
    AppendVop1(psWords, kVop1MovB32, 3, SgprSrc(11));
    AppendExpMrt0(psWords, 0, 1, 2, 3, 0xFu, true, true);
    AppendSopp(psWords, kSoppEndpgm);

    DecodedProgram vsDec{}, psDec{};
    std::string err;
    SC_EXPECT(DecodeProgram(vsWords.data(), vsWords.size() * sizeof(uint32_t), vsDec, err));
    SC_EXPECT(DecodeProgram(psWords.data(), psWords.size() * sizeof(uint32_t), psDec, err));
    SC_EXPECT(Disassemble(vsDec).find("s_load_dwordx4") != std::string::npos);
    SC_EXPECT(Disassemble(vsDec).find("exp pos0") != std::string::npos);
    SC_EXPECT(Disassemble(psDec).find("v_interp_mov_f32") != std::string::npos);

    auto vsScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        vsWords.data(), vsWords.size() * sizeof(uint32_t), 6, 6);
    auto psScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        psWords.data(), psWords.size() * sizeof(uint32_t), 6, 6);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue)));

    D3D12_ROOT_PARAMETER cbvParam{};
    cbvParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbvParam.Descriptor = { 0, 0 };
    cbvParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &cbvParam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_INPUT_ELEMENT_DESC inputElems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsScxs.data(), vsScxs.size() };
    psoDesc.PS = { psScxs.data(), psScxs.size() };
    psoDesc.InputLayout = { inputElems, static_cast<UINT>(_countof(inputElems)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));

    struct TestVert { float pos[4]; float col[4]; };
    const TestVert verts[3] = {
        { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } },
        { { -1.0f, -0.5f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } },
        { { -0.5f, -1.0f, 0.0f, 1.0f }, { 0.8f, 0.5f, 0.8f, 1.0f } }
    };

    alignas(256) float cbData[64]{};
    cbData[0] = 8.0f; cbData[1] = 8.0f; cbData[2] = 7.0f;  cbData[3] = 7.0f; // gCb0[0]
    cbData[4] = 0.5f; cbData[5] = 1.0f; cbData[6] = 0.25f; cbData[7] = 1.0f; // gCb0[1]

    D3D12_HEAP_PROPERTIES upHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    auto makeBufDesc = [](UINT64 bytes) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };

    ComPtr<ID3D12Resource> vbBuf, cbBuf, rtTex, rbBuf;
    D3D12_RESOURCE_DESC vbDesc = makeBufDesc(sizeof(verts));
    D3D12_RESOURCE_DESC cbDesc = makeBufDesc(sizeof(cbData));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbBuf))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbBuf))));

    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(vbBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, verts, sizeof(verts));
    vbBuf->Unmap(0, nullptr);
    SC_EXPECT(SUCCEEDED(cbBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, cbData, sizeof(cbData));
    cbBuf->Unmap(0, nullptr);

    constexpr UINT kRtSize = 16;
    constexpr UINT kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT kRbBytes = kRowPitch * kRtSize;

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = rtDesc.Format;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc = makeBufDesc(kRbBytes);
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&rtvHeap))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kRtSize), static_cast<float>(kRtSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kRtSize), static_cast<LONG>(kRtSize) };
    D3D12_VERTEX_BUFFER_VIEW vbv{ vbBuf->GetGPUVirtualAddress(), sizeof(verts), sizeof(TestVert) };

    cmdList->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, cbBuf->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmdList->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rtTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kRtSize, kRtSize, 1, kRowPitch };
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(cmdQueue->Signal(fence.Get(), 1)));

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    D3D12_RANGE rr{ 0, kRbBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &rr, &mapped)));
    const auto* px = static_cast<const uint8_t*>(mapped) + (8u * kRowPitch) + (8u * 4u);
    const bool ok = (std::abs(static_cast<int>(px[0]) - 102) <= 1) &&
                    (std::abs(static_cast<int>(px[1]) - 128) <= 1) &&
                    (std::abs(static_cast<int>(px[2]) - 51)  <= 1) &&
                    (px[3] == 255);
    rbBuf->Unmap(0, &noRead);
    SC_EXPECT(ok);
    return true;
}

// 21. xg_xs + d3d12_xs: RDNA2 64KB_S swizzle & BC1/BC3/BC7 NPOT multi-mip round-trips + RTX 5070 BC1 sample

bool TestRdna2ComputeDispatchE2E() {
    using Microsoft::WRL::ComPtr;
    using namespace sc::d3d12;
    using namespace sc::d3d12::rdna2;

    // 1. Assemble binary RDNA2 compute shader: gOut[i] = (7 + gIn[i]) + gIn[i] = 2 * gIn[i] + 7
    std::vector<uint32_t> isa;
    AppendMubufLoadDword(isa, 1, 0, 0);                        // u1 = gIn[u0]
    AppendVop2LiteralU32(isa, kVop2AddNcU32, 2, 7u, 1);        // u2 = 7u + u1
    AppendVop2(isa, kVop2AddNcU32, 2, kSrcVgprBase + 2u, 1);   // u2 = u2 + u1
    AppendMubufStoreDword(isa, 2, 0, 0);                       // gOut[u0] = u2
    AppendSopp(isa, kSoppEndpgm);

    auto scxs = BuildScxsContainer(
        ScxsShaderStage::Compute,
        ScxsTargetArch::Rdna2Scarlett,
        ScxsPayloadFormat::Rdna2Isa,
        isa.data(), isa.size() * sizeof(uint32_t),
        6, 6);

    D3D12_SHADER_BYTECODE inBc{ scxs.data(), scxs.size() };
    Rdna2IsaTranslator translator;
    TranslatedShader translated{};
    std::string err;
    SC_EXPECT(translator.CanTranslate(inBc));
    SC_EXPECT(SUCCEEDED(translator.Translate(inBc, translated, err)));
    SC_EXPECT(!translated.bytecode.empty());

    // 2. Create real D3D12 device, root signature (t0 SRV + u0 UAV), and compute PSO
    HMODULE d3d12Mod = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(d3d12Mod != nullptr);
    auto pD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Mod, "D3D12CreateDevice"));
    auto pD3D12SerializeRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
        GetProcAddress(d3d12Mod, "D3D12SerializeRootSignature"));
    SC_EXPECT(pD3D12CreateDevice != nullptr && pD3D12SerializeRootSignature != nullptr);

    ComPtr<ID3D12Device> dev;
    SC_EXPECT(SUCCEEDED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    SC_EXPECT(SUCCEEDED(pD3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(dev->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS.pShaderBytecode = translated.bytecode.data();
    psoDesc.CS.BytecodeLength = translated.bytecode.size();
    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso))));

    // 3. Allocate 64-uint input (UPLOAD), output (DEFAULT+UAV), and readback (READBACK) buffers
    constexpr uint32_t kCount = 64;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC inDesc{};
    inDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    inDesc.Width = kBytes;
    inDesc.Height = 1;
    inDesc.DepthOrArraySize = 1;
    inDesc.MipLevels = 1;
    inDesc.SampleDesc.Count = 1;
    inDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RESOURCE_DESC outDesc = inDesc;
    outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> inBuf, outBuf, rbBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &inDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inBuf))));
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outBuf))));
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &inDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    uint32_t* inPtr = nullptr;
    SC_EXPECT(SUCCEEDED(inBuf->Map(0, nullptr, reinterpret_cast<void**>(&inPtr))));
    for (uint32_t i = 0; i < kCount; ++i) {
        inPtr[i] = i * 11u + 5u;
    }
    inBuf->Unmap(0, nullptr);

    // 4. Dispatch(1, 1, 1) on GPU and read back all 64 outputs
    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> cq;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&cq))));
    ComPtr<ID3D12CommandAllocator> ca;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))));
    ComPtr<ID3D12GraphicsCommandList> cl;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), pso.Get(), IID_PPV_ARGS(&cl))));

    cl->SetComputeRootSignature(rootSig.Get());
    cl->SetComputeRootShaderResourceView(0, inBuf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = outBuf.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);
    cl->CopyBufferRegion(rbBuf.Get(), 0, outBuf.Get(), 0, kBytes);
    SC_EXPECT(SUCCEEDED(cl->Close()));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEv != nullptr);

    ID3D12CommandList* lists[] = { cl.Get() };
    cq->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(cq->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, fenceEv)));
    SC_EXPECT(WaitForSingleObject(fenceEv, 5000) == WAIT_OBJECT_0);
    CloseHandle(fenceEv);

    uint32_t* rbPtr = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&rbPtr))));
    bool allMatch = true;
    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t expected = (i * 11u + 5u) * 2u + 7u;
        if (rbPtr[i] != expected) {
            allMatch = false;
            break;
        }
    }
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(allMatch);
    return true;
}

bool TestRdna2TexturedDrawE2E() {
    using namespace sc::d3d12::rdna2;
    using Microsoft::WRL::ComPtr;

    // 1. Build, decode, lift, and compile RDNA2 IMAGE_SAMPLE pixel shader
    std::vector<uint32_t> isa;
    AppendVintrpMov(isa, 0, 0, 0);
    AppendVintrpMov(isa, 1, 0, 1);
    AppendMimgImageSample(isa, 2, 0, 0, 0, 0xFu, 1u);
    AppendExp(isa, kExpTargetMrt0, 2, 3, 4, 5, 0xFu, true, false);
    AppendSopp(isa, kSoppEndpgm);

    DecodedProgram prog{};
    std::string err;
    SC_EXPECT(DecodeProgram(isa.data(), isa.size() * sizeof(uint32_t), prog, err));

    std::string psHlsl;
    SC_EXPECT(LiftPixelShaderToHlsl(prog, psHlsl, err));

    ComPtr<ID3DBlob> psBlob, vsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(
        psHlsl.data(), psHlsl.size(), "rdna2_e2e_ps.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob)));

    const char* vsHlsl = R"(
struct VSOut {
    float4 svPos  : SV_POSITION;
    float4 param0 : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.svPos  = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.param0 = float4(uv, 0.0f, 1.0f);
    return o;
}
)";
    SC_EXPECT(SUCCEEDED(D3DCompile(
        vsHlsl, std::strlen(vsHlsl), "rdna2_e2e_vs.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob)));

    // 2. Create D3D12 device, queue, allocator, list, and fence
    HMODULE d3d12Mod = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(d3d12Mod != nullptr);
    auto pD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Mod, "D3D12CreateDevice"));
    SC_EXPECT(pD3D12CreateDevice != nullptr);

    ComPtr<ID3D12Device> dev;
    SC_EXPECT(SUCCEEDED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmd;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvent != nullptr);

    // 3. Root signature: 1 SRV descriptor table (t0) + 1 static sampler (s0)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC staticSamp{};
    staticSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamp.MaxLOD = D3D12_FLOAT32_MAX;
    staticSamp.ShaderRegister = 0;
    staticSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSamp;

    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    // 4. Graphics PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))));

    // 5. Create 2x2 SRV texture + upload buffer, 64x64 RTV texture + readback buffer
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 2;
    texDesc.Height = 2;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> srcTex;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&srcTex))));

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = 512; // 2 rows * 256-byte aligned pitch
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf))));

    uint8_t* upPtr = nullptr;
    SC_EXPECT(SUCCEEDED(upBuf->Map(0, nullptr, reinterpret_cast<void**>(&upPtr))));
    std::memset(upPtr, 0, 512);
    const uint8_t kExpected[4] = { 40, 180, 240, 255 };
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            std::memcpy(upPtr + y * 256 + x * 4, kExpected, 4);
        }
    }
    upBuf->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC rtDesc = texDesc;
    rtDesc.Width = 64;
    rtDesc.Height = 64;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = rtDesc.Format;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC rbDesc = upDesc;
    rbDesc.Width = 64 * 256; // 64 rows * 256 bytes/row (64 * 4 = 256)
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    // 6. Descriptor heaps (RTV + shader-visible SRV)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));
    dev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap))));
    dev->CreateShaderResourceView(srcTex.Get(), nullptr, srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 7. Record upload -> transition -> draw -> readback
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upBuf.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = srcTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition = { srcTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    cmd->ResourceBarrier(1, &toSrv);

    const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, 64, 64 };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmd->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION rbDst{};
    rbDst.pResource = rbBuf.Get();
    rbDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rbDst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };

    D3D12_TEXTURE_COPY_LOCATION rtSrc{};
    rtSrc.pResource = rtTex.Get();
    rtSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&rbDst, 0, 0, 0, &rtSrc, nullptr);

    SC_EXPECT(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, fenceEvent)));
    WaitForSingleObject(fenceEvent, 5000);
    CloseHandle(fenceEvent);

    // 8. Verify center pixel (32, 32) matches sampled RGBA texel
    uint8_t* rbPtr = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&rbPtr))));
    const uint8_t* px = rbPtr + 32 * 256 + 32 * 4;
    const bool match = (px[0] == kExpected[0] && px[1] == kExpected[1] &&
                        px[2] == kExpected[2] && px[3] == kExpected[3]);
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(match);

    return true;
}

bool TestRdna2ImageSampleShader() {
    using namespace sc::d3d12::rdna2;

    // Build RDNA2 PS:
    //   v0 = param0.x (U), v1 = param0.y (V)
    //   v2..v5 = image_sample(v0..v1, t0, s0, dmask=0xF)
    //   exp mrt0, v2, v3, v4, v5 (done)
    //   s_endpgm
    std::vector<uint32_t> isa;
    AppendVintrpMov(isa, 0, 0, 0);
    AppendVintrpMov(isa, 1, 0, 1);
    AppendMimgImageSample(isa, 2, 0, 0, 0, 0xFu, 1u);
    AppendExp(isa, kExpTargetMrt0, 2, 3, 4, 5, 0xFu, true, false);
    AppendSopp(isa, kSoppEndpgm);

    DecodedProgram prog{};
    std::string err;
    SC_EXPECT(DecodeProgram(isa.data(), isa.size() * sizeof(uint32_t), prog, err));
    SC_EXPECT(prog.insts.size() == 5u);
    SC_EXPECT(prog.insts[2].format == InstFormat::Mimg);
    SC_EXPECT(prog.insts[2].opcode == kMimgImageSample);
    SC_EXPECT(prog.insts[2].mimgDmask == 0xFu);
    SC_EXPECT(prog.insts[2].mimgVdata == 2u);
    SC_EXPECT(prog.insts[2].mimgVaddr == 0u);

    std::string hlsl;
    SC_EXPECT(LiftPixelShaderToHlsl(prog, hlsl, err));
    SC_EXPECT(hlsl.find("Texture2D<float4> gTex0 : register(t0);") != std::string::npos);
    SC_EXPECT(hlsl.find("SamplerState gSamp0 : register(s0);") != std::string::npos);
    SC_EXPECT(hlsl.find("gTex0.Sample(gSamp0, float2(asfloat(v0), asfloat(v1)))") != std::string::npos);

    // Compile lifted HLSL to real DXBC ps_5_0 bytecode
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(), "rdna2_image_sample_ps.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &bytecode, &errors);
    if (errors) errors->Release();
    SC_EXPECT(SUCCEEDED(hr) && bytecode != nullptr && bytecode->GetBufferSize() > 0);
    bytecode->Release();

    return true;
}

bool TestD3D12Xs3DPerspectiveDepthRenderE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));
    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
        reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    // 1. Compile 3D perspective VS + PS
    static const char* k3dHlsl = R"(
        cbuffer CameraCB : register(b0) {
            row_major float4x4 mvp;
        };
        struct VSIn {
            float3 pos : POSITION;
            float4 col : COLOR;
        };
        struct PSIn {
            float4 pos : SV_POSITION;
            float4 col : COLOR;
        };
        PSIn VSMain(VSIn input) {
            PSIn o;
            o.pos = mul(float4(input.pos, 1.0f), mvp);
            o.col = input.col;
            return o;
        }
        float4 PSMain(PSIn input) : SV_TARGET {
            return input.col;
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(k3dHlsl, std::strlen(k3dHlsl), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(k3dHlsl, std::strlen(k3dHlsl), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob)));

    // 2. Root signature with 1 CBV at b0
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor = { 0, 0 };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rp;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateRootSignature(0, sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    // 3. Graphics PSO with hardware depth testing enabled (D32_FLOAT, LESS)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.InputLayout = { inputLayout, 2 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))));

    // 4. Create 64x64 RTV + D32_FLOAT DSV + Readback buffer
    constexpr uint32_t kW = 64, kH = 64, kRowPitch = 256;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC dsvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    SC_EXPECT(SUCCEEDED(devX->CreateDescriptorHeap(&rtvHD, IID_PPV_ARGS(&rtvHeap))));
    SC_EXPECT(SUCCEEDED(devX->CreateDescriptorHeap(&dsvHD, IID_PPV_ARGS(&dsvHeap))));

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uplHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kW; rtDesc.Height = kH; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtClear.Color[0] = 16.0f / 255.0f; rtClear.Color[1] = 16.0f / 255.0f;
    rtClear.Color[2] = 24.0f / 255.0f; rtClear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rtTex))));

    D3D12_RESOURCE_DESC dsDesc{};
    dsDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dsDesc.Width = kW; dsDesc.Height = kH; dsDesc.DepthOrArraySize = 1; dsDesc.MipLevels = 1;
    dsDesc.Format = DXGI_FORMAT_D32_FLOAT; dsDesc.SampleDesc.Count = 1;
    dsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dsClear{};
    dsClear.Format = DXGI_FORMAT_D32_FLOAT;
    dsClear.DepthStencil.Depth = 1.0f;
    ComPtr<ID3D12Resource> dsTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &dsDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dsClear, IID_PPV_ARGS(&dsTex))));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvH);
    nativeDev->CreateDepthStencilView(dsTex.Get(), nullptr, dsvH);

    // 5. Upload 3D Vertex Buffer (Near Crimson z=2.0 + Far Blue z=3.5), Index Buffer, and Perspective MVP CBV
    struct Vertex3D { float x, y, z; float r, g, b, a; };
    const Vertex3D verts[8] = {
        // Near quad (z = 2.0, Scarlett Crimson) — drawn FIRST
        { -0.7f, -0.6f, 2.0f, 1.0f, 0.0f, 64.0f / 255.0f, 1.0f },
        {  0.3f, -0.6f, 2.0f, 1.0f, 0.0f, 64.0f / 255.0f, 1.0f },
        {  0.3f,  0.6f, 2.0f, 1.0f, 0.0f, 64.0f / 255.0f, 1.0f },
        { -0.7f,  0.6f, 2.0f, 1.0f, 0.0f, 64.0f / 255.0f, 1.0f },
        // Far quad (z = 3.5, Cobalt Blue) — drawn SECOND, overlaps center x=32
        { -0.5f, -1.0f, 3.5f, 0.0f, 128.0f / 255.0f, 1.0f, 1.0f },
        {  1.4f, -1.0f, 3.5f, 0.0f, 128.0f / 255.0f, 1.0f, 1.0f },
        {  1.4f,  1.0f, 3.5f, 0.0f, 128.0f / 255.0f, 1.0f, 1.0f },
        { -0.5f,  1.0f, 3.5f, 0.0f, 128.0f / 255.0f, 1.0f, 1.0f },
    };
    const uint16_t indices[12] = { 0,1,2, 0,2,3, 4,5,6, 4,6,7 };

    // Left-handed perspective projection: fovY=60deg (f=1.73205), aspect=1, near=0.5, far=10.0
    const float f = 1.7320508f;
    const float a = 10.0f / (10.0f - 0.5f);
    const float b = (-0.5f * 10.0f) / (10.0f - 0.5f);
    float mvp[16] = {
        f,    0.0f, 0.0f, 0.0f,
        0.0f, f,    0.0f, 0.0f,
        0.0f, 0.0f, a,    1.0f,
        0.0f, 0.0f, b,    0.0f
    };

    auto makeBuf = [&](size_t sz, const D3D12_HEAP_PROPERTIES& hp, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(devX->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
    };

    ComPtr<ID3D12Resource> vbBuf, ibBuf, cbBuf, rbBuf;
    SC_EXPECT(makeBuf(sizeof(verts), uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, vbBuf));
    SC_EXPECT(makeBuf(sizeof(indices), uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, ibBuf));
    SC_EXPECT(makeBuf(256, uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, cbBuf));
    SC_EXPECT(makeBuf(static_cast<size_t>(kRowPitch) * kH, rbHeap, D3D12_RESOURCE_STATE_COPY_DEST, rbBuf));

    void* pMap = nullptr;
    SC_EXPECT(SUCCEEDED(vbBuf->Map(0, nullptr, &pMap))); std::memcpy(pMap, verts, sizeof(verts)); vbBuf->Unmap(0, nullptr);
    SC_EXPECT(SUCCEEDED(ibBuf->Map(0, nullptr, &pMap))); std::memcpy(pMap, indices, sizeof(indices)); ibBuf->Unmap(0, nullptr);
    SC_EXPECT(SUCCEEDED(cbBuf->Map(0, nullptr, &pMap))); std::memcpy(pMap, mvp, sizeof(mvp)); cbBuf->Unmap(0, nullptr);

    // 6. Record 3D draw commands through ID3D12GraphicsCommandListX
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH) };
    D3D12_VERTEX_BUFFER_VIEW vbv{ vbBuf->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex3D) };
    D3D12_INDEX_BUFFER_VIEW  ibv{ ibBuf->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT };

    clX->SetPipelineState(pso.Get());
    clX->SetGraphicsRootSignature(rootSig.Get());
    clX->SetGraphicsRootConstantBufferView(0, cbBuf->GetGPUVirtualAddress());
    clX->RSSetViewports(1, &vp);
    clX->RSSetScissorRects(1, &scissor);
    clX->OMSetRenderTargets(1, &rtvH, FALSE, &dsvH);
    clX->ClearRenderTargetView(rtvH, rtClear.Color, 0, nullptr);
    clX->ClearDepthStencilView(dsvH, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    clX->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    clX->IASetVertexBuffers(0, 1, &vbv);
    clX->IASetIndexBuffer(&ibv);
    clX->DrawIndexedInstanced(12, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    clX->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION srcL{ rtTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { 0 } };
    D3D12_TEXTURE_COPY_LOCATION dstL{};
    dstL.pResource = rbBuf.Get();
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstL.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kRowPitch };
    clX->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, nullptr);
    SC_EXPECT(SUCCEEDED(clX->Close()));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(ev != nullptr);
    ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
    queueX->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(queueX->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
    SC_EXPECT(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);

    // 7. Verify 3D perspective + Z-buffer occlusion readback
    uint8_t* px = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&px))));
    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return px + static_cast<size_t>(y) * kRowPitch + static_cast<size_t>(x) * 4u;
    };

    const uint8_t* bg      = pixelAt(2, 2);   // Background dark slate (16, 16, 24)
    const uint8_t* nearPx  = pixelAt(20, 32); // Near Crimson (255, 0, 64)
    const uint8_t* overlap = pixelAt(32, 32); // Overlap: Near Crimson MUST win over Far Blue
    const uint8_t* farPx   = pixelAt(46, 32); // Non-occluded Far Blue (0, 128, 255)

    SC_EXPECT(bg[0] == 16 && bg[1] == 16 && bg[2] == 24 && bg[3] == 255);
    SC_EXPECT(nearPx[0] == 255 && nearPx[1] == 0 && nearPx[2] == 64 && nearPx[3] == 255);
    SC_EXPECT(overlap[0] == 255 && overlap[1] == 0 && overlap[2] == 64 && overlap[3] == 255);
    SC_EXPECT(farPx[0] == 0 && farPx[1] == 128 && farPx[2] == 255 && farPx[3] == 255);

    // Dump 8x upscaled 512x512 24-bit BMP to build\scarlett_3d_render.bmp
    {
        constexpr uint32_t kScale = 8, outW = kW * kScale, outH = kH * kScale;
        constexpr uint32_t imgBytes = outW * outH * 3;
        constexpr uint32_t fileBytes = 54 + imgBytes;
        uint8_t hdr[54]{};
        hdr[0] = 'B'; hdr[1] = 'M';
        std::memcpy(hdr + 2,  &fileBytes, 4);
        uint32_t offBits = 54, dibSize = 40;
        uint16_t planes = 1, bpp = 24;
        std::memcpy(hdr + 10, &offBits, 4);
        std::memcpy(hdr + 14, &dibSize, 4);
        std::memcpy(hdr + 18, &outW, 4);
        std::memcpy(hdr + 22, &outH, 4);
        std::memcpy(hdr + 26, &planes, 2);
        std::memcpy(hdr + 28, &bpp, 2);
        std::memcpy(hdr + 34, &imgBytes, 4);

        std::ofstream bmp("E:\\gta\\test\\scarlett-layer\\build\\scarlett_3d_render.bmp", std::ios::binary);
        if (bmp) {
            bmp.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            std::vector<uint8_t> row(outW * 3);
            for (int y = static_cast<int>(outH) - 1; y >= 0; --y) {
                const uint32_t sy = static_cast<uint32_t>(y) / kScale;
                for (uint32_t x = 0; x < outW; ++x) {
                    const uint8_t* p = pixelAt(x / kScale, sy);
                    row[x * 3 + 0] = p[2]; // B
                    row[x * 3 + 1] = p[1]; // G
                    row[x * 3 + 2] = p[0]; // R
                }
                bmp.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
            }
        }
    }

    rbBuf->Unmap(0, nullptr);
    return true;
}

bool TestD3D12XsHeadlessPresentXModeE2E() {
    using Microsoft::WRL::ComPtr;

    char prevEnv[16]{};
    const bool hadPrev = GetEnvironmentVariableA("SCARLETT_HEADLESS", prevEnv, sizeof(prevEnv)) > 0;
    SetEnvironmentVariableA("SCARLETT_HEADLESS", "1");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 64; rtDesc.Height = 64; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&rtTex))));

    ID3D12Resource* resArr[] = { rtTex.Get() };
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1;
    plane.ppResources   = resArr;
    D3D12XBOX_PRESENT_PARAMETERS params{};

    for (uint32_t i = 0; i < 5; ++i) {
        SC_EXPECT(queueX->PresentX(1, &plane, &params) == S_OK);
    }

    SetEnvironmentVariableA("SCARLETT_HEADLESS", hadPrev ? prevEnv : nullptr);
    return true;
}

bool TestD3D12XsFramePacingTokensE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    // 1. Configure 5 ms (200 Hz) frame interval and verify ScheduleFrameEventX signals
    SC_EXPECT(SUCCEEDED(devX->SetFrameIntervalX(nullptr, 5000, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)));
    HANDLE schedEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(schedEv != nullptr);
    SC_EXPECT(SUCCEEDED(devX->ScheduleFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 0, schedEv, 0)));
    SC_EXPECT(WaitForSingleObject(schedEv, 1000) == WAIT_OBJECT_0);
    CloseHandle(schedEv);

    // 2. Create 64x64 PRESENT render target for token-synced PresentX
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 64; rtDesc.Height = 64; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&rtTex))));

    ID3D12Resource* resArr[] = { rtTex.Get() };
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1;
    plane.ppResources   = resArr;
    D3D12XBOX_PRESENT_PARAMETERS params{};

    // 3. Run 4 paced frames: verify monotonic tokens + QPC pacing elapsed >= 12 ms
    LARGE_INTEGER freq{}, tStart{}, tEnd{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    D3D12XBOX_FRAME_PIPELINE_TOKEN prevToken = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
    for (uint32_t i = 0; i < 4; ++i) {
        D3D12XBOX_FRAME_PIPELINE_TOKEN tok = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
        SC_EXPECT(SUCCEEDED(devX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000, nullptr, D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &tok)));
        SC_EXPECT(tok != D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL);
        if (i > 0) {
            SC_EXPECT(tok == prevToken + 1);
        }
        prevToken = tok;

        plane.Token = tok;
        SC_EXPECT(queueX->PresentX(1, &plane, &params) == S_OK);
    }

    QueryPerformanceCounter(&tEnd);
    const double elapsedMs = static_cast<double>(tEnd.QuadPart - tStart.QuadPart) * 1000.0 /
                             static_cast<double>(freq.QuadPart);
    SC_EXPECT(elapsedMs >= 12.0);
    return true;
}

bool TestD3D12XsPresentXSwapChainE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    // 1. PresentX(0, nullptr, nullptr) clears display planes (S_OK per Microsoft SimpleTriangle);
    //    non-zero PlaneCount with null pPlaneParameters must return E_INVALIDARG.
    SC_EXPECT(queueX->PresentX(0, nullptr, nullptr) == S_OK);
    SC_EXPECT(queueX->PresentX(1, nullptr, nullptr) == E_INVALIDARG);

    // 2. Create 128x128 RGBA8 render target in PRESENT state and present 3 frames via DXGI SwapChain
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 128; rtDesc.Height = 128; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&rtTex))));

    ID3D12Resource* resArr[] = { rtTex.Get() };
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1;
    plane.ppResources   = resArr;
    D3D12XBOX_PRESENT_PARAMETERS params{};

    for (uint32_t frame = 0; frame < 3; ++frame) {
        HRESULT hr = queueX->PresentX(1, &plane, &params);
        SC_EXPECT(hr == S_OK);
    }
    return true;
}

bool TestPresentXDescriptorAudit() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = D3D12_SDK_VERSION;
    ID3D12DeviceX* dev = nullptr;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(&dev))) && dev != nullptr);

    // 1. Audit descriptor handle increment sizes for all 4 heap types
    const UINT incCbv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT incSmp = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const UINT incRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT incDsv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    SC_EXPECT(incCbv > 0 && incSmp > 0 && incRtv > 0 && incDsv > 0);

    // 2. Create command queue, allocator, command list, and RTV heap
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueueX* queue = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueueX(&qDesc, &queue)) && queue != nullptr);

    // PresentX(0, nullptr, nullptr) is a valid clear-plane call; (1, nullptr, nullptr) must fail.
    SC_EXPECT(SUCCEEDED(queue->PresentX(0, nullptr, nullptr)));
    SC_EXPECT(FAILED(queue->PresentX(1, nullptr, nullptr)));

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))) && rtvHeap != nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv1{ rtv0.ptr + incRtv };
    SC_EXPECT(rtv0.ptr != 0 && rtv1.ptr == rtv0.ptr + incRtv);

    // 3. Create 64x64 R8G8B8A8_UNORM render target + readback buffer
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 64;
    rtDesc.Height = 64;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = rtDesc.Format;
    rtClear.Color[0] = 0.25f;
    rtClear.Color[1] = 0.50f;
    rtClear.Color[2] = 0.75f;
    rtClear.Color[3] = 1.0f;

    ID3D12Resource* rt = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rt))) && rt != nullptr);

    dev->CreateRenderTargetView(rt, nullptr, rtv0);

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 64; // 256-byte aligned row pitch * 64 rows
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) && readback != nullptr);

    // 4. Bind RTV via descriptor handle, clear to non-white RGBA, copy to readback, transition to PRESENT
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))));

    const float clearColor[4] = { 0.25f, 0.50f, 0.75f, 1.0f };
    cmd->OMSetRenderTargets(1, &rtv0, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv0, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = rt;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &b0);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = readback;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = 64;
    dstLoc.PlacedFootprint.Footprint.Height = 64;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = 256;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rt;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = rt;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &b1);

    SC_EXPECT(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);

    // 5. Submit PresentX with the rendered target
    ID3D12Resource* planes[] = { rt };
    D3D12XBOX_PRESENT_PLANE_PARAMETERS planeParams{};
    planeParams.ResourceCount = 1;
    planeParams.ppResources = planes;
    SC_EXPECT(SUCCEEDED(queue->PresentX(1, &planeParams, nullptr)));

    // Fence wait and verify non-white pixel values
    ID3D12Fence* fence = nullptr;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    SC_EXPECT(SUCCEEDED(queue->Signal(fence, 1)));
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, 5000);
    }
    CloseHandle(evt);

    uint8_t* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped != nullptr);
    // Expected: R~64, G~128, B~191, A=255
    SC_EXPECT(mapped[0] >= 62 && mapped[0] <= 66);
    SC_EXPECT(mapped[1] >= 126 && mapped[1] <= 130);
    SC_EXPECT(mapped[2] >= 189 && mapped[2] <= 193);
    SC_EXPECT(mapped[3] == 255);
    readback->Unmap(0, nullptr);

    fence->Release();
    cmd->Release();
    alloc->Release();
    readback->Release();
    rt->Release();
    rtvHeap->Release();
    queue->Release();
    dev->Release();
    return true;
}

namespace {

std::vector<uint8_t> AppendSyntheticXboxChunks(
    const void* cleanDxbc,
    size_t cleanSize,
    const char* fourcc,
    size_t extraPayloadBytes) {
    const auto* src = static_cast<const uint8_t*>(cleanDxbc);
    sc::d3d12::DxbcHeader hdr{};
    std::memcpy(&hdr, src, sizeof(hdr));

    const uint32_t oldCount = hdr.partCount;
    const uint32_t newCount = oldCount + 1u;
    const uint32_t shift = sizeof(uint32_t); // 1 extra entry in part offset table

    const size_t newChunkOff = (cleanSize + shift + 3u) & ~size_t{3u};
    const size_t totalSize = (newChunkOff + sizeof(sc::d3d12::DxbcPartHeader) + extraPayloadBytes + 3u) & ~size_t{3u};

    std::vector<uint8_t> out(totalSize, 0);
    sc::d3d12::DxbcHeader newHdr = hdr;
    newHdr.fileSize  = static_cast<uint32_t>(totalSize);
    newHdr.partCount = newCount;
    std::memcpy(out.data(), &newHdr, sizeof(newHdr));

    // Shift existing part offsets by +4 bytes
    for (uint32_t i = 0; i < oldCount; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, src + sizeof(sc::d3d12::DxbcHeader) + static_cast<size_t>(i) * 4u, 4u);
        off += shift;
        std::memcpy(out.data() + sizeof(sc::d3d12::DxbcHeader) + static_cast<size_t>(i) * 4u, &off, 4u);
    }
    const uint32_t newChunkOff32 = static_cast<uint32_t>(newChunkOff);
    std::memcpy(out.data() + sizeof(sc::d3d12::DxbcHeader) + static_cast<size_t>(oldCount) * 4u, &newChunkOff32, 4u);

    // Copy original part bodies right after the expanded offset table
    const size_t oldTableEnd = sizeof(sc::d3d12::DxbcHeader) + static_cast<size_t>(oldCount) * 4u;
    std::memcpy(out.data() + oldTableEnd + shift, src + oldTableEnd, cleanSize - oldTableEnd);

    // Append synthetic Xbox proprietary chunk
    sc::d3d12::DxbcPartHeader xPart{};
    std::memcpy(xPart.fourcc, fourcc, 4);
    xPart.size = static_cast<uint32_t>(extraPayloadBytes);
    std::memcpy(out.data() + newChunkOff, &xPart, sizeof(xPart));
    for (size_t i = 0; i < extraPayloadBytes; ++i) {
        out[newChunkOff + sizeof(xPart) + i] = static_cast<uint8_t>((i * 37u) ^ 0xA5u);
    }

    uint8_t digest[16]{};
    sc::d3d12::ComputeDxbcDigest(out.data() + 20, out.size() - 20, digest);
    std::memcpy(out.data() + 4, digest, sizeof(digest));
    return out;
}

} // namespace

bool TestDxbcDigestAndXboxChunkStripperE2E() {
    using Microsoft::WRL::ComPtr;

    const char* vsSrc = R"(
        float4 main(uint vid : SV_VertexID) : SV_Position {
            float2 p[3] = { float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5) };
            return float4(p[vid], 0.0, 1.0);
        }
    )";
    const char* psSrc = R"(
        float4 main() : SV_Target { return float4(0.1, 0.8, 0.3, 1.0); }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(psSrc, std::strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob)));

    // 1. Verify our clean-room DXBC modified-MD5 matches Microsoft D3DCompile digest byte-for-byte
    auto verifyDigest = [](ID3DBlob* blob) -> bool {
        const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
        const size_t len  = blob->GetBufferSize();
        uint8_t computed[16]{};
        sc::d3d12::ComputeDxbcDigest(bytes + 20, len - 20, computed);
        return std::memcmp(bytes + 4, computed, 16) == 0;
    };
    SC_EXPECT(verifyDigest(vsBlob.Get()));
    SC_EXPECT(verifyDigest(psBlob.Get()));
    SC_EXPECT(!sc::d3d12::HasNonPcDxbcChunks(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()));

    // 2. Inject synthetic Xbox chunks ('XBOX' on VS, 'XSHD' on PS) and verify stripping + digest
    const auto xboxVs = AppendSyntheticXboxChunks(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), "XBOX", 32);
    const auto xboxPs = AppendSyntheticXboxChunks(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), "XSHD", 60);
    SC_EXPECT(sc::d3d12::HasNonPcDxbcChunks(xboxVs.data(), xboxVs.size()));
    SC_EXPECT(sc::d3d12::HasNonPcDxbcChunks(xboxPs.data(), xboxPs.size()));

    sc::d3d12::DxbcStripResult stripCheck{};
    SC_EXPECT(sc::d3d12::StripXboxDxbcChunks(xboxVs.data(), xboxVs.size(), stripCheck));
    SC_EXPECT(stripCheck.wasStripped && stripCheck.hasExecutableShader);
    SC_EXPECT(stripCheck.strippedCount == 1u && stripCheck.strippedChunkNames == "XBOX");
    SC_EXPECT(!sc::d3d12::HasNonPcDxbcChunks(stripCheck.bytecode.data(), stripCheck.bytecode.size()));

    // 3. Feed Xbox-tainted VS + PS directly into ID3D12DeviceX::CreateGraphicsPipelineState on live GPU
    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(devX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { xboxVs.data(), xboxVs.size() };
    psoDesc.PS = { xboxPs.data(), xboxPs.size() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(devX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(pso1 != nullptr);
    // Second creation hits the translation + PSO cache cleanly
    SC_EXPECT(SUCCEEDED(devX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso2 != nullptr);
    return true;
}

bool TestD3D12XsEnhancedBarriersE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 opts12{};
    SC_EXPECT(SUCCEEDED(devX->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opts12, sizeof(opts12))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    if (opts12.EnhancedBarriersSupported) {
        D3D12_GLOBAL_BARRIER gb{};
        gb.SyncBefore   = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        gb.SyncAfter    = D3D12_BARRIER_SYNC_ALL_SHADING;
        gb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        gb.AccessAfter  = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;

        D3D12_BARRIER_GROUP group{};
        group.Type            = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers     = 1;
        group.pGlobalBarriers = &gb;

        clX->Barrier(1, &group);
    }

    SC_EXPECT(SUCCEEDED(clX->Close()));
    ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
    queueX->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(devX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    SC_EXPECT(SUCCEEDED(queueX->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
    SC_EXPECT(WaitForSingleObject(evt, 5000) == WAIT_OBJECT_0);
    CloseHandle(evt);
    return true;
}

bool TestD3D12XsFeatureCapsQueryE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_8;
    SC_EXPECT(SUCCEEDED(devX->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))));
    SC_EXPECT(sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0);

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    SC_EXPECT(SUCCEEDED(devX->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7))));

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 opts12{};
    SC_EXPECT(SUCCEEDED(devX->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opts12, sizeof(opts12))));

    // Invalid arg contract check
    SC_EXPECT(FAILED(devX->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, nullptr, sizeof(opts7))));
    return true;
}

bool TestD3D12XsGdkSampleAlignmentE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    // 1. SimpleTriangle init/teardown clear-plane PresentX(0, nullptr, nullptr) + SuspendX/ResumeX
    SC_EXPECT(queueX->PresentX(0, nullptr, nullptr) == S_OK);
    SC_EXPECT(queueX->PresentX(1, nullptr, nullptr) == E_INVALIDARG);
    queueX->SuspendX(0);
    queueX->ResumeX();

    // 2. Real 5-argument WaitFrameEventX + ScheduleFrameEventX
    SC_EXPECT(SUCCEEDED(devX->SetFrameIntervalX(nullptr, D3D12XBOX_FRAME_INTERVAL_60_HZ, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)));
    SC_EXPECT(SUCCEEDED(devX->ScheduleFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 0, nullptr, D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE)));
    D3D12XBOX_FRAME_PIPELINE_TOKEN tok = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
    SC_EXPECT(SUCCEEDED(devX->WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT_ORIGIN, 1000, nullptr, D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &tok)));
    SC_EXPECT(tok != D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL);

    // 3. Console-only upper-16-bit D3D12XBOX_RESOURCE_FLAG_* bits must be stripped cleanly
    constexpr D3D12_RESOURCE_FLAGS kSyntheticXboxFlags =
        static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | 0x10000000u | 0x00040000u);
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 64; rtDesc.Height = 64; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = kSyntheticXboxFlags;

    const D3D12_RESOURCE_ALLOCATION_INFO allocInfo = devX->GetResourceAllocationInfo(0, 1, &rtDesc);
    SC_EXPECT(allocInfo.SizeInBytes > 0 && allocInfo.SizeInBytes != UINT64_MAX);

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&rtTex))));
    SC_EXPECT(rtTex != nullptr);

    // 4. Desktop GDK libHttpClient.dll alias + HCHttpCallRequestSetRequestBodyString export
    HMODULE hcMod = LoadLibraryW(L"libHttpClient.dll");
    SC_EXPECT(hcMod != nullptr);
    using PFN_HCInit = HRESULT(WINAPI*)(void*);
    using PFN_HCCleanup = void(WINAPI*)(void);
    using PFN_HCCreate = HRESULT(WINAPI*)(void**);
    using PFN_HCClose = HRESULT(WINAPI*)(void*);
    using PFN_HCSetBodyStr = HRESULT(WINAPI*)(void*, const char*);
    auto pInit = reinterpret_cast<PFN_HCInit>(GetProcAddress(hcMod, "HCInitialize"));
    auto pClean = reinterpret_cast<PFN_HCCleanup>(GetProcAddress(hcMod, "HCCleanup"));
    auto pCreate = reinterpret_cast<PFN_HCCreate>(GetProcAddress(hcMod, "HCHttpCallCreate"));
    auto pClose = reinterpret_cast<PFN_HCClose>(GetProcAddress(hcMod, "HCHttpCallCloseHandle"));
    auto pSetBodyStr = reinterpret_cast<PFN_HCSetBodyStr>(GetProcAddress(hcMod, "HCHttpCallRequestSetRequestBodyString"));
    SC_EXPECT(pInit && pClean && pCreate && pClose && pSetBodyStr);
    SC_EXPECT(SUCCEEDED(pInit(nullptr)));
    void* call = nullptr;
    SC_EXPECT(SUCCEEDED(pCreate(&call)) && call != nullptr);
    SC_EXPECT(SUCCEEDED(pSetBodyStr(call, "{\"sample\":\"SimpleTriangleDesktop\"}")));
    SC_EXPECT(pSetBodyStr(call, nullptr) == E_INVALIDARG);
    SC_EXPECT(SUCCEEDED(pClose(call)));
    pClean();
    FreeLibrary(hcMod);

    return true;
}
