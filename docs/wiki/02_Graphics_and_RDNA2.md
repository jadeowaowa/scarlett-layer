# 02 — Graphics (`d3d12_xs`), Surface Tiling (`xg_xs`) & RDNA2 Binary Lifter

## `d3d12_xs.dll`
**Files:** `src/shims/d3d12_xs/command_list_x.cpp` (187 L), `src/shims/d3d12_xs/command_queue_x.cpp` (457 L), `src/shims/d3d12_xs/d3d12_xs.cpp` (119 L), `src/shims/d3d12_xs/d3d12_xs.def` (4 L), `src/shims/d3d12_xs/d3d12_xs.hpp` (261 L), `src/shims/d3d12_xs/d3d12_xs_internal.hpp` (79 L), `src/shims/d3d12_xs/d3d12_xs_utils.cpp` (85 L), `src/shims/d3d12_xs/device_x.cpp` (742 L), `src/shims/d3d12_xs/rdna2_isa.hpp` (1040 L), `src/shims/d3d12_xs/rdna2_lifter.hpp` (1052 L), `src/shims/d3d12_xs/shader_inspector.hpp` (201 L), `src/shims/d3d12_xs/shader_stripper.hpp` (272 L), `src/shims/d3d12_xs/shader_translator.hpp` (405 L)

**DLL Exports (`.def`):** `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature = ScarlettSerializeRootSignature`

**C Entrypoints:**
- `HRESULT WINAPI D3D12XboxCreateDevice( void* pAdapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters, REFIID riid, void** ppDevice)`

**COM Interface `ID3D12CommandQueueX : IUnknown`:**
- `[03]` `void ExecuteCommandLists( UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)`
- `[04]` `HRESULT Signal(ID3D12Fence* pFence, UINT64 Value)`
- `[05]` `HRESULT Wait(ID3D12Fence* pFence, UINT64 Value)`
- `[06]` `HRESULT PresentX( UINT PlaneCount, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters, const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters)`
- `[07]` `void SuspendX(UINT32 Flags)`
- `[08]` `void ResumeX()`

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
- `[27]` `void IASetVertexBuffers( UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews)`
- `[28]` `void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView)`
- `[29]` `void DrawIndexedInstanced( UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)`
- `[30]` `void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects)`
- `[31]` `void SetGraphicsRoot32BitConstants( UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)`
- `[32]` `void SetComputeRoot32BitConstants( UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)`
- `[33]` `void CopyTextureRegion( const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)`
- `[34]` `void InsertWaitOnPresent(UINT64 PresentToken)`
- `[35]` `ID3D12GraphicsCommandList* GetNativeCommandList()`
- `[36]` `void Barrier( UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP* pBarrierGroups)`

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
- `[21]` `HRESULT WaitFrameEventX( D3D12XBOX_FRAME_EVENT Event, DWORD TimeoutInMilliseconds, HANDLE hEvent, uint32_t Flags, D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken)`
- `[22]` `HRESULT CheckFeatureSupport( D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize)`

