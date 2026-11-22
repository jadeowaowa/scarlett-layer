# Scarlett Layer — Progress & API Coverage

## Sprint Log

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
| Sprints 23-62 | DirectStorage GPU streaming, retail bring-up shims (`AcpHal`, `libHttpClient`, `XCurl`), PE/XVD inspection, `MicrosoftGame.Config`, RDNA2 textured draw & compute E2E, `d3d12_xs` command-list wrappers, 3D depth/PresentX/frame-pacing/async-compute E2E (see sprint notes below) | Done (Tier 1) |
| Sprint 63 | PE64 implicit `.tls` inspection/callbacks, isolated guest `atexit`, `XMemAllocDefault` 16-byte min alignment | Done (Tier 1) |
| Sprint 64 | W^X x64 `AutoStubEngine`, PE symbol coverage auditor (`loader --inspect`), `XAudio2` `@1` export parity | Done (Tier 1) |
| Sprint 65 | DXBC modified-MD5 digest + Xbox-chunk stripper (`shader_stripper.hpp`), Enhanced Barriers, `CheckFeatureSupport` | Done (Tier 1) |
| Sprint 66 | VFS path-escape & long-path hardening, XVD overflow + inverted encryption-flag fix (`emoose/xvdtool`), lossless ANSI/non-ASCII paths, `FreeLibrary` hook, portable `vswhere` build, 67 individual CTests, `SPEC_SOURCES.md`, CI | Done (Tier 1) |
| Sprint 67 | `SCARLETT_USE_WARP=1` software D3D12 + full WARP CI, `loader --audit-dir`, Desktop GDK `261000` `SimpleTriangleDesktop` alignment (`libHttpClient.dll` 10/10), `PresentX(0)` & 5-arg `WaitFrameEventX` | Done (Tier 1) |
| Sprint 68 | Real GDK `261000` `xgameruntime.dll` COM service broker (8 service vtables), `libHttpClient` async callbacks, local Desktop `SimpleTriangleDesktop.exe` boot (`code=0`, 0 auto-stubs), `scarlett_studio.exe` | Done (Tier 1 + Desktop sample) |
| Sprint 69 | Direct2D 1.1 + DirectWrite + DirectComposition Liquid Glass UI on `scarlett_studio.exe`, Apache-2.0 `LICENSE`, wiki rebuild, `v0.69.0-pre` release archive | Done (72/72 Tier 1) |
| Sprint 70 | Real 12 KiB `emoose/xvdtool` XVD offsets (`0x280`/`0x284`/`0x288`/`0x28C`/`0x470`) & hash-tree geometry, `game_config.hpp` path containment, `pe_inspector.hpp` bounds/overflow hardening, `tools/generate_status.py` CI drift check | Done (74/74 Tier 1) |
| Sprint 71 | 28-case independent-oracle RDNA2 differential GPU suite (4 CTests), lifter/CFG/FP16/LDS fixes, `rdna2_sweep.exe` + 255-ELF real `gfx1030` corpus sweep (`docs/RDNA2_CORPUS_REPORT.md`) | Done (78/78 Tier 1) |

## Shim & Export Coverage

### `loader.exe` + `scarlett_hook.dll` + `common/`
- [x] In-memory static, delay-load, and ordinal PE import redirection across all 12 shims (+ `libHttpClient.dll` alias)
- [x] Pre-entrypoint APC injection of `scarlett_hook.dll`, implicit `.tls` support, guest `atexit`, and W^X `AutoStubEngine`
- [x] Multi-module runtime IAT hooks (`LoadLibraryA/W`, `LoadLibraryExA/W`, `GetModuleHandle*`, `GetProcAddress`, `FreeLibrary`)
- [x] Virtual console drive mapping (`G:\`, `T:\`, `S:\`) with traversal rejection and long-path support
- [x] Synthetic `SCLZ` LZ77 codec (`ASSUMED`), standard IMA-ADPCM (`0x0011`), and read-only XVD/XVC inspection (encrypted images rejected)
- [x] Automated CTest suite (`scarlett_tests.exe`): **74/74 Tier 1 self-authored synthetic checks** (`42 cpu`, `32 gpu`) on RTX 5070 + Microsoft WARP; **0 Tier 2 automated external Xbox-targeted binary tests**

### `xgameruntime.dll`
- [x] GDK `261000` COM service broker (`DllCanUnloadNow`, `InitializeApiImpl`/`Ex`/`Ex2`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport` + 8 service vtables)
- [x] Flat `X*` exports: `XGameRuntimeInitialize`, `XTaskQueue*`, `XAsync*`, `XUser*`, `XGameSave*`, `XSystemGetDeviceType`

### `d3d12_xs.dll`
- [x] `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature`
- [x] `ID3D12DeviceX`: `CreateCommandQueueX`, `CreateCommittedResource`, `GetResourceAllocationInfo`, `CreateHeap`, `CreatePlacedResource`, `CreateRenderTargetView`, `CreateShaderResourceView`, `CreateUnorderedAccessView`, `CreateRootSignature`, `CreateGraphicsPipelineState`, `CreateComputePipelineState`, frame pacing
- [x] `ID3D12CommandQueueX` (`ExecuteCommandLists`, `Signal`, `Wait`, `PresentX` -> DXGI Flip-Discard swapchain)
- [x] Deep `DXBC` + SM 6.x `DXIL` (`DxilProgramHeader`, LLVM `BC\xC0\xDE`, `PSV0`) container inspector
- [x] Pluggable `IShaderTranslator` registry: `Rdna2IsaTranslator` (GFX10.3 `SOPP`/`VOP1`/`VOP2`/`VOP3`/`EXP` -> HLSL -> DXBC) + `ScxsContainerTranslator` + translation cache
- [x] Persistent disk-backed `ID3D12PipelineLibrary` cache for both Graphics (`GFX_*`) and Compute (`CMP_*`) PSOs

### `gameinput.dll`
- [x] `GameInputCreate`, `IGameInput`, `IGameInputReading` (XInput + Win32 keyboard)

### `xg_xs.dll`
- [x] `XGCreateTexture2DComputer`, `XGComputeTexture2DLayout`, `IXGTextureAddressComputer` (verified multi-mip & NPOT)

