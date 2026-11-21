#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shims/d3d12_xs/d3d12_xs.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth    = 800;
constexpr uint32_t kHeight   = 600;
constexpr uint32_t kRowPitch = (kWidth * 4 + 255) & ~255u;

bool  g_running  = true;
bool  g_autoSpin = true;
float g_rotY     = 0.4f;
float g_rotX     = 0.35f;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { g_running = false; DestroyWindow(hwnd); return 0; }
        if (wp == VK_SPACE)  { g_autoSpin = !g_autoSpin; return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"Scarlett3DDemoClass";
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassW(&wc);

    RECT rc{ 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Scarlett Layer - Live 3D Cube (d3d12_xs on RTX 5070)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);

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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(nativeDev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) return 11;

    // 3. Create 800x600 BGRA RTV + D32_FLOAT DSV + Readback buffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC dsvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    devX->CreateDescriptorHeap(&rtvHD, IID_PPV_ARGS(&rtvHeap));
    devX->CreateDescriptorHeap(&dsvHD, IID_PPV_ARGS(&dsvHeap));

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uplHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kWidth; rtDesc.Height = kHeight; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE rtClear{};
    rtClear.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtClear.Color[0] = 0.06f; rtClear.Color[1] = 0.06f; rtClear.Color[2] = 0.10f; rtClear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> rtTex;
    devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &rtClear, IID_PPV_ARGS(&rtTex));

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
        // Front (-Z) — Scarlett Crimson
        {-1,-1,-1,  0,0,-1,  1.00f,0.00f,0.25f}, {-1, 1,-1,  0,0,-1,  1.00f,0.00f,0.25f},
        { 1, 1,-1,  0,0,-1,  1.00f,0.00f,0.25f}, { 1,-1,-1,  0,0,-1,  1.00f,0.00f,0.25f},
        // Back (+Z) — Cobalt Blue
        { 1,-1, 1,  0,0, 1,  0.00f,0.50f,1.00f}, { 1, 1, 1,  0,0, 1,  0.00f,0.50f,1.00f},
        {-1, 1, 1,  0,0, 1,  0.00f,0.50f,1.00f}, {-1,-1, 1,  0,0, 1,  0.00f,0.50f,1.00f},
        // Top (+Y) — Gold
        {-1, 1,-1,  0,1, 0,  1.00f,0.82f,0.10f}, {-1, 1, 1,  0,1, 0,  1.00f,0.82f,0.10f},
        { 1, 1, 1,  0,1, 0,  1.00f,0.82f,0.10f}, { 1, 1,-1,  0,1, 0,  1.00f,0.82f,0.10f},
        // Bottom (-Y) — Emerald
        {-1,-1, 1,  0,-1,0,  0.05f,0.85f,0.45f}, {-1,-1,-1,  0,-1,0,  0.05f,0.85f,0.45f},
        { 1,-1,-1,  0,-1,0,  0.05f,0.85f,0.45f}, { 1,-1, 1,  0,-1,0,  0.05f,0.85f,0.45f},
        // Left (-X) — Violet
        {-1,-1, 1, -1,0, 0,  0.65f,0.25f,1.00f}, {-1, 1, 1, -1,0, 0,  0.65f,0.25f,1.00f},
        {-1, 1,-1, -1,0, 0,  0.65f,0.25f,1.00f}, {-1,-1,-1, -1,0, 0,  0.65f,0.25f,1.00f},
        // Right (+X) — Cyan
        { 1,-1,-1,  1,0, 0,  0.00f,0.88f,0.95f}, { 1, 1,-1,  1,0, 0,  0.00f,0.88f,0.95f},
        { 1, 1, 1,  1,0, 0,  0.00f,0.88f,0.95f}, { 1,-1, 1,  1,0, 0,  0.00f,0.88f,0.95f},
    };
    const uint16_t cubeIdx[36] = {
         0, 1, 2,  0, 2, 3,   4, 5, 6,  4, 6, 7,   8, 9,10,  8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23
    };

    auto makeBuf = [&](size_t sz, const D3D12_HEAP_PROPERTIES& hp, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        devX->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out));
    };

    ComPtr<ID3D12Resource> vbBuf, ibBuf, cbBuf, rbBuf;
    makeBuf(sizeof(cubeVerts), uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, vbBuf);
    makeBuf(sizeof(cubeIdx),   uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, ibBuf);
    makeBuf(256,               uplHeap, D3D12_RESOURCE_STATE_GENERIC_READ, cbBuf);
    makeBuf(static_cast<size_t>(kRowPitch) * kHeight, rbHeap, D3D12_RESOURCE_STATE_COPY_DEST, rbBuf);

    void* pMap = nullptr;
    vbBuf->Map(0, nullptr, &pMap); std::memcpy(pMap, cubeVerts, sizeof(cubeVerts)); vbBuf->Unmap(0, nullptr);
    ibBuf->Map(0, nullptr, &pMap); std::memcpy(pMap, cubeIdx, sizeof(cubeIdx));     ibBuf->Unmap(0, nullptr);

    CameraCB* cbPtr = nullptr;
    cbBuf->Map(0, nullptr, reinterpret_cast<void**>(&cbPtr));
    uint8_t* rbPtr = nullptr;
    rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&rbPtr));

    ComPtr<ID3D12Fence> fence;
    nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceVal = 0;

    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
    D3D12_VERTEX_BUFFER_VIEW vbv{ vbBuf->GetGPUVirtualAddress(), sizeof(cubeVerts), sizeof(Vertex) };
    D3D12_INDEX_BUFFER_VIEW  ibv{ ibBuf->GetGPUVirtualAddress(), sizeof(cubeIdx), DXGI_FORMAT_R16_UINT };

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(kRowPitch / 4);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(kHeight); // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const Mat4 proj = MakePerspectiveLH(3.14159265f / 3.0f, static_cast<float>(kWidth) / kHeight, 0.1f, 100.0f);
    LARGE_INTEGER freq{}, tPrev{}, tNow{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tPrev);
    uint32_t fpsFrames = 0;
    double fpsAccum = 0.0;

    HDC hdc = GetDC(hwnd);
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        QueryPerformanceCounter(&tNow);
        float dt = static_cast<float>(tNow.QuadPart - tPrev.QuadPart) / static_cast<float>(freq.QuadPart);
        tPrev = tNow;
        if (dt > 0.1f) dt = 0.016f;

        if (g_autoSpin) { g_rotY += dt * 1.1f; g_rotX += dt * 0.55f; }
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000 || GetAsyncKeyState('A') & 0x8000) g_rotY -= dt * 2.2f;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000 || GetAsyncKeyState('D') & 0x8000) g_rotY += dt * 2.2f;
        if (GetAsyncKeyState(VK_UP)    & 0x8000 || GetAsyncKeyState('W') & 0x8000) g_rotX -= dt * 2.2f;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000 || GetAsyncKeyState('S') & 0x8000) g_rotX += dt * 2.2f;

        Mat4 world = Mul(MakeRotX(g_rotX), MakeRotY(g_rotY));
        cbPtr->world = world;
        cbPtr->mvp   = Mul(Mul(world, MakeTranslate(0.0f, 0.0f, 4.2f)), proj);

        alloc->Reset();
        clX->Reset(alloc.Get(), pso.Get());
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

        D3D12_RESOURCE_BARRIER bToCopy{};
        bToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bToCopy.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
        clX->ResourceBarrier(1, &bToCopy);

        D3D12_TEXTURE_COPY_LOCATION srcL{ rtTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { 0 } };
        D3D12_TEXTURE_COPY_LOCATION dstL{};
        dstL.pResource = rbBuf.Get();
        dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstL.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, 1, kRowPitch };
        clX->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, nullptr);

        D3D12_RESOURCE_BARRIER bToRt{};
        bToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bToRt.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
        clX->ResourceBarrier(1, &bToRt);
        clX->Close();

        ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
        queueX->ExecuteCommandLists(1, lists);
        ++fenceVal;
        queueX->Signal(fence.Get(), fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        WaitForSingleObject(fenceEv, INFINITE);

        RECT client{};
        GetClientRect(hwnd, &client);
        StretchDIBits(hdc, 0, 0, client.right, client.bottom,
                      0, 0, kWidth, kHeight, rbPtr, &bmi, DIB_RGB_COLORS, SRCCOPY);

        ++fpsFrames;
        fpsAccum += dt;
        if (fpsAccum >= 0.5) {
            wchar_t title[160];
            std::swprintf(title, 160,
                L"Scarlett Layer - Live 3D Cube (d3d12_xs) | %.0f FPS | WASD/Arrows: Rotate | Space: Spin | Esc: Quit",
                fpsFrames / fpsAccum);
            SetWindowTextW(hwnd, title);
            fpsFrames = 0;
            fpsAccum = 0.0;
        }
    }

    ReleaseDC(hwnd, hdc);
    CloseHandle(fenceEv);
    rbBuf->Unmap(0, nullptr);
    cbBuf->Unmap(0, nullptr);
    return 0;
}