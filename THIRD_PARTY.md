# Third-Party Provenance & Public References

`scarlett-layer` is licensed under the [Apache License 2.0](LICENSE) and **does not vendor third-party source code**. In particular, this repository contains **no GPL-licensed code** and **no Microsoft proprietary source code, headers, binaries, firmware, or cryptographic keys**.

## Public Specifications & Open-Source References Consulted

All console-facing behavior in this repository is either derived from the public references listed below or explicitly marked `ASSUMED` in [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md):

| Reference | Scope Used in `scarlett-layer` | License / Access |
| :--- | :--- | :--- |
| **`emoose/xvdtool`** (`LibXboxOne/XVD/`) | Public XVD/XVC header magic (`0x200`), `VolumeFlags` (`0x208`, including `EncryptionDisabled = 0x2`), `FormatVersion` (`0x20C`), `DriveSize` (`0x218`), and documented offsets (`0x284`/`0x288`/`0x28C`) for read-only unencrypted container validation. | Public open-source reference (metadata/constants only; no source code copied). |
| **AMD RDNA2 (GFX10.3) ISA Reference Guide** | Instruction encoding layouts (`SOPP`, `VOP1`, `VOP2`, `VOP3`, `SMEM`, `VINTRP`, `MUBUF`, `MIMG`, `EXP` subset) used in `src/shims/d3d12_xs/rdna2_isa.hpp`. | Public AMD hardware documentation. |
| **Microsoft DirectX Shader Compiler (DXC)** | Publicly documented DXBC container structure and modified-MD5 digest specification used in `src/shims/d3d12_xs/shader_stripper.hpp`. | Public specification / NCSA-licensed upstream reference (independent implementation). |
| **Microsoft Public GDK (`261000`) & `Xbox-GDK-Samples`** | Observed public ABI facts from locally compiling the unmodified Desktop GDK sample (`SimpleTriangleDesktop`) and inspecting public import tables (`xgameruntime.dll` broker exports, `libHttpClient.dll` exports). No GDK headers or binaries are committed to this repository. | Public Microsoft documentation & user-installed SDK. |
| **Windows SDK & System Runtime APIs** | Standard Windows x64 host APIs (`Direct3D 12`, `DXGI`, `WARP`, `XAudio2`, `XInput`, `Direct2D 1.1`, `DirectWrite`, `DirectComposition`, `DWM`, Win32). | Standard OS platform APIs linked dynamically on the host. |

## Provenance & Clean-Room Note

As noted in [`README.md`](README.md), source files in this repository were generated through an AI-assisted workflow directed and reviewed by the repository owner. Because a formal two-team clean-room wall is not maintained for AI-generated code, this project does **not** claim formal clean-room certification; instead, it maintains explicit `Sourced` vs. `ASSUMED` tracking in [`docs/SPEC_SOURCES.md`](docs/SPEC_SOURCES.md) and rejects all encrypted retail content by design.

If you identify any unintended attribution gap or rights concern, please open an issue.
