#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "shims/d3d12_xs/d3d12_xs.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth  = 800;
constexpr uint32_t kHeight = 600;

struct Mat4 { float m[16]; };

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

Mat4 MakeRotY(float a) {
    float c = std::cos(a), s = std::sin(a);
    return { c,0,-s,0,  0,1,0,0,  s,0,c,0,  0,0,0,1 };
}

Mat4 MakeRotX(float a) {
    float c = std::cos(a), s = std::sin(a);
    return { 1,0,0,0,  0,c,s,0,  0,-s,c,0,  0,0,0,1 };
}

Mat4 MakeTranslate(float x, float y, float z) {
    return { 1,0,0,0,  0,1,0,0,  0,0,1,0,  x,y,z,1 };
}

Mat4 MakePerspectiveLH(float fovY, float aspect, float zn, float zf) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    float a = zf / (zf - zn);
    float b = (-zn * zf) / (zf - zn);
    return { f / aspect,0,0,0,  0,f,0,0,  0,0,a,1,  0,0,b,0 };
}

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
};

struct CameraCB {
    Mat4 mvp;
    Mat4 world;
};

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 1. Initialize d3d12_xs device & direct queue
    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{};
    devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    if (FAILED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX,
            reinterpret_cast<void**>(devX.GetAddressOf())))) return 2;

    ComPtr<ID3D12Device> nativeDev;
    if (FAILED(devX->QueryInterface(IID_ScarlettNativeD3D12Device,
            reinterpret_cast<void**>(nativeDev.GetAddressOf())))) return 3;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    if (FAILED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf()))) return 4;

    ComPtr<ID3D12CommandAllocator> alloc;
    if (FAILED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return 5;

    ComPtr<ID3D12GraphicsCommandListX> clX;
    if (FAILED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
            IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf())))) return 6;
    clX->Close();

    // 2. Compile lit 3D cube shaders
    static const char* kHlsl = R"(
        cbuffer CameraCB : register(b0) {
            row_major float4x4 mvp;
            row_major float4x4 world;
        };
        struct VSIn {
            float3 pos : POSITION;
            float3 nrm : NORMAL;
            float3 col : COLOR;
        };
        struct PSIn {
            float4 pos : SV_POSITION;
            float3 nrm : NORMAL;
            float3 col : COLOR;
        };
        PSIn VSMain(VSIn i) {
            PSIn o;
            o.pos = mul(float4(i.pos, 1.0f), mvp);
            o.nrm = normalize(mul(float4(i.nrm, 0.0f), world).xyz);
            o.col = i.col;
            return o;
        }
        float4 PSMain(PSIn i) : SV_TARGET {
            float3 lightDir = normalize(float3(0.55f, 0.75f, -0.65f));
            float ndl = max(dot(normalize(i.nrm), lightDir), 0.0f);
            float3 lit = i.col * (0.28f + 0.72f * ndl);
            return float4(lit, 1.0f);
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob))) return 7;
    if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob))) return 8;

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor = { 0, 0 };
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ComPtr<ID3DBlob> sigBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) return 9;
    ComPtr<ID3D12RootSignature> rootSig;
    if (FAILED(nativeDev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) return 10;

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.InputLayout = { il, 3 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(nativeDev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) return 11;

    // 3. Create 800x600 RGBA RTV (initial state PRESENT for PresentX) + D32_FLOAT DSV
    D3D12_DESCRIPTOR_HEAP_DESC rtvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC dsvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    devX->CreateDescriptorHeap(&rtvHD, IID_PPV_ARGS(&rtvHeap));
    devX->CreateDescriptorHeap(&dsvHD, IID_PPV_ARGS(&dsvHeap));

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uplHeap{ D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kWidth; rtDesc.Height = kHeight; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtClear.Color[0] = 0.06f; rtClear.Color[1] = 0.06f; rtClear.Color[2] = 0.10f; rtClear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> rtTex;
    devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PRESENT, &rtClear, IID_PPV_ARGS(&rtTex));

    D3D12_RESOURCE_DESC dsDesc{};
    dsDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dsDesc.Width = kWidth; dsDesc.Height = kHeight; dsDesc.DepthOrArraySize = 1; dsDesc.MipLevels = 1;
    dsDesc.Format = DXGI_FORMAT_D32_FLOAT; dsDesc.SampleDesc.Count = 1;
    dsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dsClear{};
    dsClear.Format = DXGI_FORMAT_D32_FLOAT; dsClear.DepthStencil.Depth = 1.0f;
    ComPtr<ID3D12Resource> dsTex;
    devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &dsDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dsClear, IID_PPV_ARGS(&dsTex));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvH);
    nativeDev->CreateDepthStencilView(dsTex.Get(), nullptr, dsvH);

    // 4. Unit cube (24 vertices, 36 indices, 6 distinct face colors)
    const Vertex cubeVerts[24] = {
        {-1,-1,-1,  0,0,-1,  1.00f,0.00f,0.25f}, {-1, 1,-1,  0,0,-1,  1.00f,0.00f,0.25f},
        { 1, 1,-1,  0,0,-1,  1.00f,0.00f,0.25f}, { 1,-1,-1,  0,0,-1,  1.00f,0.00f,0.25f},
        { 1,-1, 1,  0,0, 1,  0.00f,0.50f,1.00f}, { 1, 1, 1,  0,0, 1,  0.00f,0.50f,1.00f},
        {-1, 1, 1,  0,0, 1,  0.00f,0.50f,1.00f}, {-1,-1, 1,  0,0, 1,  0.00f,0.50f,1.00f},
        {-1, 1,-1,  0,1, 0,  1.00f,0.82f,0.10f}, {-1, 1, 1,  0,1, 0,  1.00f,0.82f,0.10f},
        { 1, 1, 1,  0,1, 0,  1.00f,0.82f,0.10f}, { 1, 1,-1,  0,1, 0,  1.00f,0.82f,0.10f},
        {-1,-1, 1,  0,-1,0,  0.05f,0.85f,0.45f}, {-1,-1,-1,  0,-1,0,  0.05f,0.85f,0.45f},
        { 1,-1,-1,  0,-1,0,  0.05f,0.85f,0.45f}, { 1,-1, 1,  0,-1,0,  0.05f,0.85f,0.45f},
        {-1,-1, 1, -1,0, 0,  0.65f,0.25f,1.00f}, {-1, 1, 1, -1,0, 0,  0.65f,0.25f,1.00f},
        {-1, 1,-1, -1,0, 0,  0.65f,0.25f,1.00f}, {-1,-1,-1, -1,0, 0,  0.65f,0.25f,1.00f},
        { 1,-1,-1,  1,0, 0,  0.00f,0.88f,0.95f}, { 1, 1,-1,  1,0, 0,  0.00f,0.88f,0.95f},
        { 1, 1, 1,  1,0, 0,  0.00f,0.88f,0.95f}, { 1,-1, 1,  1,0, 0,  0.00f,0.88f,0.95f},
    };
    const uint16_t cubeIdx[36] = {
         0, 1, 2,  0, 2, 3,   4, 5, 6,  4, 6, 7,   8, 9,10,  8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23
    };

    auto makeBuf = [&](size_t sz, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        devX->CreateCommittedResource(&uplHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
    };

    ComPtr<ID3D12Resource> vbBuf, ibBuf, cbBuf;
    makeBuf(sizeof(cubeVerts), vbBuf);
    makeBuf(sizeof(cubeIdx),   ibBuf);
    makeBuf(256,               cbBuf);

    void* pMap = nullptr;
    vbBuf->Map(0, nullptr, &pMap); std::memcpy(pMap, cubeVerts, sizeof(cubeVerts)); vbBuf->Unmap(0, nullptr);
    ibBuf->Map(0, nullptr, &pMap); std::memcpy(pMap, cubeIdx, sizeof(cubeIdx));     ibBuf->Unmap(0, nullptr);

    CameraCB* cbPtr = nullptr;
    cbBuf->Map(0, nullptr, reinterpret_cast<void**>(&cbPtr));

    ComPtr<ID3D12Fence> fence;
    nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceVal = 0;

    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
    D3D12_VERTEX_BUFFER_VIEW vbv{ vbBuf->GetGPUVirtualAddress(), sizeof(cubeVerts), sizeof(Vertex) };
    D3D12_INDEX_BUFFER_VIEW  ibv{ ibBuf->GetGPUVirtualAddress(), sizeof(cubeIdx), DXGI_FORMAT_R16_UINT };

    const Mat4 proj = MakePerspectiveLH(3.14159265f / 3.0f, static_cast<float>(kWidth) / kHeight, 0.1f, 100.0f);
    LARGE_INTEGER freq{}, tPrev{}, tNow{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tPrev);

    bool  autoSpin = true;
    bool  spaceWasDown = false;
    float rotY = 0.4f, rotX = 0.35f;

    ID3D12Resource* presentRes = rtTex.Get();
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1;
    plane.ppResources   = &presentRes;
    D3D12XBOX_PRESENT_PARAMETERS presentParams{};

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
        bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (spaceDown && !spaceWasDown) autoSpin = !autoSpin;
        spaceWasDown = spaceDown;

        QueryPerformanceCounter(&tNow);
        float dt = static_cast<float>(tNow.QuadPart - tPrev.QuadPart) / static_cast<float>(freq.QuadPart);
        tPrev = tNow;
        if (dt > 0.1f) dt = 0.016f;

        if (autoSpin) { rotY += dt * 1.1f; rotX += dt * 0.55f; }
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000 || GetAsyncKeyState('A') & 0x8000) rotY -= dt * 2.2f;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000 || GetAsyncKeyState('D') & 0x8000) rotY += dt * 2.2f;
        if (GetAsyncKeyState(VK_UP)    & 0x8000 || GetAsyncKeyState('W') & 0x8000) rotX -= dt * 2.2f;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000 || GetAsyncKeyState('S') & 0x8000) rotX += dt * 2.2f;

        Mat4 world = Mul(MakeRotX(rotX), MakeRotY(rotY));
        cbPtr->world = world;
        cbPtr->mvp   = Mul(Mul(world, MakeTranslate(0.0f, 0.0f, 4.2f)), proj);

        alloc->Reset();
        clX->Reset(alloc.Get(), pso.Get());

        D3D12_RESOURCE_BARRIER bToRt{};
        bToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bToRt.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        clX->ResourceBarrier(1, &bToRt);

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
        clX->DrawIndexedInstanced(36, 1, 0, 0, 0);

        D3D12_RESOURCE_BARRIER bToPresent{};
        bToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bToPresent.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT };
        clX->ResourceBarrier(1, &bToPresent);
        clX->Close();

        ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
        queueX->ExecuteCommandLists(1, lists);

        HRESULT hr = queueX->PresentX(1, &plane, &presentParams);
        if (hr != S_OK) break; // S_FALSE when host window closed

        ++fenceVal;
        queueX->Signal(fence.Get(), fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        WaitForSingleObject(fenceEv, INFINITE);
    }

    CloseHandle(fenceEv);
    cbBuf->Unmap(0, nullptr);
    return 0;
}