### `dstorage_xs.dll`
- [x] `DStorageGetFactory`, `DStorageSetConfiguration`, `IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, `IDStorageStatusArrayX`
- [x] Virtual console drive translation inside `OpenFile` (`G:\`, `T:\`, `S:\`)
- [x] Live background worker `SCLZ` CPU decompression with integrity verification & `RetrieveErrorRecord` reporting

### `pixevt.dll`
- [x] `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker` (loaded dynamically via `scarlett_hook.dll`)

### `xaudio2_9.dll`
- [x] `XAudio2Create` proxying onto native `System32\xaudio2_9.dll`
- [x] Full `IXAudio2` COM wrapper + `ScarlettDecodingSourceVoice` live 4:1 `WAVE_FORMAT_IMA_ADPCM` (`0x0011`) -> PCM16 decode bridge
- [x] `WAVE_FORMAT_XMA2` (`0x0166`) detection hook and performance telemetry

### `xmem.dll`
- [x] `XMemAlloc`, `XMemFree`, `XMemSize`, `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`
- [x] 64 KB `VirtualAlloc` physical/graphics backing, aligned heap pools, per-allocator peak & leak report

### `kernelx.dll`
- [x] `TitleMemoryStatus` (13.5 GiB Series X title memory budget + live/peak virtual tracking)
- [x] `VirtualAllocX`, `VirtualFreeX` (`SC_MEM_GRAPHICS` unified memory reservation + unload leak report)
- [x] `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX` (7-core/14-thread Series X topology + safe host mask clamping)

### `AcpHal.dll`, `libHttpClient.dll` / `libHttpClient.GDK.dll`, `XCurl.dll`
- [x] `AcpHalCreate` (`IAcpHal` vtable), `AcpHalAllocateShapeContexts`/`ReleaseShapeContexts`, `ApuAlloc`/`ApuFree`
- [x] `libHttpClient.dll` & `libHttpClient.GDK.dll` (10/10 symbols observed on Desktop `SimpleTriangleDesktop.exe` + async callback completion)
- [x] `XCurl.dll` offline HTTP/cURL request shims

## Upcoming Milestones (Sprints 70-73)
See [`docs/ROADMAP.md`](ROADMAP.md) for the active real-Xbox-Series execution timeline:
- [x] **Sprint 70:** Real 12 KiB `emoose/xvdtool` XVD offsets (`0x280`/`0x284`/`0x288`/`0x28C`/`0x470`), PE/`MicrosoftGame.Config` parser hardening, and `tools/generate_status.py` CI drift check
- [x] **Sprint 71:** Independent-oracle differential RDNA2 shader suite (28 cases across 4 GPU CTests) + reproducible 255-ELF real `gfx1030` corpus sweep (`docs/RDNA2_CORPUS_REPORT.md`)
- [ ] **Sprint 72:** First unmodified **Xbox-platform-targeted (`Gaming.Xbox.Scarlett.x64`)** unencrypted binary boot & verified frame (Tier 2)
- [ ] **Sprint 73:** Multi-title Tier 2 validation, parser fuzzing, second GPU vendor verification, and human loader audit (`TODO(jade)`)

## Sprint 23: DirectStorage Direct-to-VRAM GPU Buffer Streaming
- Added DSTORAGE_DESTINATION_BUFFER to DSTORAGE_DESTINATION union in dstorage_xs.hpp.
- Implemented dedicated D3D12_COMMAND_LIST_TYPE_COPY queue, fence/event sync, and auto-growing persistently mapped UPLOAD staging buffer in ScarlettStorageQueueX.
- Added DSTORAGE_REQUEST_DESTINATION_BUFFER support (raw + SCARLETT_LZ decompression directly into staging -> CopyBufferRegion to DEFAULT VRAM) with GPU telemetry logging.
- Added CTest #22 (DStorageGpuBufferStreaming) verifying bit-for-bit GPU readback and E_INVALIDARG error recording on null GPU resources (22 passed, 0 failed, zero leaks).

## Sprint 24: DirectStorage Direct-to-VRAM GPU Texture Streaming
- Added `DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION` and `DSTORAGE_DESTINATION_TEXTURE_REGION` to `dstorage_xs.hpp`.
- Implemented `ExecuteTextureRequest` in `ScarlettStorageQueueX` using `GetCopyableFootprints`, automatic 256-byte row-pitch staging repacking (for unaligned/sub-region uploads), and `CopyTextureRegion` on the dedicated `COPY` queue.
- Added CTest #23 (`DStorageGpuTextureStreaming`) verifying `SCARLETT_LZ`-compressed 64x64 `RGBA8_UNORM` streaming into a `DEFAULT` heap `TEXTURE2D`, bit-for-bit `DIRECT`-queue readback, and `E_INVALIDARG` error recording on inverted/empty `D3D12_BOX` regions (`23 passed, 0 failed`, zero leaks).

## Sprint 25: d3d12_xs <-> dstorage_xs Bridge & Mip-Chain Streaming
- Added `tools/verify.bat` (fail-fast build + CTest) and `tools/patch_lib.ps1` (strict single-match UTF-8 patch helper).
- Exposed `IID_ScarlettNativeD3D12Device` in `ScarlettDeviceX::QueryInterface` (`d3d12_xs`) and wired `ScarlettStorageQueueX` (`dstorage_xs`) to unwrap shimmed `ID3D12DeviceX*` devices directly (`gpu=yes(d3d12_xs)`).
- Added `DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES` and `ExecuteMultipleSubresourcesRequest` to stream tightly packed mip chains in a single request using `GetCopyableFootprints` repacking on the `COPY` queue.
- Added CTest #24 (`DStorageDeviceXBridgeAndMipChain`) verifying 3-mip `SCARLETT_LZ` streaming through `ID3D12DeviceX*`, bit-for-bit `ID3D12CommandQueueX` readback, and `E_INVALIDARG` on out-of-bounds `FirstSubresource` (`24 passed, 0 failed`, zero leaks).

## Sprint 26: Retail Bring-Up Shims (ScarlettX Collab)
- Updated `gameinput.def` to export `GameInputCreate` at ordinal `@1` for retail GDK import compatibility.
- Added `AcpHal.dll` (`src/shims/acphal/`) implementing `AcpHalCreate` (`IAcpHal` vtable: `Connect`, `Disconnect`, `SubmitCommand`, `PopMessage` with `0x80` disconnect event), `AcpHalAllocateShapeContexts`/`ReleaseShapeContexts`, and aligned `ApuAlloc`/`ApuFree`.
- Added `libHttpClient.GDK.dll` (`src/shims/libhttpclient/`) and `XCurl.dll` (`src/shims/xcurl/`) with offline mock routing for retail bring-up without live network dependencies.
- Added CTest #25 (`RetailBringUpShims`) verifying `gameinput.dll` dynamic hook + ordinal `#1`, `AcpHal` lifecycle/alignment, and `libHttpClient`/`XCurl` mock round-trips (`25 passed, 0 failed`).

## Sprint 27: Loader PE Import Inspector & Unencrypted XVD Container
- Added `src/common/xvd_container.hpp` implementing independent unencrypted XVD header parsing (`msft-xvd` magic at `0x200`), encryption rejection, bounds-checked `ReadUserData`, and a synthetic XVD builder.
- Added `src/loader/pe_inspector.hpp` to audit PE64 import tables against `build/bin/shims/` and classify imports into Shimmed, System (including MSVC Debug/Release CRTs), and Missing.
- Added `loader.exe --inspect <target.exe>` and `--inspect-xvd <file.xvd>` CLI modes in `src/loader/main.cpp`.
- Added CTest #26 (`LoaderInspectAndXvdMount`) verifying PE64 import auditing and unencrypted/encrypted XVD handling (`26 passed, 0 failed`).

