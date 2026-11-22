#include "test_harness.hpp"

using Microsoft::WRL::ComPtr;

bool TestD3D12HeadlessDeviceAndPso() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    SC_EXPECT(SUCCEEDED(deviceX->SetFrameIntervalX(nullptr, 16667, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)));

    D3D12XBOX_FRAME_PIPELINE_TOKEN t1 = 0;
    D3D12XBOX_FRAME_PIPELINE_TOKEN t2 = 0;
    SC_EXPECT(SUCCEEDED(deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &t1)));
    SC_EXPECT(SUCCEEDED(deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &t2)));
    SC_EXPECT(t2 == t1 + 1);

    const char* hlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION { return float4(0, 0, 0, 1); }\n"
        "float4 PSMain() : SV_TARGET { return float4(0.1, 0.9, 0.4, 1.0); }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "ctest.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "ctest.hlsl", nullptr, nullptr,
                                   "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))));
    SC_EXPECT(pso != nullptr);
    return true;
}

bool TestD3D12PlacedResourcesAndHeap() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_RESOURCE_DESC texA{};
    texA.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texA.Width = 128;
    texA.Height = 128;
    texA.DepthOrArraySize = 1;
    texA.MipLevels = 1;
    texA.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texA.SampleDesc.Count = 1;

    D3D12_RESOURCE_DESC texB = texA;
    texB.Width = 256;
    texB.Height = 256;

    D3D12_RESOURCE_ALLOCATION_INFO infoA = deviceX->GetResourceAllocationInfo(0, 1, &texA);
    D3D12_RESOURCE_ALLOCATION_INFO infoB = deviceX->GetResourceAllocationInfo(0, 1, &texB);
    SC_EXPECT(infoA.SizeInBytes > 0 && infoB.SizeInBytes > 0);

    const UINT64 align = std::max<UINT64>(
        std::max<UINT64>(infoA.Alignment, infoB.Alignment),
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    const UINT64 offsetB = (infoA.SizeInBytes + (align - 1ULL)) & ~(align - 1ULL);
    const UINT64 totalHeapBytes = offsetB + infoB.SizeInBytes;

    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = totalHeapBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = align;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

    ComPtr<ID3D12Heap> heap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))));

    ComPtr<ID3D12Resource> resA, resB;
    SC_EXPECT(SUCCEEDED(deviceX->CreatePlacedResource(
        heap.Get(), 0, &texA, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resA))));
    SC_EXPECT(SUCCEEDED(deviceX->CreatePlacedResource(
        heap.Get(), offsetB, &texB, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resB))));

    SC_EXPECT(resA->GetDesc().Width == 128);
    SC_EXPECT(resB->GetDesc().Width == 256);

    resB.Reset();
    resA.Reset();
    heap.Reset();
    return true;
}

// 12. Deep SM 6.x DXIL container & LLVM bitcode header parsing

