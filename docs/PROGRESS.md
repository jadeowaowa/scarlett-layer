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
