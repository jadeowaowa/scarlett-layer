<div align="center">

# scarlett-layer

**Clean-room Windows / D3D12 binary compatibility & shim layer for Xbox Series X|S (Scarlett / GDK) x64 executables.**

[![Release](https://img.shields.io/badge/release-v1.0.0-ff0040?style=flat-square)](https://github.com/jadeowaowa/scarlett-layer/releases/tag/v1.0.0-scarlett-layer) [![Build](https://img.shields.io/badge/build-CMake%20%2B%20Ninja%20%2B%20MSVC-0078D4?style=flat-square)](#build--verify) [![Tests](https://img.shields.io/badge/CTests-54%2F54%20passing-2ea44f?style=flat-square)](#verification-suite) [![Baseline](https://img.shields.io/badge/baseline-sprint--55-8957e5?style=flat-square)](#delivery-timeline) [![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20D3D12-blue?style=flat-square)](#requirements)

</div>

---

## Overview

`scarlett-layer` is a clean-room host-based translation layer that redirects unmodified x64 game executables to native Windows 11 / DirectX 12 hardware without mutating title directories or host `System32` binaries.

`loader.exe` launches target packages in a suspended state, performs PE import rewriting and pre-entrypoint APC injection of `scarlett_hook.dll` for recursive multi-module IAT/dynamic loader hooking (`LoadLibrary*`, `GetModuleHandle*`), mounts `G:\`, `T:\`, and `S:\` virtual filesystems, and routes console API calls into isolated native shims under `shims\`.

> **Scope Note:** Retail encrypted XVD/XVC images are intentionally rejected; `scarlett-layer` operates strictly on unpacked, unencrypted x64 PE32+ binaries and implements no DRM circumvention.

## Architecture

```text
Unpacked Scarlett Title Package (MicrosoftGame.Config | Unencrypted XVD | Title .exe)
  -> loader.exe + scarlett_hook.dll (Suspended PE rewrite, APC inject, multi-module IAT hooks)
     ├── d3d12_xs.dll         : Swapchain-less PresentX, async COMPUTE, PSO cache, RDNA2 (GFX10.3) lifter
     ├── dstorage_xs.dll      : VFS -> Velocity async queues -> SCLZ decode -> Direct-to-VRAM + ID3D12Fence
     ├── xmem / xg_xs         : 64 KiB physical pools + 2D_THIN / 64KB_S / BC1-BC7 Morton swizzle
     ├── xgameruntime.dll     : XTaskQueue, XUser identity parity, XGameSave local persistence
     ├── gameinput.dll        : Live XInput #0 + Win32 keyboard-to-gamepad + synthetic override
     ├── xaudio2_9 / AcpHal   : Native XAudio2 bridge + real-time 4:1 IMA-ADPCM voice decoding
     ├── libHttpClient / XCurl: GDK HTTP / cURL networking shims
     └── kernelx / pixevt     : 13.5 GiB title budget, 7c/14t topology, PIX profiler markers
  -> Native Windows x64 / DirectX 12 GPU
```

## v1.0.0 Subsystem Coverage (50 Sprints Complete)

| Subsystem | Binaries | Status | Highlights |
| :--- | :--- | :---: | :--- |
| **Loader & Packaging** | `loader.exe`, `scarlett_hook.dll` | **v1.0 Complete** | Suspended PE import rewrite, APC injection, multi-module `LoadLibrary*` / `GetModuleHandle*` hooks, `MicrosoftGame.Config` parser, unencrypted XVD mount/inspect, `G:\` / `T:\` / `S:\` VFS |
| **Graphics & Shaders** | `d3d12_xs.dll` | **v1.0 Complete** | Swapchain-less `PresentX` bridge, `ID3D12Heap` placed resources, async `COMPUTE` queues, disk-backed PSO cache, clean-room AMD RDNA2 (GFX10.3) VS/PS/CS decoder & HLSL/DXBC lifter |
| **Velocity Streaming** | `dstorage_xs.dll` | **v1.0 Complete** | VFS path translation, worker-threaded async queues, `SCLZ` decompression, `IDStorageStatusArrayX` telemetry, direct-to-VRAM GPU buffer/texture/mip-chain streaming + `ID3D12Fence` |
| **Memory & Swizzle** | `xmem.dll`, `xg_xs.dll` | **v1.0 Complete** | Leak-free pool tracking, 64 KiB `XALLOC_MEMTYPE_PHYSICAL` allocations, `2D_THIN` & `64KB_S` Morton tiling, BC1/BC3/BC7 macro/micro-tiled swizzle computers |
| **GDK Runtime & Saves** | `xgameruntime.dll` | **v1.0 Complete** | `XGameRuntimeInitialize`, `XTaskQueue` dispatch, `XUser` identity parity, `XGameSave` container/blob persistence |
| **Input** | `gameinput.dll` | **v1.0 Complete** | Live XInput #0, Win32 keyboard, virtual keyboard-to-gamepad fallback, synthetic test override |
| **Audio & Networking** | `xaudio2_9.dll`, `AcpHal`, `libHttpClient.GDK.dll`, `XCurl` | **v1.0 Complete** | Runtime XAudio2 playback + 4:1 IMA-ADPCM decode bridge; HTTP/cURL request/response round-trip shims |
| **OS & Profiler** | `kernelx.dll`, `pixevt.dll` | **v1.0 Complete** | 13.5 GiB Series X `TitleMemoryStatus`, `VirtualAllocX`, 7-core / 14-thread `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX`, nested `PIXBeginEvent` / `PIXSetMarker` / `PIXEndEvent` |

## Requirements

- **OS:** Windows 10 / 11 (x64)
- **GPU:** DirectX 12 hardware (verified on NVIDIA GeForce RTX 5070)
- **Toolchain:** Visual Studio 2022 / MSVC (`cl.exe`, `-std:c++latest`), CMake 3.24+, Ninja, Python 3

## Build & Verify

```powershell
git clone https://github.com/jadeowaowa/scarlett-layer.git
cd scarlett-layer
.\tools\verify.bat --verbose
```

### Verification Suite (`54/54` Passing)

All **54 CTests** execute end-to-end on real D3D12 hardware with zero D3D12 validation errors and zero memory leaks. Key integration benchmarks include:

- **`ScarlettFullStack50SprintCapstoneE2E` (#49)** — Full Golden Master boot-to-frame-to-save run uniting all 9 shims + unencrypted XVD / `MicrosoftGame.Config` packaging in a single pipeline.
- **`ScarlettTitleBringUpE2E` (#44)** — End-to-end `xgameruntime` (`XUser` + `XGameSave`), `libHttpClient`, `gameinput`, `xaudio2_9`, and `d3d12_xs` title bring-up loop.
- **`Rdna2TexturedDrawE2E` & `Rdna2ComputeDispatchE2E`** — Binary RDNA2 `MIMG IMAGE_SAMPLE` PS draw and `MUBUF` CS `Dispatch` with GPU readback verification.
- **`DStorageDeviceXBridgeAndMipChain` & `DStorageVelocityDynamicE2E`** — `SCLZ` direct-to-VRAM mip-chain streaming through `ID3D12DeviceX*` and dynamic Velocity status-array telemetry.
- **`XMemAndXgXsSwizzleE2E` & `KernelXAndPixEvtE2E`** — 64 KiB physical allocations with `64KB_S` BC7 swizzle round-trips, OS budget/topology parity, and PIX profiler hooks.

## Delivery Timeline

| Phase | Sprints | Delivered Scope | Status |
| :--- | :--- | :--- | :---: |
| **Phase 1A: Foundation** | Sprints 1–29 | CMake/Ninja scaffold, VFS, `xmem`/`kernelx`, `xg_xs` swizzle, DXIL SM6 inspector, XVD loader (28 CTests) | **Done** |
| **Phase 1B: RDNA2 & Manifest** | Sprints 30–31 | Native `MicrosoftGame.Config` parser, binary RDNA2 `MIMG IMAGE_SAMPLE` decoder & lifter (30 CTests) | **Done** |
| **Phase 1C: E2E GPU & Input** | Sprints 32–33 | E2E RDNA2 textured draw on RTX 5070, live XInput + keyboard-to-gamepad fallback (32 CTests) | **Done** |
| **Phase 2A: Hooks, I/O & CS** | Sprints 34–36 | `GetModuleHandle*` hooks, `dstorage_xs` GPU buffer streaming + fence E2E, RDNA2 Compute `Dispatch` E2E (35 CTests) | **Done** |
| **Phase 2B: D3D12_XS Parity** | Sprints 37–40 | `ID3D12GraphicsCommandListX` wrapper coverage, extended RDNA2 ALU/CBV/texture ops, PSO cache hardening (38 CTests) | **Done** |
| **Phase 3: Runtime Bring-Up** | Sprints 41–45 | `xaudio2_9`/`AcpHal` playback, `libHttpClient`/`XCurl` networking, `XUser`/`XGameSave`, `gameinput`, `ScarlettTitleBringUpE2E` (44 CTests) | **Done** |
| **Phase 4: OS, Telemetry & v1.0** | Sprints 46–50 | `kernelx`/`pixevt`, `xmem`/`xg_xs` BC7 swizzle, dynamic `dstorage_xs`, XVD/Config E2E, 50-Sprint Golden Master Capstone (49 CTests) | **Done** |

## Future Roadmap (`v2.0+`)

| Milestone | Target Scope |
| :--- | :--- |
| **v1.1 — Binary ABI Conformance** | Retail-exact COM vtable slot & IID alignment (`ID3D12DeviceX`, `IDStorageFactoryX`, `IXGTextureAddressComputer`), ordinal/delay-load hooking, and `LdrLoadDll` interception. |
| **v2.0 — Shader Lifter v2 & Codecs** | Direct RDNA2 -> DXIL (SM 6.6) bitcode emission (Wave Ops, bindless resources, LDS/control flow), BCPack decompression in `dstorage_xs`, XMA2 audio decoding in `xaudio2_9`, and `64KB_D`/`64KB_R` tiling. |
| **v2.5 — Real-World Bring-Up & Tooling** | Unmodified unpacked GDK sample title bring-up, live D3D12 telemetry HUD (`pixevt` + `xmem` + `dstorage_xs`), automatic crash/shader-hash dumps, and CI regression automation. |

## Documentation

- **Sprint Engineering Log:** [`docs/PROGRESS.md`](docs/PROGRESS.md)
- **Architecture Monograph:** [`docs/ARCHITECTURE_PAPER.md`](docs/ARCHITECTURE_PAPER.md)