bool TestDxilSm6BitcodeInspector() {
    // Synthesize a valid DXBC container with PSV0 + DXIL (ms_6_6, DXIL 1.6, LLVM BC\xC0\xDE)
    const uint8_t fakeLlvmBc[16] = {
        0x42u, 0x43u, 0xC0u, 0xDEu, 0x21u, 0x0Cu, 0x00u, 0x00u,
        0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u
    };

    sc::d3d12::DxilProgramHeader dxilHdr{};
    dxilHdr.programVersion = (13u << 16) | (6u << 4) | 6u; // ms_6_6 (Mesh Shader SM 6.6)
    dxilHdr.sizeInUint32   = static_cast<uint32_t>((sizeof(dxilHdr) + sizeof(fakeLlvmBc)) / 4u);
    dxilHdr.dxilMagic      = sc::d3d12::kDxilMagicFourCc;
    dxilHdr.dxilVersion    = (1u << 8) | 6u;               // DXIL 1.6
    dxilHdr.bitcodeOffset  = 16u;                          // +8 + 16 = 24 (immediately after header)
    dxilHdr.bitcodeSize    = sizeof(fakeLlvmBc);

    const uint32_t psvPayload = 0x00000002u;

    const uint32_t partCount = 2;
    const uint32_t psvOff = sizeof(sc::d3d12::DxbcHeader) + partCount * sizeof(uint32_t);
    const uint32_t dxilOff = psvOff + sizeof(sc::d3d12::DxbcPartHeader) + sizeof(psvPayload);
    const uint32_t totalSize = dxilOff + sizeof(sc::d3d12::DxbcPartHeader)
                             + sizeof(dxilHdr) + sizeof(fakeLlvmBc);

    std::vector<uint8_t> container(totalSize, 0);

    sc::d3d12::DxbcHeader hdr{};
    std::memcpy(hdr.magic, "DXBC", 4);
    hdr.majorVersion = 1;
    hdr.minorVersion = 0;
    hdr.fileSize = totalSize;
    hdr.partCount = partCount;
    std::memcpy(container.data(), &hdr, sizeof(hdr));
    std::memcpy(container.data() + sizeof(hdr), &psvOff, sizeof(uint32_t));
    std::memcpy(container.data() + sizeof(hdr) + sizeof(uint32_t), &dxilOff, sizeof(uint32_t));

    sc::d3d12::DxbcPartHeader psvPart{ {'P','S','V','0'}, sizeof(psvPayload) };
    std::memcpy(container.data() + psvOff, &psvPart, sizeof(psvPart));
    std::memcpy(container.data() + psvOff + sizeof(psvPart), &psvPayload, sizeof(psvPayload));

    sc::d3d12::DxbcPartHeader dxilPart{ {'D','X','I','L'}, sizeof(dxilHdr) + sizeof(fakeLlvmBc) };
    std::memcpy(container.data() + dxilOff, &dxilPart, sizeof(dxilPart));
    std::memcpy(container.data() + dxilOff + sizeof(dxilPart), &dxilHdr, sizeof(dxilHdr));
    std::memcpy(container.data() + dxilOff + sizeof(dxilPart) + sizeof(dxilHdr),
                fakeLlvmBc, sizeof(fakeLlvmBc));

    D3D12_SHADER_BYTECODE bc{ container.data(), container.size() };
    auto info = sc::d3d12::InspectShaderBytecodeDetailed(bc);
    SC_EXPECT(info.kind == sc::d3d12::ShaderContainerKind::Dxil);
    SC_EXPECT(info.profile == "ms_6_6");
    SC_EXPECT(info.dxilMajor == 1u);
    SC_EXPECT(info.dxilMinor == 6u);
    SC_EXPECT(info.hasLlvmBitcode);
    SC_EXPECT(info.bitcodeSize == sizeof(fakeLlvmBc));
    SC_EXPECT(info.hasPsv0);
    SC_EXPECT(info.summary.find("ms_6_6/DXIL 1.6 (LLVM BC)") != std::string::npos);
    return true;
}

// 13. Pluggable SCXS console shader container translator + translation/PSO cache + RDNA2/CRC traps

bool TestScxsTranslatorAndPsoCache() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    const char* hlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_POSITION { return float4(0, 0, 0, 1); }\n"
        "float4 PSMain() : SV_TARGET { return float4(0.2, 0.8, 0.9, 1.0); }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "scxs_test.hlsl", nullptr, nullptr,
                                   "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "scxs_test.hlsl", nullptr, nullptr,
                                   "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    auto vsScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 5, 0);
    auto psScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Pixel,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), 5, 0);

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsScxs.data(), vsScxs.size() };
    psoDesc.PS = { psScxs.data(), psScxs.size() };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Pass 1: Translate SCXS & create/load PSO
    ComPtr<ID3D12PipelineState> pso1;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(pso1 != nullptr);

    // Pass 2: Same SCXS containers -> exercises in-memory translation cache + PSO library HIT
    ComPtr<ID3D12PipelineState> pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso2 != nullptr);

    // Pass 3: Corrupted SCXS payload -> must fail checksum validation with E_INVALIDARG
    auto badCrcVs = vsScxs;
    badCrcVs.back() ^= 0xFFu;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC badCrcDesc = psoDesc;
    badCrcDesc.VS = { badCrcVs.data(), badCrcVs.size() };
    ComPtr<ID3D12PipelineState> psoBadCrc;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&badCrcDesc, IID_PPV_ARGS(&psoBadCrc)) == E_INVALIDARG);
    SC_EXPECT(psoBadCrc == nullptr);

    // Pass 4: Raw RDNA2 ISA SCXS container -> must cleanly trap with E_NOTIMPL at translator seam
    const uint32_t fakeRdna2Words[4] = { 0xBF810000u, 0xBF800000u, 0xBF800000u, 0xBF810000u };
    auto rdna2Vs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Vertex,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        fakeRdna2Words, sizeof(fakeRdna2Words), 6, 6);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rdna2Desc = psoDesc;
    rdna2Desc.VS = { rdna2Vs.data(), rdna2Vs.size() };
    ComPtr<ID3D12PipelineState> psoRdna2;
    SC_EXPECT(deviceX->CreateGraphicsPipelineState(&rdna2Desc, IID_PPV_ARGS(&psoRdna2)) == E_NOTIMPL);
    SC_EXPECT(psoRdna2 == nullptr);
    return true;
}

// 14. dstorage_xs: Live SCLZ CPU decompression over VFS + corrupt stream error trapping

