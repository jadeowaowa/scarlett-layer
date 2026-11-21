#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <windows.h>
#include <unknwn.h>

inline constexpr HRESULT GAMEINPUT_E_READING_NOT_FOUND = static_cast<HRESULT>(0x838A0002L);

enum GameInputKind : uint32_t {
    GameInputKindUnknown  = 0x00000000,
    GameInputKindKeyboard = 0x00000010,
    GameInputKindGamepad  = 0x00020000
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

struct GameInputGamepadState {
    uint32_t buttons;
    float    leftTrigger;
    float    rightTrigger;
    float    leftThumbstickX;
    float    leftThumbstickY;
    float    rightThumbstickX;
    float    rightThumbstickY;
};

struct GameInputKeyState {
    uint32_t scanCode;
    uint32_t codePoint;
    uint8_t  virtualKey;
    bool     isDeadKey;
};

inline constexpr GUID IID_IGameInput =
    { 0x5c4a2101, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x01 } };

inline constexpr GUID IID_IGameInputReading =
    { 0x5c4a2102, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x02 } };

struct IGameInputReading : public IUnknown {
    virtual GameInputKind STDMETHODCALLTYPE GetInputKind() = 0;
    virtual uint64_t      STDMETHODCALLTYPE GetTimestamp() = 0;
    virtual bool          STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) = 0;
    virtual uint32_t      STDMETHODCALLTYPE GetKeyCount() = 0;
    virtual uint32_t      STDMETHODCALLTYPE GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;
};

struct IGameInput : public IUnknown {
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    virtual HRESULT  STDMETHODCALLTYPE GetCurrentReading(
        GameInputKind inputKind,
        void* device,
        IGameInputReading** reading) = 0;
};

extern "C" {
    HRESULT WINAPI GameInputCreate(IGameInput** gameInput);
    void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state);
}