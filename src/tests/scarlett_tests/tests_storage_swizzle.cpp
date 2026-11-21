#include "test_harness.hpp"

using Microsoft::WRL::ComPtr;

bool TestVfsPathTranslation() {
    bool isVirt = false;
    auto gPath = sc::vfs::TranslatePath(L"G:\\assets\\crest_2dthin.scx", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(gPath.filename() == L"crest_2dthin.scx");

    auto tPath = sc::vfs::TranslatePath(L"t:/scratch_dir/probe.bin", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(tPath.filename() == L"probe.bin");
    SC_EXPECT(tPath.wstring().find(L"scratch") != std::wstring::npos);

    auto sPath = sc::vfs::TranslatePath(L"S:\\slot0.tmp", &isVirt);
    SC_EXPECT(isVirt);
    SC_EXPECT(sPath.wstring().find(L"save_temp") != std::wstring::npos);

    auto hostPath = sc::vfs::TranslatePath(L"C:\\Windows\\System32\\kernel32.dll", &isVirt);
    SC_EXPECT(!isVirt);
    SC_EXPECT(hostPath == std::filesystem::path(L"C:\\Windows\\System32\\kernel32.dll"));
    return true;
}

bool TestXgXsMultiMipAndNpotSwizzle() {
    XG_TEXTURE2D_DESC desc{};
    desc.Width = 77;
    desc.Height = 53;
    desc.MipLevels = 4;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.TileMode = XG_TILE_MODE_2D_THIN;

    ComPtr<IXGTextureAddressComputer> computer;
    XG_RESOURCE_LAYOUT layout{};
    SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&desc, &computer)));
    SC_EXPECT(SUCCEEDED(computer->GetResourceLayout(&layout)));
    SC_EXPECT(layout.BaseAlignmentBytes == 65536ULL);
    SC_EXPECT(layout.Plane[0].MipLevels == 4u);

    const uint32_t physAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    void* tiledMem = XMemAlloc(static_cast<size_t>(layout.SizeBytes), physAttrs);
    SC_EXPECT(tiledMem != nullptr);

    for (uint32_t m = 0; m < 4; ++m) {
        const uint32_t w = std::max(1u, desc.Width >> m);
        const uint32_t h = std::max(1u, desc.Height >> m);
        const uint32_t rowPitch = w * 4;
        SC_EXPECT((layout.Plane[0].MipLayout[m].PaddedWidthElements % 8u) == 0);
        SC_EXPECT((layout.Plane[0].MipLayout[m].PaddedHeightElements % 8u) == 0);

        std::vector<uint8_t> src(static_cast<size_t>(rowPitch) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                src[idx + 0] = static_cast<uint8_t>((x * 7u + m * 19u) & 0xFFu);
                src[idx + 1] = static_cast<uint8_t>((y * 13u + m * 31u) & 0xFFu);
                src[idx + 2] = static_cast<uint8_t>(((x ^ y) + m * 53u) & 0xFFu);
                src[idx + 3] = 0xFFu;
            }
        }

        SC_EXPECT(SUCCEEDED(computer->CopyIntoSubresource(tiledMem, m, src.data(), rowPitch, 0)));

        std::vector<uint8_t> roundTrip(src.size(), 0);
        SC_EXPECT(SUCCEEDED(computer->CopyFromSubresource(roundTrip.data(), m, tiledMem, rowPitch, 0)));
        if (std::memcmp(src.data(), roundTrip.data(), src.size()) != 0) {
            XMemFree(tiledMem, physAttrs);
            SC_EXPECT(false);
        }
    }

    XMemFree(tiledMem, physAttrs);
    return true;
}

