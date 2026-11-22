# scarlett-layer

An experimental Windows / Direct3D 12 compatibility layer for running **unpacked, unencrypted** Xbox Series X|S (Scarlett / GDK) x64 executables on a Windows PC.

![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20D3D12-blue?style=flat-square)
[![CI](https://github.com/jadeowaowa/scarlett-layer/actions/workflows/ci.yml/badge.svg)](https://github.com/jadeowaowa/scarlett-layer/actions/workflows/ci.yml)
![Tests](https://img.shields.io/badge/checks-67%2F67-2ea44f?style=flat-square)
![Status](https://img.shields.io/badge/status-experimental-orange?style=flat-square)

> **Status: experimental.** The subsystems below are implemented and exercised by an automated test suite. No commercial title has been run end-to-end through the layer yet. See [Current status](#current-status).

## What it does

`loader.exe` starts a target executable suspended, rewrites its imports (including delay-load and ordinal imports), and injects `scarlett_hook.dll` before the entry point runs. The hook DLL intercepts `LoadLibrary*`, `GetModuleHandle*` and `GetProcAddress`, so console-specific calls are redirected to native Windows shims instead of the missing console runtime. The title's directory and the host's `System32` are never modified.

Virtual drives `G:\`, `T:\` and `S:\` are mounted for the title.

## What it does not do

- It does **not** decrypt or run retail XVD/XVC packages. Encrypted images are rejected on purpose.
- It implements **no DRM circumvention**. It only works on unpacked, unencrypted x64 PE32+ binaries.
- It is **not** affiliated with or endorsed by Microsoft or Xbox. All trademarks belong to their owners.

## Architecture

```
Unpacked title (MicrosoftGame.Config | unencrypted XVD | title .exe)
  -> loader.exe + scarlett_hook.dll   suspended launch, import rewrite, APC injection, IAT hooks
       |-- d3d12_xs.dll       graphics: device/queue/command-list wrappers, PresentX, async compute, PSO cache, RDNA2 shader lifter
       |-- dstorage_xs.dll    asset streaming: VFS -> async queues -> SCLZ decode -> GPU upload with fences
       |-- xmem / xg_xs       64 KiB physical pools, 2D_THIN / 64KB_S / BC1-BC7 swizzle
       |-- xgameruntime.dll   XTaskQueue, XUser, XGameSave
       |-- gameinput.dll      XInput, keyboard-to-gamepad fallback
       |-- xaudio2_9 / AcpHal XAudio2 bridge, IMA-ADPCM voice decoding
       |-- libHttpClient / XCurl   HTTP and cURL shims
       `-- kernelx / pixevt   Series X memory budget and CPU topology, PIX markers
  -> native Windows x64 + DirectX 12 GPU
```

Shared runtime flags live in `common/config.hpp`.

## Components

| Component | Binaries | What it provides |
| --- | --- | --- |
| Loader | `loader.exe`, `scarlett_hook.dll` | Suspended launch, PE import / delay-load / ordinal rewriting, loader-API hooks, `MicrosoftGame.Config` parser, unencrypted XVD mount and inspect, virtual drives |
| Graphics | `d3d12_xs.dll` | Console-style D3D12 device, queue and command-list wrappers, DXGI and headless `PresentX`, frame pacing, async compute with cross-queue fences, disk PSO cache, RDNA2 (GFX10.3) shader decoder and HLSL/DXBC lifter |
| Streaming | `dstorage_xs.dll` | Path translation, worker-threaded async queues, SCLZ decompression, status-array telemetry, direct-to-VRAM buffer, texture and mip-chain streaming |
| Memory and swizzle | `xmem.dll`, `xg_xs.dll` | Pool tracking, 64 KiB physical allocations, Morton tiling, BC1/BC3/BC7 swizzle |
| GDK runtime | `xgameruntime.dll` | Runtime init, task queues, user identity, local save containers |
| Input | `gameinput.dll` | XInput pad 0, keyboard-to-gamepad fallback, synthetic test override |
| Audio and network | `xaudio2_9.dll`, `AcpHal`, `libHttpClient.GDK.dll`, `XCurl` | XAudio2 playback with ADPCM decode, HTTP / cURL request shims |
| OS and profiler | `kernelx.dll`, `pixevt.dll` | 13.5 GiB title memory budget, 7-core / 14-thread topology report, PIX event markers |

## Current status

What is demonstrated today:

- All **67 individual CTest targets** (`36 cpu`, `31 gpu`) pass on the author's machine (NVIDIA RTX 5070), with a warning-free `/W4` build and no D3D12 debug-layer errors. Each test launches `scarlett_tests.exe --exact --filter <TestName>` through `loader.exe`. The 36 `cpu` tests also run on GitHub Actions Windows CI. Current baseline: Sprint 66.
- The tests run real D3D12 work with GPU readback: a textured RDNA2 draw, a compute dispatch, 3D depth rendering, and cross-queue fence sync. They also cover import and ordinal rewriting, SCLZ streaming, and a boot-to-frame-to-save run (`ScarlettTitleBringUpE2E`).

What is **not** demonstrated yet:


- Compatibility with real console data formats and unmodified external binaries. `SCLZ` compression, `SCXS` shader containers, and several console COM interfaces are **project-defined (`ASSUMED`)** rather than matched to retail vtables and GUIDs — see [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md). Real titles will not use them.
- Testing on GPUs other than the one above.

Planned work (see [`docs/ROADMAP.md`](docs/ROADMAP.md)): external validation against Microsoft's public Xbox GDK samples, an expanded differential RDNA2 shader corpus, WARP / second-GPU CI coverage, and real XVD header offset alignment (`0x284`/`0x288`/`0x28C`), before resuming DXR, XMA2, and additional shims.

## Requirements

- Windows 10 or 11, x64
- A DirectX 12 GPU
- Visual Studio 2022+ (MSVC, C++23), CMake 3.25+, Ninja, Python 3

## Build and test

```powershell
git clone https://github.com/jadeowaowa/scarlett-layer.git
cd scarlett-layer

$env:SCARLETT_STRICT_D3D12 = "1"
.\tools\verify.bat --verbose
```

`verify.bat` builds everything and runs the full CTest suite.

### Runtime flags

| Variable | Effect |
| --- | --- |
| `SCARLETT_STRICT_D3D12=1` | Abort on any D3D12 debug-layer validation error (debug builds) |
| `SCARLETT_HEADLESS=1` | Route `PresentX` to an offscreen backbuffer ring, with no window or swapchain |
| `SCARLETT_SHIM_DIR` | Directory the loader searches for shim DLLs |

### Running a subset of tests

The test runner is split into five suites: `tests_d3d12_core`, `tests_d3d12_shaders_present`, `tests_storage_swizzle`, `tests_runtime_io` and `tests_loader_e2e`.

```powershell
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --list
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --filter PresentX
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --exact --filter VfsPathEscapeAndLongPaths
ctest --test-dir build -L cpu --output-on-failure
```

## Documentation

- [`docs/ROADMAP.md`](docs/ROADMAP.md): planned work
- [`docs/PROGRESS.md`](docs/PROGRESS.md): sprint-by-sprint engineering log
- [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md): sourced vs. `ASSUMED` specs and format contracts
- [`docs/ARCHITECTURE_PAPER.md`](docs/ARCHITECTURE_PAPER.md): architecture write-up (archived Sprint 21 snapshot)
- [`docs/wiki/`](docs/wiki): generated wiki

## Development notes

All source code in this repository was generated with Gemini, working through an agent workflow (see [`AGENTS.md`](AGENTS.md)), and directed and reviewed by the repository owner. Treat it accordingly: the test suite is the main safeguard, so run it, and read the code before relying on it for anything important.



## Legal

This project is an independent implementation from public documentation and observed behavior and is not affiliated with or endorsed by Microsoft or Xbox. The author has not intentionally included Microsoft proprietary source code, console firmware, or keys, and the project does not decrypt retail content. Because the code was AI-generated, its exact provenance can't be fully documented. If you believe something here infringes your rights, please open an issue.

## License

Licensed under the [Apache License 2.0](LICENSE).