bool TestD3D12ComputePsoAndUavExecution() {
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

    const char* csHlsl =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void CSMain(uint3 dtid : SV_DispatchThreadID) {\n"
        "    gOut[dtid.x] = dtid.x * 7u + 0x53430000u;\n"
        "}\n";

    ComPtr<ID3DBlob> csBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(csHlsl, std::strlen(csHlsl), "ctest_compute.hlsl", nullptr, nullptr,
                                   "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errBlob)));

    auto csScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), 5, 0);

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &uavRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csScxs.data(), csScxs.size() };

    // Pass 1 & 2: Translate SCXS compute shader + hit translation & PSO library cache
    ComPtr<ID3D12PipelineState> pso1, pso2;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso1))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso2))));
    SC_EXPECT(pso1 != nullptr && pso2 != nullptr);

    // Corrupted CRC & RDNA2 ISA traps on Compute PSO path
    auto badCrcCs = csScxs;
    badCrcCs.back() ^= 0xFFu;
    D3D12_COMPUTE_PIPELINE_STATE_DESC badDesc = psoDesc;
    badDesc.CS = { badCrcCs.data(), badCrcCs.size() };
    ComPtr<ID3D12PipelineState> badPso;
    SC_EXPECT(deviceX->CreateComputePipelineState(&badDesc, IID_PPV_ARGS(&badPso)) == E_INVALIDARG);

    const uint32_t fakeRdna2[4] = { 0xBF810000u, 0xBF800000u, 0xBF800000u, 0xBF810000u };
    auto rdna2Cs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::Rdna2Isa,
        fakeRdna2, sizeof(fakeRdna2), 6, 6);
    D3D12_COMPUTE_PIPELINE_STATE_DESC rdna2Desc = psoDesc;
    rdna2Desc.CS = { rdna2Cs.data(), rdna2Cs.size() };
    SC_EXPECT(deviceX->CreateComputePipelineState(&rdna2Desc, IID_PPV_ARGS(&badPso)) == E_NOTIMPL);

    // Allocate GPU UAV buffer + CPU readback buffer (64 uint32_t = 256 bytes)
    constexpr UINT kElemCount = 64;
    constexpr UINT kBufBytes = kElemCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC uavBufDesc{};
    uavBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavBufDesc.Width = kBufBytes;
    uavBufDesc.Height = 1;
    uavBufDesc.DepthOrArraySize = 1;
    uavBufDesc.MipLevels = 1;
    uavBufDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavBufDesc.SampleDesc.Count = 1;
    uavBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &uavBufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));

    D3D12_RESOURCE_DESC rbDesc = uavBufDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readbackBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kElemCount;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    deviceX->CreateUnorderedAccessView(
        uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso1.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(rootSig.Get());
    cmdList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = uavBuf.Get();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = uavBuf.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(2, barriers);

    cmdList->CopyResource(readbackBuf.Get(), uavBuf.Get());
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
    D3D12_RANGE readRange{ 0, kBufBytes };
    SC_EXPECT(SUCCEEDED(readbackBuf->Map(0, &readRange, &mapped)));
    const auto* results = static_cast<const uint32_t*>(mapped);
    bool allExact = true;
    for (UINT i = 0; i < kElemCount; ++i) {
        if (results[i] != (i * 7u + 0x53430000u)) {
            allExact = false;
            break;
        }
    }
    D3D12_RANGE emptyWrite{ 0, 0 };
    readbackBuf->Unmap(0, &emptyWrite);
    SC_EXPECT(allExact);
    return true;
}

// 17. d3d12_xs: Real binary RDNA2 ISA pixel shader -> Decode -> Lift -> DXBC -> RTX 5070 execution & readback