## Sprint 28: RDNA2 IMAGE_SAMPLE (MIMG) Texture Sampling
- Added GFX10.3 MIMG `IMAGE_SAMPLE` (`0x3C` / opcode `0x20`) encoding (`AppendMimgImageSample`) and binary decoding in `src/shims/d3d12_xs/rdna2_isa.hpp`.
- Extended `LiftPixelShaderToHlsl` to emit `Texture2D<float4> gTex0 : register(t0)` and `SamplerState gSamp0 : register(s0)` declarations and lower MIMG `IMAGE_SAMPLE` into `gTex0.Sample(gSamp0, float2(...))` with `dmask` channel routing.
- Added CTest #27 (`Rdna2ImageSampleShader`) verifying binary decode, HLSL lifting, and `D3DCompile` (`ps_5_0`) bytecode compilation (`27 passed, 0 failed`).

## Sprint 29: PresentX & Descriptor-Handle Audit
- Updated `PresentX` in `src/shims/d3d12_xs/d3d12_xs.cpp` with `Texture2D` dimension/size validation and a clamped `CopyTextureRegion` fallback when source and swapchain backbuffer dimensions or formats differ.
- Added CTest #28 (`PresentXDescriptorAudit`) verifying all 4 `GetDescriptorHandleIncrementSize` strides, `CreateRenderTargetView` -> `OMSetRenderTargets` -> `ClearRenderTargetView` -> `PresentX` submission, and non-white RGBA GPU readback (`28 passed, 0 failed`).

## Sprint 30: ScarlettX ABI Conformance & Synthetic Game Manifest Suite
- Added CTest #29 (`ScarlettXConformanceSuite`) in `src/tests/scarlett_tests/main.cpp`.
- Verified retail COM vtable slot conformance on `gameinput.dll` (`IGameInput` slots 3/4, `IGameInputReading` slots 3/4) matching ScarlettX's `test_gameinput.cpp`.
- Verified `xgameruntime.dll` initialization, device-type reporting, and manual `XTaskQueue` dispatch lifecycle.
- Audited all 4 ScarlettX synthetic game manifests (`hello_scarlett`, `iat_probe`, `rt_showcase`, `scarlett_arena`) when present (`29 passed, 0 failed`).

## Sprint 31: Native MicrosoftGame.Config Parser & Package Boot
- Added header-only `src/loader/game_config.hpp` parsing `<Identity>`, `<TitleId>`, `<ShellVisuals>`, and `<Executable>` from GDK `MicrosoftGame.Config` manifests.
- Wired `loader.exe --inspect-package <dir>` and `loader.exe --boot <dir> [args...]` into `src/loader/main.cpp` to auto-resolve title executables and forward arguments into the injection pipeline.
- Added CTest #30 (`GameConfigBootParser`) verifying synthetic manifest round-trips, error handling, and all 4 ScarlettX synthetic game manifests (`30 passed, 0 failed`).

## Sprint 32: End-to-End RDNA2 IMAGE_SAMPLE Textured GPU Draw
- Added CTest #31 (`Rdna2TexturedDrawE2E`) in `src/tests/scarlett_tests/main.cpp`.
- Decoded binary RDNA2 `MIMG IMAGE_SAMPLE` pixel shader, lifted to HLSL, compiled to `ps_5_0`, bound a `2x2` `RGBA8` `Texture2D` SRV + static `SamplerState`, drew a fullscreen triangle on D3D12 hardware, and verified exact `(40, 180, 240, 255)` RGBA pixel readback.
- Fixed `AppendExp` argument order (`v0..v3, en, done, compr`) in both `TestRdna2ImageSampleShader` and `TestRdna2TexturedDrawE2E` (`31 passed, 0 failed`).

## Sprint 33: Live Host Input Polling & Keyboard-to-Gamepad Fallback
- Added keyboard-to-virtual-gamepad fallback (WASD -> Left Stick, Arrows -> D-pad, Space/Enter -> A, Esc -> Menu, Shift -> Right Trigger) in `src/shims/gameinput/gameinput.cpp` when no physical XInput controller #0 is connected.
- Exported `ScarlettGameInputSetSyntheticGamepad` (`@2` in `gameinput.def`) for deterministic gamepad state injection.
- Added CTest #32 (`GameInputLivePolling`) verifying `IGameInput::GetCurrentReading`, `GetGamepadState` parity, and monotonic timestamps (`32 passed, 0 failed`). Phase 1 complete.

