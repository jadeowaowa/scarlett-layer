# Scarlett Layer â€” Progress & API Coverage

## Sprint Log

| Sprint | Focus | Status |
| :--- | :--- | :--- |
| Sprint 1 | CMake/Ninja/MSVC C++23 scaffold, `sc::log` + `SC_STUB_ONCE`, suspended `loader.exe` | Done |
| Sprint 2 | Dual-port `XTaskQueue`, `XAsyncBlock` engine, offline `XUser` profile | Done |
| Sprint 3 | `d3d12_xs.dll` COM wrapper and `PresentX` -> DXGI Flip-Discard swapchain bridge | Done |
| Sprint 4 | Isolated `shims/` & `titles/` layout, in-memory PE import patcher, `audit_imports.py` | Done |
| Sprint 5 | `d3d12_xs.dll` Root Signatures + PSOs + HLSL draw pipeline, `gameinput.dll` shim | Done |
| Sprint 6 | Shared async runtime + atomic disk-backed `XGameSave` Connected Storage | Done |
| Sprint 7 | `xg_xs.dll` texture layout/swizzle shim, SRV textured pipeline, `DXBC`/`DXIL` chunk inspector | Done |
| Sprint 8 | `dstorage_xs.dll` Velocity async asset streaming + pre-swizzled `.scx` texture pipeline | Done |
| Sprint 9 | `scarlett_hook.dll` runtime `LoadLibrary` IAT hooks, `pixevt.dll`, and `--gen-stub` generator | Done |
| Sprint 10 | `xaudio2_9.dll` real-ABI `IXAudio2` proxy shim, XMA2 inspector, and live procedural audio | Done |
| Sprint 11 | `xmem.dll` aligned console memory pools, allocator tagging, and peak/leak telemetry | Done |

## Shim & Export Coverage

### `loader.exe` + `scarlett_hook.dll`
- [x] In-memory static & delay-load PE import redirection
- [x] Pre-entrypoint APC injection of `scarlett_hook.dll`
- [x] Runtime IAT hooks for `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA`, `LoadLibraryExW`
- [x] `tools/audit_imports.py` coverage report + `--gen-stub` scaffolding

### `xgameruntime.dll`
- [x] `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`
- [x] `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`
- [x] `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserCloseHandle`
- [x] `XGameSave*` provider, container, atomic update, and blob read/write APIs

### `d3d12_xs.dll`
- [x] `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature`
- [x] `ID3D12DeviceX` & `ID3D12CommandQueueX` (`PresentX` -> DXGI Flip-Discard swapchain)
- [x] `DXBC`/`DXIL` container header & chunk table inspector

### `gameinput.dll`
- [x] `GameInputCreate`, `IGameInput`, `IGameInputReading` (XInput + Win32 keyboard)

### `xg_xs.dll`
- [x] `XGCreateTexture2DComputer`, `XGComputeTexture2DLayout`, `IXGTextureAddressComputer`

### `dstorage_xs.dll`
- [x] `DStorageGetFactory`, `DStorageSetConfiguration`, `IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, `IDStorageStatusArrayX`

### `pixevt.dll`
- [x] `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker` (loaded dynamically via `scarlett_hook.dll`)

### `xaudio2_9.dll`
- [x] `XAudio2Create` proxying onto native `System32\xaudio2_9.dll`
- [x] Full `IXAudio2` COM wrapper with `WAVE_FORMAT_XMA2` (`0x0166`) detection hook and performance telemetry

### `xmem.dll`
- [x] `XMemAlloc`, `XMemFree`, `XMemSize`
- [x] `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`
- [x] 64 KB `VirtualAlloc` physical/graphics backing, aligned heap pools, per-allocator peak & leak report

## Upcoming Subsystems
- [ ] Disk-backed PSO pipeline library + multi-module `scarlett_hook` IAT patching
- [ ] Automated CTest shim verification suite + virtual console drive mapping (`G:\`, `T:\`, `S:\`)
- [ ] Real ABI vtable / GUID alignment audit
- [ ] Console RDNA2 shader container & translation research
