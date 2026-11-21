<div align="center">

# scarlett-layer

**Native Windows/D3D12 binary compatibility & shim layer for Xbox Series X|S (Scarlett / GDK) titles.**

[![Build](https://img.shields.io/badge/build-CMake%20%2B%20Ninja%20%2B%20MSVC-0078D4?style=flat-square)](#build--verify) [![Tests](https://img.shields.io/badge/CTests-38%2F38%20passing-2ea44f?style=flat-square)](#verification-suite) [![Baseline](https://img.shields.io/badge/baseline-sprint--39-8957e5?style=flat-square)](#delivery-timeline) [![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20D3D12-blue?style=flat-square)](#requirements)

</div>

---

## Overview

`scarlett-layer` intercepts Xbox Series X|S (Scarlett) GDK system calls, dynamic library loads, DirectStorage asset streams, and RDNA2 shader payloads at runtime, redirecting them to native Windows 11 / DirectX 12 hardware without full-system emulation.

`loader.exe` boots a title package, injects `scarlett_hook.dll` to hijack IAT and dynamic loader lookups (`LoadLibrary*`, `GetModuleHandle*`), and routes Scarlett API calls into native shims under `shims\`.

## Architecture

```text
Scarlett Title Package (MicrosoftGame.Config | XVD | Title .exe)
  -> loader.exe + scarlett_hook.dll (Multi-module LoadLibrary* / GetModuleHandle* IAT hooks)
     ├── d3d12_xs.dll    : RDNA2 ISA -> HLSL -> DXBC/D3D12 (VS / PS / CS)
     ├── dstorage_xs.dll : VFS -> Async Queue -> ID3D12Resource + ID3D12Fence
     ├── gameinput.dll   : XInput #0 + Win32 Keyboard -> Virtual Gamepad
     └── xmem/kernelx/xg : Pool tracking + 2D_THIN / 64KB_S / BCn swizzle
  -> Native Windows x64 / DirectX 12 GPU
```

## Subsystem Status (~74% Overall)

| Subsystem | Binaries | Completion | Highlights |
| :--- | :--- | :---: | :--- |
| **Loader & Hooking** | `loader.exe`, `scarlett_hook.dll` | **90%** | `MicrosoftGame.Config` parser, XVD mount/inspect, multi-module `LoadLibrary*` & `GetModuleHandle*` hooks |
| **Input** | `gameinput.dll` | **90%** | Live XInput #0, Win32 keyboard, virtual keyboard-to-gamepad fallback, synthetic test override |
| **VFS & DirectStorage** | `dstorage_xs.dll` | **85%** | VFS path translation, async worker queue, VFS-to-GPU buffer streaming + `ID3D12Fence` signaling |
| **Memory & Swizzle** | `xmem`, `kernelx`, `xg_xs` | **85%** | Leak-free pool tracking, 2D_THIN & 64KB_S Morton tiling, BC1/BC3/BC7 swizzle computers |
| **RDNA2 Lifter & D3D12** | `d3d12_xs.dll` | **55%** | Binary RDNA2 ISA decoder & HLSL lifter (VS/PS/CS), SCXS translator, E2E GPU draw & compute dispatch |
| **Audio & Networking** | `xaudio2_9`, `AcpHal`, `XCurl` | **40%** | Export shims linked and passing baseline suite; runtime stress coverage in Phase 3 |

## Requirements

- **OS:** Windows 10 / 11 (x64)
- **GPU:** DirectX 12 hardware (verified on NVIDIA GeForce RTX 5070)
- **Toolchain:** Visual Studio 2022 / MSVC (`cl.exe`, C++latest), CMake 3.24+, Ninja, Python 3

## Build & Verify

```powershell
git clone [https://github.com/jadeowaowa/scarlett-layer.git](https://github.com/jadeowaowa/scarlett-layer.git)
cd scarlett-layer
.\tools\verify.bat --verbose
```

### Verification Suite

All **38 CTests** run end-to-end on real D3D12 hardware, including:
- **`Rdna2TexturedDrawE2E`** — Binary RDNA2 `MIMG IMAGE_SAMPLE` PS -> HLSL -> `ps_5_0` GPU textured draw + RGBA readback.
- **`Rdna2ComputeDispatchE2E`** — Binary RDNA2 `MUBUF` load/store CS -> SCXS -> `cs_5_0` GPU `Dispatch` + 64-uint UAV readback.
- **`DStorageGpuBufferAndFenceE2E`** — Async 4 KB VFS file -> D3D12 `DEFAULT` buffer + `ID3D12Fence` + readback.
- **`GameInputLivePolling`** & **`DynamicLoaderHooks`** — Live GameInput polling and multi-module IAT redirection.

## Delivery Timeline

| Phase | Sprints | Target Scope | Status |
| :--- | :--- | :--- | :---: |
| **Phase 1A: Foundation** | Sprints 1–29 | CMake/Ninja scaffold, VFS, `xmem`/`kernelx`, `xg_xs` swizzle, DXIL SM6 inspector, XVD loader (28 CTests) | **Done** |
| **Phase 1B: RDNA2 & Manifest** | Sprints 30–31 | Native `MicrosoftGame.Config` parser, binary RDNA2 `MIMG IMAGE_SAMPLE` decoder & lifter (30 CTests) | **Done** |
| **Phase 1C: E2E GPU & Input** | Sprints 32–33 | E2E RDNA2 textured draw on RTX 5070, live XInput + keyboard-to-gamepad fallback (32 CTests) | **Done** |
| **Phase 2A: Hooks, I/O & CS** | Sprints 34–36 | `GetModuleHandle*` hooks, `dstorage_xs` GPU buffer streaming + fence E2E, RDNA2 Compute `Dispatch` E2E (35 CTests) | **Done** |
| **Phase 2B: D3D12_XS Parity** | Sprints 37–40 | `ID3D12GraphicsCommandListX` wrapper coverage, extended RDNA2 ALU/CBV/texture ops, PSO cache hardening | **Next** |
| **Phase 3: Audio, Net & Boot** | Sprints 41–45 | `xaudio2_9`/`AcpHal` runtime audio, `XCurl`/`libHttpClient` stubs, first full Scarlett title bring-up | **Planned** |

## Documentation

Sprint-by-sprint engineering logs live in [`docs/PROGRESS.md`](docs/PROGRESS.md).