## Sprint 34: Dynamic GetModuleHandleA/W & GetModuleHandleExA/W Redirects
- Hooked `GetModuleHandleA`, `GetModuleHandleW`, `GetModuleHandleExA`, and `GetModuleHandleExW` in `src/loader/scarlett_hook.cpp` (with safe pass-through for `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` and `nullptr`).
- Redirected runtime Scarlett DLL handle probes to the loaded `shims\` binaries alongside existing `LoadLibrary*` hooks.
- Added CTest #33 (`DynamicLoaderHooks`) verifying `GetModuleHandle*`, `FROM_ADDRESS` pass-through, and dynamic `LoadLibraryW` + `GetProcAddress` resolution (`33 passed, 0 failed`).

## Sprint 35: DirectStorage GPU Buffer Streaming & Fence Synchronization E2E
- Added CTest #34 (`DStorageGpuBufferAndFenceE2E`) in `src/tests/scarlett_tests/main.cpp`.
- Verified end-to-end async streaming from VFS (`T:\dstorage_gpu_stream.bin`) directly into a D3D12 `DEFAULT` heap GPU buffer via `IDStorageQueueX::EnqueueRequest` (`DSTORAGE_REQUEST_DESTINATION_BUFFER`) + `EnqueueSignal(ID3D12Fence*, 1)` with zero error records and exact 4096-byte GPU readback parity (`34 passed, 0 failed`).

## Sprint 36: RDNA2 Compute Shader Lifter (MUBUF Load/Store) & GPU Dispatch E2E
- Added `kMubufLoadDword` (`0x14`) decoding, `AppendMubufLoadDword` encoding, and `StructuredBuffer<uint> gIn : register(t0)` lifting in `src/shims/d3d12_xs/rdna2_isa.hpp`.
- Added CTest #35 (`Rdna2ComputeDispatchE2E`) in `src/tests/scarlett_tests/main.cpp`: wrapped a binary RDNA2 compute shader (`out[i] = 2 * in[i] + 7`) in an SCXS container, translated to `cs_5_0` via `Rdna2IsaTranslator`, dispatched 64 threads on D3D12 hardware with root SRV `t0` + UAV `u0`, and verified all 64 `uint32_t` outputs via GPU readback (`35 passed, 0 failed`).

## Sprint 37: Disk-Backed RDNA2-to-DXBC Shader Translation Cache
- Added `GetShaderCachePath(stage, payloadFnv1a)` and disk cache read/write fast paths to `Rdna2IsaTranslator::Translate` in `src/shims/d3d12_xs/shader_translator.hpp`.
- Warm hits bypass ISA decoding, HLSL lifting, and `D3DCompile`, returning cached DXBC directly with `SCXS(RDNA2-ISA/DISK-CACHE)->`.
- Added CTest #36 (`ShaderDiskCacheE2E`) in `src/tests/scarlett_tests/main.cpp` verifying cold miss -> disk write -> warm cache hit with exact byte-for-byte DXBC parity (`36 passed, 0 failed`).

## Sprint 38: ID3D12GraphicsCommandListX Wrapper Parity & GPU Execution E2E
- Added `IID_ID3D12GraphicsCommandListX` and `ID3D12GraphicsCommandListX` interface in `src/shims/d3d12_xs/d3d12_xs.hpp`.
- Implemented `ScarlettGraphicsCommandListX` in `src/shims/d3d12_xs/d3d12_xs.cpp` and wired `ScarlettDeviceX::CreateCommandList` to return it when `IID_ID3D12GraphicsCommandListX` is requested.
- Added CTest #37 (`D3D12XsCommandListWrapperE2E`) in `src/tests/scarlett_tests/main.cpp`, recording `BeginEvent`, `CopyBufferRegion` (4 KB), `SetMarker`, `InsertWaitOnPresent`, and `EndEvent`, executing via `ID3D12CommandQueueX`, and verifying exact 4096-byte GPU readback (`37 passed, 0 failed`).

## Sprint 39: ID3D12GraphicsCommandListX Render & Descriptor Parity E2E
- Extended `ID3D12GraphicsCommandListX` / `ScarlettGraphicsCommandListX` with `IASetPrimitiveTopology`, `RSSetViewports`, `RSSetScissorRects`, `OMSetRenderTargets`, `ClearRenderTargetView`, `SetDescriptorHeaps`, `SetGraphics/ComputeRootDescriptorTable`, `SetGraphics/ComputeRootConstantBufferView`, and `CopyTextureRegion`.
- Added CTest #38 (`D3D12XsCmdListRenderClearE2E`) verifying a 4x4 `R8G8B8A8_UNORM` render-target clear and `CopyTextureRegion` GPU readback through `ID3D12GraphicsCommandListX` (`38 passed, 0 failed`).

## Sprint 40: Phase 2B Closeout — ID3D12GraphicsCommandListX Draw/Depth/RootConstant Parity & Compute Dispatch E2E
- Added `IASetVertexBuffers`, `IASetIndexBuffer`, `DrawIndexedInstanced`, `ClearDepthStencilView`, `SetGraphicsRoot32BitConstants`, and `SetComputeRoot32BitConstants` to `ID3D12GraphicsCommandListX` / `ScarlettGraphicsCommandListX`.
- Added CTest #39 (`D3D12XsCmdListComputeDispatchE2E`) executing a lifted binary RDNA2 compute shader end-to-end through `ID3D12DeviceX` + `ID3D12GraphicsCommandListX` + `ID3D12CommandQueueX` (`39 passed, 0 failed`).
- Added `AGENTS.md` to repo root and marked **Phase 2B** complete.

## Sprint 41: Phase 3 Audio Runtime E2E (xaudio2_9 + AcpHal)
- Added CTest #40 (`XAudio2RuntimePlaybackE2E`) in `src/tests/scarlett_tests/main.cpp` exercising `shims\xaudio2_9.dll` (`XAudio2Create` -> `CreateMasteringVoice` -> `CreateSourceVoice` -> `SubmitSourceBuffer` -> `GetState` -> `Start`/`Stop`/`FlushSourceBuffers`) and `shims\AcpHal.dll` (`AcpHalCreate` / `AcpHalReleaseShapeContexts`) (`40 passed, 0 failed`).

## Sprint 42: Phase 3 Networking Shims E2E (libHttpClient + XCurl)
- Added CTest #41 (`NetworkingShimsE2E`) in `src/tests/scarlett_tests/main.cpp` dynamically loading `libHttpClient` and `XCurl` shims and validating full mock-backed HTTP request/response lifecycles, headers, write callbacks, and status codes (`41 passed, 0 failed`).

## Sprint 43: XUser Profile Parity & XGameSave Offline E2E
- Added `XUserState`, `XUserLocalId`, and exported `XUserGetId`, `XUserGetState`, and `XUserGetLocalId` in `src/shims/xgameruntime/` (`gdk_types.hpp`, `xgameruntime.def`, `xgameruntime.cpp`).
- Added CTest #42 (`XUserAndGameSaveProfileE2E`) in `src/tests/scarlett_tests/main.cpp` exercising the full offline `XUser` identity + `XGameSave` write/read round-trip (`42 passed, 0 failed`).

## Sprint 44: GameInput Runtime Polling E2E
- Added CTest #43 (`GameInputRuntimePollingE2E`) in `src/tests/scarlett_tests/main.cpp` dynamically loading `shims\gameinput.dll` and validating `GameInputCreate`, monotonic timestamps, synthetic gamepad injection, and `GetCurrentReading` -> `GetGamepadState` parity (`43 passed, 0 failed`).

## Sprint 45: Phase 3 Closeout — Full Scarlett Title Bring-Up E2E
- Added CTest #44 (`ScarlettTitleBringUpE2E`) in `src/tests/scarlett_tests/main.cpp` integrating `xgameruntime` (`XUser` + `XGameSave`), `libHttpClient`, `gameinput`, `xaudio2_9`, and `d3d12_xs` into a single end-to-end boot-to-frame-to-save loop (`44 passed, 0 failed`).
- Marked **Phase 3 (Runtime Bring-Up)** complete at `sprint-45-baseline`.

## Sprint 46: Phase 4 Kickoff — kernelx & pixevt OS/Profiler E2E
- Added CTest #45 (`KernelXAndPixEvtE2E`) in `src/tests/scarlett_tests/main.cpp` dynamically loading `shims\kernelx.dll` and `shims\pixevt.dll` to validate 13.5 GiB `TitleMemoryStatus`, `VirtualAllocX`/`VirtualFreeX`, `GetConsoleProcessorInfo` (7 cores / 14 SMT threads), `SetThreadAffinityMaskX`, and nested `PIXBeginEvent`/`PIXSetMarker`/`PIXEndEvent` calls (`45 passed, 0 failed`).

## Sprint 47: xmem & xg_xs Physical Memory + 64KB_S BC7 Swizzle E2E
- Added CTest #46 (`XMemAndXgXsSwizzleE2E`) in `src/tests/scarlett_tests/main.cpp` dynamically loading `shims\xmem.dll` and `shims\xg_xs.dll` to validate 64 KiB-aligned `XALLOC_MEMTYPE_PHYSICAL` allocations paired with `256x256` 3-mip `BC7_UNORM` `64KB_S` macro- and micro-tiled swizzle/detile round-trips (`46 passed, 0 failed`).

## Sprint 48: dstorage_xs Dynamic Velocity Streaming & Status-Array E2E
- Added CTest #47 (`DStorageVelocityDynamicE2E`) in `src/tests/scarlett_tests/main.cpp` dynamically loading `shims\dstorage_xs.dll` and validating `DStorageSetConfiguration`, `DStorageGetFactory`, high-priority multi-request batching, `IDStorageStatusArrayX` completion, and bit-for-bit memory streaming parity (`47 passed, 0 failed`).

## Sprint 49: Loader Packaging — Unencrypted XVD & MicrosoftGame.config E2E
- Added CTest #48 (`LoaderXvdAndGameConfigE2E`) in `src/tests/scarlett_tests/main.cpp` validating encrypted XVD rejection, unencrypted XVD `ReadUserData` extraction, and `sc::loader::ParseGameConfig` manifest/executable resolution (`48 passed, 0 failed`).

## Sprint 50: Phase 4 Closeout & 50-Sprint Full-Stack Synthetic E2E (`v1.0.0` tag — pending retitle to `v0.x` pre-release)
- Added CTest #49 (`ScarlettFullStack50SprintCapstoneE2E`) in `src/tests/scarlett_tests/main.cpp` uniting all 9 shims (`d3d12_xs`, `xgameruntime`, `gameinput`, `dstorage_xs`, `xmem`, `kernelx`, `pixevt`, `xaudio2_9`, `xg_xs`) plus unencrypted XVD / `MicrosoftGame.Config` packaging into a single end-to-end boot-to-frame-to-save synthetic full-stack pipeline (`49 passed, 0 failed`, zero leaks).
- Completed all **50 Sprints (Phases 1–4)** and tagged **`sprint-50-baseline`** + **`v1.0.0-scarlett-layer`**.

## Sprint 51: Phase 5 Kickoff — PE64 Delay-Load & Ordinal Import Auditing
- Extended `sc::loader::InspectPeImports` in `src/loader/pe_inspector.hpp` to parse `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT` (`IMAGE_DELAYLOAD_DESCRIPTOR`) and count `IMAGE_ORDINAL_FLAG64` ordinal-only imports across both standard and delay-load tables.
- Added CTest #50 (`PeDelayLoadAndOrdinalAuditE2E`) in `src/tests/scarlett_tests/main.cpp` validating synthetic PE64 delay-load and ordinal import auditing (`50 passed, 0 failed`).

## Sprint 52: Runtime Delay-Load IAT Patching & GetProcAddress Ordinal Hook
- Extended `src/loader/scarlett_hook.cpp` to patch `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT` thunks alongside standard IAT entries, hook `GetProcAddress` with ordinal-lookup tracking, and expose `ScarlettHookPatchModuleForTest` / `ScarlettHookGetRuntimeStats`.
- Added CTest #51 (`RuntimeDelayLoadAndOrdinalHookE2E`) in `src/tests/scarlett_tests/main.cpp` validating live in-memory delay-load IAT rewriting and ordinal `GetProcAddress` resolution (`51 passed, 0 failed`).

## Sprint 53: 3D Perspective-Projected Indexed Geometry & Hardware Z-Buffer E2E
- Added CTest #52 (`D3D12Xs3DPerspectiveDepthRenderE2E`) in `src/tests/scarlett_tests/main.cpp` rendering overlapping 3D indexed quads through a `4x4` perspective MVP matrix (`cbuffer b0`) with `D32_FLOAT` hardware depth testing (`D3D12_COMPARISON_FUNC_LESS`) via `ID3D12GraphicsCommandListX`, verifying near/far Z-buffer occlusion via GPU readback (`52 passed, 0 failed`).

## Sprint 54: COM Vtable ABI Conformance
- Added CTest #53 (`D3D12XsComVtableAbiConformanceE2E`) in `src/tests/scarlett_tests/main.cpp` validating raw x64 MSVC COM vtable slot layouts, direct vtable-pointer invocation (`IUnknown` slots 0-2, `ID3D12GraphicsCommandListX::Close` slot 3, `ID3D12CommandQueueX::Signal` slot 4), `IUnknown` identity/reflexivity, monotonic ref-counting, and `E_NOINTERFACE` null-out semantics (`53 passed, 0 failed`).

## Sprint 55: Zero-Readback GPU PresentX & DXGI SwapChain E2E
- Fixed `ScarlettWndProc` / `PumpMessages` in `src/shims/d3d12_xs/d3d12_xs.cpp` to scope message pumping to `m_hwnd` and avoid posting thread-wide `WM_QUIT` on swapchain teardown.
- Upgraded `src/tests/scarlett_3d_demo/main.cpp` to present directly on the GPU via `ID3D12CommandQueueX::PresentX` (DXGI Flip-Discard swapchain), removing CPU readback and GDI blitting.
- Added CTest #54 (`D3D12XsPresentXSwapChainE2E`) in `src/tests/scarlett_tests/main.cpp` verifying `PresentX` argument validation and multi-frame hardware DXGI swapchain presentation (`54 passed, 0 failed`).

## Sprint 57: Headless PresentX Mode (SCARLETT_HEADLESS=1)
- Added `SCARLETT_HEADLESS=1` support in `src/shims/d3d12_xs/d3d12_xs.cpp` (`InitPresentationPipeline` / `PresentX`), omitting `WS_VISIBLE` and presenting with `syncInterval=0` to eliminate CTest window popups and DWM recomposition hitches while preserving windowed VSync for interactive demos.
- Configured `scarlett_shim_suite` in `CMakeLists.txt` with `ENVIRONMENT "SCARLETT_HEADLESS=1"` and added CTest #56 (`D3D12XsHeadlessPresentXModeE2E`) in `src/tests/scarlett_tests/main.cpp` (`56 passed, 0 failed`).

## Sprint 58: Async Compute & Cross-Queue Fence Synchronization
- Updated `ScarlettCommandQueueX` in `src/shims/d3d12_xs/d3d12_xs.cpp`: release backbuffers and `IDXGISwapChain3` before `DestroyWindow`, back `SCARLETT_HEADLESS=1` with offscreen `ID3D12Resource` ring buffers, retain in-flight `PresentX` source resources until their GPU copy fence completes, and register an `ID3D12InfoQueue1` debug callback in `_DEBUG` builds.
- Added CTest #57 (`D3D12XsAsyncComputeCrossQueueSyncE2E`) in `src/tests/scarlett_tests/main.cpp` verifying `D3D12_COMMAND_LIST_TYPE_COMPUTE` and `D3D12_COMMAND_LIST_TYPE_DIRECT` queues running a CS 5.0 producer with GPU-to-GPU `Signal()`/`Wait()` synchronization and readback validation (`57 passed, 0 failed`).

## Sprint 59: Zero-Warning D3D12 Validation & Strict Mode
- Silenced raw `ID3D12CommandList::BeginEvent`/`EndEvent`/`SetMarker` forwarding in `ScarlettGraphicsCommandListX` (`src/shims/d3d12_xs/d3d12_xs.cpp`) to eliminate D3D12 debug-layer `BeginEvent` and unmatched `PixEndEvent` errors.
- Added `SCARLETT_STRICT_D3D12=1` abort enforcement inside the `_DEBUG` `ID3D12InfoQueue1` validation callback.
- Standardized UAV buffer initial states to `D3D12_RESOURCE_STATE_COMMON` and supplied optimized `D3D12_CLEAR_VALUE` structs on render-target creation in `src/tests/scarlett_tests/main.cpp` (`57 passed, 0 failed`, 0 D3D12 debug errors/warnings).

## Sprint 60: Modularize `d3d12_xs` Shim Architecture
- Decomposed the 1,440-line `src/shims/d3d12_xs/d3d12_xs.cpp` monolith into focused translation units behind `d3d12_xs_internal.hpp`: `d3d12_xs_utils.cpp` (93 lines), `command_list_x.cpp` (172 lines), `command_queue_x.cpp` (443 lines), `device_x.cpp` (665 lines), and a thin `d3d12_xs.cpp` export shell (109 lines).
- Cleared the final `ClearRenderTargetView` optimized-clear-value warning in `src/tests/scarlett_tests/main.cpp`.
- Updated `CMakeLists.txt` target sources and verified `57 passed, 0 failed` with zero `d3d12_debug` errors or warnings under `SCARLETT_STRICT_D3D12=1`.

## Sprint 61: Modularize `scarlett_tests` Verification Suite
- Split the 6,500-line `src/tests/scarlett_tests/main.cpp` monolith into `test_harness.hpp` and five subsystem suites: `tests_d3d12_core.cpp` (13 tests), `tests_d3d12_shaders_present.cpp` (11 tests), `tests_storage_swizzle.cpp` (12 tests), `tests_runtime_io.cpp` (12 tests), and `tests_loader_e2e.cpp` (9 tests).
- Collapsed duplicate curl write callbacks (`TestXCurlWriteCb` / `CurlTestWriteCb`) into a single shared `CurlStringWriteCb` helper in `test_harness.hpp`.
- Replaced `main.cpp` with a 125-line table-driven test runner supporting `--list` and `--filter <substring>` CLI flags, verified `57 passed, 0 failed` with zero `d3d12_debug` output under `SCARLETT_STRICT_D3D12=1`.

## Sprint 62: COM/RAII Hygiene, Centralized Config & Warning-Free Polish
- Added `src/common/config.hpp` (`sc::config::IsHeadless`, `IsStrictD3D12`, `GetShimDir`, `ResolveDataDir`) to replace duplicated `GetEnvironmentVariable*` blocks and fixed `MAX_PATH` buffers across `d3d12_xs`, `xgameruntime`, and `scarlett_hook`.
- Added `sc::d3d12xs::MakeCom<Interface, Impl>()` in `d3d12_xs_internal.hpp` and migrated `CreateScarlettDeviceX`, `CreateScarlettCommandQueueX`, and `CreateScarlettGraphicsCommandListX` to return `Microsoft::WRL::ComPtr<T>`, eliminating all raw `new` and manual `->Release()` calls across `d3d12_xs` translation units.
- Verified clean `/W4 /permissive-` build with zero compiler warnings and `57 passed, 0 failed` with zero `d3d12_debug` output under `SCARLETT_STRICT_D3D12=1`.

## Sprint 63: PE64 Implicit TLS, Isolated Guest `atexit` & `XMem` 16-Byte Alignment
- Updated `XMemAllocDefault` (`src/shims/xmem/xmem.cpp`) to enforce a 16-byte minimum alignment floor even for sub-16B alignment codes (matching Xbox Series X|S / `SlimEra` ABI invariants).
- Extended `sc::loader::InspectPeImports` (`src/loader/pe_inspector.hpp`) to parse `IMAGE_DIRECTORY_ENTRY_TLS` (`IMAGE_TLS_DIRECTORY64`) raw template size, zero-fill size, characteristics, and callback table counts.
- Added runtime implicit TLS initialization/teardown (`ScarlettHookInspectModuleTls`, `ScarlettHookInitImplicitTls`, `ScarlettHookFreeImplicitTls`) and an isolated LIFO guest `atexit` registry (`ScarlettHookRegisterGuestAtExit`, `ScarlettHookRunGuestAtExit`, auto-drained on `DLL_PROCESS_DETACH`) to `src/loader/scarlett_hook.cpp`.
- Added `XMemMin16ByteAlignment` and `Pe64ImplicitTlsAndGuestAtExitE2E` (`59 passed, 0 failed`, zero `/W4` warnings, zero `d3d12_debug` output under `SCARLETT_STRICT_D3D12=1`).

## Sprint 64: JIT x64 Auto-Stub Thunk Engine, Symbol-Level Audit CLI & `XAudio2` Export Parity
- Added `src/loader/auto_stub.hpp` (`sc::loader::AutoStubEngine`): W^X x64 ABI trampoline slab (`PAGE_READWRITE` <-> `PAGE_EXECUTE_READ` + `FlushInstructionCache`), deduplicated per `dll!symbol` / `dll!#ord`, policy-driven defaults (`E_NOTIMPL`, `S_OK` for `PIX*`/`Etw*`/`Event*`), runtime policy overrides, and sorted Missing Export Hotlist reporting on `DLL_PROCESS_DETACH` (`SCARLETT_STUB_REPORT`).
- Wired `AutoStubEngine` into `HookGetProcAddress` in `src/loader/scarlett_hook.cpp` (scoped to shimmed DLLs, toggled via `sc::config::IsAutoStubEnabled()` / `SCARLETT_AUTO_STUB=0`).
- Exported `XAudio2CreateWithVersionInfo` and ordinal `@1` (`XAudio2Create`) from `src/shims/xaudio2_9/` after the auto-stub engine immediately surfaced the `<xaudio2.h>` inline lookup gap.
- Added PE64 export-table parsing and symbol-level coverage classification (`sc::loader::InspectPeSymbolCoverage`) in `src/loader/pe_inspector.hpp` and upgraded `loader.exe --inspect` / `inspect` in `src/loader/main.cpp` to print `.tls` metadata and exact implemented vs. auto-stubbed symbol coverage.
- Added `.gitignore` rule for `docs/internal/` and 2 new CTests (`AutoStubThunkSurvivalE2E`, `LoaderSymbolCoverageAuditE2E`) -> `61 passed, 0 failed`, zero `/W4` warnings, zero `d3d12_debug` output.

