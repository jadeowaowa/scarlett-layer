# Scarlett Layer — Roadmap (Sprint 66 Baseline)

**Current Baseline:** `sprint-66-baseline` — **67/67 individual CTests passing** (`36 cpu`, `31 gpu`), zero `/W4 /permissive-` warnings, zero `d3d12_debug` errors under `SCARLETT_STRICT_D3D12=1`, portable `vswhere` build scripts, and GitHub Actions Windows CI (`cpu` suite).

## Completed Recently (Sprints 63–66)

| Sprint | Completed Scope | Verification |
| :---: | :--- | :--- |
| **63** | PE64 implicit `.tls` inspection/callbacks, isolated guest `atexit`, `XMemAllocDefault` 16-byte minimum alignment | `59/59` tests |
| **64** | W^X x64 `AutoStubEngine`, symbol-level PE coverage auditor (`loader --inspect`), `XAudio2` `@1` + `XAudio2CreateWithVersionInfo` | `61/61` tests |
| **65** | DXBC modified-MD5 digest + Xbox-chunk stripper (`shader_stripper.hpp`), Enhanced Barriers (`Barrier()`), `CheckFeatureSupport()` | `64/64` tests |
| **66** | VFS traversal/long-path hardening, XVD overflow + inverted encryption-bit fix (`emoose/xvdtool`), lossless ANSI/non-ASCII path handling, `FreeLibrary` stale-handle fix, portable `vswhere` build, 67 individual CTests, `SPEC_SOURCES.md`, CI & repo hygiene | `67/67` tests (`36 cpu` / `31 gpu`) |
| **67** | `SCARLETT_USE_WARP=1` software D3D12 backend + full 70/70 WARP CI, `loader --audit-dir` read-only corpus auditor, unmodified GDK `261000` `SimpleTriangleDesktop` build/boot (`libHttpClient.dll` 10/10 parity), `SimpleTriangle` `d3d12_xs` alignment (`PresentX(0)`, 5-arg `WaitFrameEventX`, `SuspendX`/`ResumeX`, Xbox resource-flag stripping) | `70/70` tests (`38 cpu` / `32 gpu`) on RTX 5070 + WARP |
| **68** | Real GDK `261000` `xgameruntime.dll` COM service broker (`InitializeApiImplEx2` / `QueryApiImpl` / `UninitializeApiImpl` + 8 service vtables), `libHttpClient` async callbacks, unmodified `SimpleTriangleDesktop.exe` boot (`code=0`, 0 auto-stubs), and `scarlett_studio.exe` desktop GUI launcher & test hub | `72/72` tests (`40 cpu` / `32 gpu`) + unmodified GDK sample boot |
| **69** | Direct2D 1.1 + DirectWrite + DirectComposition Liquid Glass UI on `scarlett_studio.exe` (Windows 11 Acrylic backdrop, specular glass cards, animated capsule toggles, coverage bars, inspector header, color-coded live console) + wiki & pre-release | `72/72` tests (`40 cpu` / `32 gpu`) + `ScarlettStudioSelfTest` |

## Next Sprints (Reordered Around External Validation)

Following an external architecture review, feature expansion (DXR, XMA2, additional shims) is paused until the layer is validated against **external, non-self-authored inputs** and a second execution environment:

| Sprint | Focus Area | Deliverables | Acceptance Test |
| :---: | :--- | :--- | :--- |
| **69** | **Differential RDNA2 Shader Lifter Corpus & Real XVD Offsets** | Expand `rdna2_isa.hpp` / `shader_translator.hpp` with a multi-case differential test corpus comparing lifted RDNA2 shaders against reference HLSL/DXBC outputs (ALU edge cases, CBVs, SRV/UAV loads/stores, FP16/packed ops, control flow); align `XvdContainer` header fields to real `emoose/xvdtool` offsets (`0x284`/`0x288`/`0x28C`); run local verification on a second GPU vendor when available. | Differential corpus runs >= 20 distinct shader cases with exact GPU readback match against reference shaders; real XVD offset parser passes negative/positive suite. |
| **69** | **Differential RDNA2 Shader Lifter Corpus & Real XVD Offsets** | Expand `rdna2_isa.hpp` / `shader_translator.hpp` with a multi-case differential test corpus comparing lifted RDNA2 shaders against reference HLSL/DXBC outputs (ALU edge cases, CBVs, SRV/UAV loads/stores, FP16/packed ops, control flow); align `XvdContainer` header fields to real `emoose/xvdtool` offsets (`0x284`/`0x288`/`0x28C`); run local verification on a second GPU vendor when available. | Differential corpus runs >= 20 distinct shader cases with exact GPU readback match against reference shaders; real XVD offset parser passes negative/positive suite. |
| **70** | **Independent Loader Audit & Release Cleanup** | **TODO(jade):** Arrange independent code review of `src/loader/pe_patcher.cpp` and `src/loader/scarlett_hook.cpp` (suspended-process APC/IAT patching); retitle the `v1.0.0` GitHub release to a `v0.x` pre-release; add `tools/generate_status.py` for factual coverage tables. | All review findings resolved or tracked; GitHub release accurately labeled `v0.x` pre-release; status table auto-generated from CTest + `.def` exports. |

## Deferred Backlog (After External Binaries Run)

These items remain on hold until Sprints 67–70 prove external binary execution and shader correctness:

- **Chained `dstorage_xs` Pipeline Parity:** Zlib / GDeflate / BCPack stage hooks, 4-tier priorities, tag cancellation.
- **`GameInput` v2 Parity:** Ring-buffered reading history, 4-motor rumble / impulse trigger state.
- **DXR Acceleration Structures & `XboxBvhBuilder`:** Hardware DXR pass-through + software fallback.
- **`XAudio2_9` + `AcpHal` XMA2 Software Bridge:** Software XMA2-to-PCM16 decode and SHAPE flowgraphs.
- **ERA / GDK System DLLs & Metadata:** `EtwPlus.dll`, `toolhelpx.dll`, MSIXVC2 / `Chunkinfo.xml` metadata-only parsing (strictly no decryption or DRM circumvention).
