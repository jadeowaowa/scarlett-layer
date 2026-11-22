# Specification Sources & Assumed Contracts

This document separates behaviors backed by public documentation or open-source references from project-assumed contracts (`ASSUMED`). Anything in the `ASSUMED` column must be treated as synthetic scaffolding until matched against public headers or unmodified external binaries.

## 1. XVD / XVC Container Header (`src/common/xvd_container.hpp`)

| Field / Rule | Offset / Value | Status | Source / Notes |
| :--- | :--- | :--- | :--- |
| Header magic (`"msft-xvd"`) | `0x200` (8 bytes) | Sourced | `emoose/xvdtool` (`LibXboxOne/XVD/XVDStructs.cs`); corroborated against local retail Series X\|S `.xvc` header |
| `VolumeFlags` (`uint32_t`) | `0x208` | Sourced | `emoose/xvdtool` (`XVDEnums.cs`): `ReadOnly = 0x1`, `EncryptionDisabled = 0x2`, `DataIntegrityDisabled = 0x4`, `LegacySectorSize = 0x8`, `ResiliencyEnabled = 0x10`, `SraReadOnly = 0x20`, `RegionIdInXts = 0x40`, `EraSpecific = 0x80` |
| Encryption check | `(volumeFlags & 0x2u) == 0u` | Sourced | `emoose/xvdtool` (`XVDFile.cs`): `IsEncrypted => (VolumeFlags & EncryptionDisabled) == 0`. Fixed in Sprint 66 (was previously inverted as `(volumeFlags & 0x1u) != 0`). Encrypted images are rejected unconditionally. |
| `FormatVersion` (`uint32_t`) | `0x20C` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`); observed `3` on retail Series X\|S `.xvc` |
| `DriveSize` (`uint64_t`) | `0x218` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`); bounds-checked against file size in `XvdContainer::Open` |
| Header total size (`0x3000`) | `0x000..0x2FFF` (12 KiB) | Sourced | `emoose/xvdtool` (`XVDFile.cs`): `0x200` RSA signature + `0x2E00` XVD header (`3` pages of `0x1000`). |
| `Type` (`uint32_t`) | `0x280` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`): `0` = Fixed, `1` = Dynamic. |
| `ContentType` (`uint32_t`) | `0x284` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`); observed `1` on retail `.xvc`. Migrated off synthetic `0x240`. |
| `EmbeddedXVDLength` (`uint32_t`) | `0x288` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`). Migrated off synthetic `0x244`. |
| `UserDataLength` (`uint32_t`) | `0x28C` | Sourced | `emoose/xvdtool` (`XVDStructs.cs`). Migrated off synthetic `0x258`. |
| `MutableDataPageCount` (`uint8_t`) | `0x470` (`0x200 + 0x270`) | Sourced | `emoose/xvdtool` (`XVDStructs.cs`); contributes `MutableDataPageCount * 0x1000` bytes before `HashTreeOffset`. |
| `UserDataOffset` (computed) | `0x3000` + aligned embedded XVD + mutable data + hash tree | Sourced | Computed from `emoose/xvdtool` (`XVDFile.cs`) section geometry (`0xAA` hash entries/page when `DataIntegrityDisabled` is clear). |

## 2. Project-Defined & Modeled Formats

| Subsystem | Artifact | Status | Notes |
| :--- | :--- | :--- | :--- |
| Shader container (`SCXS`) | `src/shims/d3d12_xs/shader_translator.hpp` | **ASSUMED** | Project-defined 32-byte wrapper (`FNV-1a` checksum) for testing the RDNA2 ISA lifter seam. Real titles ship DXBC/DXIL containers. |
| Compression (`SCLZ`) | `src/common/codecs.hpp`, `src/shims/dstorage_xs/` | **ASSUMED** | Project-defined LZ77 block stream used to test `dstorage_xs` decompression plumbing. Real titles use Zlib / GDeflate / BCPack. |
| Console COM interfaces | `ID3D12DeviceX`, `ID3D12CommandQueueX`, `ID3D12GraphicsCommandListX`, `IXGTextureAddressComputer`, `IDStorageFactoryX` | **ASSUMED** (partial) | Modeled COM interfaces and IIDs; vtable slot order and GUIDs have not been audited against retail GDK headers/binaries except `gameinput.dll` slots 3/4 and ordinal `@1`. |
| RDNA2 ISA encodings | `src/shims/d3d12_xs/rdna2_isa.hpp` | Sourced (subset) | AMD RDNA2 (GFX10.3) ISA Reference Guide (`SOPP`, `VOP1`, `VOP2`, `VOP3`, `SMEM`, `VINTRP`, `MUBUF`, `MIMG`, `EXP` subset). |
| DXBC modified MD5 digest | `src/shims/d3d12_xs/shader_stripper.hpp` | Sourced | Publicly documented DirectX Shader Compiler (`microsoft/DirectXShaderCompiler`) DXBC hash algorithm. |
| Virtual drive prefixes | `src/common/vfs.hpp` | Sourced (convention) | `G:\` (game root), `T:\` (scratch), `S:\` (save temp) mapped to isolated host directories with traversal rejection. Unmapped drives (e.g., `C:\`) pass through unchanged (`*outWasVirtual = false`). |
| `XMem` 16-byte floor & topology | `src/shims/xmem/`, `src/shims/kernelx/` | Sourced / **ASSUMED** | 16-byte minimum alignment floor, 13.5 GiB title budget, and 7-core / 14-thread Series X topology based on public Xbox Series X specifications. |

## 3. Verification Boundaries

- **TODO(jade):** Audit `src/loader/pe_patcher.cpp` and `src/loader/scarlett_hook.cpp` with an independent reviewer (suspended-process APC/IAT patching).
- **TODO(jade):** Retitle the existing GitHub `v1.0.0` release ("Golden Master") to a `v0.x` pre-release to match the experimental status.

## 4. Microsoft Public GDK 261000 & `Xbox-GDK-Samples` Findings (Sprint 67)

Empirical findings from compiling unmodified `Samples/IntroGraphics/SimpleTriangleDesktop` against Microsoft Public GDK `261000` (`October 2026`) and inspecting `Samples/IntroGraphics/SimpleTriangle`:

| Subsystem | Observed Real GDK Behavior | Status in `scarlett-layer` |
| :--- | :--- | :--- |
| `xgameruntime.dll` | Exports only 7 bootstrap functions (`InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2`, `QueryApiImpl`, `UninitializeApiImpl`, etc.). All 390 `X*` C APIs live in `xgameruntime.thunks.dll` / static `xgameruntime.lib`, which dynamically load `xgameruntime.dll` and invoke `InitializeApiImplEx2` + `QueryApiImpl` to fetch internal service vtables (`SSystemImpl`, `SUsersImpl`, `SAsyncImpl`, etc.). | **Implemented (Sprint 68):** `src/shims/xgameruntime/api_broker.cpp` implements `DllCanUnloadNow`, `InitializeApiImpl`/`Ex`/`Ex2`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport`, and verified GDK `261000` COM service vtables (`XAsync`/`XTaskQueue`, `XSystem`, `XAnalyticsInfo`, `XGame`, `XUser`, `XGameSave`, `XError`, `XFeature`) alongside flat `X*` exports. Unmodified `SimpleTriangleDesktop.exe` boots and exits cleanly (`code=0`) with zero auto-stub hits. |
| `libHttpClient.dll` | Desktop GDK links `libHttpClient.dll` (not `libHttpClient.GDK.dll`) and imports 10 symbols including `HCHttpCallRequestSetRequestBodyString`. | **Aligned (Sprint 67):** Exported `HCHttpCallRequestSetRequestBodyString` and staged `shims/libHttpClient.dll` alongside `libHttpClient.GDK.dll` (10/10 symbols implemented). |
| `d3d12_xs` (`SimpleTriangle`) | Calls `PresentX(0, nullptr, nullptr)` to clear display planes, 5-arg `WaitFrameEventX(..., Flags, &token)`, `SuspendX`/`ResumeX`, `m_d3dDevice.As(&dxgiDevice)`, and ORs console-only `D3D12XBOX_RESOURCE_FLAG_*` bits into `D3D12_RESOURCE_DESC::Flags`. | **Aligned (Sprint 67):** Fixed `PresentX(0, nullptr, nullptr)`, 5-arg `WaitFrameEventX`, `SuspendX`/`ResumeX`, and upper-16-bit Xbox resource flag stripping. `IDXGIDevice1` QI bridge and retail COM GUID/vtable alignment remain open (`ASSUMED`). |