bool TestD3D12AsyncComputeCrossQueueSync() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueueX> computeQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&cqDesc, &computeQueue)));

    D3D12_COMMAND_QUEUE_DESC dqDesc{};
    dqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> directQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&dqDesc, &directQueue)));

    const char* csHlsl =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void CSMain(uint3 dtid : SV_DispatchThreadID) {\n"
        "    gOut[dtid.x] = dtid.x * 13u + 0xA5A50000u;\n"
        "}\n";

    ComPtr<ID3DBlob> csBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(csHlsl, std::strlen(csHlsl), "async_cs.hlsl", nullptr, nullptr,
                                   "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errBlob)));

    auto csScxs = sc::d3d12::BuildScxsContainer(
        sc::d3d12::ScxsShaderStage::Compute,
        sc::d3d12::ScxsTargetArch::Rdna2Scarlett,
        sc::d3d12::ScxsPayloadFormat::EmbeddedDxbcOrDxil,
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), 5, 0);

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &uavRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csScxs.data(), csScxs.size() };

    ComPtr<ID3D12PipelineState> computePso;
    SC_EXPECT(SUCCEEDED(deviceX->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePso))));

    constexpr UINT kElemCount = 64;
    constexpr UINT kBufBytes = kElemCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC uavBufDesc{};
    uavBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavBufDesc.Width = kBufBytes;
    uavBufDesc.Height = 1;
    uavBufDesc.DepthOrArraySize = 1;
    uavBufDesc.MipLevels = 1;
    uavBufDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavBufDesc.SampleDesc.Count = 1;
    uavBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> uavBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &uavBufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));

    D3D12_RESOURCE_DESC rbDesc = uavBufDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readbackBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf))));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kElemCount;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    deviceX->CreateUnorderedAccessView(
        uavBuf.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> compAlloc, dirAlloc;
    ComPtr<ID3D12GraphicsCommandList> compList, dirList;
    ComPtr<ID3D12Fence> syncFence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compAlloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compAlloc.Get(), computePso.Get(), IID_PPV_ARGS(&compList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dirAlloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dirAlloc.Get(), nullptr, IID_PPV_ARGS(&dirList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&syncFence))));

    // Record & submit on COMPUTE queue
    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    compList->SetDescriptorHeaps(1, heaps);
    compList->SetComputeRootSignature(rootSig.Get());
    compList->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
    compList->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = uavBuf.Get();
    compList->ResourceBarrier(1, &uavBar);
    SC_EXPECT(SUCCEEDED(compList->Close()));

    ID3D12CommandList* compLists[] = { compList.Get() };
    computeQueue->ExecuteCommandLists(1, compLists);
    SC_EXPECT(SUCCEEDED(computeQueue->Signal(syncFence.Get(), 1)));

    // GPU-side cross-queue Wait() on DIRECT queue (no CPU wait between compute & copy)
    SC_EXPECT(SUCCEEDED(directQueue->Wait(syncFence.Get(), 1)));
    dirList->CopyResource(readbackBuf.Get(), uavBuf.Get());
    SC_EXPECT(SUCCEEDED(dirList->Close()));

    ID3D12CommandList* dirLists[] = { dirList.Get() };
    directQueue->ExecuteCommandLists(1, dirLists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(syncFence.Get(), 2)));

    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEvt != nullptr);
    if (syncFence->GetCompletedValue() < 2) {
        syncFence->SetEventOnCompletion(2, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
    CloseHandle(fenceEvt);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kBufBytes };
    SC_EXPECT(SUCCEEDED(readbackBuf->Map(0, &readRange, &mapped)));
    const auto* results = static_cast<const uint32_t*>(mapped);
    bool allExact = true;
    for (UINT i = 0; i < kElemCount; ++i) {
        if (results[i] != (i * 13u + 0xA5A50000u)) {
            allExact = false;
            break;
        }
    }
    D3D12_RANGE emptyWrite{ 0, 0 };
    readbackBuf->Unmap(0, &emptyWrite);
    SC_EXPECT(allExact);
    return true;
}

// 19. d3d12_xs: Real binary RDNA2 ISA compute shader -> Decode -> Lift -> cs_5_0 -> RTX 5070 UAV readback

