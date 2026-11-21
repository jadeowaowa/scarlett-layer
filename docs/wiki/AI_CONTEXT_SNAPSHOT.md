# Scarlett Layer â€” AI Context Snapshot

**Commit:** `b03456b` (#111) | **Generated:** 2026-11-21 15:15 UTC  
**Totals:** 75 files (22,327 LOC) | 12 shims | 61 CTest checks

---

## 1. Sprint Status (`docs/PROGRESS.md`)
| Sprint | Focus | Status |
| :--- | :--- | :--- |
| Sprint 1 | CMake/Ninja/MSVC C++23 scaffold, `sc::log` + `SC_STUB_ONCE`, suspended `loader.exe` | Done |
| Sprint 2 | Dual-port `XTaskQueue`, `XAsyncBlock` engine, offline `XUser` profile | Done |
| Sprint 3 | `d3d12_xs.dll` COM wrapper and `PresentX` -> DXGI Flip-Discard swapchain bridge | Done |
| Sprint 4 | Isolated `shims/` & `titles/` layout, in-memory PE import patcher, `audit_imports.py` | Done |
| Sprint 5 | `d3d12_xs.dll` Root Signatures + PSOs + HLSL draw pipeline, `gameinput.dll` shim | Done |
| Sprint 6 | Shared async runtime + atomic disk-backed `XGameSave` Connected Storage | Done |
| Sprint 7 | `xg_xs.dll` texture layout/swizzle shim, SRV textured pipeline, `DXBC`/`DXIL` chunk inspector | Done |
| Sprint 8 | `dstorage_xs.dll` Velocity async asset streaming + pre-swizzled `.scx` texture pipeline | Done |
| Sprint 9 | `scarlett_hook.dll` runtime `LoadLibrary` IAT hooks, `pixevt.dll`, and `--gen-stub` generator | Done |
| Sprint 10 | `xaudio2_9.dll` real-ABI `IXAudio2` proxy shim, XMA2 inspector, and live procedural audio | Done |
| Sprint 11 | `xmem.dll` aligned console memory pools, allocator tagging, and peak/leak telemetry | Done |
| Sprint 12 | Disk-backed `ID3D12PipelineLibrary` PSO cache, multi-module IAT hooking, `engine_plugin.dll` | Done |
| Sprint 13 | Virtual console paths (`G:\`, `T:\`, `S:\`) and headless CTest verification suite (`scarlett_tests`) | Done |
| Sprint 14 | `kernelx.dll` unified memory & CPU topology shim + `d3d12_xs.dll` `CreateHeap` / `CreatePlacedResource` | Done |
| Sprint 15 | SM 6.x `DXIL` LLVM bitcode inspector & pluggable `IShaderTranslator` (`SCXS` container pipeline) | Done |
| Sprint 16 | `dstorage_xs.dll` live `SCLZ` CPU decompression & `xaudio2_9.dll` live IMA-ADPCM decode voice bridge | Done |
| Sprint 17 | `d3d12_xs.dll` Compute PSOs (`CreateComputePipelineState`), UAVs, and live CS 5.0 procedural pass | Done |
| Sprint 18 | Proof-of-concept RDNA2 ISA binary decoder, HLSL lifter & `Rdna2IsaTranslator` GPU execution | Done |
| Sprint 19 | `d3d12_xs.dll` async `COMPUTE` command queues & cross-queue GPU `Wait`/`Signal` sync | Done |
| Sprint 20 | Binary RDNA2 Compute ISA lifter (`v_mul_lo_u32`, `v_add_nc_u32`, `buffer_store_dword`) on `COMPUTE` queue | Done |
| Sprint 21 | Binary RDNA2 Vertex Shader lifter (`SMEM` CBVs, `VINTRP`, `EXP pos0`/`param0`) + full VS+PS GPU pipeline | Done |
| Sprint 22 | `xg_xs.dll` RDNA2 `64KB_S` swizzling & BC1/BC3/BC7 multi-mip block compression + GPU BC1 sample | Done |

---

# Scarlett Layer â€” Engineering Invariants & Sharp Edges

> **Hand-maintained file.** `tools/build_wiki.py` never overwrites this document and embeds it verbatim into `AI_CONTEXT_SNAPSHOT.md`. Only record non-obvious architectural constraints, past failure modes, and hard rules here.

## 1. Loader, PE Rewriting & Runtime Hooking
- **Near-4GB RVA Window:** PE32+ `IMAGE_IMPORT_DESCRIPTOR::Name` and `IMAGE_DELAYLOAD_DESCRIPTOR::DllNameRVA` are 32-bit unsigned RVAs relative to `ImageBase`. Remote shim path strings MUST be committed inside `[ImageBase, ImageBase + 0xFFFFFFFF]` via `VirtualQueryEx` scanning, then hardened to `PAGE_READONLY`.
- **Clear Bound Imports:** Always zero `IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT` when rewriting static imports so `ntdll` does not short-circuit patched descriptors with stale timestamps.
- **Loader-Lock Safety (`scarlett_hook.dll`):** Inside `DllMain`, avoid C++ filesystem iterators and heavy CRT allocations; enumerate shims with raw Win32 `FindFirstFileW`.
- **Recursion & System Exclusion:** Never IAT-hook modules residing under `%SystemRoot%` or `SCARLETT_SHIM_DIR`. This guarantees `xaudio2_9.dll` can load native `System32\xaudio2_9.dll` without self-recursion.
- **Search-Flag Sanitization:** When redirecting `LoadLibraryExA/W` to an absolute shim path outside `System32`, mask off `LOAD_LIBRARY_SEARCH_*` flags (`SYSTEM32`, `APPLICATION_DIR`, `USER_DIRS`, `DEFAULT_DIRS`) to prevent `ERROR_INVALID_PARAMETER`.

## 2. Graphics, PSO Caching & Binary RDNA2 Lifting (`d3d12_xs.dll`)
- **No `D3DCOMPILE_DEBUG` on Cached Shaders:** Compiling HLSL with `D3DCOMPILE_DEBUG` embeds a `SPDB` chunk with a freshly generated GUID on every run, breaking cross-launch PSO cache determinism. Always compile runtime/lifted shaders with `D3DCOMPILE_OPTIMIZATION_LEVEL3`.
- **Post-Translation Content Hashing:** Key `ID3D12PipelineLibrary` entries (`GFX_<hash>`, `CMP_<hash>`) on **post-translation** DXBC/DXIL bytes plus the content hash of serialized root-signature blobsâ€”never raw COM pointer addresses.
- **Translator Precedence & Error Contracts:** `Rdna2IsaTranslator` MUST be registered ahead of `ScxsContainerTranslator` in `ShaderTranslatorRegistry`. Corrupted `SCXS` checksums return `E_INVALIDARG`; missing required stage outputs (`EXP pos0` for VS, `EXP mrt0` for PS, `MUBUF` store for CS) return `E_NOTIMPL`.
- **Cache-Stable HLSL Emission:** Keep lifted Pixel Shader HLSL byte-identical when no `SMEM` or `VINTRP` instructions are present so existing cached PSO keys (`GFX_F15306853EDC188B`) do not churn across refactors.
- **Cross-Queue GPU Synchronization:** Handoffs between `COMPUTE` and `DIRECT` queues must transition resources through `D3D12_RESOURCE_STATE_COMMON` paired with GPU-side `Signal`/`Wait` fences to maintain zero D3D12 Debug Layer warnings. Buffer copies after compute writes require a UAV barrier plus `UNORDERED_ACCESS -> COPY_SOURCE`.

## 3. Memory, Audio & Storage Shims
- **Unified Memory & Alignment (`xmem.dll`, `kernelx.dll`):** Route `XALLOC_MEMTYPE_PHYSICAL` and `>= 64 KB` alignments to `VirtualAlloc`; smaller heap pools to `_aligned_malloc`. Strip console-only flags (`SC_MEM_GRAPHICS = 0x10000000`) before calling host `VirtualAlloc`, clamp oversized affinity masks against host process affinity (`0xFFF` on 12-thread testbed), and maintain **0 leaks (0 B)** on unload.
- **Audio Proxy & Decode Lifetimes (`xaudio2_9.dll`):** Resolve native `xaudio2_9.dll` via `GetSystemDirectoryW` and assert `mod != g_selfModule`. `ScarlettDecodingSourceVoice` owns decoded PCM16 blocks until `OnBufferEnd` via an internal `IXAudio2VoiceCallback` proxy and translates `pContext` back to the caller's pointer in callbacks and `GetState()`.
- **Velocity Streaming (`dstorage_xs.dll`):** Translate `G:\`, `T:\`, and `S:\` VFS paths in `OpenFile`. Run `SCLZ` decompression and FNV-1a verification on the background worker thread *before* updating status slots or signaling `ID3D12Fence`.

## 4. Build, Testing & Documentation Policy
- **Toolchain & Encoding:** MSVC x64 C++23 (`/W4 /permissive-`), CMake + Ninja, UTF-8 without BOM. CTest always launches `scarlett_tests.exe` *through* `loader.exe` to verify full-stack interposition.
- **Docs Cadence:** Update `docs/PROGRESS.md` every sprint; regenerate `docs/wiki/` locally via `py tools\build_wiki.py` every 3â€“5 commits (zero AI tokens); update `docs/ARCHITECTURE_PAPER.md` strictly every 5 sprints (next scheduled revision: **Sprint 26**).

---

## 2. File Inventory
```text
  201  CMakeLists.txt
  343  src/common/codecs.hpp
   69  src/common/config.hpp
   43  src/common/stub.hpp
   76  src/common/vfs.hpp
  161  src/common/xvd_container.hpp
  267  src/loader/auto_stub.hpp
  172  src/loader/game_config.hpp
  208  src/loader/main.cpp
  451  src/loader/pe_inspector.hpp
  396  src/loader/pe_patcher.cpp
   19  src/loader/pe_patcher.hpp
  729  src/loader/scarlett_hook.cpp
  105  src/shims/acphal/acphal.cpp
    7  src/shims/acphal/acphal.def
   33  src/shims/acphal/acphal.hpp
  172  src/shims/d3d12_xs/command_list_x.cpp
  442  src/shims/d3d12_xs/command_queue_x.cpp
  104  src/shims/d3d12_xs/d3d12_xs.cpp
    4  src/shims/d3d12_xs/d3d12_xs.def
  237  src/shims/d3d12_xs/d3d12_xs.hpp
   79  src/shims/d3d12_xs/d3d12_xs_internal.hpp
   85  src/shims/d3d12_xs/d3d12_xs_utils.cpp
  664  src/shims/d3d12_xs/device_x.cpp
 1191  src/shims/d3d12_xs/rdna2_isa.hpp
  201  src/shims/d3d12_xs/shader_inspector.hpp
  401  src/shims/d3d12_xs/shader_translator.hpp
 1269  src/shims/dstorage_xs/dstorage_xs.cpp
    4  src/shims/dstorage_xs/dstorage_xs.def
  181  src/shims/dstorage_xs/dstorage_xs.hpp
  321  src/shims/gameinput/gameinput.cpp
    4  src/shims/gameinput/gameinput.def
   83  src/shims/gameinput/gameinput.hpp
  212  src/shims/kernelx/kernelx.cpp
    7  src/shims/kernelx/kernelx.def
   40  src/shims/kernelx/kernelx.hpp
  220  src/shims/libhttpclient/libhttpclient.cpp
   21  src/shims/libhttpclient/libhttpclient.def
   33  src/shims/libhttpclient/libhttpclient.hpp
   58  src/shims/pixevt/pixevt.cpp
    5  src/shims/pixevt/pixevt.def
   14  src/shims/pixevt/pixevt.hpp
  660  src/shims/xaudio2_9/xaudio2_9.cpp
    4  src/shims/xaudio2_9/xaudio2_9.def
  206  src/shims/xcurl/xcurl.cpp
   16  src/shims/xcurl/xcurl.def
   51  src/shims/xcurl/xcurl.hpp
  325  src/shims/xg_xs/xg_64kbs.hpp
  304  src/shims/xg_xs/xg_xs.cpp
    4  src/shims/xg_xs/xg_xs.def
   75  src/shims/xg_xs/xg_xs.hpp
  485  src/shims/xgameruntime/game_save.cpp
  101  src/shims/xgameruntime/gdk_types.hpp
   87  src/shims/xgameruntime/runtime_internal.hpp
  191  src/shims/xgameruntime/task_queue.cpp
   13  src/shims/xgameruntime/task_queue.hpp
  173  src/shims/xgameruntime/xgameruntime.cpp
   29  src/shims/xgameruntime/xgameruntime.def
  279  src/shims/xmem/xmem.cpp
    8  src/shims/xmem/xmem.def
   62  src/shims/xmem/xmem.hpp
 1651  src/tests/dummy_title/main.cpp
   59  src/tests/engine_plugin/engine_plugin.cpp
    3  src/tests/engine_plugin/engine_plugin.def
  328  src/tests/scarlett_3d_demo/main.cpp
  140  src/tests/scarlett_tests/main.cpp
  170  src/tests/scarlett_tests/test_harness.hpp
 1312  src/tests/scarlett_tests/tests_d3d12_core.cpp
 1658  src/tests/scarlett_tests/tests_d3d12_shaders_present.cpp
 1346  src/tests/scarlett_tests/tests_loader_e2e.cpp
  876  src/tests/scarlett_tests/tests_runtime_io.cpp
 1524  src/tests/scarlett_tests/tests_storage_swizzle.cpp
  242  tools/audit_imports.py
  505  tools/build_wiki.py
  108  tools/test_brief.py
```

## 3. Shim Exports & COM VTable Layouts

## `acphal.dll`
**Files:** `src/shims/acphal/acphal.cpp` (105 L), `src/shims/acphal/acphal.def` (7 L), `src/shims/acphal/acphal.hpp` (33 L)

**DLL Exports (`.def`):** `AcpHalCreate`, `AcpHalAllocateShapeContexts`, `AcpHalReleaseShapeContexts`, `ApuAlloc`, `ApuFree`

**C Entrypoints:**
- `HRESULT WINAPI AcpHalCreate(void** acpHal)`
- `HRESULT WINAPI AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXT_COUNTS* ctx)`
- `HRESULT WINAPI AcpHalReleaseShapeContexts(void)`
- `HRESULT WINAPI ApuAlloc(void** virtualAddress, void* physicalAddress, uint32_t sizeInBytes, uint32_t alignmentInBytes)`
- `HRESULT WINAPI ApuFree(void* virtualAddress)`

## `d3d12_xs.dll`
**Files:** `src/shims/d3d12_xs/command_list_x.cpp` (172 L), `src/shims/d3d12_xs/command_queue_x.cpp` (442 L), `src/shims/d3d12_xs/d3d12_xs.cpp` (104 L), `src/shims/d3d12_xs/d3d12_xs.def` (4 L), `src/shims/d3d12_xs/d3d12_xs.hpp` (237 L), `src/shims/d3d12_xs/d3d12_xs_internal.hpp` (79 L), `src/shims/d3d12_xs/d3d12_xs_utils.cpp` (85 L), `src/shims/d3d12_xs/device_x.cpp` (664 L), `src/shims/d3d12_xs/rdna2_isa.hpp` (1191 L), `src/shims/d3d12_xs/shader_inspector.hpp` (201 L), `src/shims/d3d12_xs/shader_translator.hpp` (401 L)

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
- `[27]` `void IASetVertexBuffers( UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews)`
- `[28]` `void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView)`
- `[29]` `void DrawIndexedInstanced( UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)`
- `[30]` `void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects)`
- `[31]` `void SetGraphicsRoot32BitConstants( UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)`
- `[32]` `void SetComputeRoot32BitConstants( UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)`
- `[33]` `void CopyTextureRegion( const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)`
- `[34]` `void InsertWaitOnPresent(UINT64 PresentToken)`
- `[35]` `ID3D12GraphicsCommandList* GetNativeCommandList()`

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

## `dstorage_xs.dll`
**Files:** `src/shims/dstorage_xs/dstorage_xs.cpp` (1269 L), `src/shims/dstorage_xs/dstorage_xs.def` (4 L), `src/shims/dstorage_xs/dstorage_xs.hpp` (181 L)

**DLL Exports (`.def`):** `DStorageGetFactory`, `DStorageSetConfiguration`

**C Entrypoints:**
- `HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv)`
- `HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration)`

**COM Interface `IDStorageFileX : IUnknown`:**
- `[03]` `void Close()`
- `[04]` `HRESULT GetFileInformation( BY_HANDLE_FILE_INFORMATION* info)`

**COM Interface `IDStorageStatusArrayX : IUnknown`:**
- `[03]` `bool IsComplete(uint32_t index)`
- `[04]` `HRESULT GetHResult(uint32_t index)`

**COM Interface `IDStorageQueueX : IUnknown`:**
- `[03]` `void EnqueueRequest(const DSTORAGE_REQUEST* request)`
- `[04]` `void EnqueueStatus( IDStorageStatusArrayX* statusArray, uint32_t index)`
- `[05]` `void EnqueueSignal( ID3D12Fence* fence, uint64_t value)`
- `[06]` `void Submit()`
- `[07]` `void RetrieveErrorRecord( DSTORAGE_ERROR_RECORD* record)`
- `[08]` `uint32_t GetPendingRequestCount()`

**COM Interface `IDStorageFactoryX : IUnknown`:**
- `[03]` `HRESULT CreateQueue( const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv)`
- `[04]` `HRESULT OpenFile( const wchar_t* path, REFIID riid, void** ppv)`
- `[05]` `HRESULT CreateStatusArray( uint32_t capacity, const char* name, REFIID riid, void** ppv)`
- `[06]` `void SetDebugFlags(uint32_t flags)`
- `[07]` `HRESULT SetStagingBufferSize(uint32_t size)`

## `gameinput.dll`
**Files:** `src/shims/gameinput/gameinput.cpp` (321 L), `src/shims/gameinput/gameinput.def` (4 L), `src/shims/gameinput/gameinput.hpp` (83 L)

**DLL Exports (`.def`):** `GameInputCreate @1`, `ScarlettGameInputSetSyntheticGamepad @2`

**C Entrypoints:**
- `HRESULT WINAPI GameInputCreate(IGameInput** gameInput)`
- `void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state)`

**COM Interface `IGameInputReading : IUnknown`:**
- `[03]` `GameInputKind GetInputKind()`
- `[04]` `uint64_t GetTimestamp()`
- `[05]` `bool GetGamepadState(GameInputGamepadState* state)`
- `[06]` `uint32_t GetKeyCount()`
- `[07]` `uint32_t GetKeyState( uint32_t stateArrayCount, GameInputKeyState* stateArray)`

**COM Interface `IGameInput : IUnknown`:**
- `[03]` `uint64_t GetCurrentTimestamp()`
- `[04]` `HRESULT GetCurrentReading( GameInputKind inputKind, void* device, IGameInputReading** reading)`

## `kernelx.dll`
**Files:** `src/shims/kernelx/kernelx.cpp` (212 L), `src/shims/kernelx/kernelx.def` (7 L), `src/shims/kernelx/kernelx.hpp` (40 L)

**DLL Exports (`.def`):** `TitleMemoryStatus`, `VirtualAllocX`, `VirtualFreeX`, `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX`

**C Entrypoints:**
- `BOOL WINAPI TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer)`
- `void* WINAPI VirtualAllocX(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect)`
- `BOOL WINAPI VirtualFreeX(void* lpAddress, size_t dwSize, DWORD dwFreeType)`
- `BOOL WINAPI GetConsoleProcessorInfo(SCARLETT_PROCESSOR_INFO* lpInfo)`
- `DWORD_PTR WINAPI SetThreadAffinityMaskX(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)`

## `libhttpclient.dll`
**Files:** `src/shims/libhttpclient/libhttpclient.cpp` (220 L), `src/shims/libhttpclient/libhttpclient.def` (21 L), `src/shims/libhttpclient/libhttpclient.hpp` (33 L)

**DLL Exports (`.def`):** `HCInitialize`, `HCCleanup`, `HCIsInitialized`, `HCGetLibVersion`, `HCHttpCallCreate`, `HCHttpCallCloseHandle`, `HCHttpCallRequestSetUrl`, `HCHttpCallRequestSetHeader`, `HCHttpCallRequestSetRequestBodyBytes`, `HCHttpCallRequestSetTimeout`, `HCHttpCallPerformAsync`, `HCHttpCallResponseGetStatusCode`, `HCHttpCallResponseGetNetworkErrorCode`, `HCHttpCallResponseGetResponseString`, `HCHttpCallResponseGetResponseBodyBytesSize`, `HCHttpCallResponseGetResponseBodyBytes`, `HCHttpCallResponseGetHeader`, `HCHttpCallRegisterMock`, `HCHttpCallClearMocks`

**C Entrypoints:**
- `HRESULT WINAPI HCInitialize(void* args)`
- `void WINAPI HCCleanup(void)`
- `uint32_t WINAPI HCIsInitialized(void)`
- `HRESULT WINAPI HCGetLibVersion(const char** version)`
- `HRESULT WINAPI HCHttpCallCreate(HCCallHandle* call)`
- `HRESULT WINAPI HCHttpCallCloseHandle(HCCallHandle call)`
- `HRESULT WINAPI HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url)`
- `HRESULT WINAPI HCHttpCallRequestSetHeader(HCCallHandle call, const char* name, const char* value, uint32_t allowTracing)`
- `HRESULT WINAPI HCHttpCallRequestSetRequestBodyBytes(HCCallHandle call, const uint8_t* bodyBytes, uint32_t bodySize)`
- `HRESULT WINAPI HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeoutInSeconds)`
- `HRESULT WINAPI HCHttpCallPerformAsync(HCCallHandle call, void* asyncBlock)`
- `HRESULT WINAPI HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* statusCode)`
- `HRESULT WINAPI HCHttpCallResponseGetNetworkErrorCode(HCCallHandle call, HRESULT* networkErrorCode, uint32_t* platformErrorCode)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseString(HCCallHandle call, const char** responseString)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* bufferSize)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytes(HCCallHandle call, size_t bufferSize, uint8_t* buffer, size_t* bufferUsed)`
- `HRESULT WINAPI HCHttpCallResponseGetHeader(HCCallHandle call, const char* headerName, const char** headerValue)`
- `HRESULT WINAPI HCHttpCallRegisterMock(const char* method, const char* urlPrefix, uint32_t statusCode, const char* responseBody)`
- `void WINAPI HCHttpCallClearMocks(void)`

## `pixevt.dll`
**Files:** `src/shims/pixevt/pixevt.cpp` (58 L), `src/shims/pixevt/pixevt.def` (5 L), `src/shims/pixevt/pixevt.hpp` (14 L)

**DLL Exports (`.def`):** `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker`

**C Entrypoints:**
- `void WINAPI PIXBeginEvent(uint64_t color, const char* name)`
- `void WINAPI PIXEndEvent()`
- `void WINAPI PIXSetMarker(uint64_t color, const char* name)`

## `xaudio2_9.dll`
**Files:** `src/shims/xaudio2_9/xaudio2_9.cpp` (660 L), `src/shims/xaudio2_9/xaudio2_9.def` (4 L)

**DLL Exports (`.def`):** `XAudio2Create = ScarlettXAudio2Create @1`, `XAudio2CreateWithVersionInfo = ScarlettXAudio2CreateWithVersionInfo`

## `xcurl.dll`
**Files:** `src/shims/xcurl/xcurl.cpp` (206 L), `src/shims/xcurl/xcurl.def` (16 L), `src/shims/xcurl/xcurl.hpp` (51 L)

**DLL Exports (`.def`):** `curl_global_init`, `curl_global_cleanup`, `curl_easy_init`, `curl_easy_cleanup`, `curl_easy_reset`, `curl_easy_setopt`, `curl_easy_perform`, `curl_easy_getinfo`, `curl_easy_strerror`, `curl_slist_append`, `curl_slist_free_all`, `curl_version`, `xcurl_register_mock_response`, `xcurl_clear_mocks`

**C Entrypoints:**
- `CURLcode curl_global_init(int64_t flags)`
- `void curl_global_cleanup(void)`
- `CURL* curl_easy_init(void)`
- `void curl_easy_cleanup(CURL* curl)`
- `void curl_easy_reset(CURL* curl)`
- `CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...)`
- `CURLcode curl_easy_perform(CURL* curl)`
- `CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...)`
- `const char* curl_easy_strerror(CURLcode code)`
- `curl_slist* curl_slist_append(curl_slist* list, const char* string)`
- `void curl_slist_free_all(curl_slist* list)`
- `const char* curl_version(void)`
- `CURLcode xcurl_register_mock_response(const char* urlPrefix, long statusCode, const char* body)`
- `void xcurl_clear_mocks(void)`

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

## `xgameruntime.dll`
**Files:** `src/shims/xgameruntime/game_save.cpp` (485 L), `src/shims/xgameruntime/gdk_types.hpp` (101 L), `src/shims/xgameruntime/runtime_internal.hpp` (87 L), `src/shims/xgameruntime/task_queue.cpp` (191 L), `src/shims/xgameruntime/task_queue.hpp` (13 L), `src/shims/xgameruntime/xgameruntime.cpp` (173 L), `src/shims/xgameruntime/xgameruntime.def` (29 L)

**DLL Exports (`.def`):** `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`, `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`, `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserGetId`, `XUserGetState`, `XUserGetLocalId`, `XUserCloseHandle`, `XGameSaveInitializeProviderAsync`, `XGameSaveInitializeProviderResult`, `XGameSaveCloseProvider`, `XGameSaveCreateContainer`, `XGameSaveCloseContainer`, `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete`, `XGameSaveSubmitUpdateAsync`, `XGameSaveSubmitUpdateResult`, `XGameSaveReadBlobDataAsync`, `XGameSaveReadBlobDataResult`

## `xmem.dll`
**Files:** `src/shims/xmem/xmem.cpp` (279 L), `src/shims/xmem/xmem.def` (8 L), `src/shims/xmem/xmem.hpp` (62 L)

**DLL Exports (`.def`):** `XMemAlloc`, `XMemFree`, `XMemSize`, `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`

**C Entrypoints:**
- `void* WINAPI XMemAlloc(size_t dwSize, uint32_t dwAllocAttributes)`
- `void WINAPI XMemFree(void* pAddress, uint32_t dwAllocAttributes)`
- `size_t WINAPI XMemSize(void* pAddress, uint32_t dwAllocAttributes)`
- `void* WINAPI XMemAllocDefault(size_t dwSize, uint32_t dwAllocAttributes)`
- `void WINAPI XMemFreeDefault(void* pAddress, uint32_t dwAllocAttributes)`
- `size_t WINAPI XMemSizeDefault(void* pAddress, uint32_t dwAllocAttributes)`


## 4. RDNA2 ISA Decoder Opcodes (`rdna2_isa.hpp`)
- `SoppOp`: kSoppNop = 0x00u, kSoppEndpgm = 0x01u, kSoppWaitcnt = 0x0Cu
- `Vop1Op`: kVop1Nop = 0x00u, kVop1MovB32 = 0x01u, kVop1RcpF32 = 0x2Au, kVop1SqrtF32 = 0x33u
- `Vop2Op`: kVop2AddF32 = 0x03u, kVop2SubF32 = 0x04u, kVop2SubRevF32 = 0x05u, kVop2MulF32 = 0x08u, kVop2MinF32 = 0x0Fu, kVop2MaxF32 = 0x10u, kVop2AddNcU32 = 0x25u, kVop2FmacF32 = 0x2Bu
- `Vop3Op`: kVop3MadF32 = 0x141u, kVop3FmaF32 = 0x14Bu, kVop3Min3F32 = 0x150u, kVop3Max3F32 = 0x151u, kVop3Med3F32 = 0x152u, kVop3MulLoU32 = 0x169u
- `MubufOp`: kMubufLoadDword = 0x14u, kMubufStoreDword = 0x1Cu
- `SmemOp`: kSmemLoadDword = 0x00u, kSmemLoadDwordX2 = 0x01u, kSmemLoadDwordX4 = 0x02u
- `VintrpOp`: kVintrpP1F32 = 0x00u, kVintrpP2F32 = 0x01u, kVintrpMovF32 = 0x02u
- `MimgOp`: kMimgImageSample = 0x20u
- `InstFormat`: Sopp, Vop1, Vop2, Vop3, Exp, Mubuf, Smem, Vintrp, Mimg

## 5. Exact CTest Registration Anchors (`src/tests/scarlett_tests/main.cpp`, 61 total)
```cpp
    {"VfsPathTranslation",                 TestVfsPathTranslation},
    {"RuntimeAndXUser",                    TestRuntimeAndXUser},
    {"XGameSaveEdgeCases",                 TestXGameSaveEdgeCases},
    {"XgXsMultiMipAndNpotSwizzle",         TestXgXsMultiMipAndNpotSwizzle},
    {"DStorageVfsAndErrorRecord",          TestDStorageVfsAndErrorRecord},
    {"XAudio2Xma2Rejection",               TestXAudio2Xma2Rejection},
    {"XMemAlignmentAndInvariants",         TestXMemAlignmentAndInvariants},
    {"DynamicAndMultiModuleHooks",         TestDynamicAndMultiModuleHooks},
    {"D3D12HeadlessDeviceAndPso",          TestD3D12HeadlessDeviceAndPso},
    {"KernelXMemoryAndTopology",           TestKernelXMemoryAndTopology},
    {"D3D12PlacedResourcesAndHeap",        TestD3D12PlacedResourcesAndHeap},
    {"DxilSm6BitcodeInspector",            TestDxilSm6BitcodeInspector},
    {"ScxsTranslatorAndPsoCache",          TestScxsTranslatorAndPsoCache},
    {"DStorageLzDecompression",            TestDStorageLzDecompression},
    {"XAudio2ImaAdpcmDecodeBridge",        TestXAudio2ImaAdpcmDecodeBridge},
    {"D3D12ComputePsoAndUavExecution",     TestD3D12ComputePsoAndUavExecution},
    {"Rdna2IsaDecoderAndGpuExecution",     TestRdna2IsaDecoderAndGpuExecution},
    {"D3D12AsyncComputeCrossQueueSync",    TestD3D12AsyncComputeCrossQueueSync},
    {"Rdna2ComputeIsaExecution",           TestRdna2ComputeIsaExecution},
    {"Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv},
    {"XgXs64KbsAndBcnBlockSwizzle",        TestXgXs64KbsAndBcnBlockSwizzle},
    {"DStorageGpuBufferStreaming",         TestDStorageGpuBufferStreaming},
    {"DStorageGpuTextureStreaming",        TestDStorageGpuTextureStreaming},
    {"DStorageDeviceXBridgeAndMipChain",   TestDStorageDeviceXBridgeAndMipChain},
    {"RetailBringUpShims",                 TestRetailBringUpShims},
    {"LoaderInspectAndXvdMount",           TestLoaderInspectAndXvdMount},
    {"Rdna2ImageSampleShader",             TestRdna2ImageSampleShader},
    {"PresentXDescriptorAudit",            TestPresentXDescriptorAudit},
    {"ScarlettXConformanceSuite",          TestScarlettXConformanceSuite},
    {"GameConfigBootParser",               TestGameConfigBootParser},
    {"Rdna2TexturedDrawE2E",               TestRdna2TexturedDrawE2E},
    {"GameInputLivePolling",               TestGameInputLivePolling},
    {"DynamicLoaderHooks",                 TestDynamicLoaderHooks},
    {"DStorageGpuBufferAndFenceE2E",       TestDStorageGpuBufferAndFenceE2E},
    {"Rdna2ComputeDispatchE2E",            TestRdna2ComputeDispatchE2E},
    {"ShaderDiskCacheE2E",                 TestShaderDiskCacheE2E},
    {"D3D12XsCommandListWrapperE2E",       TestD3D12XsCommandListWrapperE2E},
    {"D3D12XsCmdListRenderClearE2E",       TestD3D12XsCmdListRenderClearE2E},
    {"D3D12XsCmdListComputeDispatchE2E",   TestD3D12XsCmdListComputeDispatchE2E},
    {"XAudio2RuntimePlaybackE2E",          TestXAudio2RuntimePlaybackE2E},
    {"NetworkingShimsE2E",                 TestNetworkingShimsE2E},
    {"XUserAndGameSaveProfileE2E",         TestXUserAndGameSaveProfileE2E},
    {"GameInputRuntimePollingE2E",         TestGameInputRuntimePollingE2E},
    {"ScarlettTitleBringUpE2E",            TestScarlettTitleBringUpE2E},
    {"KernelXAndPixEvtE2E",                TestKernelXAndPixEvtE2E},
    {"XMemAndXgXsSwizzleE2E",              TestXMemAndXgXsSwizzleE2E},
    {"DStorageVelocityDynamicE2E",         TestDStorageVelocityDynamicE2E},
    {"LoaderXvdAndGameConfigE2E",          TestLoaderXvdAndGameConfigE2E},
    {"ScarlettFullStack50SprintCapstoneE2E", TestScarlettFullStack50SprintCapstoneE2E},
    {"PeDelayLoadAndOrdinalAuditE2E",      TestPeDelayLoadAndOrdinalAuditE2E},
    {"RuntimeDelayLoadAndOrdinalHookE2E",  TestRuntimeDelayLoadAndOrdinalHookE2E},
    {"D3D12Xs3DPerspectiveDepthRenderE2E", TestD3D12Xs3DPerspectiveDepthRenderE2E},
    {"D3D12XsComVtableAbiConformanceE2E",  TestD3D12XsComVtableAbiConformanceE2E},
    {"D3D12XsPresentXSwapChainE2E",        TestD3D12XsPresentXSwapChainE2E},
    {"D3D12XsFramePacingTokensE2E",        TestD3D12XsFramePacingTokensE2E},
    {"D3D12XsHeadlessPresentXModeE2E",     TestD3D12XsHeadlessPresentXModeE2E},
    {"D3D12XsAsyncComputeCrossQueueSyncE2E", TestD3D12XsAsyncComputeCrossQueueSyncE2E},
    {"XMemMin16ByteAlignment",             TestXMemMin16ByteAlignment},
    {"Pe64ImplicitTlsAndGuestAtExitE2E",   TestPe64ImplicitTlsAndGuestAtExitE2E},
    {"AutoStubThunkSurvivalE2E",           TestAutoStubThunkSurvivalE2E},
    {"LoaderSymbolCoverageAuditE2E",       TestLoaderSymbolCoverageAuditE2E},
```

## 6. Recent Git History
- `b03456b Merge Sprint 63: PE64 implicit TLS & XMem 16-byte alignment (59/59 CTests)`
- `86d7633 Sprint 63: PE64 implicit TLS, isolated guest atexit, and XMem 16-byte alignment (59/59 CTests)`
- `56bfe0e Merge docs refresh: Sprint 62 README, Sprints 63-72 roadmap, wiki test parser`
- `ed5696e Docs: Add 10-sprint roadmap (Sprints 63-72), refresh README for sprint-62-baseline, fix build_wiki kTests parser`
- `6b866ec Merge Sprint 62: COM/RAII hygiene & centralized config (57/57 CTests)`
- `ef51508 Sprint 62: Centralized sc::config, MakeCom RAII factories, and /W4 hygiene`
- `f8f6eeb Merge Sprint 61: Modularize scarlett_tests suite (57/57 CTests)`
- `199d672 Sprint 61: Modularize scarlett_tests into subsystem suites + CLI filtering`
- `d5f5fb0 Merge Sprint 60: Modularize d3d12_xs shim (57/57 CTests)`
- `6e5f1ef Sprint 60: Decompose d3d12_xs.cpp monolith into modular translation units`
