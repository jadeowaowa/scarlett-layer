(`ScarlettTitleBringUpE2E`).

What is **not** demonstrated yet:

- `[TODO: confirm]` Running an unmodified commercial title. The E2E tests use purpose-built test executables, not retail software.
- Compatibility with real console data formats. `SCLZ` compression and `SCXS` shader containers are **project-defined** formats, and several console COM interfaces are modeled rather than matched to real vtables and GUIDs. Real titles will not use them. See `docs/ARCHITECTURE_PAPER.md` section 1.2.
- Testing on GPUs other than the one above.

Planned work (see [`docs/ROADMAP.md`](docs/ROADMAP.md)): chained `dstorage_xs` pipelines (Zlib, BCPack, swizzle), DXR acceleration structures, GameInput v2, software XMA2 decoding, RDNA2 FP16 and a differential shader test corpus, `EtwPlus`/`toolhelpx` shims, MSIXVC2 / `Chunkinfo.xml` handling, and generated status matrices. Already done as of Sprint 65: PE64 implicit TLS, `XMem` 16-byte alignment, auto-stub engine, Xbox-chunk stripping, Enhanced Barriers, and feature-caps queries.

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

> **Note:** `tools\build.bat` currently hard-codes a Visual Studio 18 Build Tools path (`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\...`). Edit it to match your install until this is made portable.

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
```

## Documentation

- [`docs/ROADMAP.md`](docs/ROADMAP.md): planned work
- [`docs/PROGRESS.md`](docs/PROGRESS.md): sprint-by-sprint engineering log
- [`docs/ARCHITECTURE_PAPER.md`](docs/ARCHITECTURE_PAPER.md): detailed design write-up
- [`docs/wiki/`](docs/wiki): generated wiki

## Development notes

All source code in this repository was generated with Gemini, working through an agent workflow (see [`AGENTS.md`](AGENTS.md)), and directed and reviewed by the repository owner. Treat it accordingly: the test suite is the main safeguard, so run it, and read the code before relying on it for anything important.

`[TODO: if true, add one sentence on what you made, reviewed or verified yourself]`

## Legal

This is an independent implementation written to interoperate with software that targets the Xbox GDK. `[TODO: only keep the next sentence if it is accurate]` It was developed from publicly available documentation and observed behavior, without using Microsoft proprietary or confidential source code, headers or SDK material that is not publicly licensed.

## License

Licensed under the [Apache License 2.0](LICENSE).
