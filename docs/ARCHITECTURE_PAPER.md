# Scarlett Layer: Clean-Room Host-Based ABI Interposition, Graphics Pipeline Translation, and Asynchronous I/O Virtualization for Xbox Series X Binaries on Windows x64

**Project Version:** Sprint 16 Baseline (9 Shims, 15 Automated Verification Tests)  
**Target Host Architecture:** Windows 11 x64 (MSVC C++23, Direct3D 12, DXGI 1.6, XAudio2 2.9)  
**Revision Cadence:** Formal monograph updated every 5 sprints (Next scheduled revision: Sprint 21)

---

## Abstract

Console compatibility layers on general-purpose desktop operating systems sit at an architectural inflection point between full-system hardware virtualization and user-space binary translation. Because ninth-generation Xbox Series X (codename *Scarlett* / *Anaconda*) titles compile to standard x86-64 PE32+ executables built atop a specialized Game Core / Direct3D 12.x Windows NT derivative, CPU instruction-set emulation is unnecessary; the dominant engineering challenges shift to **loader-time and runtime ABI interposition**, **graphics state and shader container translation**, **unified memory and address-tiling emulation**, and **asynchronous storage/audio codec bridging**.

This paper documents the design, formal invariants, and empirical evaluation of **Scarlett Layer**, a clean-room host-based translation layer implemented across 16 incremental engineering sprints. Scarlett Layer intercepts unmodified x64 executables without mutating title directories or host `System32` binaries by combining suspended-process PE import-table rewriting with pre-entrypoint Asynchronous Procedure Call (APC) injection and recursive multi-module Import Address Table (IAT) hooking. Across nine isolated dynamic-link shims (`xgameruntime`, `d3d12_xs`, `gameinput`, `xg_xs`, `dstorage_xs`, `pixevt`, `xaudio2_9`, `xmem`, and `kernelx`), we demonstrate deterministic disk-backed Pipeline State Object (PSO) caching (achieving 35xâ€“47x warm-load speedups), a pluggable console shader container (`SCXS`) and Shader Model 6.x `DXIL`/LLVM bitcode inspection boundary, 8x8 Morton micro-tiled surface transfers, worker-threaded `SCLZ` DirectStorage decompression, and a real-time 4:1 IMA-ADPCM voice-decoding bridge. Verified via an automated headless CTest suite (15/15 passing checks in 1.21 s) and an interactive synthetic title running on an NVIDIA GeForce RTX 5070, the stack exhibits zero D3D12 validation faults, zero audio glitches, and zero leaked allocations across both virtual and heap memory domains.

---

## 1. Introduction and Scope of Compatibility

### 1.1 Architectural Taxonomy: Emulation vs. Host-Based Translation
Traditional console emulators targeting non-x86 architectures (e.g., PowerPC, Cell, MIPS) must dynamically recompile CPU instructions into host machine code while simultaneously simulating bus timings and MMIO device registers. In contrast, Xbox Series X titles execute native x86-64 user-mode code against a Windows NT-derived kernel (`kernelx`) and a console-specialized Direct3D 12 API (`d3d12_xs`). Consequently, Scarlett Layer adopts a **user-space host-based translation architecture** (analogous in structural philosophy to Wine/Proton on Linux, but targeting console-to-desktop Windows NT divergence):

1. **Zero CPU ISA translation:** Host x86-64 threads execute title machine code natively at full clock rate.
2. **Process-isolated DLL redirection:** Console-exclusive and colliding system DLLs (e.g., `xaudio2_9.dll`) are redirected strictly within the target process without installing system-wide hooks or copying DLLs into title folders.
3. **Clean-room semantic bridging:** Console abstractionsâ€”swapchain-less frame pacing (`PresentX`), unified 13.5 GiB memory accounting, tiled texture address computation, Velocity streaming queues, and compressed audio voicesâ€”are mapped onto desktop Win32, Direct3D 12, DXGI, XInput, and `System32\xaudio2_9.dll`.

### 1.2 Explicit Non-Goals and External Boundaries
To maintain rigorous technical clarity, we distinguish architectural boundaries from unresolved external constraints:

