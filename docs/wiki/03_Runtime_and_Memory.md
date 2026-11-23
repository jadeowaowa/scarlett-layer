# 03 — Runtime (`xgameruntime`), Kernel (`kernelx`), Memory (`xmem`), Input & PIX

## `xgameruntime.dll`
**Files:** `src/shims/xgameruntime/api_broker.cpp` (676 L), `src/shims/xgameruntime/game_save.cpp` (485 L), `src/shims/xgameruntime/gdk_types.hpp` (119 L), `src/shims/xgameruntime/runtime_internal.hpp` (106 L), `src/shims/xgameruntime/task_queue.cpp` (361 L), `src/shims/xgameruntime/task_queue.hpp` (21 L), `src/shims/xgameruntime/xgameruntime.cpp` (186 L), `src/shims/xgameruntime/xgameruntime.def` (36 L)

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
**Files:** `src/shims/xmem/xmem.cpp` (953 L), `src/shims/xmem/xmem.def` (17 L), `src/shims/xmem/xmem.hpp` (62 L)

**DLL Exports (`.def`):** `XMemAlloc`, `XMemFree`, `XMemSize`, `XMemAllocDefault`, `XMemFreeDefault`, `XMemSizeDefault`, `XMemAllocatePhysicalPages`, `XMemMapPhysicalPages`, `XMemFreePhysicalPages`, `XMemVirtualAlloc`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable`, `XMemMapPhysicalPagesScatter`, `XMemTransferMemory`, `XMemVirtualQuery`

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
**Files:** `src/shims/gameinput/gameinput.cpp` (534 L), `src/shims/gameinput/gameinput.def` (4 L), `src/shims/gameinput/gameinput.hpp` (191 L)

**DLL Exports (`.def`):** `GameInputCreate @1`, `ScarlettGameInputSetSyntheticGamepad @2`

**C Entrypoints:**
- `HRESULT WINAPI GameInputCreate(IGameInput** gameInput)`
- `void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state)`

**COM Interface `IGameInputDevice : IUnknown`:**
- `[03]` `const GameInputDeviceInfo* GetDeviceInfo()`
- `[04]` `GameInputDeviceStatus GetDeviceStatus()`
- `[05]` `void GetBatteryState(GameInputBatteryState* state)`
- `[06]` `HRESULT CreateForceFeedbackEffect(uint32_t, const void*, void**)`
- `[07]` `bool IsForceFeedbackMotorPoweredOn(uint32_t)`
- `[08]` `void SetForceFeedbackMotorGain(uint32_t, float)`
- `[09]` `HRESULT SetHapticMotorState(uint32_t, const void*)`
- `[10]` `void SetRumbleState(const GameInputRumbleParams* params)`
- `[11]` `void SetInputSynchronizationState(bool)`
- `[12]` `void SendInputSynchronizationHint()`
- `[13]` `void PowerOff()`
- `[14]` `HRESULT CreateRawDeviceReport(uint32_t, uint32_t, void**)`
- `[15]` `HRESULT GetRawDeviceFeature(uint32_t, void**)`
- `[16]` `HRESULT SetRawDeviceFeature(void*)`
- `[17]` `HRESULT SendRawDeviceOutput(void*)`
- `[18]` `HRESULT SendRawDeviceOutputWithResponse(void*, void**)`
- `[19]` `HRESULT ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t*)`
- `[20]` `bool AcquireExclusiveRawDeviceAccess(uint64_t)`
- `[21]` `void ReleaseExclusiveRawDeviceAccess()`

**COM Interface `IGameInputReading : IUnknown`:**
- `[03]` `GameInputKind GetInputKind()`
- `[04]` `uint64_t GetSequenceNumber(GameInputKind)`
- `[05]` `uint64_t GetTimestamp()`
- `[06]` `void GetDevice(IGameInputDevice** device)`
- `[07]` `bool GetRawReport(void** report)`
- `[08]` `uint32_t GetControllerAxisCount()`
- `[09]` `uint32_t GetControllerAxisState(uint32_t, float*)`
- `[10]` `uint32_t GetControllerButtonCount()`
- `[11]` `uint32_t GetControllerButtonState(uint32_t, bool*)`
- `[12]` `uint32_t GetControllerSwitchCount()`
- `[13]` `uint32_t GetControllerSwitchState(uint32_t, uint32_t*)`
- `[14]` `uint32_t GetKeyCount()`
- `[15]` `uint32_t GetKeyState(uint32_t stateArrayCount, GameInputKeyState* stateArray)`
- `[16]` `bool GetMouseState(GameInputMouseState* state)`
- `[17]` `uint32_t GetTouchCount()`
- `[18]` `uint32_t GetTouchState(uint32_t, void*)`
- `[19]` `bool GetMotionState(void*)`
- `[20]` `bool GetArcadeStickState(void*)`
- `[21]` `bool GetFlightStickState(void*)`
- `[22]` `bool GetGamepadState(GameInputGamepadState* state)`
- `[23]` `bool GetRacingWheelState(void*)`
- `[24]` `bool GetUiNavigationState(void*)`

**COM Interface `IGameInput : IUnknown`:**
- `[03]` `uint64_t GetCurrentTimestamp()`
- `[04]` `HRESULT GetCurrentReading(GameInputKind, IGameInputDevice*, IGameInputReading**)`
- `[05]` `HRESULT GetNextReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**)`
- `[06]` `HRESULT GetPreviousReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**)`
- `[07]` `HRESULT GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading**)`
- `[08]` `HRESULT RegisterReadingCallback(IGameInputDevice*, GameInputKind, float, void*, void*, uint64_t*)`
- `[09]` `HRESULT RegisterDeviceCallback(IGameInputDevice*, GameInputKind, uint32_t, uint32_t, void*, void*, uint64_t*)`
- `[10]` `HRESULT RegisterSystemButtonCallback(IGameInputDevice*, uint32_t, void*, void*, uint64_t*)`
- `[11]` `HRESULT RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, void*, uint64_t*)`
- `[12]` `void StopCallback(uint64_t)`
- `[13]` `bool UnregisterCallback(uint64_t, uint64_t)`
- `[14]` `HRESULT CreateDispatcher(void**)`
- `[15]` `HRESULT CreateAggregateDevice(GameInputKind, IGameInputDevice**)`
- `[16]` `HRESULT FindDeviceFromId(const void*, IGameInputDevice**)`
- `[17]` `HRESULT FindDeviceFromObject(IUnknown*, IGameInputDevice**)`
- `[18]` `HRESULT FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice**)`
- `[19]` `HRESULT FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice**)`
- `[20]` `HRESULT EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t)`
- `[21]` `void SetFocusPolicy(uint32_t)`

**Key Enums:**
- `GameInputKind`: GameInputKindUnknown = 0x00000000, GameInputKindRawDeviceReport = 0x00000001, GameInputKindControllerAxis = 0x00000002, GameInputKindControllerButton = 0x00000004, GameInputKindControllerSwitch = 0x00000008, GameInputKindController = 0x0000000E, GameInputKindKeyboard = 0x00000010, GameInputKindMouse = 0x00000020, GameInputKindTouch = 0x00000100, GameInputKindMotion = 0x00001000, GameInputKindArcadeStick = 0x00010000, GameInputKindFlightStick = 0x00020000, GameInputKindGamepad = 0x00040000, GameInputKindRacingWheel = 0x00080000, ... (+1 more)
- `GameInputDeviceStatus`: GameInputDeviceNoStatus = 0x00000000, GameInputDeviceConnected = 0x00000001, GameInputDeviceInputEnabled = 0x00000002, GameInputDeviceOutputEnabled = 0x00000004, GameInputDeviceAnyStatus = 0x00FFFFFF
- `GameInputEnumerationKind`: GameInputNoEnumeration = 0, GameInputAsyncEnumeration = 1, GameInputBlockingEnumeration = 2
- `GameInputGamepadButtons`: GameInputGamepadNone = 0x00000000, GameInputGamepadMenu = 0x00000001, GameInputGamepadView = 0x00000002, GameInputGamepadA = 0x00000004, GameInputGamepadB = 0x00000008, GameInputGamepadX = 0x00000010, GameInputGamepadY = 0x00000020, GameInputGamepadDPadUp = 0x00000040, GameInputGamepadDPadDown = 0x00000080, GameInputGamepadDPadLeft = 0x00000100, GameInputGamepadDPadRight = 0x00000200, GameInputGamepadLeftShoulder = 0x00000400, GameInputGamepadRightShoulder = 0x00000800, GameInputGamepadLeftThumbstick = 0x00001000, ... (+1 more)
- `GameInputMouseButtons`: GameInputMouseNone = 0x00000000, GameInputMouseLeftButton = 0x00000001, GameInputMouseRightButton = 0x00000002, GameInputMouseMiddleButton = 0x00000004, GameInputMouseButton4 = 0x00000008, GameInputMouseButton5 = 0x00000010

## `pixevt.dll`
**Files:** `src/shims/pixevt/pixevt.cpp` (58 L), `src/shims/pixevt/pixevt.def` (5 L), `src/shims/pixevt/pixevt.hpp` (14 L)

**DLL Exports (`.def`):** `PIXBeginEvent`, `PIXEndEvent`, `PIXSetMarker`

**C Entrypoints:**
- `void WINAPI PIXBeginEvent(uint64_t color, const char* name)`
- `void WINAPI PIXEndEvent()`
- `void WINAPI PIXSetMarker(uint64_t color, const char* name)`