bool TestDStorageVfsAndErrorRecord() {
    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\dstorage_probe.bin");
    std::error_code ec;
    std::filesystem::create_directories(hostProbe.parent_path(), ec);

    std::vector<uint8_t> expected(4096);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    {
        std::ofstream out(hostProbe, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.is_open());
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX,
                                           reinterpret_cast<void**>(factory.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "CTestQueue";

    ComPtr<IDStorageQueueX> queue;
    ComPtr<IDStorageFileX> file;
    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX,
                                             reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\dstorage_probe.bin", IID_IDStorageFileX,
                                          reinterpret_cast<void**>(file.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "CTestStatus", IID_IDStorageStatusArrayX,
                                                   reinterpret_cast<void**>(status.GetAddressOf()))));

    std::vector<uint8_t> actual(expected.size(), 0);
    DSTORAGE_REQUEST okReq{};
    okReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    okReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    okReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    okReq.Source.File.Source = file.Get();
    okReq.Source.File.Offset = 0;
    okReq.Source.File.Size = static_cast<uint32_t>(expected.size());
    okReq.Destination.Memory.Buffer = actual.data();
    okReq.Destination.Memory.Size = static_cast<uint32_t>(actual.size());
    okReq.UncompressedSize = static_cast<uint32_t>(expected.size());
    okReq.Name = "vfs_ok_read";

    queue->EnqueueRequest(&okReq);
    queue->EnqueueStatus(status.Get(), 0);
    queue->Submit();

    while (!status->IsComplete(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(SUCCEEDED(status->GetHResult(0)));
    SC_EXPECT(std::memcmp(expected.data(), actual.data(), expected.size()) == 0);

    DSTORAGE_REQUEST badReq = okReq;
    badReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_ZLIB;
    badReq.Name = "unsupported_zlib_req";

    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->Submit();

    while (!status->IsComplete(1)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(status->GetHResult(1) == E_NOTIMPL);

    DSTORAGE_ERROR_RECORD errRec{};
    queue->RetrieveErrorRecord(&errRec);
    SC_EXPECT(errRec.FailureCount == 1u);
    SC_EXPECT(errRec.FirstFailureHResult == E_NOTIMPL);
    SC_EXPECT(std::strcmp(errRec.RequestName, "unsupported_zlib_req") == 0);

    file->Close();
    file.Reset();
    std::filesystem::remove(hostProbe, ec);
    return true;
}

bool TestXMemAlignmentAndInvariants() {
    struct Case {
        size_t size;
        XMemAlignment alignCode;
        size_t expectedAlign;
        XMemType memType;
    };
    const Case cases[] = {
        { 131072, XALLOC_ALIGNMENT_64K, 65536, XALLOC_MEMTYPE_PHYSICAL },
        { 4096,   XALLOC_ALIGNMENT_4K,  4096,  XALLOC_MEMTYPE_HEAP },
        { 1024,   XALLOC_ALIGNMENT_256, 256,   XALLOC_MEMTYPE_HEAP },
        { 320,    XALLOC_ALIGNMENT_64,  64,    XALLOC_MEMTYPE_HEAP }
    };

    for (const auto& c : cases) {
        const uint32_t attrs = MakeXAllocAttributes(XALLOC_ID_DEFAULT, c.alignCode, c.memType, true);
        void* p = XMemAlloc(c.size, attrs);
        SC_EXPECT(p != nullptr);
        SC_EXPECT((reinterpret_cast<uintptr_t>(p) % c.expectedAlign) == 0);
        SC_EXPECT(XMemSize(p, attrs) == c.size);

        const auto* bytes = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < c.size; ++i) {
            if (bytes[i] != 0) {
                XMemFree(p, attrs);
                SC_EXPECT(false);
            }
        }
        XMemFree(p, attrs);
    }
    return true;
}

bool TestDStorageLzDecompression() {
    constexpr size_t kRawSize = 65536;
    std::vector<uint8_t> raw(kRawSize);
    for (size_t i = 0; i < kRawSize; ++i) {
        // Structured repeating tile-like pattern that compresses well
        raw[i] = static_cast<uint8_t>(((i & 0x3Fu) * 3u + ((i >> 8) & 0x0Fu)) & 0xFFu);
    }

    std::vector<uint8_t> comp = sc::codec::CompressLzBlock(raw.data(), raw.size());
    SC_EXPECT(!comp.empty());
    SC_EXPECT(comp.size() < raw.size() / 2u); // Expect > 2x compression

    const std::filesystem::path hostPath = sc::vfs::TranslatePath(L"T:\\compressed_probe.sclz");
    std::error_code ec;
    std::filesystem::create_directories(hostPath.parent_path(), ec);
    {
        std::ofstream out(hostPath, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.is_open());
        out.write(reinterpret_cast<const char*>(comp.data()),
                  static_cast<std::streamsize>(comp.size()));
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX,
                                           reinterpret_cast<void**>(factory.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_HIGH;
    qd.Name = "CTestLzQueue";

    ComPtr<IDStorageQueueX> queue;
    ComPtr<IDStorageFileX> file;
    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX,
                                             reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\compressed_probe.sclz", IID_IDStorageFileX,
                                          reinterpret_cast<void**>(file.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "CTestLzStatus", IID_IDStorageStatusArrayX,
                                                   reinterpret_cast<void**>(status.GetAddressOf()))));

    std::vector<uint8_t> decompressed(kRawSize, 0);
    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(comp.size());
    req.Destination.Memory.Buffer = decompressed.data();
    req.Destination.Memory.Size = static_cast<uint32_t>(decompressed.size());
    req.UncompressedSize = static_cast<uint32_t>(kRawSize);
    req.Name = "sclz_vfs_read";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->Submit();

    while (!status->IsComplete(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(SUCCEEDED(status->GetHResult(0)));
    SC_EXPECT(std::memcmp(raw.data(), decompressed.data(), kRawSize) == 0);

    // Corrupt compressed stream in memory -> must fail cleanly with E_FAIL and log error record
    std::vector<uint8_t> corrupt = comp;
    corrupt.back() ^= 0xFFu;
    std::vector<uint8_t> discard(kRawSize, 0);

    DSTORAGE_REQUEST badReq{};
    badReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    badReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    badReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    badReq.Source.Memory.Source = corrupt.data();
    badReq.Source.Memory.Size = static_cast<uint32_t>(corrupt.size());
    badReq.Destination.Memory.Buffer = discard.data();
    badReq.Destination.Memory.Size = static_cast<uint32_t>(discard.size());
    badReq.UncompressedSize = static_cast<uint32_t>(kRawSize);
    badReq.Name = "corrupt_sclz_req";

    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->Submit();

    while (!status->IsComplete(1)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SC_EXPECT(status->GetHResult(1) == E_FAIL);

    DSTORAGE_ERROR_RECORD errRec{};
    queue->RetrieveErrorRecord(&errRec);
    SC_EXPECT(errRec.FailureCount == 1u);
    SC_EXPECT(errRec.FirstFailureHResult == E_FAIL);
    SC_EXPECT(std::strcmp(errRec.RequestName, "corrupt_sclz_req") == 0);

    file->Close();
    file.Reset();
    std::filesystem::remove(hostPath, ec);
    return true;
}

// 15. xaudio2_9: Live 4:1 IMA-ADPCM codec accuracy + ScarlettDecodingSourceVoice bridge

bool TestXgXs64KbsAndBcnBlockSwizzle() {
    auto verifyBcnSwizzle = [](DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t bpe) -> bool {
        XG_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = mips;
        d.ArraySize = 1;
        d.Format = fmt;
        d.TileMode = XG_TILE_MODE_64KB_S;

        ComPtr<IXGTextureAddressComputer> comp;
        XG_RESOURCE_LAYOUT layout{};
        SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&d, &comp)));
        SC_EXPECT(SUCCEEDED(comp->GetResourceLayout(&layout)));
        SC_EXPECT(layout.SizeBytes >= 65536ULL && (layout.SizeBytes % 65536ULL) == 0ULL);

        const uint32_t physAttrs = MakeXAllocAttributes(
            XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
        void* tiledMem = XMemAlloc(static_cast<size_t>(layout.SizeBytes), physAttrs);
        SC_EXPECT(tiledMem != nullptr);
        SC_EXPECT((reinterpret_cast<uintptr_t>(tiledMem) % 65536ULL) == 0ULL);

        std::vector<std::vector<uint8_t>> expectedMips(mips);
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t bw = (std::max<uint32_t>(1u, w >> m) + 3u) / 4u;
            const uint32_t bh = (std::max<uint32_t>(1u, h >> m) + 3u) / 4u;
            const uint32_t rowBytes = bw * bpe;
            expectedMips[m].resize(static_cast<size_t>(rowBytes) * bh);
            for (size_t i = 0; i < expectedMips[m].size(); ++i) {
                expectedMips[m][i] = static_cast<uint8_t>(((i * 37u) ^ (m * 97u) ^ (bpe * 13u)) & 0xFFu);
            }
            SC_EXPECT(SUCCEEDED(comp->CopyIntoSubresource(tiledMem, m, expectedMips[m].data(), rowBytes, 0)));
        }

        // Verify real swizzling altered layout on mip 0
        SC_EXPECT(std::memcmp(tiledMem, expectedMips[0].data(), std::min<size_t>(256, expectedMips[0].size())) != 0);

        bool allExact = true;
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t bw = (std::max<uint32_t>(1u, w >> m) + 3u) / 4u;
            const uint32_t rowBytes = bw * bpe;
            std::vector<uint8_t> roundTrip(expectedMips[m].size(), 0xCCu);
            if (FAILED(comp->CopyFromSubresource(roundTrip.data(), m, tiledMem, rowBytes, 0)) ||
                std::memcmp(roundTrip.data(), expectedMips[m].data(), expectedMips[m].size()) != 0) {
                allExact = false;
                break;
            }
        }
        XMemFree(tiledMem, physAttrs);
        SC_EXPECT(allExact);
        return true;
    };

    // NPOT multi-mip BC1 (8B), BC3 (16B), and multi-macro-tile BC7 (16B, 300x190 = 75x48 blocks > 64x64)
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC1_UNORM, 130,  90, 4,  8));
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC3_UNORM,  64,  64, 3, 16));
    SC_EXPECT(verifyBcnSwizzle(DXGI_FORMAT_BC7_UNORM, 300, 190, 5, 16));

    // End-to-end GPU test: BC1 16x16 swizzled in 64KB_S -> detiled to LINEAR -> uploaded & sampled on RTX 5070
    constexpr UINT kSize = 16;
    XG_TEXTURE2D_DESC bc1Tiled{};
    bc1Tiled.Width = kSize;
    bc1Tiled.Height = kSize;
    bc1Tiled.MipLevels = 1;
    bc1Tiled.ArraySize = 1;
    bc1Tiled.Format = DXGI_FORMAT_BC1_UNORM;
    bc1Tiled.TileMode = XG_TILE_MODE_64KB_S;

    XG_TEXTURE2D_DESC bc1Linear = bc1Tiled;
    bc1Linear.TileMode = XG_TILE_MODE_LINEAR;

    ComPtr<IXGTextureAddressComputer> bc1Comp;
    XG_RESOURCE_LAYOUT tiledLayout{}, linearLayout{};
    SC_EXPECT(SUCCEEDED(XGCreateTexture2DComputer(&bc1Tiled, &bc1Comp)));
    SC_EXPECT(SUCCEEDED(bc1Comp->GetResourceLayout(&tiledLayout)));
    SC_EXPECT(SUCCEEDED(XGComputeTexture2DLayout(&bc1Linear, &linearLayout)));

    // Solid BC1 block: color0 = 0xFC00 (R5=31->255, G6=32->130, B5=0->0), color1 = 0x0000, all indices = 0
    std::vector<uint8_t> bc1Raw(4u * 4u * 8u, 0u);
    for (size_t blk = 0; blk < 16u; ++blk) {
        bc1Raw[blk * 8u + 0] = 0x00u;
        bc1Raw[blk * 8u + 1] = 0xFCu;
    }

    const uint32_t physAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    void* tiledBuf = XMemAlloc(static_cast<size_t>(tiledLayout.SizeBytes), physAttrs);
    SC_EXPECT(tiledBuf != nullptr);
    SC_EXPECT(SUCCEEDED(bc1Comp->CopyIntoSubresource(tiledBuf, 0, bc1Raw.data(), 4u * 8u, 0)));

    const UINT linearPitch = static_cast<UINT>(linearLayout.Plane[0].MipLayout[0].PitchBytes);
    std::vector<uint8_t> detiledUpload(static_cast<size_t>(linearLayout.SizeBytes), 0u);
    SC_EXPECT(SUCCEEDED(bc1Comp->CopyFromSubresource(detiledUpload.data(), 0, tiledBuf, linearPitch, 0)));
    XMemFree(tiledBuf, physAttrs);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&qd, &cmdQueue)));

    const char* hlsl = R"HLSL(