* **Container Decryption (XVD/XVC):** Retail Series X packages are encrypted at rest by hardware-backed hypervisor keys. Scarlett Layer operates strictly on decrypted, unpacked x64 PE32+ binaries and makes no attempt to bypass platform DRM.
* **Clean-Room Interface Definitions:** Where console-specific headers are proprietary (`ID3D12DeviceX`, `IXGTextureAddressComputer`, `IDStorageFactoryX`, `SCXS`), Scarlett Layer defines clean-room COM interfaces and binary contracts to validate end-to-end architectural mechanics prior to exact binary-offset and vtable-ordinal alignment.
* **Host Hardware Assumptions:** All empirical measurements assume a modern Direct3D 12 Feature Level 12_0+ discrete GPU and Windows 10/11 x64 user-mode APIs.

---

## 2. Process Bootstrap, Binary Rewriting, and Multi-Module Interposition

Redirecting imports in a Windows x64 process is complicated by two realities: (i) static imports are resolved by `ntdll!LdrInitializeThunk` *before* the process entry point executes, and (ii) modern game engines dynamically load both Xbox libraries and third-party middleware DLLs at runtime via `LoadLibrary{A,W}` and `LoadLibraryEx{A,W}`. Scarlett Layer solves both paths through a two-stage bootstrap protocol.

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
In PE32+ binaries, `IMAGE_IMPORT_DESCRIPTOR::Name` and `IMAGE_DELAYLOAD_DESCRIPTOR::DllNameRVA` are unsigned 32-bit Relative Virtual Addresses (`DWORD`) added to the 64-bit `ImageBase`. Consequently, redirected absolute shim paths written into the suspended target process **must** reside within the interval `[ImageBase, ImageBase + 0xFFFFFFFF]`.

`loader.exe` determines the remote `ImageBase` by querying `NtQueryInformationProcess(ProcessBasicInformation)` and reading the 64-bit pointer at `PEB + 0x10`. It then scans free virtual address regions starting immediately above `ImageBase + SizeOfImage` via `VirtualQueryEx`, commits a single 64 KB-aligned string table inside the 4 GB window, writes null-terminated ANSI paths for every discovered shim in `build/bin/shims/`, and updates matching descriptor RVAs under transient `PAGE_READWRITE` protection. Any `IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT` directory is explicitly zeroed to prevent `ntdll` from short-circuiting rewritten descriptors using stale bound timestamps. Finally, the string table is hardened to `PAGE_READONLY`.

### 2.2 Pre-Entrypoint APC Injection and Multi-Module Hooking (`scarlett_hook.cpp`)
Static rewriting cannot intercept runtime `LoadLibrary` callsâ€”including SDK inline wrappers such as `<xaudio2.h>` invoking `LoadLibraryExA("xaudio2_9.dll", ...)` or middleware DLLs dynamically probing for `pixevt.dll` and `xmem.dll`.

Before resuming the child process, `loader.exe` allocates the wide path to `scarlett_hook.dll` in the target and queues `kernel32!LoadLibraryW` onto the primary suspended thread via `QueueUserAPC`. During `ntdll!LdrInitializeThunk`, Windows invokes `NtTestAlert` prior to jumping to the executable entry point, guaranteeing `scarlett_hook.dll` initializes before any title code runs.

To remain loader-lock safe inside `DllMain`, shim discovery uses raw Win32 `FindFirstFileW` rather than C++ filesystem iterators. `scarlett_hook.dll` patches the IAT entries for `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA`, and `LoadLibraryExW` across both the main executable and all non-system, non-shim modules enumerated via `K32EnumProcessModules`. When any hooked call loads a non-shim sub-module (e.g., `engine_plugin.dll`), the hook automatically patches the newly loaded module's IAT upon return before passing the `HMODULE` back to the caller.