bool TestD3D12XsAsyncComputeCrossQueueSyncE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(devX.GetAddressOf()))));

    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
        reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    // 1. Create separate COMPUTE and DIRECT queues via ID3D12DeviceX
    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueueX> computeQueueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&cqDesc, computeQueueX.GetAddressOf())));

    D3D12_COMMAND_QUEUE_DESC dqDesc{};
    dqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> directQueueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&dqDesc, directQueueX.GetAddressOf())));

    // 2. Create allocators & command lists for each queue type
    ComPtr<ID3D12CommandAllocator> computeAlloc, directAlloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAlloc))));
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,  IID_PPV_ARGS(&directAlloc))));

    ComPtr<ID3D12GraphicsCommandListX> computeClX, directClX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAlloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(computeClX.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, directAlloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(directClX.GetAddressOf()))));

    // 3. Compile minimal CS 5.0 producer shader: uav[tid] = tid * 10 + 7
    static const char* kCsHlsl = R"(
        RWStructuredBuffer<uint> OutBuf : register(u0);
        [numthreads(64, 1, 1)]
        void CSMain(uint3 tid : SV_DispatchThreadID) {
            OutBuf[tid.x] = tid.x * 10u + 7u;
        }
    )";
    ComPtr<ID3DBlob> csBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(kCsHlsl, std::strlen(kCsHlsl), nullptr, nullptr, nullptr,
        "CSMain", "cs_5_0", 0, 0, &csBlob, &errBlob)));

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.Descriptor = { 0, 0 };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> sigBlob;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    ComPtr<ID3D12PipelineState> csPso;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&csPso))));

    // 4. Allocate DEFAULT UAV buffer (64 * uint32) and READBACK buffer
    constexpr uint32_t kCount = 64;
    constexpr UINT64   kBytes = kCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC uavDesc{};
    uavDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavDesc.Width = kBytes; uavDesc.Height = 1; uavDesc.DepthOrArraySize = 1; uavDesc.MipLevels = 1;
    uavDesc.SampleDesc.Count = 1; uavDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> uavBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &uavDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavBuf))));

    D3D12_RESOURCE_DESC rbDesc = uavDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    // 5. Record & submit async compute pass, then signal cross-queue fence
    computeClX->SetPipelineState(csPso.Get());
    computeClX->SetComputeRootSignature(rootSig.Get());
    computeClX->SetComputeRootUnorderedAccessView(0, uavBuf->GetGPUVirtualAddress());
    computeClX->Dispatch(1, 1, 1);
    SC_EXPECT(SUCCEEDED(computeClX->Close()));

    ComPtr<ID3D12Fence> crossFence, doneFence;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&crossFence))));
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&doneFence))));

    ID3D12CommandList* computeLists[] = { computeClX->GetNativeCommandList() };
    computeQueueX->ExecuteCommandLists(1, computeLists);
    SC_EXPECT(SUCCEEDED(computeQueueX->Signal(crossFence.Get(), 1)));

    // 6. Direct queue GPU-waits on compute fence, then copies UAV -> READBACK
    SC_EXPECT(SUCCEEDED(directQueueX->Wait(crossFence.Get(), 1)));

    directClX->CopyBufferRegion(rbBuf.Get(), 0, uavBuf.Get(), 0, kBytes);
    SC_EXPECT(SUCCEEDED(directClX->Close()));

    ID3D12CommandList* directLists[] = { directClX->GetNativeCommandList() };
    directQueueX->ExecuteCommandLists(1, directLists);
    SC_EXPECT(SUCCEEDED(directQueueX->Signal(doneFence.Get(), 1)));

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(ev != nullptr);
    SC_EXPECT(SUCCEEDED(doneFence->SetEventOnCompletion(1, ev)));
    SC_EXPECT(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);

    // 7. Verify all 64 compute-produced values arrived via cross-queue sync
    uint32_t* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, reinterpret_cast<void**>(&mapped))) && mapped != nullptr);
    bool allMatch = true;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (mapped[i] != i * 10u + 7u) {
            allMatch = false;
            break;
        }
    }
    D3D12_RANGE writeRange{ 0, 0 };
    rbBuf->Unmap(0, &writeRange);
    SC_EXPECT(allMatch);

    return true;
}

