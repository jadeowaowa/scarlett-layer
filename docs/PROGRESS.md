# Scarlett Layer â€” Progress & API Coverage

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

## Shim & Export Coverage

### `loader.exe` + `scarlett_hook.dll` + `common/`
- [x] In-memory static & delay-load PE import redirection across all 9 shims
- [x] Pre-entrypoint APC injection of `scarlett_hook.dll`
- [x] Multi-module runtime IAT hooks (`LoadLibraryA/W`, `LoadLibraryExA/W`) across main `.exe` and loaded sub-DLLs
- [x] Virtual console drive mapping (`G:\` game root, `T:\` scratch, `S:\` save temp)
- [x] Zero-dependency `SCLZ` LZ77 block codec + standard IMA-ADPCM (`0x0011`) audio codec
- [x] Automated headless CTest suite (`scarlett_tests.exe`, 21 checks)

### `xgameruntime.dll`
- [x] `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`
- [x] `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`
- [x] `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserCloseHandle`
- [x] `XGameSave*` provider, container, atomic update, blob delete, and undersized-buffer retry

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

## Upcoming Subsystems
- [ ] Async compute command queues (`D3D12_COMMAND_LIST_TYPE_COMPUTE`) & cross-queue GPU fence sync
- [ ] RDNA2 `64KB_S` swizzling & BCn block-compressed texture support (`xg_xs.dll`)
- [ ] Next scheduled revision of `docs/ARCHITECTURE_PAPER.md`: Sprint 21

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
- Added `src/common/xvd_container.hpp` implementing clean-room unencrypted XVD header parsing (`msft-xvd` magic at `0x200`), encryption rejection, bounds-checked `ReadUserData`, and a synthetic XVD builder.
- Added `src/loader/pe_inspector.hpp` to audit PE64 import tables against `build/bin/shims/` and classify imports into Shimmed, System (including MSVC Debug/Release CRTs), and Missing.
- Added `loader.exe --inspect <target.exe>` and `--inspect-xvd <file.xvd>` CLI modes in `src/loader/main.cpp`.
- Added CTest #26 (`LoaderInspectAndXvdMount`) verifying PE64 import auditing and unencrypted/encrypted XVD handling (`26 passed, 0 failed`).

## Sprint 28: RDNA2 IMAGE_SAMPLE (MIMG) Texture Sampling
- Added GFX10.3 MIMG `IMAGE_SAMPLE` (`0x3C` / opcode `0x20`) encoding (`AppendMimgImageSample`) and binary decoding in `src/shims/d3d12_xs/rdna2_isa.hpp`.
- Extended `LiftPixelShaderToHlsl` to emit `Texture2D<float4> gTex0 : register(t0)` and `SamplerState gSamp0 : register(s0)` declarations and lower MIMG `IMAGE_SAMPLE` into `gTex0.Sample(gSamp0, float2(...))` with `dmask` channel routing.
- Added CTest #27 (`Rdna2ImageSampleShader`) verifying binary decode, HLSL lifting, and `D3DCompile` (`ps_5_0`) bytecode compilation (`27 passed, 0 failed`).

## Sprint 29: PresentX & Descriptor-Handle Audit
- Hardened `PresentX` in `src/shims/d3d12_xs/d3d12_xs.cpp` with `Texture2D` dimension/size validation and a clamped `CopyTextureRegion` fallback when source and swapchain backbuffer dimensions or formats differ.
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

## Sprint 50: Phase 4 Closeout & 50-Sprint Golden Master Capstone (`v1.0.0`)
- Added CTest #49 (`ScarlettFullStack50SprintCapstoneE2E`) in `src/tests/scarlett_tests/main.cpp` uniting all 9 shims (`d3d12_xs`, `xgameruntime`, `gameinput`, `dstorage_xs`, `xmem`, `kernelx`, `pixevt`, `xaudio2_9`, `xg_xs`) plus unencrypted XVD / `MicrosoftGame.Config` packaging into a single end-to-end boot-to-frame-to-save Golden Master pipeline (`49 passed, 0 failed`, zero leaks).
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