Texture2D<float4> gTex : register(t0);
SamplerState gSamp : register(s0);
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    return float4((id == 2) ? 3.0f : -1.0f, (id == 1) ? 3.0f : -1.0f, 0.0f, 1.0f);
}
float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
    return gTex.Sample(gSamp, float2(0.5f, 0.5f));
}
)HLSL";

    ComPtr<ID3DBlob> vsBlob, psBlob, rsBlob, errBlob;
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "bc1_sample.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob)));
    SC_EXPECT(SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "bc1_sample.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob)));

    D3D12_DESCRIPTOR_RANGE srvRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 1, &srvRange };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ComPtr<ID3D12RootSignature> rootSig;
    SC_EXPECT(SUCCEEDED(deviceX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig))));

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

    D3D12_HEAP_PROPERTIES upHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC bc1Desc{};
    bc1Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bc1Desc.Width = kSize;
    bc1Desc.Height = kSize;
    bc1Desc.DepthOrArraySize = 1;
    bc1Desc.MipLevels = 1;
    bc1Desc.Format = DXGI_FORMAT_BC1_UNORM;
    bc1Desc.SampleDesc.Count = 1;

    D3D12_RESOURCE_DESC rtDesc = bc1Desc;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    auto mkBuf = [](UINT64 sz) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = sz;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };

    constexpr UINT kRtPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT kRbBytes = kRtPitch * kSize;
    D3D12_RESOURCE_DESC upDesc = mkBuf(detiledUpload.size());
    D3D12_RESOURCE_DESC rbDesc = mkBuf(kRbBytes);

    ComPtr<ID3D12Resource> bc1Tex, upBuf, rtTex, rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bc1Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bc1Tex))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&upHeap,  D3D12_HEAP_FLAG_NONE, &upDesc,  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtTex))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(&rbHeap,  D3D12_HEAP_FLAG_NONE, &rbDesc,  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    SC_EXPECT(SUCCEEDED(upBuf->Map(0, &noRead, &mapped)));
    std::memcpy(mapped, detiledUpload.data(), detiledUpload.size());
    upBuf->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC srvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&srvHd, IID_PPV_ARGS(&srvHeap))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&rtvHeap))));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_BC1_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    deviceX->CreateShaderResourceView(bc1Tex.Get(), &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&cmdList))));
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    D3D12_TEXTURE_COPY_LOCATION dstTex{};
    dstTex.pResource = bc1Tex.Get();
    dstTex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstTex.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcBuf{};
    srcBuf.pResource = upBuf.Get();
    srcBuf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcBuf.PlacedFootprint.Footprint = { DXGI_FORMAT_BC1_UNORM, kSize, kSize, 1, linearPitch };
    cmdList->CopyTextureRegion(&dstTex, 0, 0, 0, &srcBuf, nullptr);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition = { bc1Tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    cmdList->ResourceBarrier(1, &b1);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize) };
    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(rootSig.Get());
    cmdList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cmdList->ResourceBarrier(1, &b2);

    D3D12_TEXTURE_COPY_LOCATION rtSrc{};
    rtSrc.pResource = rtTex.Get();
    rtSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rtSrc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION rbDst{};
    rbDst.pResource = rbBuf.Get();
    rbDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rbDst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kRtPitch };
    cmdList->CopyTextureRegion(&rbDst, 0, 0, 0, &rtSrc, nullptr);
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
    const auto* px = static_cast<const uint8_t*>(mapped) + (8u * kRtPitch) + (8u * 4u);
    const bool ok = (px[0] == 255) && (std::abs(static_cast<int>(px[1]) - 130) <= 2) && (px[2] == 0) && (px[3] == 255);
    rbBuf->Unmap(0, &noRead);
    SC_EXPECT(ok);
    return true;
}

