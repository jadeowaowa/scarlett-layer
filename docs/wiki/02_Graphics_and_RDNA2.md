# 02 â€” Graphics (`d3d12_xs`), Surface Tiling (`xg_xs`) & RDNA2 Binary Lifter

## `d3d12_xs.dll`
**Files:** `src/shims/d3d12_xs/d3d12_xs.cpp` (1288 L), `src/shims/d3d12_xs/d3d12_xs.def` (4 L), `src/shims/d3d12_xs/d3d12_xs.hpp` (222 L), `src/shims/d3d12_xs/rdna2_isa.hpp` (1191 L), `src/shims/d3d12_xs/shader_inspector.hpp` (201 L), `src/shims/d3d12_xs/shader_translator.hpp` (401 L)

**DLL Exports (`.def`):** `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature = ScarlettSerializeRootSignature`

**C Entrypoints:**
- `HRESULT WINAPI D3D12XboxCreateDevice( void* pAdapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters, REFIID riid, void** ppDevice)`

**COM Interface `ID3D12CommandQueueX : IUnknown`:**
- `[03]` `void ExecuteCommandLists( UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)`
- `[04]` `HRESULT Signal(ID3D12Fence* pFence, UINT64 Value)`
- `[05]` `HRESULT Wait(ID3D12Fence* pFence, UINT64 Value)`
- `[06]` `HRESULT PresentX( UINT PlaneCount, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters, const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters)`

**COM Interface `ID3D12GraphicsCommandListX : IUnknown`:**
- `[03]` `HRESULT Close()`
- `[04]` `HRESULT Reset( ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState)`
- `[05]` `void SetPipelineState(ID3D12PipelineState* pPipelineState)`
- `[06]` `void SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature)`
- `[07]` `void SetComputeRootSignature(ID3D12RootSignature* pRootSignature)`
- `[08]` `void SetComputeRootShaderResourceView( UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)`
- `[09]` `void SetComputeRootUnorderedAccessView( UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)`
- `[10]` `void ResourceBarrier( UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers)`
- `[11]` `void CopyBufferRegion( ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)`
- `[12]` `void DrawInstanced( UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)`
- `[13]` `void Dispatch( UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)`
- `[14]` `void BeginEvent(UINT Metadata, const void* pData, UINT Size)`
- `[15]` `void EndEvent()`
- `[16]` `void SetMarker(UINT Metadata, const void* pData, UINT Size)`
- `[17]` `void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)`
- `[18]` `void RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports)`
- `[19]` `void RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects)`
- `[20]` `void OMSetRenderTargets( UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)`
- `[21]` `void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects)`
- `[22]` `void SetDescriptorHeaps( UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps)`
- `[23]` `void SetGraphicsRootDescriptorTable( UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)`
- `[24]` `void SetComputeRootDescriptorTable( UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)`
- `[25]` `void SetGraphicsRootConstantBufferView( UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)`
- `[26]` `void SetComputeRootConstantBufferView( UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)`
- `[27]` `void CopyTextureRegion( const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)`
- `[28]` `void InsertWaitOnPresent(UINT64 PresentToken)`
- `[29]` `ID3D12GraphicsCommandList* GetNativeCommandList()`