## Sprint 65: DXIL/DXBC Xbox-Chunk Stripper, Enhanced Barriers & Hardware Feature Caps
- Added `src/shims/d3d12_xs/shader_stripper.hpp` with independent DXBC modified-MD5 digest calculation (`ComputeDxbcDigest`), PC chunk whitelist validation (`HasNonPcDxbcChunks`), and Xbox-chunk stripping + container rebuilding (`StripXboxDxbcChunks`).
- Integrated automatic Xbox-chunk stripping into `ScarlettDeviceX::TranslateStage` (`src/shims/d3d12_xs/device_x.cpp`) with zero-overhead passthrough for clean PC DXBC/DXIL and FNV-1a cache memoization for stripped containers.
- Appended `ID3D12GraphicsCommandListX::Barrier()` (forwarding to `ID3D12GraphicsCommandList7::Barrier` for Enhanced Barriers) and `ID3D12DeviceX::CheckFeatureSupport()` (Mesh Shaders / Sampler Feedback / Enhanced Barriers / Shader Model queries) while preserving all existing COM vtable slots.
- Added `DxbcDigestAndXboxChunkStripperE2E`, `D3D12XsEnhancedBarriersE2E`, and `D3D12XsFeatureCapsQueryE2E` (`64 passed, 0 failed`, zero `/W4` warnings).

