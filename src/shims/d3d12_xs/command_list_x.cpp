#include "d3d12_xs_internal.hpp"
#include "common/stub.hpp"

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace {
class ScarlettGraphicsCommandListX final : public ID3D12GraphicsCommandListX {
public:
    explicit ScarlettGraphicsCommandListX(ID3D12GraphicsCommandList* nativeCl)
        : m_cmdList(nativeCl) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ID3D12GraphicsCommandListX)) {
            *ppvObject = static_cast<ID3D12GraphicsCommandListX*>(this);
            AddRef();
            return S_OK;
        }
        return m_cmdList ? m_cmdList->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Close() override { return m_cmdList->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override {
        return m_cmdList->Reset(pAllocator, pInitialState);
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override {
        m_cmdList->SetPipelineState(pPipelineState);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override {
        m_cmdList->SetGraphicsRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override {
        m_cmdList->SetComputeRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT idx, D3D12_GPU_VIRTUAL_ADDRESS addr) override {
        m_cmdList->SetComputeRootShaderResourceView(idx, addr);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT idx, D3D12_GPU_VIRTUAL_ADDRESS addr) override {
        m_cmdList->SetComputeRootUnorderedAccessView(idx, addr);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT num, const D3D12_RESOURCE_BARRIER* pBarriers) override {
        m_cmdList->ResourceBarrier(num, pBarriers);
    }
    void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override {
        m_cmdList->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT vtx, UINT inst, UINT startVtx, UINT startInst) override {
        m_cmdList->DrawInstanced(vtx, inst, startVtx, startInst);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
        m_cmdList->Dispatch(x, y, z);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT /*Metadata*/, const void* /*pData*/, UINT /*Size*/) override {
        // Intentional no-op: raw ID3D12CommandList::BeginEvent triggers D3D12 debug-layer errors
        // unless invoked via PIX runtime; Scarlett titles route markers through pixevt.dll.
    }
    void STDMETHODCALLTYPE EndEvent() override {
    }
    void STDMETHODCALLTYPE SetMarker(UINT /*Metadata*/, const void* /*pData*/, UINT /*Size*/) override {
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override {
        m_cmdList->IASetPrimitiveTopology(PrimitiveTopology);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) override {
        m_cmdList->RSSetViewports(NumViewports, pViewports);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) override {
        m_cmdList->RSSetScissorRects(NumRects, pRects);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumRenderTargetDescriptors,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override {
        m_cmdList->OMSetRenderTargets(
            NumRenderTargetDescriptors, pRenderTargetDescriptors,
            RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView,
        const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) override {
        m_cmdList->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(
        UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override {
        m_cmdList->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {
        m_cmdList->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {
        m_cmdList->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        m_cmdList->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        m_cmdList->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override {
        m_cmdList->IASetVertexBuffers(StartSlot, NumViews, pViews);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override {
        m_cmdList->IASetIndexBuffer(pView);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount,
        UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override {
        m_cmdList->DrawIndexedInstanced(
            IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
        FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override {
        m_cmdList->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override {
        m_cmdList->SetGraphicsRoot32BitConstants(
            RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override {
        m_cmdList->SetComputeRoot32BitConstants(
            RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override {
        m_cmdList->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    }
    void STDMETHODCALLTYPE InsertWaitOnPresent(UINT64 /*PresentToken*/) override {
        // Scarlett flip-queue wait; host PresentX synchronizes via DXGI swapchain
    }
    ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetNativeCommandList() override {
        return m_cmdList.Get();
    }
    void STDMETHODCALLTYPE Barrier(
        UINT32 NumBarrierGroups,
        const D3D12_BARRIER_GROUP* pBarrierGroups) override {
        if (!m_cmdList || NumBarrierGroups == 0 || !pBarrierGroups) {
            return;
        }
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> cl7;
        if (SUCCEEDED(m_cmdList.As(&cl7)) && cl7) {
            cl7->Barrier(NumBarrierGroups, pBarrierGroups);
        } else {
            sc::log::Write(sc::log::Level::Warn, "d3d12_xs", "Barrier",
                           "-> ID3D12GraphicsCommandList7 unavailable; Enhanced Barrier ignored");
        }
    }

private:
    std::atomic<ULONG> m_refs{1};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
};


} // namespace

namespace sc::d3d12xs {

ComPtr<ID3D12GraphicsCommandListX> CreateScarlettGraphicsCommandListX(
    ID3D12GraphicsCommandList* nativeCl) {
    return MakeCom<ID3D12GraphicsCommandListX, ScarlettGraphicsCommandListX>(nativeCl);
}

} // namespace sc::d3d12xs