**Key Enums:**
- `D3D12XBOX_FRAME_INTERVAL_FLAGS`: D3D12XBOX_FRAME_INTERVAL_FLAG_NONE = 0
- `D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAGS`: D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE = 0
- `D3D12XBOX_WAIT_FRAME_EVENT_FLAGS`: D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE = 0
- `D3D12XBOX_FRAME_EVENT`: D3D12XBOX_FRAME_EVENT_ORIGIN = 0, D3D12XBOX_FRAME_EVENT_DISPLAY = 1
- `SoppOp`: kSoppNop = 0x00u, kSoppEndpgm = 0x01u, kSoppBranch = 0x02u, kSoppCbranchScc0 = 0x04u, kSoppCbranchScc1 = 0x05u, kSoppCbranchVccz = 0x06u, kSoppCbranchVccnz = 0x07u, kSoppCbranchExecz = 0x08u, kSoppCbranchExecnz = 0x09u, kSoppBarrier = 0x0Au, kSoppWaitcnt = 0x0Cu, kSoppSendmsg = 0x10u, kSoppInstPrefetch = 0x20u, kSoppClause = 0x21u, ... (+1 more)
- `Sop1Op`: kSop1MovB32 = 0x03u, kSop1MovB64 = 0x04u, kSop1NotB32 = 0x07u, kSop1NotB64 = 0x08u, kSop1WqmB64 = 0x09u, kSop1BrevB32 = 0x0Bu, kSop1Bcnt1I32B32 = 0x0Eu, kSop1Bcnt1I32B64 = 0x0Fu, kSop1FlbitI32B32 = 0x16u, kSop1Bitset1B32 = 0x1Au, kSop1GetpcB64 = 0x1Fu, kSop1AndSaveexecB64 = 0x24u, kSop1OrSaveexecB64 = 0x25u, kSop1Andn2SaveexecB64 = 0x27u, ... (+3 more)
- `Sop2Op`: kSop2AddU32 = 0x00u, kSop2SubU32 = 0x01u, kSop2AddI32 = 0x02u, kSop2SubI32 = 0x03u, kSop2AddcU32 = 0x04u, kSop2MinI32 = 0x06u, kSop2MinU32 = 0x07u, kSop2MaxI32 = 0x08u, kSop2MaxU32 = 0x09u, kSop2CselectB32 = 0x0Au, kSop2CselectB64 = 0x0Bu, kSop2AndB32 = 0x0Eu, kSop2AndB64 = 0x0Fu, kSop2OrB32 = 0x10u, ... (+14 more)
- `SopcOp`: kSopcCmpEqI32 = 0x00u, kSopcCmpLgI32 = 0x01u, kSopcCmpGtI32 = 0x02u, kSopcCmpGeI32 = 0x03u, kSopcCmpLtI32 = 0x04u, kSopcCmpLeI32 = 0x05u, kSopcCmpEqU32 = 0x06u, kSopcCmpLgU32 = 0x07u, kSopcCmpGtU32 = 0x08u, kSopcCmpGeU32 = 0x09u, kSopcCmpLtU32 = 0x0Au, kSopcCmpLeU32 = 0x0Bu, kSopcBitcmp0B32 = 0x0Cu, kSopcBitcmp1B32 = 0x0Du
- `SopkOp`: kSopkMovkI32 = 0x00u, kSopkCmovkI32 = 0x02u, kSopkCmpkEqI32 = 0x03u, kSopkCmpkLgI32 = 0x04u, kSopkCmpkGtI32 = 0x05u, kSopkCmpkGeI32 = 0x06u, kSopkCmpkLtI32 = 0x07u, kSopkCmpkLeI32 = 0x08u, kSopkCmpkEqU32 = 0x09u, kSopkCmpkLgU32 = 0x0Au, kSopkCmpkGtU32 = 0x0Bu, kSopkCmpkGeU32 = 0x0Cu, kSopkCmpkLtU32 = 0x0Du, kSopkCmpkLeU32 = 0x0Eu, ... (+5 more)
- `Vop1Op`: kVop1Nop = 0x00u, kVop1MovB32 = 0x01u, kVop1Readfirstlane = 0x02u, kVop1CvtF32I32 = 0x05u, kVop1CvtF32U32 = 0x06u, kVop1CvtU32F32 = 0x07u, kVop1CvtI32F32 = 0x08u, kVop1CvtF16F32 = 0x0Au, kVop1CvtF32F16 = 0x0Bu, kVop1CvtF32Ubyte0 = 0x11u, kVop1FractF32 = 0x20u, kVop1TruncF32 = 0x21u, kVop1CeilF32 = 0x22u, kVop1RndneF32 = 0x23u, ... (+12 more)
- `Vop2Op`: kVop2CndmaskB32 = 0x01u, kVop2AddF32 = 0x03u, kVop2SubF32 = 0x04u, kVop2SubRevF32 = 0x05u, kVop2MulLegacyF32 = 0x07u, kVop2MulF32 = 0x08u, kVop2MulI32I24 = 0x09u, kVop2MulU32U24 = 0x0Bu, kVop2MinF32 = 0x0Fu, kVop2MaxF32 = 0x10u, kVop2MinI32 = 0x11u, kVop2MaxI32 = 0x12u, kVop2MinU32 = 0x13u, kVop2MaxU32 = 0x14u, ... (+19 more)
- `Vop3Op`: kVop3MadF32 = 0x141u, kVop3MadU32U24 = 0x143u, kVop3BfeU32 = 0x148u, kVop3BfeI32 = 0x149u, kVop3BfiB32 = 0x14Au, kVop3FmaF32 = 0x14Bu, kVop3AlignbitB32 = 0x14Eu, kVop3Min3F32 = 0x151u, kVop3Max3F32 = 0x154u, kVop3Med3F32 = 0x157u, kVop3CvtPkrtzF16F32 = 0x168u, kVop3MulLoU32 = 0x169u, kVop3MulHiU32 = 0x16Au, kVop3MulHiI32 = 0x16Cu, ... (+15 more)
- `Vop3pOp`: kVop3pPkMadI16 = 0x00u, kVop3pPkMulLoU16 = 0x01u, kVop3pPkAddI16 = 0x02u, kVop3pPkSubI16 = 0x03u, kVop3pPkFmaF16 = 0x0Eu, kVop3pPkAddF16 = 0x0Fu, kVop3pPkMulF16 = 0x10u, kVop3pPkMinF16 = 0x11u, kVop3pPkMaxF16 = 0x12u
- `MubufOp`: kMubufLoadDword = 0x0Cu, kMubufLoadDwordX2 = 0x0Du, kMubufLoadDwordX4 = 0x0Eu, kMubufLoadDwordX3 = 0x0Fu, kMubufLoadDwordLegacy = 0x14u, kMubufStoreDword = 0x1Cu, kMubufStoreDwordX2 = 0x1Du, kMubufStoreDwordX4 = 0x1Eu, kMubufStoreDwordX3 = 0x1Fu, kMubufAtomicAdd = 0x32u
- `SmemOp`: kSmemLoadDword = 0x00u, kSmemLoadDwordX2 = 0x01u, kSmemLoadDwordX4 = 0x02u, kSmemLoadDwordX8 = 0x03u, kSmemLoadDwordX16 = 0x04u, kSmemBufferLoadDword = 0x08u, kSmemBufferLoadDwordX2 = 0x09u, kSmemBufferLoadDwordX4 = 0x0Au, kSmemBufferLoadDwordX8 = 0x0Bu
- `VintrpOp`: kVintrpP1F32 = 0x00u, kVintrpP2F32 = 0x01u, kVintrpMovF32 = 0x02u
- `MimgOp`: kMimgImageLoad = 0x00u, kMimgImageStore = 0x08u, kMimgImageSample = 0x20u, kMimgImageSampleL = 0x24u, kMimgImageSampleLz = 0x27u, kMimgImageSampleLO = 0x34u
- `InstFormat`: Sopp, Sop1, Sop2, Sopc, Sopk, Vop1, Vop2, Vopc, Vop3, Vop3p, Exp, Mubuf, Mtbuf, Smem, ... (+4 more)
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