## Sprint 66: Fix & Clean — Security, Bounds, XVD Flags, Portable Build, Individual CTests & CI
- **66.1 VFS Path Escape (`src/common/vfs.hpp`):** Replaced fixed `MAX_PATH` `ReadEnvPath` with `sc::config::GetEnvPath`; added component-level path normalization, trailing-dot stripping, mixed-slash handling, >`MAX_PATH` support, and case-insensitive root containment checks rejecting `..` escapes and embedded drive/colon tokens. Added `VfsPathEscapeAndLongPaths`.
- **66.2 XVD Bounds Overflow (`src/common/xvd_container.hpp`):** Replaced overflow-prone `userDataOffset + userDataLength > fileSize` with subtraction-safe bounds checks and added `driveSize` and `embeddedXvdLen` validation.
- **66.3 XVD Encryption Flag & Header Audit (`src/common/xvd_container.hpp`, `docs/SPEC_SOURCES.md`):** Fixed an inverted encryption check against public `emoose/xvdtool` (`kXvdFlagEncryptionDisabled = 0x2u`; encrypted when `(volumeFlags & 0x2u) == 0u`). Previously, `kXvdFlagEncrypted = 0x1u` erroneously accepted encrypted images (`0x0`) and rejected unencrypted read-only images (`0x3`). Also documented an honest header-layout discrepancy: `0x200`/`0x208`/`0x20C`/`0x218` match real XVD/XVC headers (confirmed against a local retail `.xvc` with `volumeFlags=0x41`), while `0x240`/`0x244`/`0x250`/`0x258` are project-assumed (`ASSUMED`) synthetic test offsets (real `ContentType`/`EmbeddedXVDLength`/`UserDataLength` are at `0x284`/`0x288`/`0x28C`). Updated 5 test call sites in `tests_loader_e2e.cpp` and added `XvdFlagsAndBoundsNegative`.
- **66.4 Non-ASCII Paths (`src/common/ansi_path.hpp`, `src/loader/pe_patcher.cpp`, `src/loader/scarlett_hook.cpp`):** Replaced silent lossy `WideCharToMultiByte(CP_ACP, ...)` with `WC_NO_BEST_FIT_CHARS` + `usedDefault` detection and `GetShortPathNameW` 8.3 fallback (failing loudly if still unrepresentable). Routed `HookLoadLibraryA`/`ExA` and `HookGetModuleHandleA`/`ExA` directly through `g_shimsWide` -> wide `g_orig*W` APIs.
- **66.5 Stale Module Handles (`src/loader/scarlett_hook.cpp`):** Hooked `FreeLibrary` to untrack unloaded modules and switched `g_seenModules` to `std::unordered_map<HMODULE, ModuleIdentity>` keyed on PE `(TimeDateStamp, SizeOfImage)` so unhooked unload/reload at the same base address re-patches cleanly. Added `NonAsciiShimAndFreeLibraryHook`.
- **66.6 Portable Build (`tools/build.bat`, `tools/verify.bat`):** Removed hardcoded Visual Studio 18 paths; scripts now use `cl.exe`/`cmake.exe`/`ctest.exe` on `PATH` when available and otherwise locate Visual Studio via `vswhere.exe`.
- **66.7 Individual CTest Targets (`CMakeLists.txt`, `src/tests/scarlett_tests/main.cpp`):** Added `--exact` matching to `scarlett_tests.exe` and registered all **67 tests** individually in CTest with `cpu` (36 tests) and `gpu` (31 tests) labels (`67 passed, 0 failed`, 0 `/W4` warnings).
- **66.8–66.9 Docs, Mojibake, CI & Repo Hygiene:** Fixed double-encoded UTF-8 mojibake across `docs/` and `tools/build_wiki.py`; created `docs/SPEC_SOURCES.md`, reordered `docs/ROADMAP.md` to prioritize external GDK samples, differential shaders, and WARP/CI; added `.github/workflows/ci.yml`, `SECURITY.md`, `CONTRIBUTING.md`, and `.gitattributes`.

