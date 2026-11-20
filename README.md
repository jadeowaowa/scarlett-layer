# Scarlett Layer

Clean-room host-based Xbox Series X (Scarlett / Game Core) translation layer for Windows x64.

## Architecture

- **`loader.exe` + `scarlett_hook.dll` (`src/loader/`)**: Spawns target x64 titles suspended, mounts virtual console drives (`G:\`, `T:\`, `S:\`), patches static and delay-load PE imports in memory to redirect Xbox DLLs to `build/bin/shims/`, and injects `scarlett_hook.dll` via pre-entrypoint APC to IAT-hook runtime `LoadLibraryA/W` and `LoadLibraryExA/W` calls across the main executable and loaded sub-DLLs.
- **`common/` (`vfs.hpp`, `codecs.hpp`, `stub.hpp`)**: Virtual console drive translator (`G:\`, `T:\`, `S:\`), `SCLZ` LZ77 streaming block codec, and standard IMA-ADPCM (`0x0011`) audio codec.
- **`xgameruntime.dll` (`src/shims/xgameruntime/`)**: Spoofs `XboxScarlettAnaconda` (`0x07`), provides a dual-port `XTaskQueue` + ref-counted `XAsyncBlock` runtime, offline `XUser` sign-in, and crash-safe local disk `XGameSave` persistence under `build/bin/saves/`.
- **`d3d12_xs.dll` (`src/shims/d3d12_xs/`)**: Clean-room COM wrapper over desktop D3D12/DXGI implementing swapchain-less `WaitFrameEventX` / `PresentX` presentation, explicit `CreateHeap` / `CreatePlacedResource` arenas, graphics + compute PSO pipelines (`CreateGraphicsPipelineState`, `CreateComputePipelineState`, `CreateUnorderedAccessView`), deep `DXBC`/`DXIL` (SM 6.x) inspection, pluggable `IShaderTranslator` (`SCXS`) containers, and persistent disk-backed `ID3D12PipelineLibrary` caching.
- **`gameinput.dll` (`src/shims/gameinput/`)**: Clean-room `IGameInput` / `IGameInputReading` COM shim backed by XInput (gamepad) and focus-aware Win32 keyboard polling.
- **`xg_xs.dll` (`src/shims/xg_xs/`)**: Clean-room `IXGTextureAddressComputer` shim computing console texture alignments, 64 KB tiled layouts, and 8x8 Morton micro-tile swizzle/detile transfers.
- **`dstorage_xs.dll` (`src/shims/dstorage_xs/`)**: Clean-room Velocity / DirectStorage shim with VFS path support (`G:\`, `T:\`, `S:\`), background worker-threaded `SCLZ` CPU decompression, status arrays, error records, and `ID3D12Fence` signaling.
- **`pixevt.dll` (`src/shims/pixevt/`)**: Clean-room PIX event/marker capture shim tested via runtime `LoadLibraryW` redirection.
- **`xaudio2_9.dll` (`src/shims/xaudio2_9/`)**: Real-ABI `IXAudio2` proxy shim bridging console XAudio2 calls onto `System32\xaudio2_9.dll`, featuring a transparent `ScarlettDecodingSourceVoice` for live 4:1 `WAVE_FORMAT_IMA_ADPCM` (`0x0011`) decoding alongside `WAVE_FORMAT_XMA2` inspection and performance telemetry.
- **`xmem.dll` (`src/shims/xmem/`)**: Clean-room console memory manager (`XMemAlloc` / `XMemFree` / `XMemSize`) supporting 64 KB `VirtualAlloc` physical/graphics pages, aligned heap pools, allocator ID tagging, and shutdown peak/leak telemetry.
- **`kernelx.dll` (`src/shims/kernelx/`)**: Clean-room Xbox OS kernel extension shim providing `TitleMemoryStatus` (13.5 GiB title budget), `VirtualAllocX` / `VirtualFreeX` (`SC_MEM_GRAPHICS` tracking), and Series X CPU topology / clamped thread affinity helpers.
- **`dummy_title.exe` + `engine_plugin.dll` + `scarlett_tests.exe` (`src/tests/`)**: Interactive synthetic Series X title (with live CS 5.0 UAV pass), multi-module middleware plugin, and automated headless CTest verification suite.
- **`tools/audit_imports.py`**: Zero-dependency PE32+ static/delay import auditor, shim coverage checker, and C++ stub generator (`--gen-stub`).

## Build, Test & Run (Windows x64 MSVC + Ninja)

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    ctest --test-dir build --output-on-failure
    build\bin\loader.exe

See [docs/PROGRESS.md](docs/PROGRESS.md) for the sprint log and [docs/ARCHITECTURE_PAPER.md](docs/ARCHITECTURE_PAPER.md) for the technical architecture monograph.