bool TestD3D12XsComVtableAbiConformanceE2E() {
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

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    // 1. Raw x64 COM vtable pointer inspection (slots 0..2 = IUnknown, plus interface slots)
    auto checkVtableSlots = [](void* comObj, size_t expectedSlots) -> bool {
        if (!comObj) return false;
        auto** vtbl = *reinterpret_cast<void***>(comObj);
        if (!vtbl) return false;
        for (size_t i = 0; i < expectedSlots; ++i) {
            if (vtbl[i] == nullptr) return false;
        }
        return true;
    };
    SC_EXPECT(checkVtableSlots(queueX.Get(), 7));
    SC_EXPECT(checkVtableSlots(clX.Get(), 36));
    SC_EXPECT(checkVtableSlots(devX.Get(), 10));

    // 2. Direct raw vtable invocation of IUnknown (slot 0 = QI, slot 1 = AddRef, slot 2 = Release)
    using PFN_QI      = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
    using PFN_AddRef  = ULONG(STDMETHODCALLTYPE*)(void*);
    using PFN_Release = ULONG(STDMETHODCALLTYPE*)(void*);

    auto** devVtbl = *reinterpret_cast<void***>(devX.Get());
    auto rawQI      = reinterpret_cast<PFN_QI>(devVtbl[0]);
    auto rawAddRef  = reinterpret_cast<PFN_AddRef>(devVtbl[1]);
    auto rawRelease = reinterpret_cast<PFN_Release>(devVtbl[2]);

    const ULONG r1 = rawAddRef(devX.Get());
    const ULONG r2 = rawAddRef(devX.Get());
    SC_EXPECT(r2 == r1 + 1);
    const ULONG r3 = rawRelease(devX.Get());
    const ULONG r4 = rawRelease(devX.Get());
    SC_EXPECT(r3 == r1 && r4 == r1 - 1);

    void* unkPtr = nullptr;
    void* devPtr = nullptr;
    SC_EXPECT(SUCCEEDED(rawQI(devX.Get(), __uuidof(IUnknown), &unkPtr)) && unkPtr != nullptr);
    SC_EXPECT(SUCCEEDED(rawQI(devX.Get(), IID_ID3D12DeviceX, &devPtr)) && devPtr != nullptr);
    SC_EXPECT(unkPtr == devPtr);
    rawRelease(unkPtr);
    rawRelease(devPtr);

    static constexpr GUID kBogusGuid =
        { 0xdeadbeef, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44 } };
    void* bogusPtr = reinterpret_cast<void*>(0x12345678ULL);
    SC_EXPECT(rawQI(devX.Get(), kBogusGuid, &bogusPtr) == E_NOINTERFACE);
    SC_EXPECT(bogusPtr == nullptr);

    // 3. Direct raw vtable invocation of ID3D12GraphicsCommandListX::Close (slot 3)
    using PFN_Close = HRESULT(STDMETHODCALLTYPE*)(void*);
    auto** clVtbl = *reinterpret_cast<void***>(clX.Get());
    auto rawClose = reinterpret_cast<PFN_Close>(clVtbl[3]);
    SC_EXPECT(SUCCEEDED(rawClose(clX.Get())));

    // 4. Direct raw vtable invocation of ID3D12CommandQueueX::Signal (slot 4)
    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
        reinterpret_cast<void**>(nativeDev.GetAddressOf()))));
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    using PFN_Signal = HRESULT(STDMETHODCALLTYPE*)(void*, ID3D12Fence*, UINT64);
    auto** qVtbl = *reinterpret_cast<void***>(queueX.Get());
    auto rawSignal = reinterpret_cast<PFN_Signal>(qVtbl[4]);
    SC_EXPECT(SUCCEEDED(rawSignal(queueX.Get(), fence.Get(), 77)));

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(ev != nullptr);
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(77, ev)));
    SC_EXPECT(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);
    SC_EXPECT(fence->GetCompletedValue() == 77);

    return true;
}

bool TestD3D12XsCmdListComputeDispatchE2E() {
    using Microsoft::WRL::ComPtr;
    using namespace sc::d3d12;
    using namespace sc::d3d12::rdna2;

    // Assemble RDNA2 compute shader: gOut[i] = gIn[i] + 13
    std::vector<uint32_t> isa;
    AppendMubufLoadDword(isa, 1, 0, 0);
    AppendVop2LiteralU32(isa, kVop2AddNcU32, 2, 13u, 1);
    AppendMubufStoreDword(isa, 2, 0, 0);
    AppendSopp(isa, kSoppEndpgm);

    auto scxs = BuildScxsContainer(
        ScxsShaderStage::Compute, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa,
        isa.data(), isa.size() * sizeof(uint32_t), 6, 6);

    D3D12_SHADER_BYTECODE inBc{ scxs.data(), scxs.size() };
    Rdna2IsaTranslator translator;
    TranslatedShader translated{};
    std::string err;
    SC_EXPECT(SUCCEEDED(translator.Translate(inBc, translated, err)));
    SC_EXPECT(!translated.bytecode.empty());

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));

    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
                                             reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    HMODULE d3d12Mod = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(d3d12Mod != nullptr);
    auto pSerializeRS = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
        GetProcAddress(d3d12Mod, "D3D12SerializeRootSignature"));
    SC_EXPECT(pSerializeRS != nullptr);

    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> rsBlob, rsErr;
    SC_EXPECT(SUCCEEDED(pSerializeRS(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr)));

    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { translated.bytecode.data(), translated.bytecode.size() };
    ComPtr<ID3D12PipelineState> pso;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso))));

    constexpr uint32_t kCount = 64;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC inDesc{};
    inDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    inDesc.Width = kBytes; inDesc.Height = 1; inDesc.DepthOrArraySize = 1;
    inDesc.MipLevels = 1; inDesc.SampleDesc.Count = 1;
    inDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_DESC outDesc = inDesc;
    outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> inBuf, outBuf, rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &inDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inBuf))));
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outBuf))));
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &inDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    uint32_t* pIn = nullptr;
    SC_EXPECT(SUCCEEDED(inBuf->Map(0, nullptr, reinterpret_cast<void**>(&pIn))));
    for (uint32_t i = 0; i < kCount; ++i) pIn[i] = i * 3u + 1u;
    inBuf->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    clX->SetPipelineState(pso.Get());
    clX->SetComputeRootSignature(rootSig.Get());
    clX->SetComputeRootShaderResourceView(0, inBuf->GetGPUVirtualAddress());
    clX->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());
    clX->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = outBuf.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    clX->ResourceBarrier(1, &barrier);
    clX->CopyBufferRegion(rbBuf.Get(), 0, outBuf.Get(), 0, kBytes);
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

    uint32_t* pRb = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&pRb))));
    bool ok = true;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (pRb[i] != (i * 3u + 1u) + 13u) { ok = false; break; }
    }
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(ok);
    return true;
}