## Sprint 67: Microsoft WARP CI Backend, Read-Only Corpus Auditor & Public GDK Sample Alignment
- **WARP Software D3D12 Backend (`src/common/config.hpp`, `src/shims/d3d12_xs/d3d12_xs.cpp`, `.github/workflows/ci.yml`):** Added `sc::config::UseWarp()` (`SCARLETT_USE_WARP=1`) and `IDXGIFactory4::EnumWarpAdapter` routing (with automatic WARP fallback when no hardware adapter is present). Verified all **70/70** tests pass on Microsoft WARP and enabled both `cpu` (38) and `gpu` (32) CTest labels in GitHub Actions Windows CI.
- **Read-Only Local Corpus Auditor (`src/loader/main.cpp`, `src/tests/scarlett_tests/tests_loader_e2e.cpp`):** Added `loader.exe --audit-dir <dir>` to recursively audit external directories read-only (`MicrosoftGame.Config`, PE64 symbol coverage, and XVD/XVC encryption rejection) plus opt-in `SCARLETT_LOCAL_XVC_PATH` / `SCARLETT_LOCAL_CORPUS_DIR` hooks verified against a live encrypted retail `.xvc` without touching encrypted payload bytes.
- **Public Microsoft GDK `261000` & `Xbox-GDK-Samples` Audit (`docs/SPEC_SOURCES.md`):** Compiled unmodified `SimpleTriangleDesktop` against Microsoft Public GDK `261000` (`October 2026`) and booted it through `loader.exe --boot`:
  - `AutoStubEngine` surfaced that real `xgameruntime.dll` exports only 7 service-broker entry points (`InitializeApiImplEx2`, `QueryApiImpl`, etc.) while the 390 `X*` C APIs live in `xgameruntime.thunks.dll` / static `xgameruntime.lib`. Documented this ABI gap in `docs/SPEC_SOURCES.md` for Sprint 68.
  - Exported `HCHttpCallRequestSetRequestBodyString` in `src/shims/libhttpclient/` and staged `shims/libHttpClient.dll` alongside `libHttpClient.GDK.dll`, bringing `loader.exe --inspect` coverage on `SimpleTriangleDesktop.exe` to **10/10 (100.0%)** implemented symbols.
- **Console `SimpleTriangle` `d3d12_xs` Fixes (`src/shims/d3d12_xs/`):** Aligned `PresentX(0, nullptr, nullptr)` to return `S_OK` (clear display planes), updated `WaitFrameEventX` to the real 5-argument GDKX signature `(Event, Timeout, hEvent, Flags, pToken)`, added `ID3D12CommandQueueX::SuspendX`/`ResumeX`, and added upper-16-bit `D3D12XBOX_RESOURCE_FLAG_*` stripping in `CreateCommittedResource`, `CreatePlacedResource`, and `GetResourceAllocationInfo`.
- **Tests (`src/tests/scarlett_tests/`):** Added `WarpAdapterRenderReadback`, `LocalCorpusReadOnlyAudit`, and `D3D12XsGdkSampleAlignmentE2E` -> **70 passed, 0 failed** (`38 cpu`, `32 gpu`) on both NVIDIA RTX 5070 and Microsoft WARP with zero `/W4` warnings.

