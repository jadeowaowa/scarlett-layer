#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstddef>
#include <cstdint>
#include <windows.h>
#include <unknwn.h>

inline constexpr HRESULT GAMEINPUT_E_READING_NOT_FOUND = static_cast<HRESULT>(0x838A0002L);

enum GameInputKind : uint32_t {
    GameInputKindUnknown          = 0x00000000,
    GameInputKindRawDeviceReport  = 0x00000001,
    GameInputKindControllerAxis   = 0x00000002,
    GameInputKindControllerButton = 0x00000004,
    GameInputKindControllerSwitch = 0x00000008,
    GameInputKindController       = 0x0000000E,
    GameInputKindKeyboard         = 0x00000010,
    GameInputKindMouse            = 0x00000020,
    GameInputKindTouch            = 0x00000100,
    GameInputKindMotion           = 0x00001000,
    GameInputKindArcadeStick      = 0x00010000,
    GameInputKindFlightStick      = 0x00020000,
    GameInputKindGamepad          = 0x00040000,
    GameInputKindRacingWheel      = 0x00080000,
    GameInputKindUiNavigation     = 0x01000000
};

enum GameInputDeviceStatus : uint32_t {
    GameInputDeviceNoStatus      = 0x00000000,
    GameInputDeviceConnected     = 0x00000001,
    GameInputDeviceInputEnabled  = 0x00000002,
    GameInputDeviceOutputEnabled = 0x00000004,
    GameInputDeviceAnyStatus     = 0x00FFFFFF
};

enum GameInputEnumerationKind : uint32_t {
    GameInputNoEnumeration       = 0,
    GameInputAsyncEnumeration    = 1,
    GameInputBlockingEnumeration = 2
};

enum GameInputGamepadButtons : uint32_t {
    GameInputGamepadNone            = 0x00000000,
    GameInputGamepadMenu            = 0x00000001,
    GameInputGamepadView            = 0x00000002,
    GameInputGamepadA               = 0x00000004,
    GameInputGamepadB               = 0x00000008,
    GameInputGamepadX               = 0x00000010,
    GameInputGamepadY               = 0x00000020,
    GameInputGamepadDPadUp          = 0x00000040,
    GameInputGamepadDPadDown        = 0x00000080,
    GameInputGamepadDPadLeft        = 0x00000100,
    GameInputGamepadDPadRight       = 0x00000200,
    GameInputGamepadLeftShoulder    = 0x00000400,
    GameInputGamepadRightShoulder   = 0x00000800,
    GameInputGamepadLeftThumbstick  = 0x00001000,
    GameInputGamepadRightThumbstick = 0x00002000
};

enum GameInputMouseButtons : uint32_t {
    GameInputMouseNone         = 0x00000000,
    GameInputMouseLeftButton   = 0x00000001,
    GameInputMouseRightButton  = 0x00000002,
    GameInputMouseMiddleButton = 0x00000004,
    GameInputMouseButton4      = 0x00000008,
    GameInputMouseButton5      = 0x00000010
};

struct GameInputGamepadState {
    uint32_t buttons;
    float leftTrigger, rightTrigger, leftThumbstickX, leftThumbstickY, rightThumbstickX, rightThumbstickY;
};
struct GameInputKeyState { uint32_t scanCode, codePoint; uint8_t virtualKey; bool isDeadKey; };
struct GameInputMouseState { uint32_t buttons; int64_t positionX, positionY, wheelX, wheelY; };
struct GameInputRumbleParams { float lowFrequency, highFrequency, leftTrigger, rightTrigger; };
struct GameInputBatteryState { float chargeRate, maxChargeRate, remainingCapacity, fullChargeCapacity; uint32_t status; };
struct GameInputUsage { uint16_t page, id; };
struct GameInputVersion { uint16_t major, minor, build, revision; };
struct GameInputString { uint32_t sizeInBytes, codePointCount; const char* data; };
struct GameInputKeyboardInfo { uint32_t kind, layout, keyCount, functionKeyCount, maxSimultaneousKeys, platformType, platformSubtype; const GameInputString* nativeLanguage; };
struct GameInputMouseInfo { uint32_t supportedButtons, sampleRate, sensorDpi; bool hasWheelX, hasWheelY; };
struct GameInputGamepadInfo { uint32_t supportedLayout, labels[14]; };

