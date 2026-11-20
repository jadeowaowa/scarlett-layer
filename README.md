# Scarlett Layer

Clean-room host-based Xbox Series X (Scarlett / Game Core) translation layer for Windows x64.

## Architecture

- **`loader.exe` (`src/loader/`)**: Spawns target x64 titles suspended, reads the remote PEB to locate `ImageBase`, auto-discovers every `*.dll` inside `build/bin/shims/`, and patches matching static and delay-load PE import descriptors in memory before `ntdll` resolves imports (beating `System32` without modifying title folders).
- **`xgameruntime.dll` (`src/shims/xgameruntime/`)**: Spoofs `XboxScarlettAnaconda` (`0x07`), provides a dual-port `XTaskQueue` + ref-counted `XAsyncBlock` runtime, offline `XUser` sign-in, and crash-safe local disk `XGameSave` persistence under `build/bin/saves/`.
- **`d3d12_xs.dll` (`src/shims/d3d12_xs/`)**: Clean-room COM wrapper over desktop D3D12/DXGI implementing swapchain-less `WaitFrameEventX` / `PresentX` presentation, SRV/RTV descriptor management, and `DXBC`/`DXIL` shader container inspection inside `CreateGraphicsPipelineState`.
- **`gameinput.dll` (`src/shims/gameinput/`)**: Clean-room `IGameInput` / `IGameInputReading` COM shim backed by XInput (gamepad) and focus-aware Win32 keyboard polling.
- **`xg_xs.dll` (`src/shims/xg_xs/`)**: Clean-room `IXGTextureAddressComputer` shim computing console texture alignments, 64 KB tiled layouts, and 8x8 Morton micro-tile swizzle/detile transfers.
- **`dstorage_xs.dll` (`src/shims/dstorage_xs/`)**: Clean-room Velocity / DirectStorage shim (`IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, `IDStorageStatusArrayX`) streaming binary assets asynchronously on background I/O workers with status-array and `ID3D12Fence` signaling.
- **`dummy_title.exe` (`src/tests/dummy_title/`)**: Isolated synthetic Series X title linking strictly to our shims to exercise every subsystem end-to-end.
- **`tools/audit_imports.py`**: Zero-dependency PE32+ import auditor highlighting Xbox/GDK DLL dependencies.

## Build & Run (Windows x64 MSVC + Ninja)

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    build\bin\loader.exe

To inspect the imports of any x64 executable or DLL:

    py tools\audit_imports.py build\bin\titles\dummy_title\dummy_title.exe

See [docs/PROGRESS.md](docs/PROGRESS.md) for the sprint log and API coverage matrix.