Two subtle correctness invariants are enforced:
1. **Recursion & System-Boundary Exclusion:** Modules residing under `%SystemRoot%` or `SCARLETT_SHIM_DIR` are added to `g_seenModules` and skipped. This guarantees our own shimsâ€”most critically `xaudio2_9.dll` loading `C:\Windows\System32\xaudio2_9.dll`â€”never re-enter the redirector.
2. **Search-Flag Sanitization:** When redirecting `LoadLibraryEx{A,W}` to an absolute path outside `System32`, restrictive flags (`LOAD_LIBRARY_SEARCH_SYSTEM32`, `LOAD_LIBRARY_SEARCH_APPLICATION_DIR`, `LOAD_LIBRARY_SEARCH_USER_DIRS`, `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`) are masked off so Win32 does not reject the absolute shim path with `ERROR_INVALID_PARAMETER`.

### 2.3 Virtual File System (VFS) Drive Mapping (`common/vfs.hpp`)
Console titles address storage via fixed logical drive letters. `loader.exe` initializes isolated host roots via child environment variables:
* **`G:\`** (Game package root) -> `SCARLETT_GAME_ROOT` (title executable directory)
* **`T:\`** (Title scratch storage) -> `SCARLETT_SCRATCH_DIR` (`build/bin/vfs/scratch/`)
* **`S:\`** (Save/system temporary area) -> `SCARLETT_SAVE_TEMP_DIR` (`build/bin/vfs/save_temp/`)

`sc::vfs::TranslatePath` performs case-insensitive prefix matching, slash normalization, and lexical path canonicalization while passing standard host paths (e.g., `C:\...`) through unmodified.

---

## 3. Runtime Services, Concurrency, and Unified Memory Semantics

### 3.1 Dual-Port Asynchronous Execution (`xgameruntime.dll`)
The Xbox Game Development Kit (GDK) decouples background execution from main-thread callback delivery through `XTaskQueue` and `XAsyncBlock`. Our implementation separates each queue into a **Work Port** (backed by worker threads) and a **Completion Port** (manually drained on the title's main thread via `XTaskQueueDispatch`), synchronized via mutexes and condition variables. Asynchronous operations share a reference-counted `AsyncState` record stored in `XAsyncBlock::internal` to prevent use-after-free races if a caller queries `XAsyncGetStatus` concurrently with completion or retries a result call after an undersized buffer error.

### 3.2 Crash-Safe Local Connected Storage (`XGameSave`)
`XGameSave` maps title Service Configuration IDs (SCIDs) and container names to local directories under `build/bin/saves/<UserId>/<Scid>/<Container>/`. Every blob write commits via a temporary staging file followed by `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` to provide atomic crash-safe updates. On `XGameSaveReadBlobDataResult`, if the caller's buffer is smaller than the packed `XGameSaveBlob` header array plus payload bytes, the shim returns `E_GS_PROVIDED_BUFFER_TOO_SMALL` *without* releasing the underlying `AsyncState`, allowing standard GDK retry-with-larger-buffer loops to succeed identically to console hardware.

### 3.3 Dual-Tier Console Memory & Unified Kernel Accounting (`xmem.dll` & `kernelx.dll`)
Xbox Series X exposes 13.5 GiB (`14,495,514,624 B`) of unified GDDR6 memory to titles across 7 Zen 2 cores (14 SMT threads). Scarlett Layer models this memory hierarchy across two complementary shims:

* **`xmem.dll`:** Decodes packed `dwAllocAttributes` (`AllocatorId`, power-of-two `Alignment` from 4 B to 64 KB, `MemType`, and `ZeroInit`). Requests tagged `XALLOC_MEMTYPE_PHYSICAL` or requiring alignment of at least 64 KB route directly to `VirtualAlloc(..., MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)`, guaranteeing strict 64 KB OS page alignment and zero-fill; smaller heap requests route through `_aligned_malloc`. A mutex-guarded registry tracks global and per-pool live/peak footprints, implements `XMemSize`, and emits a full leak audit on `DLL_PROCESS_DETACH`.
* **`kernelx.dll`:** Exposes `TitleMemoryStatus` backed by the 13.5 GiB Series X budget, `VirtualAllocX` / `VirtualFreeX` (stripping console-only flags such as `SC_MEM_GRAPHICS = 0x10000000` prior to host `VirtualAlloc` while tracking 64 KB-granular graphics virtual memory usage), `GetConsoleProcessorInfo`, and `SetThreadAffinityMaskX`. To prevent affinity failures when a 14-thread console title runs on a host with fewer logical processors (e.g., our 12-thread test host, mask `0xFFF`), `SetThreadAffinityMaskX` intersects the requested mask with the host process affinity mask, falling back to the lowest set host bit (`procMask & (~procMask + 1)`) if the intersection is empty.

---

## 4. Graphics Translation, Address Tiling, and Shader Interposition

### 4.1 Swapchain-Less Presentation & Explicit Placed Heaps (`d3d12_xs.dll`)
Console D3D12.x titles do not create a desktop `IDXGISwapChain`; instead, they allocate console-owned render targets, synchronize via `SetFrameIntervalX` / `WaitFrameEventX` pipeline tokens, and submit frames through `ID3D12CommandQueueX::PresentX`.

`ScarlettCommandQueueX::PresentX` lazily initializes a Win32 host window and a double-buffered `DXGI_SWAP_EFFECT_FLIP_DISCARD` swapchain matching the first submitted plane's dimensions and pixel format. Each frame, it records a dedicated internal copy command list that transitions the title's render target (`PRESENT -> COPY_SOURCE`) and the current DXGI back buffer (`PRESENT -> COPY_DEST`), executes `CopyResource`, restores both resources to `PRESENT`, and presents with per-back-buffer fence synchronization.

To model console unified-memory resource placement, `ID3D12DeviceX` implements `GetResourceAllocationInfo`, `CreateHeap`, and `CreatePlacedResource`, enabling titles to allocate explicit `ID3D12Heap` arenas (`D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES`) and alias or pack textures at 64 KB-aligned offsets.

### 4.2 8x8 Morton Micro-Tiled Surface Translation (`xg_xs.dll`)
Console GPUs store 2D textures in space-filling tiled layouts to maximize 2D texture-cache locality. `xg_xs.dll` implements `XGComputeTexture2DLayout` and `IXGTextureAddressComputer` supporting both `XG_TILE_MODE_LINEAR` (256-byte row pitch, 512-byte subresource placement alignment) and `XG_TILE_MODE_2D_THIN` (64 KB base alignment, dimensions padded to multiples of 8 elements per mip level).

For pixel `(x, y)` within a mip level of padded tile width `tilesX = PaddedWidth / 8`, the linear byte offset into the tiled surface is computed by decomposing into macro-tile coordinates `(tx, ty) = (x >> 3, y >> 3)` and intra-tile coordinates `(lx, ly) = (x & 7, y & 7)`, interleaving the 3-bit local coordinates into a 6-bit Z-order Morton index:

    Morton8x8(lx, ly) = spread3(lx) | (spread3(ly) << 1)
    where spread3(v)  = (v & 1) | ((v & 2) << 1) | ((v & 4) << 2)
    pixelIndex        = (ty * tilesX + tx) * 64 + Morton8x8(lx, ly)

This bijective mapping supports arbitrary non-power-of-two (NPOT) textures and multi-mip chains with bit-exact swizzle/detile round-trips.

### 4.3 Deep SM 6.x DXIL Inspection & Pluggable `SCXS` Shader Translation
Real Xbox Series X titles ship precompiled AMD RDNA2 machine code rather than portable `DXBC`/`DXIL`. To establish a clean, testable translation boundary inside `CreateGraphicsPipelineState`, Sprint 15 introduced two decoupled components:

1. **Container & Bitcode Inspector (`shader_inspector.hpp`):** Parses 32-byte `DXBC` headers and chunk tables (`RDEF`, `ISGN`, `OSGN`, `SHDR`, `SHEX`, `STAT`, `SPDB`, `PSV0`, `DXIL`). When a `DXIL` chunk is present, the inspector decodes the 24-byte `DxilProgramHeader`, extracts the Shader Model 6.x profile (including Mesh `ms_6_x`, Amplification `as_6_x`, and Raytracing Library `lib_6_x` tokens), reads the DXIL version tuple, and validates the 4-byte LLVM bitcode wrapper magic (`0x42 0x43 0xC0 0xDE`) at `bodyOff + 8 + bitcodeOffset`.
2. **Pluggable Translator Registry (`shader_translator.hpp`):** Every populated shader stage in `CreateGraphicsPipelineState` passes through `ShaderTranslatorRegistry`, backed by an in-memory FNV-1a-keyed translation cache (`std::shared_ptr<const TranslatedShader>`). Our clean-room `SCXS` 32-byte container format carries stage, target architecture, payload format (`EmbeddedDxbcOrDxil` vs. `Rdna2Isa`), Shader Model version, and an FNV-1a checksum. `ScxsContainerTranslator` validates integrity (`E_INVALIDARG` on corruption), unpacks embedded host bytecode for execution, and deterministically traps `Rdna2Isa` payloads with `E_NOTIMPL` at the exact plugin seam where an RDNA2-to-DXIL recompiler attaches.

### 4.4 Deterministic PSO Hashing and Persistent `ID3D12PipelineLibrary` Caching
To eliminate shader-compilation stutter across launches, `ScarlettDeviceX` wraps D3D12's `ID3D12PipelineLibrary` and persists serialized libraries atomically to `build/bin/cache/pso_cache_<VendorId>_<DeviceId>.bin`.

Each graphics PSO is keyed by a 64-bit FNV-1a digest over **post-translation** shader bytecodes, input-element descriptions, rasterizer/blend/depth-stencil/RTV states, and the content hash of the serialized root-signature blob (avoiding nondeterministic COM pointer addresses). Because hashing occurs *after* `IShaderTranslator`, native DXBC shaders and `SCXS`-wrapped equivalents map to the exact same PSO library key.

**Empirical Hazard Identified (Sprint 12):** Compiling HLSL with `D3DCOMPILE_DEBUG` embeds a `SPDB` (Shader PDB) chunk containing a freshly generated GUID on every invocation, mutating the DXBC digest across identical runs and causing a 100% cold-miss rate. Switching runtime test shaders to `D3DCOMPILE_OPTIMIZATION_LEVEL3` stripped the nondeterministic `SPDB` chunk (reducing VS/PS sizes from ~17.5 KB to ~1.5 KB) and restored 100% cross-launch cache determinism.

---

## 5. Asynchronous Storage & Real-Time Audio Translation Pipelines

### 5.1 Velocity Asset Streaming & `SCLZ` Worker Decompression (`dstorage_xs.dll`)
`dstorage_xs.dll` implements `IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, and `IDStorageStatusArrayX`. Enqueued read requests, status updates, and `ID3D12Fence` signals accumulate in a staging vector until `Submit()` transfers the batch atomically to a dedicated background I/O worker thread.