struct GameInputDeviceInfo {
    uint32_t infoSize; uint16_t vendorId, productId, revisionNumber;
    uint8_t interfaceNumber, collectionNumber; GameInputUsage usage;
    GameInputVersion hardwareVersion, firmwareVersion;
    uint8_t deviceId[32]; uint8_t deviceRootId[32];
    uint32_t deviceFamily, capabilities; GameInputKind supportedInput; uint32_t supportedRumbleMotors;
    uint32_t inputReportCount, outputReportCount, featureReportCount;
    uint32_t controllerAxisCount, controllerButtonCount, controllerSwitchCount;
    uint32_t touchPointCount, touchSensorCount, forceFeedbackMotorCount, hapticFeedbackMotorCount;
    uint32_t deviceStringCount, deviceDescriptorSize;
    const void *inputReportInfo, *outputReportInfo, *featureReportInfo;
    const void *controllerAxisInfo, *controllerButtonInfo, *controllerSwitchInfo;
    const GameInputKeyboardInfo* keyboardInfo; const GameInputMouseInfo* mouseInfo;
    const void *touchSensorInfo, *motionInfo, *arcadeStickInfo, *flightStickInfo;
    const GameInputGamepadInfo* gamepadInfo;
    const void *racingWheelInfo, *uiNavigationInfo, *forceFeedbackMotorInfo, *hapticFeedbackMotorInfo;
    const GameInputString *displayName, *deviceStrings; const void* deviceDescriptorData;
};
static_assert(sizeof(GameInputGamepadState) == 28 && sizeof(GameInputKeyState) == 12 && sizeof(GameInputMouseState) == 40, "State ABI");
static_assert(offsetof(GameInputDeviceInfo, deviceId) == 0x20 && offsetof(GameInputDeviceInfo, supportedInput) == 0x68 && sizeof(GameInputDeviceInfo) == 0x140, "DeviceInfo ABI");

inline constexpr GUID IID_IGameInput          = { 0x5c4a2101, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x01 } };
inline constexpr GUID IID_IGameInputReading   = { 0x5c4a2102, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x02 } };
inline constexpr GUID IID_IGameInputDevice    = { 0x5c4a2103, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x03 } };
inline constexpr GUID IID_IGameInputGdk       = { 0x11BE2A7E, 0x4254, 0x445A, { 0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18 } };
inline constexpr GUID IID_IGameInputDeviceGdk = { 0x31DD86FB, 0x4C1B, 0x408A, { 0x86, 0x8F, 0x43, 0x9B, 0x3C, 0xD4, 0x71, 0x25 } };
inline constexpr GUID IID_IGameInputReadingGdk= { 0x2156947A, 0xE1FA, 0x4DE0, { 0xA3, 0x0B, 0xD8, 0x12, 0x93, 0x1D, 0xBD, 0x8D } };

struct IGameInputDevice : public IUnknown {
    virtual const GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() = 0;
    virtual GameInputDeviceStatus STDMETHODCALLTYPE GetDeviceStatus() = 0;
    virtual void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* state) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void**) = 0;
    virtual bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) = 0;
    virtual void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) = 0;
    virtual void STDMETHODCALLTYPE SetRumbleState(const GameInputRumbleParams* params) = 0;
    virtual void STDMETHODCALLTYPE SetInputSynchronizationState(bool) = 0;
    virtual void STDMETHODCALLTYPE SendInputSynchronizationHint() = 0;
    virtual void STDMETHODCALLTYPE PowerOff() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, uint32_t, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t*) = 0;
    virtual bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) = 0;
    virtual void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() = 0;
};

struct IGameInputReading : public IUnknown {
    virtual GameInputKind STDMETHODCALLTYPE GetInputKind() = 0;
    virtual uint64_t STDMETHODCALLTYPE GetSequenceNumber(GameInputKind) = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
    virtual void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) = 0;
    virtual bool STDMETHODCALLTYPE GetRawReport(void** report) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerAxisCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t, float*) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerButtonCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t, bool*) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, uint32_t*) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;
    virtual bool STDMETHODCALLTYPE GetMouseState(GameInputMouseState* state) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetTouchCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) = 0;
    virtual bool STDMETHODCALLTYPE GetMotionState(void*) = 0;
    virtual bool STDMETHODCALLTYPE GetArcadeStickState(void*) = 0;
    virtual bool STDMETHODCALLTYPE GetFlightStickState(void*) = 0;
    virtual bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) = 0;
    virtual bool STDMETHODCALLTYPE GetRacingWheelState(void*) = 0;
    virtual bool STDMETHODCALLTYPE GetUiNavigationState(void*) = 0;
};

using GameInputDeviceCallback = void (CALLBACK*)(uint64_t, void*, IGameInputDevice*, uint64_t, GameInputDeviceStatus, GameInputDeviceStatus);

struct IGameInput : public IUnknown {
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(GameInputKind, IGameInputDevice*, IGameInputReading**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice*, GameInputKind, float, void*, void*, uint64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(IGameInputDevice*, GameInputKind, uint32_t, uint32_t, void*, void*, uint64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterSystemButtonCallback(IGameInputDevice*, uint32_t, void*, void*, uint64_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, void*, uint64_t*) = 0;
    virtual void STDMETHODCALLTYPE StopCallback(uint64_t) = 0;
    virtual bool STDMETHODCALLTYPE UnregisterCallback(uint64_t, uint64_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDispatcher(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateAggregateDevice(GameInputKind, IGameInputDevice**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDeviceFromId(const void*, IGameInputDevice**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice**) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) = 0;
    virtual void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) = 0;
};

extern "C" {
    HRESULT WINAPI GameInputCreate(IGameInput** gameInput);
    void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state);
}