bool TestDStorageGpuBufferStreaming() {
    ComPtr<ID3D12Device> device;
    HMODULE hD3d12 = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(hD3d12 != nullptr);
    auto pfnCreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(hD3d12, "D3D12CreateDevice"));
    SC_EXPECT(pfnCreateDevice != nullptr);
    SC_EXPECT(SUCCEEDED(pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));

    constexpr uint32_t kPayloadSize = 65536u;
    constexpr uint64_t kDstOffset = 256u;
    constexpr uint64_t kTotalBufSize = kPayloadSize + kDstOffset;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kTotalBufSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> gpuBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuBuf))));

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc = bufDesc;
    rbDesc.Width = kPayloadSize;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    std::vector<uint8_t> original(kPayloadSize);
    for (uint32_t i = 0; i < kPayloadSize; ++i) {
        original[i] = static_cast<uint8_t>((i / 16u) ^ (i & 0x0Fu) ^ 0xA5u);
    }
    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(original.data(), original.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_stream_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\gpu_stream_probe.sclz", IID_IDStorageFileX, reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qDesc.Name = "gpu_stream_test_queue";
    qDesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "gpu_status", IID_IDStorageStatusArrayX, reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.Buffer.Resource = gpuBuf.Get();
    req.Destination.Buffer.Offset = kDstOffset;
    req.Destination.Buffer.Size = kPayloadSize;
    req.UncompressedSize = kPayloadSize;
    req.Name = "gpu_lz_stream";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    // Copy from DEFAULT buffer back to READBACK buffer on a DIRECT queue and verify bit-for-bit
    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> directQueue;
    SC_EXPECT(SUCCEEDED(device->CreateCommandQueue(&dq, IID_PPV_ARGS(&directQueue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    cmdList->CopyBufferRegion(rbBuf.Get(), 0, gpuBuf.Get(), kDstOffset, kPayloadSize);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kPayloadSize };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const bool match = (std::memcmp(mapped, original.data(), kPayloadSize) == 0);
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(match);

    // Negative test: null GPU resource must record E_INVALIDARG
    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.Buffer.Resource = nullptr;
    badReq.Name = "null_gpu_resource";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
    return true;
}

bool TestDStorageGpuTextureStreaming() {
    HMODULE hD3d12 = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(hD3d12 != nullptr);
    auto pfnCreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(hD3d12, "D3D12CreateDevice"));
    SC_EXPECT(pfnCreateDevice != nullptr);

    ComPtr<ID3D12Device> device;
    SC_EXPECT(SUCCEEDED(pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));

    constexpr uint32_t kWidth = 64u;
    constexpr uint32_t kHeight = 64u;
    constexpr uint32_t kRowPitch = kWidth * 4u; // 256 bytes (D3D12 aligned)
    constexpr uint32_t kTexBytes = kRowPitch * kHeight;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kWidth;
    texDesc.Height = kHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> gpuTex;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuTex))));

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kTexBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(device->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    std::vector<uint8_t> pixels(kTexBytes);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kWidth + x) * 4u;
            pixels[idx + 0] = static_cast<uint8_t>(x * 4u);
            pixels[idx + 1] = static_cast<uint8_t>(y * 4u);
            pixels[idx + 2] = static_cast<uint8_t>((x ^ y) * 3u);
            pixels[idx + 3] = 255u;
        }
    }

    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(pixels.data(), pixels.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_tex_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\gpu_tex_probe.sclz", IID_IDStorageFileX, reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qDesc.Name = "gpu_tex_stream_queue";
    qDesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "gpu_tex_status", IID_IDStorageStatusArrayX, reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.Texture.Resource = gpuTex.Get();
    req.Destination.Texture.SubresourceIndex = 0;
    req.Destination.Texture.Region = { 0, 0, 0, kWidth, kHeight, 1 };
    req.UncompressedSize = kTexBytes;
    req.Name = "gpu_lz_tex_stream";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    // Readback texture on DIRECT queue and verify bit-for-bit
    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> directQueue;
    SC_EXPECT(SUCCEEDED(device->CreateCommandQueue(&dq, IID_PPV_ARGS(&directQueue))));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = gpuTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, kRowPitch };

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kTexBytes };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const bool match = (std::memcmp(mapped, pixels.data(), kTexBytes) == 0);
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(match);

    // Negative test: empty/inverted region must record E_INVALIDARG
    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.Texture.Region = { 16, 0, 0, 16, kHeight, 1 };
    badReq.Name = "invalid_tex_region";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
    return true;
}

