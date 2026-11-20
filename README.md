# Scarlett Layer

Clean-room host-based Xbox Series X (Scarlett / Game Core) translation layer for Windows x64.

## Architecture

- **`loader.exe` + `scarlett_hook.dll` (`src/loader/`)**: Spawns target x64 titles suspended, mounts virtual console drives (`G:\`, `T:\`, `S:\`), patches static and delay-load PE imports in memory to redirect Xbox DLLs to `build/bin/shims/`, and injects `scarlett_hook.dll` via pre-entrypoint APC to IAT-hook runtime `LoadLibraryA/W` and `LoadLibraryExA/W` calls across the main executable and loaded sub-DLLs.
- **`common/vfs.hpp` (`src/common/`)**: Virtual console drive translator mapping Xbox `G:\` (game root), `T:\` (scratch), and `S:\` (save temp) paths onto isolated host directories.
- **`xgameruntime.dll` (`src/shims/xgameruntime/`)**: Spoofs `XboxScarlettAnaconda` (`0x07`), provides a dual-port `XTaskQueue` + ref-counted `XAsyncBlock` runtime, offline `XUser` sign-in, and crash-safe local disk `XGameSave` persistence under `build/bin/saves/`.
- **`d3d12_xs.dll` (`src/shims/d3d12_xs/`)**: Clean-room COM wrapper over desktop D3D12/DXGI implementing swapchain-less `WaitFrameEventX` / `PresentX` presentation, SRV/RTV descriptor management, `DXBC`/`DXIL` shader container inspection, and a persistent disk-backed `ID3D12PipelineLibrary` PSO cache under `build/bin/cache/`.
- **`gameinput.dll` (`src/shims/gameinput/`)**: Clean-room `IGameInput` / `IGameInputReading` COM shim backed by XInput (gamepad) and focus-aware Win32 keyboard polling.
- **`xg_xs.dll` (`src/shims/xg_xs/`)**: Clean-room `IXGTextureAddressComputer` shim computing console texture alignments, 64 KB tiled layouts, and 8x8 Morton micro-tile swizzle/detile transfers.
- **`dstorage_xs.dll` (`src/shims/dstorage_xs/`)**: Clean-room Velocity / DirectStorage shim with VFS path support (`G:\`, `T:\`, `S:\`), streaming binary assets asynchronously on background I/O workers with status-array and `ID3D12Fence` signaling.
- **`pixevt.dll` (`src/shims/pixevt/`)**: Clean-room PIX event/marker capture shim tested via runtime `LoadLibraryW` redirection.
- **`xaudio2_9.dll` (`src/shims/xaudio2_9/`)**: Real-ABI `IXAudio2` proxy shim bridging console XAudio2 calls onto `System32\xaudio2_9.dll` with `WAVE_FORMAT_XMA2` format inspection and engine performance telemetry.
- **`xmem.dll` (`src/shims/xmem/`)**: Clean-room console memory manager (`XMemAlloc` / `XMemFree` / `XMemSize`) supporting 64 KB `VirtualAlloc` physical/graphics pages, aligned heap pools, allocator ID tagging, and shutdown peak/leak telemetry.
- **`dummy_title.exe` + `engine_plugin.dll` + `scarlett_tests.exe` (`src/tests/`)**: Interactive synthetic Series X title, multi-module middleware plugin, and automated headless CTest verification suite.
- **`tools/audit_imports.py`**: Zero-dependency PE32+ static/delay import auditor, shim coverage checker, and C++ stub generator (`--gen-stub`).

## Build, Test & Run (Windows x64 MSVC + Ninja)

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    ctest --test-dir build --output-on-failure
    build\bin\loader.exe

To inspect imports and shim coverage on any x64 PE binary:

    py tools\audit_imports.py build\bin\titles\dummy_title\dummy_title.exe

See [docs/PROGRESS.md](docs/PROGRESS.md) for the sprint log and API coverage matrix.