`OpenFile` transparently resolves `G:\`, `T:\`, and `S:\` VFS paths. For `DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ`, the worker reads the compressed slice into a staging buffer and executes our single-pass 64 KB sliding-window LZ77 decoder (`sc::codec::DecompressLzBlock`) directly into the title's destination buffer (e.g., 64 KB-aligned `xmem` physical memory) before updating status slots and signaling `ID3D12Fence`. Header magic validation, bounds checking, output-length enforcement, and FNV-1a checksum verification guarantee corrupted streams fail cleanly with `E_FAIL` and populate `DSTORAGE_ERROR_RECORD` for `RetrieveErrorRecord`.

### 5.2 Self-Recursion-Free Audio Proxying & Live IMA-ADPCM Bridge (`xaudio2_9.dll`)
Because Windows 10/11 ships a system `xaudio2_9.dll` in `System32`, static linking would cause infinite self-recursion. Our shim resolves the host binary explicitly via `GetSystemDirectoryW` + `LoadLibraryW`, validates `mod != g_selfModule`, and wraps the native `IXAudio2` COM interface.

Inside `CreateSourceVoice`:
* **PCM / IEEE_FLOAT:** Passed directly to native XAudio2 with zero runtime overhead.
* **`WAVE_FORMAT_XMA2` (`0x0166`):** Intercepted and logged as `E_NOTIMPL` at the console hardware-codec boundary.
* **`WAVE_FORMAT_IMA_ADPCM` (`0x0011`):** Instantiates `ScarlettDecodingSourceVoice`, a full `IXAudio2SourceVoice` ABI wrapper backed by a native 16-bit PCM voice and an internal `IXAudio2VoiceCallback` proxy. On `SubmitSourceBuffer`, incoming 256-byte mono ADPCM blocks (505 samples/block, ~3.95x compression) are decoded into heap-owned `DecodedBlock` buffers keyed in a mutex-protected map; the proxy callback reclaims each decoded buffer upon `OnBufferEnd` and translates `pBufferContext` back to the caller's original `pContext` pointer across both callbacks and `GetState()`.

---

## 6. Empirical Evaluation and Verification

### 6.1 Testbed Configuration
All benchmarks and verification runs were executed on the following host environment:
* **Operating System:** Windows 11 x64
* **Host CPU:** 12 logical threads (affinity mask `0xFFF`, emulating 7-core / 14-thread Series X topology)
* **Host GPU:** NVIDIA GeForce RTX 5070 (`VendorId 0x10DE`, `DeviceId 0x2F04`)
* **Toolchain:** MSVC x64 (C++23, `/W4 /permissive-`), CMake + Ninja, Debug build with D3D12 Debug Layer enabled

### 6.2 Automated Regression Suite (`scarlett_shim_suite`)
Every build is validated via `ctest --test-dir build`, which launches `scarlett_tests.exe` *through* `loader.exe` so PE import rewriting, APC injection, multi-module hooking, and VFS mounting are exercised identically to real titles. All **15/15 tests pass in 1.21 seconds**:

| # | Test Case | Subsystem(s) Verified | Key Invariant Asserted |
| :--- | :--- | :--- | :--- |
| 1 | `VfsPathTranslation` | `common/vfs.hpp` | `G:\`, `t:/`, `S:\` rewrite + host path passthrough |
| 2 | `RuntimeAndXUser` | `xgameruntime` | `XboxScarlettAnaconda (0x07)` + `ERROR_INSUFFICIENT_BUFFER` retry |
| 3 | `XGameSaveEdgeCases` | `xgameruntime` | Missing blob, multi-blob atomic write, undersized-buffer retry, deletion |
| 4 | `XgXsMultiMipAndNpotSwizzle` | `xg_xs`, `xmem` | `77x53` 4-mip `2D_THIN` 64 KB align + bit-exact round-trip across all mips |
| 5 | `DStorageVfsAndErrorRecord` | `dstorage_xs`, `vfs` | `T:\` streaming + `E_NOTIMPL` ZLIB `RetrieveErrorRecord` |
| 6 | `XAudio2Xma2Rejection` | `xaudio2_9`, `scarlett_hook` | Dynamic `LoadLibraryExA` redirect + `WAVE_FORMAT_XMA2` trap |
| 7 | `XMemAlignmentAndInvariants` | `xmem` | 64 KB `VirtualAlloc` + 4 KB/256 B/64 B heap alignment, zero-init, `XMemSize` |
| 8 | `DynamicAndMultiModuleHooks` | `scarlett_hook`, `pixevt`, `xmem` | Unlinked `pixevt.dll` + sub-module `engine_plugin.dll` recursive IAT hooks |
| 9 | `D3D12HeadlessDeviceAndPso` | `d3d12_xs` | Headless device, monotonic frame tokens, DXBC inspect, disk PSO hit |
| 10 | `KernelXMemoryAndTopology` | `kernelx` | 13.5 GiB budget delta, `SC_MEM_GRAPHICS` 64 KB align, `0xFFF` affinity clamp |
| 11 | `D3D12PlacedResourcesAndHeap` | `d3d12_xs` | Explicit `ID3D12Heap` + dual `CreatePlacedResource` textures (`+0x0`, `+0x10000`) |
| 12 | `DxilSm6BitcodeInspector` | `d3d12_xs` | Synthetic `ms_6_6` / `DXIL 1.6` / LLVM `BC\xC0\xDE` + `PSV0` parsing |
| 13 | `ScxsTranslatorAndPsoCache` | `d3d12_xs` | `SCXS` translation + cache hit, corrupted CRC `E_INVALIDARG`, RDNA2 `E_NOTIMPL` |
| 14 | `DStorageLzDecompression` | `dstorage_xs`, `codecs` | `T:\` `SCLZ` decompression (20.73x) + corrupt stream `E_FAIL` error record |
| 15 | `XAudio2ImaAdpcmDecodeBridge` | `xaudio2_9`, `codecs` | 4:1 IMA-ADPCM SNR > 30 dB + live decode voice `pContext` preservation |

### 6.3 Measured Performance and Memory Telemetry
The table below summarizes representative telemetry recorded across Sprints 12â€“16 on the RTX 5070 testbed:

| Metric | Observed Result | Notes |
| :--- | :--- | :--- |
| **PSO Creation (Cold Compile vs. Disk Hit)** | `44.32 ms` -> `0.93â€“1.32 ms` | **35xâ€“47x speedup** via `ID3D12PipelineLibrary` (`14,746 B` library, 3 PSOs) |
| **In-Process `SCXS` Translated PSO Hit** | `29.58 ms` -> `0.04â€“0.07 ms` | **>400x speedup** combining `m_xlatCache` + `ID3D12PipelineLibrary` |
| **DirectStorage Raw Asset Throughput** | `774â€“1,269 MB/s` | `262,176 B` (`crest_2dthin.scx`) in `0.20â€“0.32 ms` on background worker |
| **DirectStorage `SCLZ` Tiled Texture** | `1.60x` (`163,909 B` -> `262,144 B`) | `1.83 ms` (`136.3 MB/s` effective), bit-exact `crc=0xA8568DF7` |
| **DirectStorage `SCLZ` Structured Tile Test** | `20.73x` (`3,162 B` -> `65,536 B`) | `0.31 ms` (`204.2 MB/s` effective) under Debug build |
| **IMA-ADPCM Audio Compression & Footprint** | `3.85xâ€“3.92x` | `AudioPCM[20]` peak RAM reduced **41.8%** (`218,880 B` -> `127,488 B`) |
| **Audio Fidelity & Runtime Stability** | `SNR > 30 dB`, `0 glitches` | 5 ADPCM buffers (`59,904 B -> 236,340 B PCM`) over 1,431 rendered frames |
| **Memory Hygiene (`xmem` & `kernelx`)** | `0 leaks (0 B)` | Verified on unload in both `scarlett_tests` and `dummy_title` (exit code `0`) |

---

## 7. Open Challenges and Research Roadmap

While Scarlett Layer validates the complete architectural skeleton of a host-based Series X translation layer, executing unmodified retail or devkit Series X titles requires addressing four major research and engineering fronts:

1. **RDNA2 ISA-to-DXIL Binary Recompilation:** The single largest technical barrier is lifting precompiled AMD RDNA2 shader machine code (SGPR/VGPR allocation, Wave32/Wave64 execution masks, structured control flow, and console descriptor table bindings) into valid host `DXIL` LLVM bitcode. Our `IShaderTranslator` registry and `SCXS` `Rdna2Isa` seam are designed to host this compiler backend incrementally.
2. **Exact Binary ABI, COM IID, and VTable Slot Alignment:** Precompiled third-party binaries bind by exact COM GUIDs, struct offsets, calling conventions, and vtable slot indices rather than source-level headers. Expanding `tools/audit_imports.py` alongside systematic ABI conformance tests will bridge clean-room interfaces to real binary expectations.
3. **Hardware-Exact Surface Tiling & Block Codecs:** Upgrading `xg_xs.dll` from 8x8 Morton `2D_THIN` to AMD RDNA2 `64KB_S` / `64KB_D` / `64KB_R` micro/macro swizzle equations (including BCn block-compressed formats), integrating hardware-equivalent BCPack decompression in `dstorage_xs.dll`, and implementing an XMA2 decoder behind `ScarlettDecodingSourceVoice`.
4. **Compute, Mesh, and Async-Queue Graphics Coverage:** Extending `d3d12_xs.dll` with `CreateComputePipelineState`, `CreateUnorderedAccessView`, async compute queues, and Mesh/Amplification shader pipelines to match modern ninth-generation rendering workloads.

---

*Document generated at Sprint 16 baseline. Next scheduled monograph revision: Sprint 21.*