bool TestDStorageDeviceXBridgeAndMipChain() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> deviceX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &devParams, IID_ID3D12DeviceX,
        reinterpret_cast<void**>(deviceX.GetAddressOf()))));

    constexpr uint32_t kMipCount = 3u;
    constexpr uint32_t kBaseSize = 64u;
    constexpr uint32_t kMipSizes[kMipCount] = { 64u, 32u, 16u };
    constexpr uint32_t kAlignedPitch = 256u;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kBaseSize;
    texDesc.Height = kBaseSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(kMipCount);
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> gpuTex;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuTex))));

    std::vector<uint8_t> tightChain;
    uint32_t mipTightOffsets[kMipCount]{};
    for (uint32_t m = 0; m < kMipCount; ++m) {
        mipTightOffsets[m] = static_cast<uint32_t>(tightChain.size());
        const uint32_t dim = kMipSizes[m];
        for (uint32_t y = 0; y < dim; ++y) {
            for (uint32_t x = 0; x < dim; ++x) {
                tightChain.push_back(static_cast<uint8_t>((m + 1u) * 60u + x));
                tightChain.push_back(static_cast<uint8_t>(y * 4u));
                tightChain.push_back(static_cast<uint8_t>((x ^ y) + m * 37u));
                tightChain.push_back(255u);
            }
        }
    }

    std::vector<uint8_t> compressed = sc::codec::CompressLzBlock(tightChain.data(), tightChain.size());
    SC_EXPECT(!compressed.empty());

    const std::filesystem::path hostProbe = sc::vfs::TranslatePath(L"T:\\gpu_mipchain_probe.sclz");
    std::filesystem::create_directories(hostProbe.parent_path());
    {
        FILE* f = nullptr;
        SC_EXPECT(_wfopen_s(&f, hostProbe.c_str(), L"wb") == 0 && f != nullptr);
        SC_EXPECT(std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size());
        std::fclose(f);
    }

    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(
        IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));

    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->OpenFile(
        L"T:\\gpu_mipchain_probe.sclz", IID_IDStorageFileX,
        reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qDesc{};
    qDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qDesc.Capacity = 32;
    qDesc.Priority = DSTORAGE_PRIORITY_HIGH;
    qDesc.Name = "devicex_mipchain_queue";
    qDesc.Device = deviceX.Get();

    ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(
        &qDesc, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));

    ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(
        2, "mipchain_status", IID_IDStorageStatusArrayX,
        reinterpret_cast<void**>(status.GetAddressOf()))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(compressed.size());
    req.Destination.MultipleSubresources.Resource = gpuTex.Get();
    req.Destination.MultipleSubresources.FirstSubresource = 0;
    req.UncompressedSize = static_cast<uint32_t>(tightChain.size());
    req.Name = "gpu_lz_mipchain";

    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(evt != nullptr);
    if (fence->GetCompletedValue() < 1) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    SC_EXPECT(status->IsComplete(0));
    SC_EXPECT(status->GetHResult(0) == S_OK);

    constexpr uint64_t kRbOffsets[kMipCount] = { 0u, 16384u, 24576u };
    constexpr uint64_t kRbTotalBytes = 28672u;

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = kRbTotalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> directQueue;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandQueueX(&dq, &directQueue)));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    SC_EXPECT(SUCCEEDED(deviceX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))));

    for (uint32_t m = 0; m < kMipCount; ++m) {
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = gpuTex.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = rbBuf.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = kRbOffsets[m];
        dstLoc.PlacedFootprint.Footprint = {
            DXGI_FORMAT_R8G8B8A8_UNORM, kMipSizes[m], kMipSizes[m], 1, kAlignedPitch
        };
        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }
    SC_EXPECT(SUCCEEDED(cmdList->Close()));

    ID3D12CommandList* lists[] = { cmdList.Get() };
    directQueue->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(directQueue->Signal(fence.Get(), 2)));
    if (fence->GetCompletedValue() < 2) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, evt)));
        WaitForSingleObject(evt, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(kRbTotalBytes) };
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, &readRange, &mapped)));
    const auto* rbBytes = static_cast<const uint8_t*>(mapped);
    bool allMatch = true;
    for (uint32_t m = 0; m < kMipCount && allMatch; ++m) {
        const uint32_t dim = kMipSizes[m];
        const uint32_t tightRow = dim * 4u;
        for (uint32_t y = 0; y < dim; ++y) {
            const uint8_t* actualRow = rbBytes + kRbOffsets[m] + static_cast<uint64_t>(y) * kAlignedPitch;
            const uint8_t* expectRow = tightChain.data() + mipTightOffsets[m] + static_cast<uint64_t>(y) * tightRow;
            if (std::memcmp(actualRow, expectRow, tightRow) != 0) {
                allMatch = false;
                break;
            }
        }
    }
    D3D12_RANGE noWrite{ 0, 0 };
    rbBuf->Unmap(0, &noWrite);
    SC_EXPECT(allMatch);

    DSTORAGE_REQUEST badReq = req;
    badReq.Destination.MultipleSubresources.FirstSubresource = kMipCount;
    badReq.Name = "oob_first_subresource";
    queue->EnqueueRequest(&badReq);
    queue->EnqueueStatus(status.Get(), 1);
    queue->EnqueueSignal(fence.Get(), 3);
    queue->Submit();

    if (fence->GetCompletedValue() < 3) {
        SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, evt)));
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    SC_EXPECT(status->IsComplete(1));
    SC_EXPECT(status->GetHResult(1) == E_INVALIDARG);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    SC_EXPECT(err.FailureCount == 1);
    SC_EXPECT(err.FirstFailureHResult == E_INVALIDARG);
    return true;
}

