<div align="center">

# scarlett-layer

**Clean-room Windows / D3D12 binary compatibility & shim layer for Xbox Series X|S (Scarlett / GDK) x64 executables.**

[![Release](https://img.shields.io/badge/release-v1.0.0-ff0040?style=flat-square)](https://github.com/jadeowaowa/scarlett-layer/releases/tag/v1.0.0-scarlett-layer) [![Build](https://img.shields.io/badge/build-CMake%20%2B%20Ninja%20%2B%20MSVC%20%2FW4-0078D4?style=flat-square)](#build--verify) [![Tests](https://img.shields.io/badge/CTests-59%2F59%20passing-2ea44f?style=flat-square)](#verification-suite) [![Baseline](https://img.shields.io/badge/baseline-sprint--63-8957e5?style=flat-square)](#delivery-timeline) [![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20D3D12-blue?style=flat-square)](#requirements)

</div>

---

## Overview

`scarlett-layer` is a clean-room host-based translation layer that redirects unmodified x64 game executables to native Windows 11 / DirectX 12 hardware without mutating title directories or host `System32` binaries.

`loader.exe` launches target packages in a suspended state, performs PE import/delay-load/ordinal rewriting and pre-entrypoint APC injection of `scarlett_hook.dll` for recursive multi-module IAT and dynamic loader hooking (`LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress`), mounts `G:\`, `T:\`, and `S:\` virtual filesystems, and routes console API calls into isolated native shims under `shims\`.

> **Scope Note:** Retail encrypted XVD/XVC images are intentionally rejected; `scarlett-layer` operates strictly on unpacked, unencrypted x64 PE32+ binaries and implements no DRM circumvention.

## Architecture

```text
Unpacked Scarlett Title Package (MicrosoftGame.Config | Unencrypted XVD | Title .exe)
  -> loader.exe + scarlett_hook.dll (Suspended PE/delay-load/ordinal rewrite, APC inject, IAT hooks)
     |-- common/config.hpp    : Centralized runtime flags (SCARLETT_HEADLESS, SCARLETT_STRICT_D3D12, SCARLETT_SHIM_DIR)
     |-- d3d12_xs.dll         : Modular device_x / command_queue_x / command_list_x / utils; PresentX (DXGI + headless),
     |                          frame-pacing tokens, async COMPUTE + cross-queue fences, PSO cache, RDNA2 (GFX10.3) lifter
     |-- dstorage_xs.dll      : VFS -> Velocity async queues -> SCLZ decode -> Direct-to-VRAM + ID3D12Fence
     |-- xmem / xg_xs         : 64 KiB physical pools + 2D_THIN / 64KB_S / BC1-BC7 Morton swizzle
     |-- xgameruntime.dll     : XTaskQueue, XUser identity parity, XGameSave local persistence
     |-- gameinput.dll        : Live XInput #0 + Win32 keyboard-to-gamepad + synthetic override
     |-- xaudio2_9 / AcpHal   : Native XAudio2 bridge + real-time 4:1 IMA-ADPCM voice decoding
     |-- libHttpClient / XCurl: GDK HTTP / cURL networking shims
     `-- kernelx / pixevt     : 13.5 GiB title budget, 7c/14t topology, PIX profiler markers
  -> Native Windows x64 / DirectX 12 GPU
```

## Subsystem Coverage (`sprint-62-baseline`)

| Subsystem | Binaries | Status | Highlights |
| :--- | :--- | :---: | :--- |
| **Loader & Packaging** | `loader.exe`, `scarlett_hook.dll` | **Hardened** | Suspended PE/delay-load/ordinal rewrite, APC injection, multi-module `LoadLibrary*` / `GetModuleHandle*` / `GetProcAddress` hooks, `MicrosoftGame.Config` parser, unencrypted XVD mount/inspect, `G:\` / `T:\` / `S:\` VFS |
| **Graphics & Shaders** | `d3d12_xs.dll` | **Modular + Zero-Warn** | Modular translation units (`device_x`, `command_queue_x`, `command_list_x`, `d3d12_xs_utils`, `MakeCom` RAII), DXGI + `SCARLETT_HEADLESS=1` `PresentX`, frame-pacing tokens, 3D depth rendering, async `COMPUTE` cross-queue sync, disk PSO cache, RDNA2 (GFX10.3) VS/PS/CS decoder & HLSL/DXBC lifter, `SCARLETT_STRICT_D3D12=1` validation |
| **Velocity Streaming** | `dstorage_xs.dll` | **Verified** | VFS path translation, worker-threaded async queues, `SCLZ` decompression, `IDStorageStatusArrayX` telemetry, direct-to-VRAM GPU buffer/texture/mip-chain streaming + `ID3D12Fence` |
| **Memory & Swizzle** | `xmem.dll`, `xg_xs.dll` | **Verified** | Leak-free pool tracking, 64 KiB `XALLOC_MEMTYPE_PHYSICAL` allocations, `2D_THIN` & `64KB_S` Morton tiling, BC1/BC3/BC7 macro/micro-tiled swizzle computers |
| **GDK Runtime & Saves** | `xgameruntime.dll` | **Verified** | `XGameRuntimeInitialize`, `XTaskQueue` dispatch, `XUser` identity parity, `XGameSave` container/blob persistence |
| **Input** | `gameinput.dll` | **Verified** | Live XInput #0, Win32 keyboard, virtual keyboard-to-gamepad fallback, synthetic test override |
| **Audio & Networking** | `xaudio2_9.dll`, `AcpHal`, `libHttpClient.GDK.dll`, `XCurl` | **Verified** | Runtime XAudio2 playback + 4:1 IMA-ADPCM decode bridge; HTTP/cURL request/response round-trip shims |
| **OS & Profiler** | `kernelx.dll`, `pixevt.dll` | **Verified** | 13.5 GiB Series X `TitleMemoryStatus`, `VirtualAllocX`, 7-core / 14-thread `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX`, nested `PIXBeginEvent` / `PIXSetMarker` / `PIXEndEvent` |

## Requirements

- **OS:** Windows 10 / 11 (x64)
- **GPU:** DirectX 12 hardware (verified on NVIDIA GeForce RTX 5070)
- **Toolchain:** Visual Studio 2022+ / MSVC (`/W4 /permissive-`, C++23), CMake 3.25+, Ninja, Python 3

## Build & Verify

```powershell
git clone https://github.com/jadeowaowa/scarlett-layer.git
cd scarlett-layer

# Full build + 59/59 CTest run (strict zero-warning D3D12 validation)
$env:SCARLETT_STRICT_D3D12 = "1"
.\tools\verify.bat --verbose
```

### Runtime Flags & Test Filtering

- **`SCARLETT_STRICT_D3D12=1`** — Aborts immediately on any `_DEBUG` `ID3D12InfoQueue1` validation error.
- **`SCARLETT_HEADLESS=1`** — Routes `PresentX` through an offscreen `D3D12_RESOURCE` backbuffer ring with zero Win32/DXGI swapchain window creation.
- **Modular Test CLI (`scarlett_tests.exe`)** — Split across 5 subsystem suites (`tests_d3d12_core`, `tests_d3d12_shaders_present`, `tests_storage_swizzle`, `tests_runtime_io`, `tests_loader_e2e`) with `--list` and `--filter <substring>`:

```powershell
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --list
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --filter PresentX
```

### Verification Suite (`59/59` Passing)

All **59 CTests** execute end-to-end on real D3D12 hardware with **0 `/W4` compiler warnings** and **0 `d3d12_debug` validation errors or warnings**. Key integration benchmarks include:

- **`ScarlettFullStack50SprintCapstoneE2E` & `ScarlettTitleBringUpE2E`** — Full boot-to-frame-to-save runs uniting all shims + unencrypted XVD / `MicrosoftGame.Config` packaging.
- **`D3D12Xs3DPerspectiveDepthRenderE2E` & `D3D12XsAsyncComputeCrossQueueSyncE2E`** — 3D depth-buffered rendering, cross-queue `DIRECT` + `COMPUTE` fence sync, and `PresentX` resource lifetime hardening.
- **`D3D12XsPresentXSwapChainE2E`, `D3D12XsHeadlessPresentXModeE2E` & `D3D12XsFramePacingTokensE2E`** — Live DXGI flip-model presentation, headless offscreen mode, and frame-pipeline token pacing.
- **`Rdna2TexturedDrawE2E` & `Rdna2ComputeDispatchE2E`** — Binary RDNA2 `MIMG IMAGE_SAMPLE` PS draw and `MUBUF` CS `Dispatch` with GPU readback verification.
- **`PeDelayLoadAndOrdinalAuditE2E` & `RuntimeDelayLoadAndOrdinalHookE2E`** — Delay-load IAT rewriting and ordinal-import interception verification.
- **`DStorageDeviceXBridgeAndMipChain` & `DStorageVelocityDynamicE2E`** — `SCLZ` direct-to-VRAM mip-chain streaming through `ID3D12DeviceX*` and dynamic Velocity status-array telemetry.

## Delivery Timeline

| Phase | Sprints | Delivered Scope | Status |
| :--- | :--- | :--- | :---: |
| **Phase 1: Foundation & RDNA2** | Sprints 1–33 | CMake/Ninja scaffold, VFS, `xmem`/`kernelx`, `xg_xs` swizzle, DXIL inspector, XVD/`MicrosoftGame.Config`, RDNA2 textured draw, live GameInput (32 CTests) | **Done** |
| **Phase 2: Hooks, I/O & D3D12_XS** | Sprints 34–40 | `GetModuleHandle*` hooks, `dstorage_xs` GPU streaming + fence E2E, RDNA2 Compute `Dispatch`, `ID3D12GraphicsCommandListX` wrapper, PSO cache hardening (38 CTests) | **Done** |
| **Phase 3: Runtime Bring-Up** | Sprints 41–45 | `xaudio2_9`/`AcpHal` playback, `libHttpClient`/`XCurl` networking, `XUser`/`XGameSave`, `gameinput`, `ScarlettTitleBringUpE2E` (44 CTests) | **Done** |
| **Phase 4: OS, Telemetry & v1.0** | Sprints 46–50 | `kernelx`/`pixevt`, `xmem`/`xg_xs` BC7 swizzle, dynamic `dstorage_xs`, XVD/Config E2E, 50-Sprint Golden Master Capstone (49 CTests) | **Done** |
| **Phase 5: ABI, PresentX & Async Compute** | Sprints 51–58 | Delay-load & ordinal hooks, 3D depth rendering, COM vtable ABI conformance, DXGI + headless `PresentX`, frame-pacing tokens, async compute cross-queue fence sync & `0x87D` lifetime hardening (59 CTests) | **Done** |
| **Phase 6: Architecture Refactor & Hardening** | Sprints 59–62 | `SCARLETT_STRICT_D3D12=1` zero-warning validation, modular `d3d12_xs` (5 TUs), modular `scarlett_tests` (5 suites + `--list`/`--filter`), `common/config.hpp`, `MakeCom` RAII, `/W4` clean build (59 CTests) | **Done** |

## Next 10-Sprint Roadmap (Sprints 63–72)

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full Sprint 63–72 plan covering PE64 implicit TLS + `XMem` 16-byte alignment, GDK 2610 chained `dstorage_xs` Velocity pipelines (`Zlib` -> `BCPack` -> `Swizzle`), D3D12 Enhanced Barriers + DXIL/DXBC Xbox-chunk stripping, DXR / `XboxBvhBuilder`, `GameInput` reading history, software XMA2 decoding, RDNA2 FP16 + differential ISA tests, `EtwPlus`/`toolhelpx`, MSIXVC2/`Chunkinfo.xml`, and automated status-matrix generation.

## Documentation

- **10-Sprint Roadmap (Sprints 63–72):** [`docs/ROADMAP.md`](docs/ROADMAP.md)
- **Sprint Engineering Log:** [`docs/PROGRESS.md`](docs/PROGRESS.md)
- **Architecture Monograph:** [`docs/ARCHITECTURE_PAPER.md`](docs/ARCHITECTURE_PAPER.md)
- **Auto-Generated Wiki & AI Context Snapshot:** [`docs/wiki/`](docs/wiki/)