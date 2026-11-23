# Scarlett Layer - Roadmap (Sprint 72 Active)

**Current Baseline:** Sprint 72 (in progress) (on top of `v0.69.0-pre`, Apache-2.0) — **78/78 Tier 1 self-authored synthetic CTests passing** (`42 cpu`, `36 gpu`), **0 Tier 2 automated external Xbox-targeted binary tests**, zero `/W4 /permissive-` warnings, zero `d3d12_debug` errors under `SCARLETT_STRICT_D3D12=1`, and GitHub Actions Windows CI running both `cpu` and `gpu` suites on Microsoft WARP.

## Completed Recently (Sprints 63-71)

| Sprint | Completed Scope | Verification |
| :---: | :--- | :--- |
| **63** | PE64 implicit `.tls` inspection/callbacks, isolated guest `atexit`, `XMemAllocDefault` 16-byte minimum alignment | `59/59` Tier 1 tests |
| **64** | W^X x64 `AutoStubEngine`, symbol-level PE coverage auditor (`loader --inspect`), `XAudio2` `@1` + `XAudio2CreateWithVersionInfo` | `61/61` Tier 1 tests |
| **65** | DXBC modified-MD5 digest + Xbox-chunk stripper (`shader_stripper.hpp`), Enhanced Barriers (`Barrier()`), `CheckFeatureSupport()` | `64/64` Tier 1 tests |
| **66** | VFS traversal/long-path hardening, XVD overflow + inverted encryption-bit fix (`emoose/xvdtool`), lossless ANSI/non-ASCII path handling, `FreeLibrary` stale-handle fix, portable `vswhere` build, 67 individual CTests, `SPEC_SOURCES.md`, CI & repo hygiene | `67/67` Tier 1 tests (`36 cpu` / `31 gpu`) |
| **67** | `SCARLETT_USE_WARP=1` software D3D12 backend + full 70/70 WARP CI, `loader --audit-dir` read-only corpus auditor, unmodified GDK `261000` `SimpleTriangleDesktop` build/boot (`libHttpClient.dll` 10/10 parity), `SimpleTriangle` `d3d12_xs` alignment (`PresentX(0)`, 5-arg `WaitFrameEventX`, `SuspendX`/`ResumeX`, Xbox resource-flag stripping) | `70/70` Tier 1 tests (`38 cpu` / `32 gpu`) on RTX 5070 + WARP |
| **68** | Real GDK `261000` `xgameruntime.dll` COM service broker (`InitializeApiImplEx2` / `QueryApiImpl` / `UninitializeApiImpl` + 8 service vtables), `libHttpClient` async callbacks, local unmodified Desktop `SimpleTriangleDesktop.exe` boot (`code=0`, 0 auto-stubs), and `scarlett_studio.exe` desktop GUI launcher | `72/72` Tier 1 tests (`40 cpu` / `32 gpu`) + local Desktop sample boot |
| **69** | Direct2D 1.1 + DirectWrite + DirectComposition Liquid Glass UI on `scarlett_studio.exe`, Apache-2.0 `LICENSE`, regenerated `docs/wiki/`, and `v0.69.0-pre` release archive | `72/72` Tier 1 tests (`40 cpu` / `32 gpu`) on RTX 5070 + WARP |
| **70** | Real 12 KiB `emoose/xvdtool` XVD offsets (`0x280`/`0x284`/`0x288`/`0x28C`/`0x470`) & hash-tree geometry, `game_config.hpp` path containment, `pe_inspector.hpp` bounds/overflow/loop hardening, `tools/generate_status.py` + `docs/STATUS.md` CI drift check | `74/74` Tier 1 tests (`42 cpu` / `32 gpu`) on RTX 5070 + WARP |
| **71** | 28-case independent-oracle RDNA2 differential GPU suite (4 CTest targets: ALU/CBV, FP16/LDS/CF, divergent CF + real RGA `gfx1030` CS, graphics pipeline), lifter fixes (`rdna2_lifter.hpp`, `scxs_v2_` cache bump), `rdna2_sweep.exe` + `tools/rdna2_corpus_sweep.py` across 255 real RGA `gfx1030` ELFs (`100.0%` decode, `49.4%` strict / `96.1%` compat DXBC compile, `docs/RDNA2_CORPUS_REPORT.md`) | `78/78` Tier 1 tests (`42 cpu` / `36 gpu`) on RTX 5070 + WARP |

## Near-Future Timeline: Real Xbox Series Execution First (Sprints 72-73)

**Policy:** Truthfulness and accuracy are the #1 priority. GUI additions (`src/gui/`) and speculative features remain **frozen** until real Xbox Series binaries execute and render. All work below targets concrete, verifiable Xbox Series compatibility (loader, RDNA2 shaders, D3D12.X graphics, and real unencrypted Xbox-targeted executables):

| Sprint | Focus Area | Concrete Deliverables | Acceptance Gate |
| :---: | :--- | :--- | :--- |
| **72** | **Real Xbox Binary Bring-Up (`Terraria` Menus -> Gameplay)** | Move beyond PC Desktop `SimpleTriangleDesktop.exe` to an unmodified, legal, unencrypted **Xbox-platform-targeted** GDK sample or homebrew executable. Drive real `d3d12_xs` COM vtable/GUID fixes, `IDXGIDevice1` QI bridge, `PresentX` / real command submission, and runtime/memory parity directly from real execution traces. | Unmodified Xbox-platform-targeted binary boots through `loader.exe --boot`, renders a verified frame, and is tracked as the first **Tier 2** test. |
| **73** | **Multi-Title Validation, Fuzzing, Second GPU & Human Audit** | Scale Tier 2 validation to multiple unencrypted Xbox-targeted titles (graphics + input + audio + saves); add fuzz harnesses for PE, XVD, `MicrosoftGame.Config`, and VFS parsers; run verification on a second GPU vendor (AMD or Intel) alongside RTX 5070 + WARP; **TODO(jade):** complete independent human Windows-internals review of `src/loader/pe_patcher.cpp` and `src/loader/scarlett_hook.cpp` and retitle the legacy `v1.0.0` GitHub release to `v0.x`. | Reproducible multi-sample Tier 2 runs; fuzzers clean; multi-vendor GPU results documented; human loader audit resolved/tracked. |

## Deferred Backlog (Frozen Until Sprints 70-72 Succeed)

- **Chained `dstorage_xs` Pipeline Parity:** Zlib / GDeflate / BCPack stage hooks, 4-tier priorities, tag cancellation.
- **`GameInput` v2 Parity:** Ring-buffered reading history, 4-motor rumble / impulse trigger state.
- **DXR Acceleration Structures & `XboxBvhBuilder`:** Hardware DXR pass-through + software fallback.
- **`XAudio2_9` + `AcpHal` XMA2 Software Bridge:** Software XMA2-to-PCM16 decode and SHAPE flowgraphs.
- **ERA / GDK System DLLs & Metadata:** `EtwPlus.dll`, `toolhelpx.dll`, MSIXVC2 / `Chunkinfo.xml` metadata-only parsing (strictly no decryption or DRM circumvention).