bool TestDStorageGpuBufferAndFenceE2E() {
    using Microsoft::WRL::ComPtr;

    // 1. Write a 4 KB deterministic payload to VFS T:\dstorage_gpu_stream.bin
    const std::filesystem::path hostFile = sc::vfs::TranslatePath(L"T:\\dstorage_gpu_stream.bin");
    std::filesystem::create_directories(hostFile.parent_path());
    std::vector<uint8_t> payload(4096);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
    }
    {
        std::ofstream ofs(hostFile, std::ios::binary | std::ios::trunc);
        SC_EXPECT(ofs.good());
        ofs.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    // 2. Create real D3D12 device, fence, default GPU buffer, and readback buffer
    HMODULE d3d12Mod = LoadLibraryW(L"d3d12.dll");
    SC_EXPECT(d3d12Mod != nullptr);
    auto pD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Mod, "D3D12CreateDevice"));
    SC_EXPECT(pD3D12CreateDevice != nullptr);

    ComPtr<ID3D12Device> dev;
    SC_EXPECT(SUCCEEDED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(fenceEv != nullptr);

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = payload.size();
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> gpuBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpuBuf))));

    // 3. Stream file -> D3D12 GPU buffer via dstorage_xs and signal fence=1
    ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(DStorageGetFactory(IID_IDStorageFactoryX,
                                           reinterpret_cast<void**>(factory.GetAddressOf()))));

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "CTestGpuStreamQueue";
    qd.Device = dev.Get();

    ComPtr<IDStorageQueueX> queue;
    ComPtr<IDStorageFileX> file;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX,
                                             reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(SUCCEEDED(factory->OpenFile(L"T:\\dstorage_gpu_stream.bin", IID_IDStorageFileX,
                                          reinterpret_cast<void**>(file.GetAddressOf()))));

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    req.Source.File.Source = file.Get();
    req.Source.File.Offset = 0;
    req.Source.File.Size = static_cast<uint32_t>(payload.size());
    req.Destination.Buffer.Resource = gpuBuf.Get();
    req.Destination.Buffer.Offset = 0;
    req.Destination.Buffer.Size = static_cast<uint32_t>(payload.size());
    req.UncompressedSize = static_cast<uint32_t>(payload.size());
    req.Name = "GpuBufferStream4KB";

    queue->EnqueueRequest(&req);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, fenceEv)));
    SC_EXPECT(WaitForSingleObject(fenceEv, 5000) == WAIT_OBJECT_0);
    SC_EXPECT(fence->GetCompletedValue() >= 1);

    DSTORAGE_ERROR_RECORD errRec{};
    queue->RetrieveErrorRecord(&errRec);
    SC_EXPECT(errRec.FailureCount == 0);

    // 4. Copy GPU buffer -> readback buffer and verify all 4096 bytes
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(dev->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> cq;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&cq))));
    ComPtr<ID3D12CommandAllocator> ca;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))));
    ComPtr<ID3D12GraphicsCommandList> cl;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))));

    cl->CopyBufferRegion(rbBuf.Get(), 0, gpuBuf.Get(), 0, payload.size());
    SC_EXPECT(SUCCEEDED(cl->Close()));
    ID3D12CommandList* lists[] = { cl.Get() };
    cq->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(cq->Signal(fence.Get(), 2)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, fenceEv)));
    SC_EXPECT(WaitForSingleObject(fenceEv, 5000) == WAIT_OBJECT_0);
    CloseHandle(fenceEv);

    uint8_t* rbPtr = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&rbPtr))));
    const bool bytesMatch = (std::memcmp(rbPtr, payload.data(), payload.size()) == 0);
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(bytesMatch);

    std::error_code ec;
    std::filesystem::remove(hostFile, ec);
    return true;
}

