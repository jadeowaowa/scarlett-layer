# scarlett-layer

An experimental Windows / Direct3D 12 compatibility layer for running **unpacked, unencrypted** Xbox Series X|S (Scarlett / GDK) x64 executables on a Windows PC.

![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20D3D12-blue?style=flat-square)
[![CI](https://github.com/jadeowaowa/scarlett-layer/actions/workflows/ci.yml/badge.svg)](https://github.com/jadeowaowa/scarlett-layer/actions/workflows/ci.yml)
![Tier 1 Synthetic Checks](https://img.shields.io/badge/tier1_synthetic-74%2F74-2ea44f?style=flat-square)
![Tier 2 Xbox Binaries](https://img.shields.io/badge/tier2_xbox_binaries-0-lightgrey?style=flat-square)
![Status](https://img.shields.io/badge/status-experimental-orange?style=flat-square)

> **Status: experimental (`v0.69.0-pre`).** All 74 automated CI checks today are **Tier 1 self-authored synthetic tests**. No unmodified Xbox-platform-targeted (`Gaming.Xbox.Scarlett.x64`) binary or commercial title runs end-to-end through the layer yet. See [Current status](#current-status).

## What it does

`loader.exe` starts a target executable suspended, rewrites its imports (including delay-load and ordinal imports), and injects `scarlett_hook.dll` before the entry point runs. The hook DLL intercepts `LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress`, and `FreeLibrary`, redirecting console-specific calls to native Windows shims instead of a missing console runtime. The title's directory and the host's `System32` are never modified.

Virtual drives `G:\`, `T:\`, and `S:\` are mounted for the title.

> **Antivirus / EDR note:** Launching a target process suspended, injecting a hook DLL via pre-entrypoint APC, and patching its IAT uses the same OS mechanisms flagged by antivirus and EDR tools. False positives are expected; loader source (`src/loader/`) is kept readable and unobfuscated for direct inspection.

## What it does not do

- It does **not** decrypt or run encrypted retail XVD/XVC packages. Encrypted images are rejected unconditionally.
- It implements **no DRM circumvention** and does not handle leaked game builds, firmware dumps, or console keys. It targets unpacked, unencrypted x64 PE32+ binaries only.
- It is **not** affiliated with or endorsed by Microsoft or Xbox. All trademarks belong to their owners.

## Architecture

```
Unpacked title (MicrosoftGame.Config | unencrypted XVD | title .exe)
  -> loader.exe + scarlett_hook.dll   suspended launch, import rewrite, APC injection, IAT hooks
       |-- d3d12_xs.dll       graphics: device/queue/command-list wrappers, PresentX, async compute, PSO cache, RDNA2 shader lifter
       |-- dstorage_xs.dll    asset streaming: VFS -> async queues -> SCLZ decode -> GPU upload with fences
       |-- xmem / xg_xs       64 KiB physical pools, 2D_THIN / 64KB_S / BC1-BC7 swizzle
       |-- xgameruntime.dll   GDK 261000 COM service broker + XTaskQueue, XUser, XGameSave
       |-- gameinput.dll      XInput, keyboard-to-gamepad fallback
       |-- xaudio2_9 / AcpHal XAudio2 bridge, IMA-ADPCM voice decoding
       |-- libHttpClient / XCurl   HTTP and cURL shims
       `-- kernelx / pixevt   Series X memory budget and CPU topology, PIX markers
  -> native Windows x64 + DirectX 12 GPU
```

Shared runtime flags live in `common/config.hpp`.

## Components

| Component | Binaries | Current Scope |
| --- | --- | --- |
| Loader | `loader.exe`, `scarlett_hook.dll` | Suspended launch, PE import / delay-load / ordinal rewriting, loader-API hooks, `MicrosoftGame.Config` parser, unencrypted XVD mount/inspect, virtual drives |
| Desktop Launcher | `scarlett_studio.exe` | Local launcher, PE shim-coverage inspector, and test runner (GUI work frozen during Sprints 70-72) |
| Graphics | `d3d12_xs.dll` | Modeled D3D12.X device/queue/command-list wrappers, DXGI and headless `PresentX`, frame pacing, async compute with cross-queue fences, disk PSO cache, partial RDNA2 (GFX10.3) shader decoder/lifter, DXBC Xbox-chunk stripper |
| Streaming | `dstorage_xs.dll` | Path translation, worker-threaded async queues, synthetic `SCLZ` decompression, status-array telemetry, direct-to-VRAM buffer/texture/mip-chain streaming |
| Memory and swizzle | `xmem.dll`, `xg_xs.dll` | Pool tracking, 64 KiB physical allocations, 16-byte min alignment, Morton / `64KB_S` / BC1/BC3/BC7 swizzle |
| GDK runtime | `xgameruntime.dll` | GDK `261000` COM service broker (`InitializeApiImplEx2` / `QueryApiImpl`) + flat `X*` exports for `XTaskQueue`, `XUser`, `XGameSave` |
| Input | `gameinput.dll` | XInput pad 0, keyboard-to-gamepad fallback, synthetic test override |
| Audio and network | `xaudio2_9.dll`, `AcpHal.dll`, `libHttpClient.dll`, `libHttpClient.GDK.dll`, `XCurl.dll` | XAudio2 playback with IMA-ADPCM decode, HTTP / cURL shims |
| OS and profiler | `kernelx.dll`, `pixevt.dll` | 13.5 GiB title memory budget, 7-core / 14-thread topology report, PIX event markers |

## Current status

### What is demonstrated today

- **Tier 1 (Self-Authored Synthetic Suite): 74/74 passing** (`42 cpu`, `32 gpu`) on both the author's NVIDIA RTX 5070 and Microsoft WARP (`SCARLETT_USE_WARP=1`), with 0 MSVC `/W4 /permissive-` warnings and 0 D3D12 debug-layer errors. Both `cpu` and `gpu` suites run on GitHub Actions Windows CI via WARP. (Includes 1 opt-in local read-only corpus audit test.)
- **Real D3D12 GPU readback inside Tier 1 tests:** textured RDNA2 draw, compute dispatch, 3D depth rendering, cross-queue fence sync, import/ordinal rewriting, synthetic `SCLZ` streaming, and a synthetic boot-to-frame-to-save test (`ScarlettTitleBringUpE2E`).
- **Local Desktop GDK sample boot:** Unmodified Microsoft Public GDK `261000` `SimpleTriangleDesktop.exe` (**PC Desktop target**, not an Xbox console target) boots, renders, and exits cleanly (`code=0`) through `loader.exe --boot` with 0 auto-stub hits on `shims/libHttpClient.dll` and `shims/xgameruntime.dll`.

### What is **not** demonstrated yet

- **Tier 2 (External Xbox-Platform-Targeted Binaries): 0 automated tests.** No unmodified `Gaming.Xbox.Scarlett.x64` GDK sample, homebrew build, or retail Xbox Series title runs end-to-end yet.
- **Real console data formats & COM ABIs:** `SCLZ` compression, `SCXS` shader containers, and several `ID3D12*X` / `IXG*` / `IDStorage*X` COM interfaces are **project-defined (`ASSUMED`)** rather than matched to real Xbox Series binaries — see [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md).
- **Corpus-scale RDNA2 shader lifting:** Only a small subset of GFX10.3 instructions is exercised today; an independent-oracle differential suite and large-corpus translate/compile sweep are scheduled for Sprint 71.
- **Multi-vendor GPU validation & independent loader audit:** Hardware GPU testing has only been run on one NVIDIA RTX 5070 (plus Microsoft WARP), and `src/loader/pe_patcher.cpp` / `src/loader/scarlett_hook.cpp` still await an independent human Windows-internals review (`TODO(jade)`).

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the Sprints 70-73 execution plan focused on real Xbox Series targets.

## Requirements

- Windows 10 or 11, x64
- A DirectX 12 GPU (or `SCARLETT_USE_WARP=1` for software D3D12 testing)
- Visual Studio 2022+ (MSVC, C++23), CMake 3.25+, Ninja, Python 3

## Build and test

```powershell
git clone https://github.com/jadeowaowa/scarlett-layer.git
cd scarlett-layer

$env:SCARLETT_STRICT_D3D12 = "1"
.\tools\verify.bat --verbose
```

### Runtime flags

| Variable | Effect |
| --- | --- |
| `SCARLETT_STRICT_D3D12=1` | Abort on any D3D12 debug-layer validation error (debug builds) |
| `SCARLETT_USE_WARP=1` | Route D3D12 to Microsoft WARP software rasterizer |
| `SCARLETT_HEADLESS=1` | Route `PresentX` to an offscreen backbuffer ring, with no window or swapchain |
| `SCARLETT_SHIM_DIR` | Directory the loader searches for shim DLLs |

### Running a subset of tests

The test runner is split into five suites: `tests_d3d12_core`, `tests_d3d12_shaders_present`, `tests_storage_swizzle`, `tests_runtime_io`, and `tests_loader_e2e`.

```powershell
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --list
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --filter PresentX
.\build\bin\titles\scarlett_tests\scarlett_tests.exe --exact --filter VfsPathEscapeAndLongPaths
ctest --test-dir build -L cpu --output-on-failure
```

## Documentation

- [`docs/STATUS.md`](docs/STATUS.md): auto-generated shim export, test-tier, and spec-source matrix (`tools/generate_status.py`)
- [`docs/ROADMAP.md`](docs/ROADMAP.md): Sprints 71-73 timeline focused on real Xbox Series targets
- [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md): sourced vs. `ASSUMED` specs and verification boundaries
- [`THIRD_PARTY.md`](THIRD_PARTY.md): public specification provenance and attribution ledger
- [`docs/PROGRESS.md`](docs/PROGRESS.md): sprint-by-sprint engineering log
- [`docs/ARCHITECTURE_PAPER.md`](docs/ARCHITECTURE_PAPER.md): archived Sprint 21 architecture snapshot
- [`CONTRIBUTING.md`](CONTRIBUTING.md) & [`SECURITY.md`](SECURITY.md): contribution rules and security policy
- [`docs/wiki/`](docs/wiki): generated wiki

## Development notes

Source code in this repository was generated with AI assistance (see [`AGENTS.md`](AGENTS.md)), directed and reviewed by the repository owner. Treat the test suite and direct source inspection as the primary safeguards before relying on any subsystem.

## Legal

This project is an independent implementation from public documentation and observed behavior and is not affiliated with or endorsed by Microsoft or Xbox. The author has not intentionally included Microsoft proprietary source code, console firmware, or keys, and the project rejects encrypted retail content by design. Because the code was AI-assisted, formal clean-room certification is not claimed (see [`THIRD_PARTY.md`](THIRD_PARTY.md)). If you believe something here infringes your rights, please open an issue.

## License

Licensed under the [Apache License 2.0](LICENSE).