bool TestD3D12XsCmdListRenderClearE2E() {
    using Microsoft::WRL::ComPtr;

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));

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
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(devX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 4;
    rtDesc.Height = 4;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearVal.Color[0] = 1.0f; clearVal.Color[1] = 0.0f;
    clearVal.Color[2] = 1.0f; clearVal.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&rtTex))));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 4;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, 4, 4 };
    clX->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    clX->RSSetViewports(1, &vp);
    clX->RSSetScissorRects(1, &scissor);
    clX->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    clX->ClearRenderTargetView(rtvHandle, clearVal.Color, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rtTex.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    clX->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = rtTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = 4;
    dstLoc.PlacedFootprint.Footprint.Height = 4;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = 256;

    clX->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
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

    uint8_t* pRb = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&pRb))));
    bool ok = true;
    for (int y = 0; y < 4 && ok; ++y) {
        for (int x = 0; x < 4; ++x) {
            const uint8_t* px = pRb + y * 256 + x * 4;
            if (px[0] != 255 || px[1] != 0 || px[2] != 255 || px[3] != 255) {
                ok = false;
                break;
            }
        }
    }
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(ok);
    return true;
}

bool TestD3D12XsCommandListWrapperE2E() {
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

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));
    SC_EXPECT(clX->GetNativeCommandList() != nullptr);

    constexpr size_t kBytes = 4096;
    D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> srcBuf, dstBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&srcBuf))));
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dstBuf))));

    std::vector<uint8_t> expected(kBytes);
    for (size_t i = 0; i < kBytes; ++i) expected[i] = static_cast<uint8_t>((i * 53u + 19u) & 0xFFu);
    uint8_t* pSrc = nullptr;
    SC_EXPECT(SUCCEEDED(srcBuf->Map(0, nullptr, reinterpret_cast<void**>(&pSrc))));
    std::memcpy(pSrc, expected.data(), kBytes);
    srcBuf->Unmap(0, nullptr);

    const char* marker = "Sprint38CmdListX";
    clX->BeginEvent(0, marker, static_cast<UINT>(std::strlen(marker) + 1));
    clX->CopyBufferRegion(dstBuf.Get(), 0, srcBuf.Get(), 0, kBytes);
    clX->SetMarker(0, marker, static_cast<UINT>(std::strlen(marker) + 1));
    clX->InsertWaitOnPresent(1);
    clX->EndEvent();
    SC_EXPECT(SUCCEEDED(clX->Close()));

    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
                                             reinterpret_cast<void**>(nativeDev.GetAddressOf()))));
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

    uint8_t* pDst = nullptr;
    SC_EXPECT(SUCCEEDED(dstBuf->Map(0, nullptr, reinterpret_cast<void**>(&pDst))));
    const bool match = (std::memcmp(pDst, expected.data(), kBytes) == 0);
    dstBuf->Unmap(0, nullptr);
    SC_EXPECT(match);
    return true;
}

bool TestShaderDiskCacheE2E() {
    using namespace sc::d3d12;
    using namespace sc::d3d12::rdna2;

    std::vector<uint32_t> isa;
    AppendMubufLoadDword(isa, 1, 0, 0);
    AppendVop2LiteralU32(isa, kVop2AddNcU32, 2, 42u, 1);
    AppendMubufStoreDword(isa, 2, 0, 0);
    AppendSopp(isa, kSoppEndpgm);

    auto scxs = BuildScxsContainer(
        ScxsShaderStage::Compute,
        ScxsTargetArch::Rdna2Scarlett,
        ScxsPayloadFormat::Rdna2Isa,
        isa.data(), isa.size() * sizeof(uint32_t),
        6, 6);

    const uint32_t hash = ComputeScxsFnv1a32(isa.data(), isa.size() * sizeof(uint32_t));
    const std::filesystem::path cachePath = GetShaderCachePath(
        static_cast<uint16_t>(ScxsShaderStage::Compute), hash);
    std::error_code ec;
    std::filesystem::remove(cachePath, ec);

    D3D12_SHADER_BYTECODE inBc{ scxs.data(), scxs.size() };
    Rdna2IsaTranslator translator;
    std::string err;

    // 1. Cold miss: lifts + compiles via D3DCompile and populates disk cache
    TranslatedShader cold{};
    SC_EXPECT(SUCCEEDED(translator.Translate(inBc, cold, err)));
    SC_EXPECT(!cold.bytecode.empty());
    SC_EXPECT(cold.sourceTag.find("DISK-CACHE") == std::string::npos);
    SC_EXPECT(std::filesystem::exists(cachePath, ec));

    // 2. Warm hit: loads directly from disk cache with identical DXBC bytes
    TranslatedShader warm{};
    SC_EXPECT(SUCCEEDED(translator.Translate(inBc, warm, err)));
    SC_EXPECT(warm.sourceTag.find("DISK-CACHE") != std::string::npos);
    SC_EXPECT(warm.bytecode == cold.bytecode);

    std::filesystem::remove(cachePath, ec);
    return true;
}

