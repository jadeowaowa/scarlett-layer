# Scarlett Layer â€” AI Context Snapshot (Slim)

**Commit:** `116f183` (#23) | **Generated:** 2026-11-20 08:57 UTC  
**Totals:** 48 files (12,987 LOC) | 9 shims | 20 CTest checks

---

## 1. Sprint Status (`docs/PROGRESS.md`)
| Sprint | Focus | Status |
| :--- | :--- | :--- |
| Sprint 17 | `d3d12_xs.dll` Compute PSOs (`CreateComputePipelineState`), UAVs, and live CS 5.0 procedural pass | Done |
| Sprint 18 | Proof-of-concept RDNA2 ISA binary decoder, HLSL lifter & `Rdna2IsaTranslator` GPU execution | Done |
| Sprint 19 | `d3d12_xs.dll` async `COMPUTE` command queues & cross-queue GPU `Wait`/`Signal` sync | Done |
| Sprint 20 | Binary RDNA2 Compute ISA lifter (`v_mul_lo_u32`, `v_add_nc_u32`, `buffer_store_dword`) on `COMPUTE` queue | Done |
| Sprint 21 | Binary RDNA2 Vertex Shader lifter (`SMEM` CBVs, `VINTRP`, `EXP pos0`/`param0`) + full VS+PS GPU pipeline | Done |

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
`CMakeLists.txt`, `src/common/codecs.hpp`, `src/common/stub.hpp`, `src/common/vfs.hpp`, `src/loader/main.cpp`, `src/loader/pe_patcher.cpp`, `src/loader/pe_patcher.hpp`, `src/loader/scarlett_hook.cpp`, `src/shims/d3d12_xs/d3d12_xs.cpp`, `src/shims/d3d12_xs/d3d12_xs.def`, `src/shims/d3d12_xs/d3d12_xs.hpp`, `src/shims/d3d12_xs/rdna2_isa.hpp`, `src/shims/d3d12_xs/shader_inspector.hpp`, `src/shims/d3d12_xs/shader_translator.hpp`, `src/shims/dstorage_xs/dstorage_xs.cpp`, `src/shims/dstorage_xs/dstorage_xs.def`, `src/shims/dstorage_xs/dstorage_xs.hpp`, `src/shims/gameinput/gameinput.cpp`, `src/shims/gameinput/gameinput.def`, `src/shims/gameinput/gameinput.hpp`, `src/shims/kernelx/kernelx.cpp`, `src/shims/kernelx/kernelx.def`, `src/shims/kernelx/kernelx.hpp`, `src/shims/pixevt/pixevt.cpp`, `src/shims/pixevt/pixevt.def`, `src/shims/pixevt/pixevt.hpp`, `src/shims/xaudio2_9/xaudio2_9.cpp`, `src/shims/xaudio2_9/xaudio2_9.def`, `src/shims/xg_xs/xg_xs.cpp`, `src/shims/xg_xs/xg_xs.def`, `src/shims/xg_xs/xg_xs.hpp`, `src/shims/xgameruntime/game_save.cpp`, `src/shims/xgameruntime/gdk_types.hpp`, `src/shims/xgameruntime/runtime_internal.hpp`, `src/shims/xgameruntime/task_queue.cpp`, `src/shims/xgameruntime/task_queue.hpp`, `src/shims/xgameruntime/xgameruntime.cpp`, `src/shims/xgameruntime/xgameruntime.def`, `src/shims/xmem/xmem.cpp`, `src/shims/xmem/xmem.def`, `src/shims/xmem/xmem.hpp`, `src/tests/dummy_title/main.cpp`, `src/tests/engine_plugin/engine_plugin.cpp`, `src/tests/engine_plugin/engine_plugin.def`, `src/tests/scarlett_tests/main.cpp`, `tools/audit_imports.py`, `tools/build_wiki.py`, `tools/test_brief.py`

## 3. Shim Exports & COM VTable Layouts

- **`d3d12_xs.dll`**
  - Exports: `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature = ScarlettSerializeRootSignature`
  - `ID3D12CommandQueueX:IUnknown`: [03] ExecuteCommandLists, [04] Signal, [05] Wait, [06] PresentX
  - `ID3D12DeviceX:IUnknown`: [03] CreateCommandQueueX, [04] CreateCommandAllocator, [05] CreateCommandList, [06] CreateDescriptorHeap, [07] GetDescriptorHandleIncrementSize, [08] CreateCommittedResource, [09] GetResourceAllocationInfo, [10] CreateHeap, [11] CreatePlacedResource, [12] CreateRenderTargetView, [13] CreateShaderResourceView, [14] CreateUnorderedAccessView, [15] CreateFence, [16] CreateRootSignature, [17] CreateGraphicsPipelineState, [18] CreateComputePipelineState, [19] SetFrameIntervalX, [20] ScheduleFrameEventX, [21] WaitFrameEventX
- **`dstorage_xs.dll`**
  - Exports: `DStorageGetFactory`, `DStorageSetConfiguration`
  - `IDStorageFileX:IUnknown`: [03] Close, [04] GetFileInformation
  - `IDStorageStatusArrayX:IUnknown`: [03] IsComplete, [04] GetHResult
  - `IDStorageQueueX:IUnknown`: [03] EnqueueRequest, [04] EnqueueStatus, [05] EnqueueSignal, [06] Submit, [07] RetrieveErrorRecord, [08] GetPendingRequestCount
  - `IDStorageFactoryX:IUnknown`: [03] CreateQueue, [04] OpenFile, [05] CreateStatusArray, [06] SetDebugFlags, [07] SetStagingBufferSize
- **`gameinput.dll`**
  - Exports: `GameInputCreate`
  - `IGameInputReading:IUnknown`: [03] GetInputKind, [04] GetTimestamp, [05] GetGamepadState, [06] GetKeyCount, [07] GetKeyState
  - `IGameInput:IUnknown`: [03] GetCurrentTimestamp, [04] GetCurrentReading
- **`kernelx.dll`**
  - Exports: `TitleMemoryStatus`, `VirtualAllocX`, `VirtualFreeX`, `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX`
- **`pixevt.dll`**
  - Exports: `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker`
- **`xaudio2_9.dll`**
  - Exports: `XAudio2Create = ScarlettXAudio2Create`
- **`xg_xs.dll`**
  - Exports: `XGCreateTexture2DComputer`, `XGComputeTexture2DLayout`
  - `IXGTextureAddressComputer:IUnknown`: [03] GetResourceLayout, [04] CopyIntoSubresource, [05] CopyFromSubresource
- **`xgameruntime.dll`**
  - Exports: `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`, `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`, `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserCloseHandle`, `XGameSaveInitializeProviderAsync`, `XGameSaveInitializeProviderResult`, `XGameSaveCloseProvider`, `XGameSaveCreateContainer`, `XGameSaveCloseContainer`, `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete`, `XGameSaveSubmitUpdateAsync`, `XGameSaveSubmitUpdateResult`, `XGameSaveReadBlobDataAsync`, `XGameSaveReadBlobDataResult`
- **`xmem.dll`**
  - Exports: `XMemAlloc`, `XMemFree`, `XMemSize`, `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`

## 4. RDNA2 ISA Decoder Opcodes (`rdna2_isa.hpp`)
- `SoppOp`: kSoppNop, kSoppEndpgm, kSoppWaitcnt
- `Vop1Op`: kVop1Nop, kVop1MovB32, kVop1RcpF32, kVop1SqrtF32
- `Vop2Op`: kVop2AddF32, kVop2SubF32, kVop2SubRevF32, kVop2MulF32, kVop2MinF32, kVop2MaxF32, kVop2AddNcU32, kVop2FmacF32
- `Vop3Op`: kVop3MadF32, kVop3FmaF32, kVop3Min3F32, kVop3Max3F32, kVop3Med3F32, kVop3MulLoU32
- `MubufOp`: kMubufStoreDword
- `SmemOp`: kSmemLoadDword, kSmemLoadDwordX2, kSmemLoadDwordX4
- `VintrpOp`: kVintrpP1F32, kVintrpP2F32, kVintrpMovF32
- `InstFormat`: Sopp, Vop1, Vop2, Vop3, Exp, Mubuf, Smem, Vintrp

## 5. Exact CTest Registration Anchors (`src/tests/scarlett_tests/main.cpp`, 20 total)
```cpp
    // ... (17 earlier tests omitted)
    RunTest("D3D12AsyncComputeCrossQueueSync", TestD3D12AsyncComputeCrossQueueSync);
    RunTest("Rdna2ComputeIsaExecution", TestRdna2ComputeIsaExecution);
    RunTest("Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv);
```

## 6. Recent Git History
- `116f183 Tools: add zero-dependency wiki generator (build_wiki.py), INVARIANTS.md, and AI context snapshot`
- `a72f224 Docs: Sprint 21 technical architecture paper revision (Sprints 17-21)`
- `5eff0aa Sprint 21: binary RDNA2 VS lifter (SMEM CBVs, VINTRP, EXP pos0/param0) and full VS+PS pipeline`
- `62d8374 Sprint 20: binary RDNA2 compute shader lifter (MUBUF + integer ALU) on async COMPUTE queue`
- `d878d33 Sprint 19: d3d12_xs async COMPUTE command queues and cross-queue GPU sync`
