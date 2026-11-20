# Scarlett Layer â€” AI Context Snapshot

**Commit:** `2c95bab` (#27) | **Generated:** 2026-11-20 19:41 UTC  
**Totals:** 49 files (14,379 LOC) | 9 shims | 23 CTest checks

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
  155  CMakeLists.txt
  343  src/common/codecs.hpp
   43  src/common/stub.hpp
   76  src/common/vfs.hpp
  128  src/loader/main.cpp
  396  src/loader/pe_patcher.cpp
   19  src/loader/pe_patcher.hpp
  401  src/loader/scarlett_hook.cpp
 1120  src/shims/d3d12_xs/d3d12_xs.cpp
    4  src/shims/d3d12_xs/d3d12_xs.def
  163  src/shims/d3d12_xs/d3d12_xs.hpp
 1085  src/shims/d3d12_xs/rdna2_isa.hpp
  201  src/shims/d3d12_xs/shader_inspector.hpp
  364  src/shims/d3d12_xs/shader_translator.hpp
 1104  src/shims/dstorage_xs/dstorage_xs.cpp
    4  src/shims/dstorage_xs/dstorage_xs.def
  174  src/shims/dstorage_xs/dstorage_xs.hpp
  277  src/shims/gameinput/gameinput.cpp
    3  src/shims/gameinput/gameinput.def
   82  src/shims/gameinput/gameinput.hpp
  212  src/shims/kernelx/kernelx.cpp
    7  src/shims/kernelx/kernelx.def
   40  src/shims/kernelx/kernelx.hpp
   58  src/shims/pixevt/pixevt.cpp
    5  src/shims/pixevt/pixevt.def
   14  src/shims/pixevt/pixevt.hpp
  652  src/shims/xaudio2_9/xaudio2_9.cpp
    3  src/shims/xaudio2_9/xaudio2_9.def
  325  src/shims/xg_xs/xg_64kbs.hpp
  304  src/shims/xg_xs/xg_xs.cpp
    4  src/shims/xg_xs/xg_xs.def
   75  src/shims/xg_xs/xg_xs.hpp
  489  src/shims/xgameruntime/game_save.cpp
   91  src/shims/xgameruntime/gdk_types.hpp
   87  src/shims/xgameruntime/runtime_internal.hpp
  191  src/shims/xgameruntime/task_queue.cpp
   13  src/shims/xgameruntime/task_queue.hpp
  151  src/shims/xgameruntime/xgameruntime.cpp
   26  src/shims/xgameruntime/xgameruntime.def
  277  src/shims/xmem/xmem.cpp
    8  src/shims/xmem/xmem.def
   62  src/shims/xmem/xmem.hpp
 1651  src/tests/dummy_title/main.cpp
   59  src/tests/engine_plugin/engine_plugin.cpp
    3  src/tests/engine_plugin/engine_plugin.def
 2575  src/tests/scarlett_tests/main.cpp
  242  tools/audit_imports.py
  505  tools/build_wiki.py
  108  tools/test_brief.py
```

## 3. Shim Exports & COM VTable Layouts

## `d3d12_xs.dll`
**Files:** `src/shims/d3d12_xs/d3d12_xs.cpp` (1120 L), `src/shims/d3d12_xs/d3d12_xs.def` (4 L), `src/shims/d3d12_xs/d3d12_xs.hpp` (163 L), `src/shims/d3d12_xs/rdna2_isa.hpp` (1085 L), `src/shims/d3d12_xs/shader_inspector.hpp` (201 L), `src/shims/d3d12_xs/shader_translator.hpp` (364 L)

**DLL Exports (`.def`):** `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature = ScarlettSerializeRootSignature`

**C Entrypoints:**
- `HRESULT WINAPI D3D12XboxCreateDevice( void* pAdapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters, REFIID riid, void** ppDevice)`

**COM Interface `ID3D12CommandQueueX : IUnknown`:**
- `[03]` `void ExecuteCommandLists( UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)`
- `[04]` `HRESULT Signal(ID3D12Fence* pFence, UINT64 Value)`
- `[05]` `HRESULT Wait(ID3D12Fence* pFence, UINT64 Value)`
- `[06]` `HRESULT PresentX( UINT PlaneCount, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters, const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters)`

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
**Files:** `src/shims/dstorage_xs/dstorage_xs.cpp` (1104 L), `src/shims/dstorage_xs/dstorage_xs.def` (4 L), `src/shims/dstorage_xs/dstorage_xs.hpp` (174 L)

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
**Files:** `src/shims/gameinput/gameinput.cpp` (277 L), `src/shims/gameinput/gameinput.def` (3 L), `src/shims/gameinput/gameinput.hpp` (82 L)

**DLL Exports (`.def`):** `GameInputCreate`

**C Entrypoints:**
- `HRESULT WINAPI GameInputCreate(IGameInput** gameInput)`

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

## `pixevt.dll`
**Files:** `src/shims/pixevt/pixevt.cpp` (58 L), `src/shims/pixevt/pixevt.def` (5 L), `src/shims/pixevt/pixevt.hpp` (14 L)

**DLL Exports (`.def`):** `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker`

**C Entrypoints:**
- `void WINAPI PIXBeginEvent(uint64_t color, const char* name)`
- `void WINAPI PIXEndEvent()`
- `void WINAPI PIXSetMarker(uint64_t color, const char* name)`

## `xaudio2_9.dll`
**Files:** `src/shims/xaudio2_9/xaudio2_9.cpp` (652 L), `src/shims/xaudio2_9/xaudio2_9.def` (3 L)

**DLL Exports (`.def`):** `XAudio2Create = ScarlettXAudio2Create`

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
**Files:** `src/shims/xgameruntime/game_save.cpp` (489 L), `src/shims/xgameruntime/gdk_types.hpp` (91 L), `src/shims/xgameruntime/runtime_internal.hpp` (87 L), `src/shims/xgameruntime/task_queue.cpp` (191 L), `src/shims/xgameruntime/task_queue.hpp` (13 L), `src/shims/xgameruntime/xgameruntime.cpp` (151 L), `src/shims/xgameruntime/xgameruntime.def` (26 L)

**DLL Exports (`.def`):** `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`, `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`, `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserCloseHandle`, `XGameSaveInitializeProviderAsync`, `XGameSaveInitializeProviderResult`, `XGameSaveCloseProvider`, `XGameSaveCreateContainer`, `XGameSaveCloseContainer`, `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete`, `XGameSaveSubmitUpdateAsync`, `XGameSaveSubmitUpdateResult`, `XGameSaveReadBlobDataAsync`, `XGameSaveReadBlobDataResult`

## `xmem.dll`
**Files:** `src/shims/xmem/xmem.cpp` (277 L), `src/shims/xmem/xmem.def` (8 L), `src/shims/xmem/xmem.hpp` (62 L)

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
- `MubufOp`: kMubufStoreDword = 0x1Cu
- `SmemOp`: kSmemLoadDword = 0x00u, kSmemLoadDwordX2 = 0x01u, kSmemLoadDwordX4 = 0x02u
- `VintrpOp`: kVintrpP1F32 = 0x00u, kVintrpP2F32 = 0x01u, kVintrpMovF32 = 0x02u
- `InstFormat`: Sopp, Vop1, Vop2, Vop3, Exp, Mubuf, Smem, Vintrp

## 5. Exact CTest Registration Anchors (`src/tests/scarlett_tests/main.cpp`, 23 total)
```cpp
    RunTest("VfsPathTranslation",         TestVfsPathTranslation);
    RunTest("RuntimeAndXUser",            TestRuntimeAndXUser);
    RunTest("XGameSaveEdgeCases",         TestXGameSaveEdgeCases);
    RunTest("XgXsMultiMipAndNpotSwizzle", TestXgXsMultiMipAndNpotSwizzle);
    RunTest("DStorageVfsAndErrorRecord",  TestDStorageVfsAndErrorRecord);
    RunTest("XAudio2Xma2Rejection",       TestXAudio2Xma2Rejection);
    RunTest("XMemAlignmentAndInvariants", TestXMemAlignmentAndInvariants);
    RunTest("DynamicAndMultiModuleHooks", TestDynamicAndMultiModuleHooks);
    RunTest("D3D12HeadlessDeviceAndPso",  TestD3D12HeadlessDeviceAndPso);
    RunTest("KernelXMemoryAndTopology",   TestKernelXMemoryAndTopology);
    RunTest("D3D12PlacedResourcesAndHeap", TestD3D12PlacedResourcesAndHeap);
    RunTest("DxilSm6BitcodeInspector",    TestDxilSm6BitcodeInspector);
    RunTest("ScxsTranslatorAndPsoCache",  TestScxsTranslatorAndPsoCache);
    RunTest("DStorageLzDecompression",    TestDStorageLzDecompression);
    RunTest("XAudio2ImaAdpcmDecodeBridge", TestXAudio2ImaAdpcmDecodeBridge);
    RunTest("D3D12ComputePsoAndUavExecution", TestD3D12ComputePsoAndUavExecution);
    RunTest("Rdna2IsaDecoderAndGpuExecution", TestRdna2IsaDecoderAndGpuExecution);
    RunTest("D3D12AsyncComputeCrossQueueSync", TestD3D12AsyncComputeCrossQueueSync);
    RunTest("Rdna2ComputeIsaExecution", TestRdna2ComputeIsaExecution);
    RunTest("Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv);
    RunTest("XgXs64KbsAndBcnBlockSwizzle",       TestXgXs64KbsAndBcnBlockSwizzle);
    RunTest("DStorageGpuBufferStreaming",        TestDStorageGpuBufferStreaming);
    RunTest("DStorageGpuTextureStreaming",       TestDStorageGpuTextureStreaming);
```

## 6. Recent Git History
- `2c95bab Sprint 23: DirectStorage direct-to-VRAM GPU buffer streaming + CTest #22`
- `9ab0d99 Tools: add directory-safe build.bat wrapper for Hermes Desktop`
- `c4b8cc0 Sprint 22: xg_xs RDNA2 64KB_S swizzling and BC1/BC3/BC7 block compression`
- `f8fcebf Tools: add test_brief.py and --slim mode for AI_CONTEXT_SNAPSHOT.md`
- `116f183 Tools: add zero-dependency wiki generator (build_wiki.py), INVARIANTS.md, and AI context snapshot`
- `a72f224 Docs: Sprint 21 technical architecture paper revision (Sprints 17-21)`
- `5eff0aa Sprint 21: binary RDNA2 VS lifter (SMEM CBVs, VINTRP, EXP pos0/param0) and full VS+PS pipeline`
- `62d8374 Sprint 20: binary RDNA2 compute shader lifter (MUBUF + integer ALU) on async COMPUTE queue`
- `d878d33 Sprint 19: d3d12_xs async COMPUTE command queues and cross-queue GPU sync`
- `f45bc16 Sprint 18: RDNA2 ISA binary decoder, HLSL lifter, and live GPU execution`