bool TestScarlettXConformanceSuite() {
    // 1. Raw COM vtable ABI conformance against gameinput.dll (matches ScarlettX test_gameinput.cpp)
    HMODULE giMod = LoadLibraryW(L"gameinput.dll");
    if (!giMod) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path shimDll =
            std::filesystem::path(exePath).parent_path().parent_path().parent_path() / "shims" / "gameinput.dll";
        giMod = LoadLibraryW(shimDll.c_str());
    }
    SC_EXPECT(giMod != nullptr);

    using PfnGameInputCreate = HRESULT(WINAPI*)(void**);
    auto pGameInputCreate = reinterpret_cast<PfnGameInputCreate>(GetProcAddress(giMod, "GameInputCreate"));
    SC_EXPECT(pGameInputCreate != nullptr);

    void* gi = nullptr;
    SC_EXPECT(SUCCEEDED(pGameInputCreate(&gi)) && gi != nullptr);

    void** giVtbl = *reinterpret_cast<void***>(gi);
    using PfnRelease = ULONG(STDMETHODCALLTYPE*)(void*);
    using PfnGetCurrentTimestamp = uint64_t(STDMETHODCALLTYPE*)(void*);
    using PfnGetCurrentReading = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*, void**);

    // Slot 3: GetCurrentTimestamp, Slot 4: GetCurrentReading (retail GameInput.dll ABI)
    const uint64_t ts = reinterpret_cast<PfnGetCurrentTimestamp>(giVtbl[3])(gi);
    SC_EXPECT(ts > 0);

    constexpr uint32_t kKindGamepad = 0x00020000u;
    void* reading = nullptr;
    HRESULT hrRead = reinterpret_cast<PfnGetCurrentReading>(giVtbl[4])(gi, kKindGamepad, nullptr, &reading);
    if (SUCCEEDED(hrRead) && reading != nullptr) {
        void** rdVtbl = *reinterpret_cast<void***>(reading);
        using PfnGetInputKind = uint32_t(STDMETHODCALLTYPE*)(void*);
        using PfnGetTimestamp = uint64_t(STDMETHODCALLTYPE*)(void*);
        SC_EXPECT(reinterpret_cast<PfnGetInputKind>(rdVtbl[3])(reading) == kKindGamepad);
        SC_EXPECT(reinterpret_cast<PfnGetTimestamp>(rdVtbl[4])(reading) > 0);
        reinterpret_cast<PfnRelease>(rdVtbl[2])(reading);
    } else {
        // GAMEINPUT_E_READING_NOT_FOUND is valid when no physical gamepad is connected
        SC_EXPECT(hrRead == static_cast<HRESULT>(0x838A0002L));
    }
    reinterpret_cast<PfnRelease>(giVtbl[2])(gi);
    FreeLibrary(giMod);

    // 2. xgameruntime.dll ABI conformance (matches ScarlettX test_xgameruntime_abi.cpp)
    SC_EXPECT(SUCCEEDED(XGameRuntimeInitialize()));
    SC_EXPECT(XSystemGetDeviceType() != XSystemDeviceType::Unknown);

    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(
        XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue)) && queue != nullptr);
    XTaskQueueDispatch(queue, XTaskQueuePort::Work, 0);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    // 3. ScarlettX synthetic game manifest audit (if sibling repo is present)
    const std::filesystem::path sxSynthRoot = "E:\\gta\\test\\ScarlettX\\tests\\synthetic_games";
    if (std::filesystem::exists(sxSynthRoot)) {
        const char* kTitles[] = { "hello_scarlett", "iat_probe", "rt_showcase", "scarlett_arena" };
        for (const char* title : kTitles) {
            const auto cfgPath = sxSynthRoot / title / "MicrosoftGame.Config";
            SC_EXPECT(std::filesystem::exists(cfgPath));
            std::ifstream ifs(cfgPath);
            std::string xml((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SC_EXPECT(xml.find("<Game") != std::string::npos);
            SC_EXPECT(xml.find("Executable") != std::string::npos);
        }
    }

    return true;
}
