# Scarlett Layer: Independent Host-Based ABI Interposition, Binary RDNA2 Shader Lifting, and Asynchronous GPU/I/O Virtualization for Xbox Series X Binaries on Windows x64

**Project Version:** Sprint 21 Baseline (9 Shims, 20 Automated Verification Tests)  
**Target Host Architecture:** Windows 11 x64 (MSVC C++23, Direct3D 12, DXGI 1.6, XAudio2 2.9)  
**Status:** Archived Sprint 21 architecture snapshot. For current Sprint 66 implementation status (`67/67` individual CTests across 12 shims), see [`docs/PROGRESS.md`](PROGRESS.md) and [`docs/SPEC_SOURCES.md`](SPEC_SOURCES.md).

---

## Abstract

Console compatibility layers on desktop operating systems occupy an architectural middle ground between full-system hardware virtualization and user-space binary translation. Because ninth-generation Xbox Series X (*Scarlett* / *Anaconda*) titles compile to standard x86-64 PE32+ executables targeting a specialized Game Core / Direct3D 12.x Windows NT derivative, CPU instruction-set emulation is unnecessary. Instead, compatibility hinges on four core systems challenges: **loader-time and runtime ABI interposition**, **graphics pipeline, async-queue, and RDNA2 ISA shader translation**, **unified memory and surface-tiling emulation**, and **asynchronous storage and compressed-audio bridging**.

This monograph documents the design, formal invariants, and empirical evaluation of **Scarlett Layer**, a independent host-based translation layer developed across 21 incremental sprints. Scarlett Layer redirects unmodified x64 executables without mutating title directories or host `System32` binaries by pairing suspended-process PE import rewriting with pre-entrypoint Asynchronous Procedure Call (APC) injection and recursive multi-module Import Address Table (IAT) hooking. Across nine isolated dynamic-link shims (`xgameruntime`, `d3d12_xs`, `gameinput`, `xg_xs`, `dstorage_xs`, `pixevt`, `xaudio2_9`, `xmem`, and `kernelx`), we implement swapchain-less `PresentX` bridging, explicit `ID3D12Heap` placed resources, dedicated async `COMPUTE` queues with GPU-side cross-queue fence synchronization (`Signal`/`Wait`), persistent disk-backed Graphics and Compute PSO caching (`ID3D12PipelineLibrary`), a independent AMD RDNA2 (GFX10.3) binary decoder and HLSL/DXBC lifter covering **Vertex, Pixel, and Compute** stages (`SOPP`, `VOP1`, `VOP2`, `VOP3`, `SMEM`, `VINTRP`, `MUBUF`, and `EXP`), 8x8 Morton micro-tiled surface translation, worker-threaded `SCLZ` DirectStorage decompression, and a real-time 4:1 IMA-ADPCM voice-decoding bridge. Evaluated via a 20-check automated headless CTest suite (`2.59 s` wall time) and an interactive synthetic title executing on an NVIDIA GeForce RTX 5070, the stack exhibits zero D3D12 validation errors, zero audio glitches, and zero memory leaks across both virtual and heap domains.

---

## 1. Introduction and Scope of Compatibility

### 1.1 Architectural Taxonomy: Emulation vs. Host-Based Translation
Classic console emulators targeting non-x86 ISAs (PowerPC, Cell, MIPS) must dynamically recompile CPU machine code while modeling bus timings and MMIO peripherals. Xbox Series X titles execute native x86-64 instructions atop a Windows NT kernel derivative (`kernelx`) and a console-extended Direct3D 12 runtime (`d3d12_xs`). Scarlett Layer therefore implements a **user-space host-based translation architecture**:

1. **Native CPU Execution:** Title x86-64 threads run directly on host cores with zero binary CPU translation overhead.
2. **Process-Isolated ABI Interposition:** Console-exclusive DLLs and colliding system libraries (e.g., `xaudio2_9.dll`) are redirected strictly inside the target process address space without global hooks or file-system pollution.
3. **Independent Semantic Bridging:** Console-specific subsystems—swapchain-less frame pacing (`PresentX`), 13.5 GiB unified memory accounting, tiled surface addressing, Velocity async I/O queues, compressed audio voices, and precompiled RDNA2 shader containers—map onto desktop Win32, Direct3D 12, DXGI, XInput, and native `System32\xaudio2_9.dll`.

