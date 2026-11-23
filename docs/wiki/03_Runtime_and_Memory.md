# 03 — Runtime (`xgameruntime`), Kernel (`kernelx`), Memory (`xmem`), Input & PIX

## `xgameruntime.dll`
**Files:** `src/shims/xgameruntime/api_broker.cpp` (538 L), `src/shims/xgameruntime/game_save.cpp` (485 L), `src/shims/xgameruntime/gdk_types.hpp` (119 L), `src/shims/xgameruntime/runtime_internal.hpp` (104 L), `src/shims/xgameruntime/task_queue.cpp` (361 L), `src/shims/xgameruntime/task_queue.hpp` (21 L), `src/shims/xgameruntime/xgameruntime.cpp` (173 L), `src/shims/xgameruntime/xgameruntime.def` (36 L)

**DLL Exports (`.def`):** `XGameRuntimeInitialize`, `XGameRuntimeUninitialize`, `XSystemGetDeviceType`, `XTaskQueueCreate`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle`, `XAsyncGetStatus`, `XUserAddAsync`, `XUserAddResult`, `XUserGetGamertag`, `XUserGetId`, `XUserGetState`, `XUserGetLocalId`, `XUserCloseHandle`, `XGameSaveInitializeProviderAsync`, `XGameSaveInitializeProviderResult`, `XGameSaveCloseProvider`, `XGameSaveCreateContainer`, `XGameSaveCloseContainer`, `XGameSaveCreateUpdate`, `XGameSaveCloseUpdate`, `XGameSaveSubmitBlobWrite`, `XGameSaveSubmitBlobDelete`, `XGameSaveSubmitUpdateAsync`, `XGameSaveSubmitUpdateResult`, `XGameSaveReadBlobDataAsync`, `XGameSaveReadBlobDataResult`, `DllCanUnloadNow`, `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport`

**Key Enums:**
- `XSystemDeviceType`: Unknown = 0x00, Pc = 0x01, XboxOne = 0x02, XboxOneS = 0x03, XboxOneX = 0x04, XboxOneXDevkit = 0x05, XboxScarlettLockhart = 0x06, XboxScarlettAnaconda = 0x07, XboxScarlettDevkit = 0x08
- `XTaskQueueDispatchMode`: Manual = 0, ThreadPool = 1, SerializedThreadPool = 2, Immediate = 3
- `XTaskQueuePort`: Work = 0, Completion = 1
- `XUserAddOptions`: None = 0x00, AddDefaultUserSilently = 0x01, AllowGuests = 0x02, AddDefaultUserAllowingUI = 0x04
- `XUserState`: SignedIn = 0, SigningOut = 1, SignedOut = 2
- `XUserGamertagComponent`: Classic = 0, Modern = 1, ModernSuffix = 2, UniqueModern = 3

## `kernelx.dll`
**Files:** `src/shims/kernelx/kernelx.cpp` (212 L), `src/shims/kernelx/kernelx.def` (7 L), `src/shims/kernelx/kernelx.hpp` (40 L)

**DLL Exports (`.def`):** `TitleMemoryStatus`, `VirtualAllocX`, `VirtualFreeX`, `GetConsoleProcessorInfo`, `SetThreadAffinityMaskX`

**C Entrypoints:**
- `BOOL WINAPI TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer)`
- `void* WINAPI VirtualAllocX(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect)`
- `BOOL WINAPI VirtualFreeX(void* lpAddress, size_t dwSize, DWORD dwFreeType)`
- `BOOL WINAPI GetConsoleProcessorInfo(SCARLETT_PROCESSOR_INFO* lpInfo)`
- `DWORD_PTR WINAPI SetThreadAffinityMaskX(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)`

## `xmem.dll`
**Files:** `src/shims/xmem/xmem.cpp` (311 L), `src/shims/xmem/xmem.def` (13 L), `src/shims/xmem/xmem.hpp` (62 L)

**DLL Exports (`.def`):** `XMemAlloc`, `XMemFree`, `XMemSize`, `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`, `XMemAllocatePhysicalPages`, `XMemMapPhysicalPages`, `XMemFreePhysicalPages`, `XMemVirtualAlloc`, `XMemGetWorkingSetStatistics`

**C Entrypoints:**
- `void* WINAPI XMemAlloc(size_t dwSize, uint32_t dwAllocAttributes)`
- `void WINAPI XMemFree(void* pAddress, uint32_t dwAllocAttributes)`
- `size_t WINAPI XMemSize(void* pAddress, uint32_t dwAllocAttributes)`
- `void* WINAPI XMemAllocDefault(size_t dwSize, uint32_t dwAllocAttributes)`
- `void WINAPI XMemFreeDefault(void* pAddress, uint32_t dwAllocAttributes)`
- `size_t WINAPI XMemSizeDefault(void* pAddress, uint32_t dwAllocAttributes)`

**Key Enums:**
- `XMemAlignment`: XALLOC_ALIGNMENT_DEFAULT = 0, XALLOC_ALIGNMENT_4 = 1, XALLOC_ALIGNMENT_8 = 2, XALLOC_ALIGNMENT_16 = 3, XALLOC_ALIGNMENT_32 = 4, XALLOC_ALIGNMENT_64 = 5, XALLOC_ALIGNMENT_128 = 6, XALLOC_ALIGNMENT_256 = 7, XALLOC_ALIGNMENT_512 = 8, XALLOC_ALIGNMENT_1K = 9, XALLOC_ALIGNMENT_2K = 10, XALLOC_ALIGNMENT_4K = 11, XALLOC_ALIGNMENT_8K = 12, XALLOC_ALIGNMENT_16K = 13, ... (+2 more)
- `XMemType`: XALLOC_MEMTYPE_HEAP = 0, XALLOC_MEMTYPE_PHYSICAL = 1
- `XMemAllocatorId`: XALLOC_ID_DEFAULT = 0, XALLOC_ID_TEXTURE_TILED = 10, XALLOC_ID_TEXTURE_UPLOAD = 11, XALLOC_ID_AUDIO_PCM = 20

## `gameinput.dll`
**Files:** `src/shims/gameinput/gameinput.cpp` (427 L), `src/shims/gameinput/gameinput.def` (4 L), `src/shims/gameinput/gameinput.hpp` (163 L)

**DLL Exports (`.def`):** `GameInputCreate @1`, `ScarlettGameInputSetSyntheticGamepad @2`

**C Entrypoints:**
- `HRESULT WINAPI GameInputCreate(IGameInput** gameInput)`
- `void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state)`

**COM Interface `IGameInputReading : IUnknown`:**
- `[03]` `GameInputKind GetInputKind()`
- `[04]` `uint64_t GetTimestamp()`
- `[05]` `bool GetGamepadState(GameInputGamepadState* state)`
- `[06]` `uint32_t GetKeyCount()`
- `[07]` `uint32_t GetKeyState( uint32_t stateArrayCount, GameInputKeyState* stateArray)`

**COM Interface `IGameInput : IUnknown`:**
- `[03]` `uint64_t GetCurrentTimestamp()`
- `[04]` `HRESULT GetCurrentReading( GameInputKind inputKind, void* device, IGameInputReading** reading)`
- `[05]` `HRESULT GetNextReading( IGameInputReading* referenceReading, GameInputKind inputKind, void* device, IGameInputReading** reading)`
- `[06]` `HRESULT GetPreviousReading( IGameInputReading* referenceReading, GameInputKind inputKind, void* device, IGameInputReading** reading)`
- `[07]` `HRESULT GetTemporalReading( uint64_t timestamp, void* device, IGameInputReading** reading)`
- `[08]` `HRESULT RegisterReadingCallback( void* device, GameInputKind inputKind, float analogThreshold, void* context, void* callbackFunc, uint64_t* callbackToken)`
- `[09]` `HRESULT RegisterDeviceCallback( void* device, GameInputKind inputKind, uint32_t statusFilter, uint32_t enumerationKind, void* context, void* callbackFunc, uint64_t* callbackToken)`
- `[10]` `HRESULT RegisterSystemButtonCallback( void* device, uint32_t buttonFilter, void* context, void* callbackFunc, uint64_t* callbackToken)`
- `[11]` `HRESULT RegisterKeyboardLayoutCallback( void* device, void* context, void* callbackFunc, uint64_t* callbackToken)`
- `[12]` `void StopCallback(uint64_t callbackToken)`
- `[13]` `bool UnregisterCallback( uint64_t callbackToken, uint64_t timeoutInMicroseconds)`
- `[14]` `HRESULT CreateDispatcher(void** dispatcher)`
- `[15]` `HRESULT CreateAggregateDevice( GameInputKind inputKind, void** device)`
- `[16]` `HRESULT FindDeviceFromId( const void* value, void** device)`
- `[17]` `HRESULT FindDeviceFromObject( IUnknown* value, void** device)`
- `[18]` `HRESULT FindDeviceFromPlatformHandle( HANDLE value, void** device)`
- `[19]` `HRESULT FindDeviceFromPlatformString( LPCWSTR value, void** device)`
- `[20]` `HRESULT EnableOemDeviceSupport( uint16_t vendorId, uint16_t productId, uint8_t interfaceNumber, uint8_t collectionNumber)`
- `[21]` `void SetFocusPolicy(uint32_t policy)`

**Key Enums:**
- `GameInputKind`: GameInputKindUnknown = 0x00000000, GameInputKindKeyboard = 0x00000010, GameInputKindGamepad = 0x00020000
- `GameInputGamepadButtons`: GameInputGamepadNone = 0x00000000, GameInputGamepadMenu = 0x00000001, GameInputGamepadView = 0x00000002, GameInputGamepadA = 0x00000004, GameInputGamepadB = 0x00000008, GameInputGamepadX = 0x00000010, GameInputGamepadY = 0x00000020, GameInputGamepadDPadUp = 0x00000040, GameInputGamepadDPadDown = 0x00000080, GameInputGamepadDPadLeft = 0x00000100, GameInputGamepadDPadRight = 0x00000200, GameInputGamepadLeftShoulder = 0x00000400, GameInputGamepadRightShoulder = 0x00000800, GameInputGamepadLeftThumbstick = 0x00001000, ... (+1 more)

## `pixevt.dll`
**Files:** `src/shims/pixevt/pixevt.cpp` (58 L), `src/shims/pixevt/pixevt.def` (5 L), `src/shims/pixevt/pixevt.hpp` (14 L)

**DLL Exports (`.def`):** `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker`

**C Entrypoints:**
- `void WINAPI PIXBeginEvent(uint64_t color, const char* name)`
- `void WINAPI PIXEndEvent()`
- `void WINAPI PIXSetMarker(uint64_t color, const char* name)`