**COM Interface `ID3D12DeviceX : IUnknown`:**
- `[03]` `HRESULT CreateCommandQueueX( const D3D12_COMMAND_QUEUE_DESC* pDesc, ID3D12CommandQueueX** ppCommandQueue)`
- `[04]` `HRESULT CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator)`
- `[05]` `HRESULT CreateCommandList( UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList)`
- `[06]` `HRESULT CreateDescriptorHeap( const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap)`
- `[07]` `UINT GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType)`
- `[08]` `HRESULT CreateCommittedResource( const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource)`
- `[09]` `D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo( UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs)`
- `[10]` `HRESULT CreateHeap( const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap)`
- `[11]` `HRESULT CreatePlacedResource( ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource)`
- `[12]` `void CreateRenderTargetView( ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
- `[13]` `void CreateShaderResourceView( ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
- `[14]` `void CreateUnorderedAccessView( ID3D12Resource* pResource, ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
- `[15]` `HRESULT CreateFence( UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence)`
- `[16]` `HRESULT CreateRootSignature( UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature)`
- `[17]` `HRESULT CreateGraphicsPipelineState( const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState)`
- `[18]` `HRESULT CreateComputePipelineState( const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState)`
- `[19]` `HRESULT SetFrameIntervalX( IDXGIOutput* pOutput, uint32_t PeriodInMicroseconds, uint32_t MaxLatency, uint32_t Flags)`
- `[20]` `HRESULT ScheduleFrameEventX( D3D12XBOX_FRAME_EVENT Event, UINT64 OffsetMicroseconds, HANDLE hEvent, uint32_t Flags)`
- `[21]` `HRESULT WaitFrameEventX( D3D12XBOX_FRAME_EVENT Event, DWORD TimeoutInMilliseconds, HANDLE hEvent, D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken)`

**Key Enums:**
- `D3D12XBOX_FRAME_INTERVAL_FLAGS`: D3D12XBOX_FRAME_INTERVAL_FLAG_NONE = 0
- `D3D12XBOX_FRAME_EVENT`: D3D12XBOX_FRAME_EVENT_ORIGIN = 0, D3D12XBOX_FRAME_EVENT_DISPLAY = 1
- `SoppOp`: kSoppNop = 0x00u, kSoppEndpgm = 0x01u, kSoppWaitcnt = 0x0Cu
- `Vop1Op`: kVop1Nop = 0x00u, kVop1MovB32 = 0x01u, kVop1RcpF32 = 0x2Au, kVop1SqrtF32 = 0x33u
- `Vop2Op`: kVop2AddF32 = 0x03u, kVop2SubF32 = 0x04u, kVop2SubRevF32 = 0x05u, kVop2MulF32 = 0x08u, kVop2MinF32 = 0x0Fu, kVop2MaxF32 = 0x10u, kVop2AddNcU32 = 0x25u, kVop2FmacF32 = 0x2Bu
- `Vop3Op`: kVop3MadF32 = 0x141u, kVop3FmaF32 = 0x14Bu, kVop3Min3F32 = 0x150u, kVop3Max3F32 = 0x151u, kVop3Med3F32 = 0x152u, kVop3MulLoU32 = 0x169u
- `MubufOp`: kMubufLoadDword = 0x14u, kMubufStoreDword = 0x1Cu
- `SmemOp`: kSmemLoadDword = 0x00u, kSmemLoadDwordX2 = 0x01u, kSmemLoadDwordX4 = 0x02u
- `VintrpOp`: kVintrpP1F32 = 0x00u, kVintrpP2F32 = 0x01u, kVintrpMovF32 = 0x02u
- `MimgOp`: kMimgImageSample = 0x20u
- `InstFormat`: Sopp, Vop1, Vop2, Vop3, Exp, Mubuf, Smem, Vintrp, Mimg
- `ShaderContainerKind`: None = 0, Dxbc = 1, Dxil = 2, Scxs = 3, Unknown = 4
- `ScxsShaderStage`: Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5, Mesh = 6, Amplification = 7
- `ScxsTargetArch`: Unknown = 0, Rdna2Scarlett = 1, HostReference = 2
- `ScxsPayloadFormat`: EmbeddedDxbcOrDxil = 1, Rdna2Isa = 2

## `xg_xs.dll`
**Files:** `src/shims/xg_xs/xg_64kbs.hpp` (325 L), `src/shims/xg_xs/xg_xs.cpp` (304 L), `src/shims/xg_xs/xg_xs.def` (4 L), `src/shims/xg_xs/xg_xs.hpp` (75 L)

**DLL Exports (`.def`):** `XGCreateTexture2DComputer`, `XGComputeTexture2DLayout`

**C Entrypoints:**
- `HRESULT WINAPI XGCreateTexture2DComputer( const XG_TEXTURE2D_DESC* pDesc, IXGTextureAddressComputer** ppComputer)`
- `HRESULT WINAPI XGComputeTexture2DLayout( const XG_TEXTURE2D_DESC* pDesc, XG_RESOURCE_LAYOUT* pLayout)`

**COM Interface `IXGTextureAddressComputer : IUnknown`:**
- `[03]` `HRESULT GetResourceLayout( XG_RESOURCE_LAYOUT* pLayout)`
- `[04]` `HRESULT CopyIntoSubresource( void* pResourceBaseAddress, uint32_t Subresource, const void* pSourceData, uint32_t SourceRowPitch, uint32_t SourceSlicePitch)`
- `[05]` `HRESULT CopyFromSubresource( void* pDestinationData, uint32_t Subresource, const void* pResourceBaseAddress, uint32_t DestRowPitch, uint32_t DestSlicePitch)`

**Key Enums:**
- `XG_TILE_MODE`: XG_TILE_MODE_LINEAR = 0, XG_TILE_MODE_2D_THIN = 1, XG_TILE_MODE_64KB_S = 2