## Sprint 68: Real GDK `xgameruntime` COM Service Broker & `Scarlett Studio` Desktop GUI
- **`xgameruntime.dll` GDK `261000` Service Broker (`src/shims/xgameruntime/api_broker.cpp`, `gdk_types.hpp`, `xgameruntime.def`):** Reverse-mapped the static thunk dispatch in Microsoft's public `xgameruntime.lib` (`main.obj`) and implemented all 7 real bootstrap exports (`DllCanUnloadNow`, `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport`) plus COM service vtables (`XAsync`/`XTaskQueue`/`XThread`, `XSystem`, `XAnalyticsInfo` 144-byte hidden-sret, `XGame`, `XUser` + `XUserGamertag`, `XGameSave`, `XError`, `XFeature`) while retaining full backward compatibility with flat `X*` exports.
- **`libHttpClient` Async Completion (`src/shims/libhttpclient/libhttpclient.cpp`):** Updated `HCHttpCallPerformAsync` to complete and invoke `XAsyncBlock::callback` cleanly.
- **Unmodified Microsoft GDK Sample Boot:** Booted unmodified `SimpleTriangleDesktop.exe` end-to-end via `loader.exe --boot` using `shims/libHttpClient.dll` (static) and `shims/xgameruntime.dll` (dynamic `InitializeApiImplEx2`), rendering cleanly on RTX 5070 and exiting with `code=0` and zero auto-stub hits.
- **`Scarlett Studio` Desktop GUI (`src/gui/studio_model.hpp`, `src/gui/scarlett_studio.cpp`):** Built `scarlett_studio.exe` — a native Win32 desktop launcher and test hub featuring recursive `MicrosoftGame.Config` / `.exe` / read-only `.xvd`/`.xvc` scanning, pre-flight PE64 shim-coverage inspection, Job-Object process control with live piped log streaming, runtime toggles (`SCARLETT_USE_WARP`, `SCARLETT_HEADLESS`, `SCARLETT_STRICT_D3D12`, `SCARLETT_AUTO_STUB`, `SCARLETT_STUB_REPORT`), one-click GPU/WARP verification runs, and `--self-test`.
- **Tests (`src/tests/scarlett_tests/`):** Added `XGameRuntimeComBrokerE2E` (#71) and `ScarlettStudioSelfTest` (#72) -> **72 passed, 0 failed** (`40 cpu`, `32 gpu`) on both NVIDIA RTX 5070 and Microsoft WARP with zero `/W4` warnings.

## Sprint 69: `Scarlett Studio` Liquid Glass UI Modernization & Pre-Release
- **Direct2D 1.1 + DirectWrite + DirectComposition Liquid Glass Renderer (`src/gui/scarlett_studio.cpp`, `CMakeLists.txt`):** Replaced the Win32 GDI controls in `scarlett_studio.exe` with a Per-Monitor DPI v2 composition swapchain (`DXGI_ALPHA_MODE_PREMULTIPLIED` + `IDCompositionDevice`) layered over Windows 11 DWM Acrylic (`DWMSBT_TRANSIENTWINDOW`), dark mode, and rounded window corners.
- **Glass Cards, Capsule Controls & Telemetry (`src/gui/scarlett_studio.cpp`):** Added frosted squircle cards with top specular sheen and prismatic refraction rims, ambient aurora gradients, pill action buttons (`Launch`, `Stop`, `Scan Folder`, `Add Target`, `Refresh`, `Inspect`, `Tests (GPU)`, `Tests (WARP)`, `Copy Log`, `Clear`), animated capsule switches (`WARP Software GPU`, `Headless Mode`, `Strict D3D12`, `Auto-Stub Engine`, `Stub Hotlist Log`), per-title coverage progress bars, an Inspector hero header, and a syntax-colored scrollable live console.
- **Verification:** Passed all **72/72** CTest targets (`40 cpu`, `32 gpu`) with zero `/W4` warnings and regenerated `docs/wiki/` via `tools/build_wiki.py`.

## Sprint 70: Real XVD Header Offsets, Parser Hardening & Automated Status Sync
- **Real 12 KiB `emoose/xvdtool` XVD Layout (`src/common/xvd_container.hpp`, `docs/SPEC_SOURCES.md`):** Migrated `XvdContainer` off synthetic `0x240..0x258` offsets to real `0x3000` header geometry (`0x280` `Type`, `0x284` `ContentType`, `0x288` `EmbeddedXVDLength`, `0x28C` `uint32_t UserDataLength`, `0x470` `MutableDataPageCount`) with overflow-safe page rounding and hash-tree offset calculation (`0xAA` entries/page + resiliency doubling). Encrypted XVD/XVC containers remain rejected unconditionally.
- **Loader Parser Hardening (`src/loader/game_config.hpp`, `src/loader/pe_inspector.hpp`):** Enforced 4 MiB manifest cap, regular-file check, and package-root lexical containment in `ParseGameConfig` (blocking `..`, rooted paths, drive specifiers/NTFS ADS `:`, and control chars). Centralized bounds-checked PE64 reading (`ReadFileBytesSafe`, `ParseValidatedPe64Headers`, `GetDataDir`), 64-bit overflow-safe `RvaToFileOffset` bounded by `SizeOfRawData`, and descriptor/thunk/export loop caps.
- **Automated Status & CI Drift Check (`tools/generate_status.py`, `docs/STATUS.md`, `.github/workflows/ci.yml`):** Added zero-dependency status generator wired into GitHub Actions Windows CI (`--check`) to keep `docs/STATUS.md`, `README.md`, `docs/ROADMAP.md`, `CONTRIBUTING.md`, and `AGENTS.md` locked to live test/export counts.
- **Verification (`src/tests/scarlett_tests/tests_loader_e2e.cpp`):** Added `XvdRealOffsetsAndGeometry` (#73) and `MalformedPeAndConfigHardening` (#74) -> **74 passed, 0 failed** (`42 cpu`, `32 gpu`) on both NVIDIA RTX 5070 and Microsoft WARP with zero `/W4` warnings.

## Sprint 71: Independent-Oracle RDNA2 Differential Suite & Real `gfx1030` Corpus Sweep
- **28-Case Independent-Oracle Differential Suite (`src/tests/scarlett_tests/tests_rdna2_oracle.cpp`, `rdna2_oracle_harness.hpp`):** Added 4 GPU CTest targets (`Rdna2OracleAluAndCbvDiffE2E`, `Rdna2OracleFp16LdsAndCfDiffE2E`, `Rdna2OracleControlFlowAndRealRgaCsDiffE2E`, `Rdna2OracleGraphicsPipelineDiffE2E`) comparing lifted RDNA2 GPU readback against independent reference HLSL execution across ALU/CBV, FP16/VOP3P, LDS/atomics, uniform/divergent control flow, loops, real RGA `gfx1030` compute shaders, and graphics pipelines on both RTX 5070 and WARP.
- **RDNA2 ISA/Lifter Fixes (`src/shims/d3d12_xs/rdna2_isa.hpp`, `rdna2_lifter.hpp`, `shader_translator.hpp`):** Added branch-aware `s_code_end` decode termination, `AppendDs`/`AppendMubufOp` helpers, portable `cs_5_0` 16-bit split `umul`/`imul`, `saveexec` temporary snapshotting, `[branch]` CFG block dispatch, `kScxsFlagByteAddressed`, and `scxs_v2_` disk cache key bump.
- **Offline `gfx1030` Corpus Sweep (`src/tools/rdna2_sweep/main.cpp`, `tools/rdna2_corpus_sweep.py`, `docs/RDNA2_CORPUS_REPORT.md`):** Swept 255 real MIT-licensed HLSL shaders compiled via `dxc.exe -spirv` -> AMD RGA 2.9.1 (`gfx1030`): **255/255 (100.0%)** decode, **126/255 (49.4%)** strict lift+DXBC compile, and **245/255 (96.1%)** compat lift+DXBC compile.