bool TestDStorageVelocityDynamicE2E() {
    HMODULE dsMod = LoadLibraryW(L"shims\\dstorage_xs.dll");
    if (!dsMod) dsMod = LoadLibraryW(L"dstorage_xs.dll");
    SC_EXPECT(dsMod != nullptr);

    using PFN_SetConfig  = HRESULT(WINAPI*)(const DSTORAGE_CONFIGURATION*);
    using PFN_GetFactory = HRESULT(WINAPI*)(REFIID, void**);

    auto pSetConfig  = reinterpret_cast<PFN_SetConfig>(GetProcAddress(dsMod, "DStorageSetConfiguration"));
    auto pGetFactory = reinterpret_cast<PFN_GetFactory>(GetProcAddress(dsMod, "DStorageGetFactory"));
    SC_EXPECT(pSetConfig != nullptr && pGetFactory != nullptr);

    DSTORAGE_CONFIGURATION cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 2;
    cfg.ForceMappingLayer = false;
    cfg.DisableBypassIO = false;
    cfg.DisableTelemetry = false;
    SC_EXPECT(SUCCEEDED(pSetConfig(&cfg)));

    Microsoft::WRL::ComPtr<IDStorageFactoryX> factory;
    SC_EXPECT(SUCCEEDED(pGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(factory.GetAddressOf()))));
    SC_EXPECT(factory != nullptr);

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = 32;
    qd.Priority = DSTORAGE_PRIORITY_HIGH;
    qd.Name = "Sprint48VelocityQueue";
    qd.Device = nullptr;

    Microsoft::WRL::ComPtr<IDStorageQueueX> queue;
    SC_EXPECT(SUCCEEDED(factory->CreateQueue(&qd, IID_IDStorageQueueX, reinterpret_cast<void**>(queue.GetAddressOf()))));
    SC_EXPECT(queue != nullptr);

    Microsoft::WRL::ComPtr<IDStorageStatusArrayX> status;
    SC_EXPECT(SUCCEEDED(factory->CreateStatusArray(2, "Sprint48Status", IID_IDStorageStatusArrayX,
                                                   reinterpret_cast<void**>(status.GetAddressOf()))));
    SC_EXPECT(status != nullptr);

    std::vector<uint8_t> src0(4096), dst0(4096, 0);
    std::vector<uint8_t> src1(8192), dst1(8192, 0);
    for (size_t i = 0; i < src0.size(); ++i) src0[i] = static_cast<uint8_t>((i * 13u + 0x21u) & 0xFFu);
    for (size_t i = 0; i < src1.size(); ++i) src1[i] = static_cast<uint8_t>((i * 29u + 0x73u) & 0xFFu);

    DSTORAGE_REQUEST req0{};
    req0.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    req0.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    req0.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req0.Source.Memory.Source = src0.data();
    req0.Source.Memory.Size = static_cast<uint32_t>(src0.size());
    req0.Destination.Memory.Buffer = dst0.data();
    req0.Destination.Memory.Size = static_cast<uint32_t>(dst0.size());
    req0.UncompressedSize = static_cast<uint32_t>(src0.size());
    req0.Name = "Sprint48::Chunk0";

    DSTORAGE_REQUEST req1{};
    req1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    req1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    req1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req1.Source.Memory.Source = src1.data();
    req1.Source.Memory.Size = static_cast<uint32_t>(src1.size());
    req1.Destination.Memory.Buffer = dst1.data();
    req1.Destination.Memory.Size = static_cast<uint32_t>(dst1.size());
    req1.UncompressedSize = static_cast<uint32_t>(src1.size());
    req1.Name = "Sprint48::Chunk1";

    queue->EnqueueRequest(&req0);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueRequest(&req1);
    queue->EnqueueStatus(status.Get(), 1);
    queue->Submit();

    uint32_t spins = 0;
    while ((!status->IsComplete(0) || !status->IsComplete(1)) && spins < 5000) {
        Sleep(1);
        ++spins;
    }
    SC_EXPECT(status->IsComplete(0) && status->IsComplete(1));
    SC_EXPECT(SUCCEEDED(status->GetHResult(0)));
    SC_EXPECT(SUCCEEDED(status->GetHResult(1)));
    SC_EXPECT(std::memcmp(src0.data(), dst0.data(), src0.size()) == 0);
    SC_EXPECT(std::memcmp(src1.data(), dst1.data(), src1.size()) == 0);

    status.Reset();
    queue.Reset();
    factory.Reset();
    FreeLibrary(dsMod);
    return true;
}

