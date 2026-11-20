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

## Shim & Export Coverage

### `xgameruntime.dll`
- [x] `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`
- [x] `XSystemGetDeviceType` (spoofs `XboxScarlettAnaconda = 0x07`)
- [x] `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`
- [x] `XAsyncGetStatus`
- [x] `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserCloseHandle`
- [x] `XGameSaveInitializeProviderAsync`, `XGameSaveInitializeProviderResult`, `XGameSaveCloseProvider`
- [x] `XGameSaveCreateContainer`, `XGameSaveCloseContainer`
- [x] `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete`
- [x] `XGameSaveSubmitUpdateAsync`, `XGameSaveSubmitUpdateResult`
- [x] `XGameSaveReadBlobDataAsync`, `XGameSaveReadBlobDataResult`

### `d3d12_xs.dll`
- [x] `D3D12XboxCreateDevice`, `D3D12SerializeRootSignature`
- [x] `ID3D12DeviceX`: `CreateCommandQueueX`, `CreateCommandAllocator`, `CreateCommandList`, `CreateDescriptorHeap`, `GetDescriptorHandleIncrementSize`, `CreateCommittedResource`, `CreateRenderTargetView`, `CreateShaderResourceView`, `CreateFence`, `CreateRootSignature`, `CreateGraphicsPipelineState`, `SetFrameIntervalX`, `ScheduleFrameEventX`, `WaitFrameEventX`
- [x] `ID3D12CommandQueueX`: `ExecuteCommandLists`, `Signal`, `PresentX`
- [x] `DXBC`/`DXIL` container header & chunk table inspector

### `gameinput.dll`
- [x] `GameInputCreate`
- [x] `IGameInput`: `GetCurrentTimestamp`, `GetCurrentReading`
- [x] `IGameInputReading`: `GetInputKind`, `GetTimestamp`, `GetGamepadState`, `GetKeyCount`, `GetKeyState`

### `xg_xs.dll`
- [x] `XGCreateTexture2DComputer`, `XGComputeTexture2DLayout`
- [x] `IXGTextureAddressComputer`: `GetResourceLayout`, `CopyIntoSubresource`, `CopyFromSubresource` (linear + 8x8 Morton `XG_TILE_MODE_2D_THIN`)

### `dstorage_xs.dll`
- [x] `DStorageGetFactory`, `DStorageSetConfiguration`
- [x] `IDStorageFactoryX`: `CreateQueue`, `OpenFile`, `CreateStatusArray`, `SetDebugFlags`, `SetStagingBufferSize`
- [x] `IDStorageQueueX`: `EnqueueRequest` (file & memory sources), `EnqueueStatus`, `EnqueueSignal` (`ID3D12Fence`), `Submit`, `RetrieveErrorRecord`, `GetPendingRequestCount`
- [x] `IDStorageFileX`: `Close`, `GetFileInformation`
- [x] `IDStorageStatusArrayX`: `IsComplete`, `GetHResult`

## Upcoming Subsystems
- [ ] `xmem.dll` (aligned console memory pools + budget telemetry)
- [ ] `pixevt.dll` (PIX event markers)
- [ ] `loader.exe` runtime `LoadLibrary` / `GetProcAddress` hooks + stub generator
- [ ] `xaudio2_9.dll` / XMA audio shim
- [ ] Console RDNA2 / precompiled shader translation pipeline
