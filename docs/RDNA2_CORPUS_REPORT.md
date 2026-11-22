# RDNA2 (`gfx1030`) Corpus Sweep Report

Measured offline using `build/bin/rdna2_sweep.exe` driven by `tools/rdna2_corpus_sweep.py`.

## Corpus & Methodology

- **Corpus directory:** `E:\gta\refs\corpus_gfx1030` (MIT-licensed HLSL shaders from `DirectX-Graphics-Samples` and `Xbox-GDK-Samples`, compiled via official Microsoft `dxc.exe -spirv` -> AMD RGA 2.9.1 `-s vk-spv-offline -c gfx1030` to real `gfx1030` ELF `.text` binaries).
- **Total real `gfx1030` ELFs swept:** **255** (`35690` decoded RDNA2 instructions total; wall time `19.13s`).
- **Modes measured per shader:**
  - **Decode:** `sc::d3d12::rdna2::DecodeProgram()` decodes the full `.text` instruction stream cleanly.
  - **Strict Lift / Compile:** `allowFallbackOps = false`; lifted HLSL compiled to DXBC via `D3DCompile` (`cs_5_0` / `ps_5_0` / `vs_5_0`).
  - **Compat Lift / Compile:** `allowFallbackOps = true`; lifted HLSL compiled to DXBC via `D3DCompile`.

## Results by Stage

| Stage | ELFs | Decode OK | Strict Lift | Strict Compile (DXBC) | Compat Lift | Compat Compile (DXBC) | Decoded Insts |
| :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `cs` | 91 | 91 (100.0%) | 71 (78.0%) | 62 (68.1%) | 91 (100.0%) | 81 (89.0%) | 21279 |
| `ps` | 94 | 94 (100.0%) | 64 (68.1%) | 64 (68.1%) | 94 (100.0%) | 94 (100.0%) | 3633 |
| `vs` | 70 | 70 (100.0%) | 0 (0.0%) | 0 (0.0%) | 70 (100.0%) | 70 (100.0%) | 10778 |
| **Total** | 255 | 255 (100.0%) | 135 (52.9%) | 126 (49.4%) | 255 (100.0%) | 245 (96.1%) | 35690 |

## Top Decode Failures

None (0 failures).

## Top Strict Lift/Compile Failures

| Count | Error |
| ---: | :--- |
| 81 | `unsupported VOP3 opcode 0x365` |
| 29 | `unsupported SOP1 opcode` |
| 8 | `E:\gta\test\scarlett-layer\sweep.hlsl(L,C): error X3663: thread sync operation found in varying flow control, consider reformulating your al` |
| 5 | `SMEM CBV offset out of range for Cb0 (max 256 B)` |
| 1 | `unsupported VOP3 opcode 0x2FF` |
| 1 | `unsupported VOP3 opcode 0x305` |
| 1 | `unsupported VOP3 opcode 0x314` |
| 1 | `E:\gta\test\scarlett-layer\sweep.hlsl(L,C): error X3004: undeclared identifier 'v34'` |
| 1 | `unsupported VOP3 opcode 0x147` |
| 1 | `RDNA2 compute shader contains no MUBUF buffer_store_dword` |

## Top Compat Lift/Compile Failures

| Count | Error |
| ---: | :--- |
| 9 | `E:\gta\test\scarlett-layer\sweep.hlsl(L,C): error X3663: thread sync operation found in varying flow control, consider reformulating your al` |
| 1 | `E:\gta\test\scarlett-layer\sweep.hlsl(L,C): error X3004: undeclared identifier 'v34'` |

## Reproduction

```powershell
python tools/rdna2_corpus_sweep.py --elf-dir E:\gta\refs\corpus_gfx1030 --report docs/RDNA2_CORPUS_REPORT.md
```