### 1.2 Explicit Non-Goals and Boundaries
* **Package Decryption (XVD/XVC):** Retail Series X images are encrypted at rest by hardware-backed hypervisor keys. Scarlett Layer operates solely on unpacked, decrypted x64 PE32+ binaries and implements no DRM circumvention.
* **Independent Contracts:** Proprietary console COM interfaces (`ID3D12DeviceX`, `IXGTextureAddressComputer`, `IDStorageFactoryX`) and binary shader containers (`SCXS`) are modeled as independent interfaces to verify full-stack architecture prior to exact binary vtable-slot and GUID conformance.
* **Host Platform Requirements:** Empirical benchmarks target Windows 10/11 x64 with a Direct3D 12 Feature Level 12_0+ discrete GPU.

---

## 2. Process Bootstrap, Binary Rewriting, and Multi-Module Interposition

Windows x64 import resolution splits across two phases: static imports resolved by `ntdll!LdrInitializeThunk` prior to the entry point, and runtime dynamic loads via `LoadLibrary{A,W}` / `LoadLibraryEx{A,W}` across both the main executable and third-party middleware DLLs. Scarlett Layer intercepts both via a deterministic two-stage loader protocol:

    [loader.exe]
         |
         +--> 1. Configure VFS env (G:\, T:\, S:\) & SCARLETT_SHIM_DIR
         +--> 2. CreateProcessW(..., CREATE_SUSPENDED)
         +--> 3. NtQueryInformationProcess -> PEB+0x10 -> Remote ImageBase
         +--> 4. Patch Static & Delay-Load PE Import Descriptors (near-4GB RVA blob)
         +--> 5. Zero IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT
         +--> 6. QueueUserAPC(kernel32!LoadLibraryW, hThread, remotePath("scarlett_hook.dll"))
         +--> 7. ResumeThread(hThread)
                    |
                    v
         [ntdll!LdrInitializeThunk]
           +-- Resolves rewritten static imports straight to build/bin/shims/*.dll
           +-- Executes NtTestAlert -> fires APC -> loads scarlett_hook.dll
           +-- scarlett_hook!DllMain hooks LoadLibrary* IAT slots in .exe & loaded sub-DLLs
           +-- Transfers control to title entry point (main/WinMain)

### 2.1 Near-4GB Remote Import Rewriting (`pe_patcher.cpp`)
Because PE32+ `IMAGE_IMPORT_DESCRIPTOR::Name` and `IMAGE_DELAYLOAD_DESCRIPTOR::DllNameRVA` are 32-bit unsigned RVAs relative to `ImageBase`, redirected shim path strings injected into the suspended child process must lie within `[ImageBase, ImageBase + 0xFFFFFFFF]`. `loader.exe` extracts `ImageBase` from `PEB + 0x10` via `NtQueryInformationProcess`, locates free virtual memory immediately above `ImageBase + SizeOfImage` via `VirtualQueryEx`, commits a 64 KB-aligned string table, rewrites matching descriptor RVAs under transient `PAGE_READWRITE` protection, clears `IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT`, and sets the table to `PAGE_READONLY`.

### 2.2 Pre-Entrypoint APC Injection and Multi-Module Hooking (`scarlett_hook.cpp`)
To capture dynamic `LoadLibrary*` calls—including SDK inline shims (`<xaudio2.h>` invoking `LoadLibraryExA("xaudio2_9.dll", ...)`) and middleware plugins (`engine_plugin.dll`)—`loader.exe` queues `kernel32!LoadLibraryW` via `QueueUserAPC` onto the suspended main thread. `NtTestAlert` executes the APC inside `LdrInitializeThunk` before title code begins. Inside `DllMain`, `scarlett_hook.dll` uses loader-lock-safe Win32 `FindFirstFileW` to index all shims and patches `LoadLibrary{A,W,ExA,ExW}` IAT entries across the executable and all non-system, non-shim modules. Subsequent dynamic loads of non-shim DLLs recursively receive IAT patches before their `HMODULE` returns to the caller.

Two invariants prevent re-entrancy and loader faults:
1. **Boundary Exclusion:** Modules under `%SystemRoot%` and `SCARLETT_SHIM_DIR` are skipped, preventing `xaudio2_9.dll` from recursively intercepting its own load of `System32\xaudio2_9.dll`.
2. **Search-Flag Masking:** Restrictive `LOAD_LIBRARY_SEARCH_*` flags are cleared when rewriting `LoadLibraryEx*` calls to absolute shim paths outside `System32`.

### 2.3 Virtual File System (VFS) Drive Mapping (`common/vfs.hpp`)
`loader.exe` establishes isolated logical console drives via environment variables translated by `sc::vfs::TranslatePath`:
* **`G:\`** (Game package root) -> `SCARLETT_GAME_ROOT`
* **`T:\`** (Title scratch storage) -> `SCARLETT_SCRATCH_DIR` (`build/bin/vfs/scratch/`)
* **`S:\`** (Save temporary workspace) -> `SCARLETT_SAVE_TEMP_DIR` (`build/bin/vfs/save_temp/`)

---

## 3. Runtime Services, Concurrency, and Unified Memory Semantics

### 3.1 Dual-Port Asynchronous Execution & Crash-Safe Storage (`xgameruntime.dll`)
`XTaskQueue` separates background execution (**Work Port**, thread-pool workers) from deterministic main-thread callback dispatch (**Completion Port**, drained via `XTaskQueueDispatch`). Reference-counted `AsyncState` blocks stored inside `XAsyncBlock::internal` eliminate lifetime races during concurrent polling or buffer-size retries. `XGameSave` persists per-SCID containers to `build/bin/saves/` using temporary-file writes committed via `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` and preserves unread `AsyncState` handles on `E_GS_PROVIDED_BUFFER_TOO_SMALL`.

### 3.2 Dual-Tier Console Memory & Kernel Topology (`xmem.dll`, `kernelx.dll`)
Xbox Series X provides 13.5 GiB (`14,495,514,624 B`) of unified GDDR6 memory and 7 title cores (14 SMT threads).
* **`xmem.dll`:** Decodes packed `dwAllocAttributes` (`AllocatorId`, power-of-two `Alignment` up to 64 KB, `MemType`, `ZeroInit`), backing `XALLOC_MEMTYPE_PHYSICAL` and 64 KB-aligned allocations with `VirtualAlloc` and smaller pools with `_aligned_malloc`. A lock-guarded ledger supports `XMemSize` and emits per-pool peak/leak diagnostics on unload.
* **`kernelx.dll`:** Implements `TitleMemoryStatus` against the 13.5 GiB budget, `VirtualAllocX` / `VirtualFreeX` (stripping console flags such as `SC_MEM_GRAPHICS = 0x10000000` while accounting for 64 KB-granular graphics virtual pages), `GetConsoleProcessorInfo`, and `SetThreadAffinityMaskX`, clamping oversized console masks safely onto the host process affinity mask (e.g., `0xFFFFFFFFFFFFFFFF -> 0xFFF` on a 12-thread host).

---

## 4. Graphics Pipeline Translation, Async Compute, and RDNA2 Binary Lifting

### 4.1 Swapchain-Less Presentation, Placed Heaps, and Compute UAVs (`d3d12_xs.dll`)
`ScarlettCommandQueueX::PresentX` lazily constructs a Win32 host window and a double-buffered `DXGI_SWAP_EFFECT_FLIP_DISCARD` swapchain matching the submitted console render target, executing per-frame resource-state transitions and `CopyResource` blits synchronized by per-back-buffer fences.

`ID3D12DeviceX` exposes explicit unified-memory heap management (`GetResourceAllocationInfo`, `CreateHeap`, `CreatePlacedResource`) alongside full graphics and compute descriptor/pipeline surfaces (`CreateRenderTargetView`, `CreateShaderResourceView`, `CreateUnorderedAccessView`, `CreateGraphicsPipelineState`, `CreateComputePipelineState`). In `dummy_title`, both the streamed `crestTexture` (`heap+0x0`) and the procedurally updated UAV `energyTexture` (`heap+0x40000`) reside inside a single 1 MiB explicit `ID3D12Heap`.

### 4.2 Dedicated Async Compute Queues & Cross-Queue GPU Synchronization
Console engines overlap compute workloads (post-processing, procedural synthesis, culling) with rasterization via dedicated `D3D12_COMMAND_LIST_TYPE_COMPUTE` queues. Sprint 19 extended `ID3D12CommandQueueX` with GPU-side `Wait(ID3D12Fence*, UINT64)` alongside `Signal` and queue-type telemetry (`DIRECT`, `COMPUTE`, `COPY`).

In both `dummy_title` and `D3D12AsyncComputeCrossQueueSync`, compute dispatches execute on a dedicated `COMPUTE` queue, emit a UAV barrier plus `UNORDERED_ACCESS -> COMMON` transition, and signal `asyncComputeFence`. The `DIRECT` graphics queue immediately issues a GPU-side `cmdQueue->Wait(asyncComputeFence, val)` with zero intervening CPU wait, transitioning the resource `COMMON -> PIXEL_SHADER_RESOURCE` (or `COPY_SOURCE`) to satisfy strict D3D12 Debug Layer cross-queue decay/promotion rules.

### 4.3 8x8 Morton Micro-Tiled Surface Translation (`xg_xs.dll`)
`xg_xs.dll` implements `XGComputeTexture2DLayout` and `IXGTextureAddressComputer` for `XG_TILE_MODE_LINEAR` (256 B row pitch, 512 B subresource alignment) and `XG_TILE_MODE_2D_THIN` (64 KB base alignment, 8x8 padded macro-tiles). Intra-tile coordinates `(lx, ly) = (x & 7, y & 7)` map bijectively through a 6-bit Z-order Morton curve:

    Morton8x8(lx, ly) = spread3(lx) | (spread3(ly) << 1)
    where spread3(v)  = (v & 1) | ((v & 2) << 1) | ((v & 4) << 2)
    pixelIndex        = ((y >> 3) * (PaddedWidth / 8) + (x >> 3)) * 64 + Morton8x8(lx, ly)

This layout verifies bit-exact multi-mip and non-power-of-two (NPOT) swizzle/detile round-trips (e.g., `77x53`, 4 mips).

### 4.4 SM 6.x DXIL Inspection & Pluggable `SCXS` Translator Architecture
Every shader stage submitted to `CreateGraphicsPipelineState` or `CreateComputePipelineState` flows through a decoupled inspection and translation boundary:

    [Incoming D3D12_SHADER_BYTECODE]
         |
         +--> Native DXBC / DXIL (SM 6.x) ---------> Fast-path passthrough
         |
         +--> SCXS Container (32 B hdr + FNV-1a) --> m_xlatCache (FNV-1a keyed)
                |
                +--> Payload = EmbeddedDxbcOrDxil -> ScxsContainerTranslator (unpack)
                +--> Payload = Rdna2Isa (VS/PS/CS)-> Rdna2IsaTranslator (decode -> HLSL -> DXBC)
         |
         v
    [Post-Translation FNV-1a PSO Hash] -> ID3D12PipelineLibrary (Disk-Backed Cache)

`shader_inspector.hpp` parses both `DXBC` chunks and SM 6.x `DXIL` containers—decoding `DxilProgramHeader` (profile tokens including `ms_6_x`, `as_6_x`, `lib_6_x`), `DXIL` version tuples, `PSV0` chunks, and LLVM bitcode wrapper magic (`0x42 0x43 0xC0 0xDE`).

### 4.5 Independent AMD RDNA2 (GFX10.3) Binary Decoder & VS/PS/CS Lifter (`rdna2_isa.hpp`)
Sprints 18, 20, and 21 transformed the `Rdna2Isa` translator seam from an `E_NOTIMPL` stub into an operational binary shader translation pipeline spanning all three primary GPU stages:

| RDNA2 Format | Encoding Prefix | Decoded Instructions | Lifted Host Target |
| :--- | :--- | :--- | :--- |
| **`SOPP`** | `0x17F` (`[31:23]`) | `s_nop`, `s_endpgm`, `s_waitcnt` | Program termination & sync markers |
| **`VOP1`** | `0x3F` (`[31:25]`) | `v_nop`, `v_mov_b32`, `v_rcp_f32`, `v_sqrt_f32` (+ 32-bit inline literals) | Unary float/uint ops (`asfloat(0x...u)`) |
| **`VOP2`** | `bit 31 = 0` | `v_add_f32`, `v_sub_f32`, `v_subrev_f32`, `v_mul_f32`, `v_min_f32`, `v_max_f32`, `v_fmac_f32`, `v_add_nc_u32` | Binary float & unsigned 32-bit ALU |
| **`VOP3`** | `0x35` (`[31:26]`) | `v_mad_f32`, `v_fma_f32`, `v_min3_f32`, `v_max3_f32`, `v_med3_f32`, `v_mul_lo_u32` (`ABS`/`NEG`/`CLAMP`) | 3-operand float/uint ALU + `saturate()` |
| **`SMEM`** | `0x3D` (`[31:26]`) | `s_load_dword`, `s_load_dwordx2`, `s_load_dwordx4` | `cbuffer Cb0 : register(b0)` -> SGPRs `s0..s105` |
| **`VINTRP`** | `0x32` (`[31:26]`) | `v_interp_mov_f32` (`attr0..31.xyzw`) | PS interpolant struct (`TEXCOORDn`) |
| **`MUBUF`** | `0x38` (`[31:26]`) | `buffer_store_dword` (`offen=1`) | `RWStructuredBuffer<uint> : register(u0)` store |
| **`EXP`** | `0x3E` (`[31:26]`) | `exp mrt0`, `exp pos0`, `exp param0..31` (`EN` masks) | `SV_TARGET`, `SV_POSITION`, `TEXCOORDn` |

The translator validates `SCXS` FNV-1a checksums (`E_INVALIDARG` on corruption), enforces stage-specific output invariants (`EXP pos0` for VS, `EXP mrt0` for PS, `MUBUF` store for CS, trapping incomplete streams cleanly with `E_NOTIMPL`), compiles lifted HLSL with `D3DCOMPILE_OPTIMIZATION_LEVEL3` (stripping nondeterministic `SPDB` debug GUIDs), and feeds the resulting DXBC directly into both `m_xlatCache` and our disk-backed `ID3D12PipelineLibrary` (`GFX_<hash>` and `CMP_<hash>` keys in `pso_cache_<VendorId>_<DeviceId>.bin`).

---

## 5. Asynchronous Storage & Real-Time Audio Translation Pipelines

### 5.1 Velocity Streaming & Worker-Threaded `SCLZ` Decompression (`dstorage_xs.dll`)
`dstorage_xs.dll` batches queued file/memory reads, status-array updates, and `ID3D12Fence` signals onto a dedicated background worker thread. `OpenFile` transparently resolves `G:\`, `T:\`, and `S:\` VFS paths. For `DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ`, the worker decodes our 64 KB sliding-window LZ77 stream (`sc::codec::DecompressLzBlock`) directly into title destination buffers (such as 64 KB-aligned `xmem` physical pages) with full bounds and FNV-1a verification, populating `DSTORAGE_ERROR_RECORD` (`E_FAIL`) on stream corruption before signaling the GPU fence.

### 5.2 Self-Recursion-Free Audio Proxying & Live IMA-ADPCM Bridge (`xaudio2_9.dll`)
`xaudio2_9.dll` dynamically binds to native `System32\xaudio2_9.dll` via `GetSystemDirectoryW` + `LoadLibraryW` (asserting `mod != g_selfModule`). Inside `CreateSourceVoice`:
* **PCM / IEEE_FLOAT:** Forwarded natively with zero runtime overhead.
* **`WAVE_FORMAT_XMA2` (`0x0166`):** Intercepted and rejected deterministically (`E_NOTIMPL`) at the hardware-codec boundary.
* **`WAVE_FORMAT_IMA_ADPCM` (`0x0011`):** Wrapped in `ScarlettDecodingSourceVoice`, which decodes 256-byte mono ADPCM blocks (505 samples/block, ~3.95x reduction) into heap-owned 16-bit PCM buffers on `SubmitSourceBuffer`. An internal `IXAudio2VoiceCallback` proxy reclaims completed PCM blocks in `OnBufferEnd` and translates internal pointers back to the caller's original `pContext` across all callbacks and `GetState()`.

---

## 6. Empirical Evaluation and Verification

### 6.1 Testbed Configuration
* **OS:** Windows 11 x64
* **CPU:** 12 logical host threads (affinity mask `0xFFF`, emulating 7-core / 14-thread Series X topology)
* **GPU:** NVIDIA GeForce RTX 5070 (`VendorId 0x10DE`, `DeviceId 0x2F04`)
* **Toolchain:** MSVC x64 (C++23, `/W4 /permissive-`), CMake + Ninja, Debug build with D3D12 Debug Layer enabled

### 6.2 Automated Regression Suite (`scarlett_shim_suite`)
CTest executes `scarlett_tests.exe` *through* `loader.exe`, exercising PE rewriting, APC hooking, VFS mounting, and all 9 shims on every run. All **20/20 checks pass in 2.59 seconds**:

| # | Test Case | Subsystem(s) | Key Invariant Verified |
| :--- | :--- | :--- | :--- |
| 1 | `VfsPathTranslation` | `common/vfs.hpp` | `G:\`, `t:/`, `S:\` translation + host passthrough |
| 2 | `RuntimeAndXUser` | `xgameruntime` | `XboxScarlettAnaconda (0x07)` + `ERROR_INSUFFICIENT_BUFFER` retry |
| 3 | `XGameSaveEdgeCases` | `xgameruntime` | Missing blob, multi-blob atomic write, undersized retry, delete |
| 4 | `XgXsMultiMipAndNpotSwizzle` | `xg_xs`, `xmem` | `77x53` 4-mip `2D_THIN` 64 KB align + bit-exact round-trip |
| 5 | `DStorageVfsAndErrorRecord` | `dstorage_xs`, `vfs` | `T:\` async read + ZLIB `E_NOTIMPL` `RetrieveErrorRecord` |
| 6 | `XAudio2Xma2Rejection` | `xaudio2_9`, `scarlett_hook` | Dynamic `LoadLibraryExA` redirect + `WAVE_FORMAT_XMA2` trap |
| 7 | `XMemAlignmentAndInvariants` | `xmem` | 64 KB `VirtualAlloc` + 4 KB/256 B/64 B heap alignment & `XMemSize` |
| 8 | `DynamicAndMultiModuleHooks` | `scarlett_hook`, `pixevt`, `xmem` | Unlinked `pixevt.dll` + sub-module `engine_plugin.dll` IAT hooks |
| 9 | `D3D12HeadlessDeviceAndPso` | `d3d12_xs` | Headless device, monotonic tokens, DXBC inspect, disk PSO hit |
| 10 | `KernelXMemoryAndTopology` | `kernelx` | 13.5 GiB budget delta, `SC_MEM_GRAPHICS` 64 KB align, `0xFFF` clamp |
| 11 | `D3D12PlacedResourcesAndHeap` | `d3d12_xs` | Explicit `ID3D12Heap` + dual placed textures (`+0x0`, `+0x10000`) |
| 12 | `DxilSm6BitcodeInspector` | `d3d12_xs` | Synthetic `ms_6_6` / `DXIL 1.6` / LLVM `BC\xC0\xDE` + `PSV0` parsing |
| 13 | `ScxsTranslatorAndPsoCache` | `d3d12_xs` | `SCXS` translation + cache hit, CRC `E_INVALIDARG`, empty VS trap |
| 14 | `DStorageLzDecompression` | `dstorage_xs`, `codecs` | `T:\` `SCLZ` decompression (20.73x) + corrupt stream `E_FAIL` |
| 15 | `XAudio2ImaAdpcmDecodeBridge` | `xaudio2_9`, `codecs` | 4:1 IMA-ADPCM SNR > 30 dB + live voice `pContext` preservation |
| 16 | `D3D12ComputePsoAndUavExecution` | `d3d12_xs` | `SCXS` CS 5.0 PSO cache + 64-element UAV GPU dispatch & readback |
| 17 | `Rdna2IsaDecoderAndGpuExecution` | `d3d12_xs` | 9-inst binary RDNA2 PS -> HLSL -> DXBC -> `(64,191,143,255)` GPU readback |
| 18 | `D3D12AsyncComputeCrossQueueSync` | `d3d12_xs` | `COMPUTE` queue `Signal` -> `DIRECT` queue GPU `Wait` -> 64-uint readback |
| 19 | `Rdna2ComputeIsaExecution` | `d3d12_xs` | 4-inst binary RDNA2 CS (`v_mul_lo_u32`/`v_add_nc_u32`/`MUBUF`) on `COMPUTE` queue |
| 20 | `Rdna2VertexAndPixelPipelineWithCbv` | `d3d12_xs` | Full binary RDNA2 VS (9 insts) + PS (11 insts) + `SMEM` CBV + `VINTRP` -> `(102,128,51,255)` |

### 6.3 Measured Performance and Memory Telemetry
Representative metrics captured on the RTX 5070 testbed across Sprints 12–21:

| Metric | Cold / Baseline | Warm / Optimized | Speedup / Notes |
| :--- | :--- | :--- | :--- |
| **Graphics PSO (`dummy_title` `GFX_E604...`)** | `17.61 ms` | `1.09 ms` (disk) | **16.2x** via `ID3D12PipelineLibrary` |
| **Compute PSO (`dummy_title` `CMP_A97A...`)** | `47.61 ms` | `0.61 ms` (disk) | **78.0x** via `ID3D12PipelineLibrary` |
| **Binary RDNA2 PS (`GFX_F153...`, 9 insts)** | `43.94 ms` | `2.12–3.10 ms` (disk) / `0.04 ms` (in-proc) | **~1,098x** in-process (`m_xlatCache` + library) |
| **Binary RDNA2 CS (`CMP_94A6...`, 4 insts)** | `38.28 ms` | `1.64 ms` (disk) / `0.03 ms` (in-proc) | **~1,276x** in-process on `COMPUTE` queue |
| **Binary RDNA2 VS+PS+CBV (`GFX_B6BB...`, 20 insts)** | `47.78 ms` | `0.06 ms` (in-proc) | **~796x**; merged PSO library `40,372 B` |
| **DirectStorage Raw Asset Throughput** | `774–1,269 MB/s` | — | `262,176 B` in `0.20–0.32 ms` on I/O worker |
| **DirectStorage `SCLZ` Tiled Texture** | `262,144 B` raw | `163,909 B` (`1.60x`) | `1.83–2.46 ms` (`101–136 MB/s` eff), bit-exact CRC |
| **DirectStorage `SCLZ` Structured Test** | `65,536 B` raw | `3,162 B` (`20.73x`) | `0.26–0.42 ms` (`148–240 MB/s` eff) in Debug |
| **IMA-ADPCM Audio Footprint** | `218,880 B` peak | `127,488 B` peak | **-41.8% RAM** (`3.85x–3.92x`), `0` glitches over 1,704 frames |
| **Memory Hygiene (`xmem` & `kernelx`)** | `0 leaks (0 B)` | `0 leaks (0 B)` | Checked on unload in both CTest and `dummy_title` |

---

## 7. Open Challenges and Research Roadmap

While Scarlett Layer now demonstrates an end-to-end host translation stack—including live binary RDNA2 execution across Vertex, Pixel, and Compute stages on async queues—executing unmodified retail or devkit Series X titles requires four major technical extensions:

1. **Full RDNA2 Control Flow, Texture Sampling & Direct DXIL Emission:** Expanding `rdna2_isa.hpp` beyond straight-line ALU/CBV/MUBUF shaders to reconstruct structured control flow (`s_cbranch_scc0/1`, `exec` mask divergence/reconvergence), `MIMG` texture sampling (`image_sample`), Wave32/Wave64 intrinsics, and direct LLVM bitcode (`DXIL`) emission to remove the intermediate HLSL/`D3DCompile` step.
2. **Hardware-Exact `64KB_S` Swizzling & BCn Block-Compressed Surfaces:** Extending `xg_xs.dll` from `RGBA8` `2D_THIN` to AMD RDNA2 `64KB_S` / `64KB_D` / `64KB_R` micro/macro-tile equations over 4x4 block-compressed textures (`BC1`, `BC3`, `BC7`), paired with BCPack decompression in `dstorage_xs.dll` and XMA2 decoding behind `ScarlettDecodingSourceVoice`.
3. **DirectStorage Direct-to-VRAM Streaming (`DSTORAGE_REQUEST_DESTINATION_BUFFER`):** Adding an internal upload staging ring and `COPY` command queue inside `dstorage_xs.dll` so compressed disk slices stream directly into `D3D12_HEAP_TYPE_DEFAULT` GPU buffers before signaling `ID3D12Fence`.
4. **Exact Binary ABI, COM GUID & VTable Conformance:** Systematically auditing real GDK/Series X COM IIDs, vtable slot ordinals, and structure layouts via `tools/audit_imports.py` to bridge independent headers to unmodified third-party binaries.

---

*Archived Sprint 21 snapshot (encoding and terminology cleaned up in Sprint 66). See `docs/PROGRESS.md` and `docs/SPEC_SOURCES.md` for current state.*