bool TestXMemAndXgXsSwizzleE2E() {
    // 1. Load xmem & xg_xs shims dynamically
    HMODULE xmemMod = LoadLibraryW(L"shims\\xmem.dll");
    if (!xmemMod) xmemMod = LoadLibraryW(L"xmem.dll");
    SC_EXPECT(xmemMod != nullptr);

    HMODULE xgMod = LoadLibraryW(L"shims\\xg_xs.dll");
    if (!xgMod) xgMod = LoadLibraryW(L"xg_xs.dll");
    SC_EXPECT(xgMod != nullptr);

    using PFN_XMemAlloc        = void*(WINAPI*)(size_t, uint32_t);
    using PFN_XMemFree         = void(WINAPI*)(void*, uint32_t);
    using PFN_XMemSize         = size_t(WINAPI*)(void*, uint32_t);
    using PFN_XMemAllocDefault = void*(WINAPI*)(size_t, uint32_t);
    using PFN_XMemFreeDefault  = void(WINAPI*)(void*, uint32_t);
    using PFN_XMemSizeDefault  = size_t(WINAPI*)(void*, uint32_t);

    auto pXMemAlloc        = reinterpret_cast<PFN_XMemAlloc>(GetProcAddress(xmemMod, "XMemAlloc"));
    auto pXMemFree         = reinterpret_cast<PFN_XMemFree>(GetProcAddress(xmemMod, "XMemFree"));
    auto pXMemSize         = reinterpret_cast<PFN_XMemSize>(GetProcAddress(xmemMod, "XMemSize"));
    auto pXMemAllocDefault = reinterpret_cast<PFN_XMemAllocDefault>(GetProcAddress(xmemMod, "XMemAllocDefault"));
    auto pXMemFreeDefault  = reinterpret_cast<PFN_XMemFreeDefault>(GetProcAddress(xmemMod, "XMemFreeDefault"));
    auto pXMemSizeDefault  = reinterpret_cast<PFN_XMemSizeDefault>(GetProcAddress(xmemMod, "XMemSizeDefault"));
    SC_EXPECT(pXMemAlloc && pXMemFree && pXMemSize && pXMemAllocDefault && pXMemFreeDefault && pXMemSizeDefault);

    // Verify default allocator path
    const uint32_t defAttrs = (0u) | (3u << 8) | (0u << 12) | (1u << 14); // Default, 16B, Heap, ZeroInit
    void* defBuf = pXMemAllocDefault(4096, defAttrs);
    SC_EXPECT(defBuf != nullptr);
    SC_EXPECT((reinterpret_cast<uintptr_t>(defBuf) & 15u) == 0);
    SC_EXPECT(pXMemSizeDefault(defBuf, defAttrs) >= 4096);
    pXMemFreeDefault(defBuf, defAttrs);

    // 2. xg_xs 256x256 3-mip BC7_UNORM 64KB_S layout + computer
    struct LocalXgDesc {
        uint32_t    Width;
        uint32_t    Height;
        uint32_t    MipLevels;
        uint32_t    ArraySize;
        DXGI_FORMAT Format;
        uint32_t    TileMode; // 2 = XG_TILE_MODE_64KB_S
    };
    struct LocalXgMipLayout {
        uint64_t OffsetBytes;
        uint64_t SizeBytes;
        uint32_t PitchBytes;
        uint32_t PaddedWidthElements;
        uint32_t PaddedHeightElements;
        uint32_t TileMode;
    };
    struct LocalXgPlaneLayout {
        uint64_t         BaseOffsetBytes;
        uint64_t         SizeBytes;
        uint32_t         BytesPerElement;
        uint32_t         MipLevels;
        LocalXgMipLayout MipLayout[15];
    };
    struct LocalXgResourceLayout {
        uint64_t           SizeBytes;
        uint64_t           BaseAlignmentBytes;
        uint32_t           Planes;
        LocalXgPlaneLayout Plane[1];
    };
    struct ILocalXgComputer : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetResourceLayout(LocalXgResourceLayout* pLayout) = 0;
        virtual HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
            void* pResourceBaseAddress, uint32_t Subresource,
            const void* pSourceData, uint32_t SourceRowPitch, uint32_t SourceSlicePitch) = 0;
        virtual HRESULT STDMETHODCALLTYPE CopyFromSubresource(
            void* pDestinationData, uint32_t Subresource,
            const void* pResourceBaseAddress, uint32_t DestRowPitch, uint32_t DestSlicePitch) = 0;
    };

    auto pComputeLayout = reinterpret_cast<HRESULT(WINAPI*)(const LocalXgDesc*, LocalXgResourceLayout*)>(
        GetProcAddress(xgMod, "XGComputeTexture2DLayout"));
    auto pCreateComp = reinterpret_cast<HRESULT(WINAPI*)(const LocalXgDesc*, ILocalXgComputer**)>(
        GetProcAddress(xgMod, "XGCreateTexture2DComputer"));
    SC_EXPECT(pComputeLayout && pCreateComp);

    LocalXgDesc desc{ 256, 256, 3, 1, DXGI_FORMAT_BC7_UNORM, 2u };
    LocalXgResourceLayout layout{};
    SC_EXPECT(SUCCEEDED(pComputeLayout(&desc, &layout)));
    SC_EXPECT(layout.SizeBytes >= 65536ULL);
    SC_EXPECT(layout.BaseAlignmentBytes == 65536ULL);
    SC_EXPECT(layout.Plane[0].BytesPerElement == 16u);

    ILocalXgComputer* comp = nullptr;
    SC_EXPECT(SUCCEEDED(pCreateComp(&desc, &comp)) && comp != nullptr);

    // Allocate 64 KiB-aligned physical tiled surface via xmem
    const uint32_t tiledAttrs = (10u) | (15u << 8) | (1u << 12) | (1u << 14); // TextureTiled, 64K, Physical, ZeroInit
    void* tiledMem = pXMemAlloc(static_cast<size_t>(layout.SizeBytes), tiledAttrs);
    SC_EXPECT(tiledMem != nullptr);
    SC_EXPECT((reinterpret_cast<uintptr_t>(tiledMem) & 65535u) == 0);
    SC_EXPECT(pXMemSize(tiledMem, tiledAttrs) >= layout.SizeBytes);

    // Mip 0 (64x64 BC7 blocks -> macro-tiled) & Mip 1 (32x32 BC7 blocks -> micro-tiled)
    auto verifyMip = [&](uint32_t mip, uint32_t bw, uint32_t bh, uint8_t seed) -> bool {
        const uint32_t rowPitch = bw * 16u;
        const size_t totalBytes = static_cast<size_t>(rowPitch) * bh;
        std::vector<uint8_t> src(totalBytes);
        std::vector<uint8_t> dst(totalBytes, 0);
        for (size_t i = 0; i < totalBytes; ++i) {
            src[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFFu);
        }
        if (FAILED(comp->CopyIntoSubresource(tiledMem, mip, src.data(), rowPitch, static_cast<uint32_t>(totalBytes)))) {
            return false;
        }
        if (FAILED(comp->CopyFromSubresource(dst.data(), mip, tiledMem, rowPitch, static_cast<uint32_t>(totalBytes)))) {
            return false;
        }
        return std::memcmp(src.data(), dst.data(), totalBytes) == 0;
    };

    SC_EXPECT(verifyMip(0, 64, 64, 0x11));
    SC_EXPECT(verifyMip(1, 32, 32, 0x5A));

    comp->Release();
    pXMemFree(tiledMem, tiledAttrs);
    FreeLibrary(xgMod);
    FreeLibrary(xmemMod);
    return true;
}

bool TestXMemMin16ByteAlignment() {
    // Request sub-16B alignments (code=1 -> 4B, code=2 -> 8B) and default (code=0):
    // Xbox Series X|S / SlimEra ABI mandates >= 16-byte alignment from XMemAllocDefault.
    const uint32_t attr4B   = (1u << 8) | (1u << 14); // 4B code + zero-init
    const uint32_t attr8B   = (2u << 8) | (1u << 14); // 8B code + zero-init
    const uint32_t attrDef  = (0u << 8) | (1u << 14); // default + zero-init
    const uint32_t attr256B = (7u << 8);              // 256B code

    void* p4   = XMemAllocDefault(24, attr4B);
    void* p8   = XMemAllocDefault(37, attr8B);
    void* pDef = XMemAllocDefault(19, attrDef);
    void* p256 = XMemAllocDefault(512, attr256B);

    SC_EXPECT(p4 != nullptr && p8 != nullptr && pDef != nullptr && p256 != nullptr);
    SC_EXPECT((reinterpret_cast<uintptr_t>(p4)   % 16u) == 0u);
    SC_EXPECT((reinterpret_cast<uintptr_t>(p8)   % 16u) == 0u);
    SC_EXPECT((reinterpret_cast<uintptr_t>(pDef) % 16u) == 0u);
    SC_EXPECT((reinterpret_cast<uintptr_t>(p256) % 256u) == 0u);
    SC_EXPECT(XMemSizeDefault(p4, attr4B) == 24u);
    SC_EXPECT(XMemSizeDefault(p8, attr8B) == 37u);

    const auto* bytes8 = static_cast<const uint8_t*>(p8);
    for (size_t i = 0; i < 37; ++i) {
        SC_EXPECT(bytes8[i] == 0u);
    }

    XMemFreeDefault(p256, attr256B);
    XMemFreeDefault(pDef, attrDef);
    XMemFreeDefault(p8, attr8B);
    XMemFreeDefault(p4, attr4B);
    return true;
}
