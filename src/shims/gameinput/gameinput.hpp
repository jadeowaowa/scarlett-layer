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

inline constexpr GUID IID_IGameInputGdk =
    { 0x11BE2A7E, 0x4254, 0x445A, { 0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18 } };

struct IGameInput : public IUnknown {
    // Slot 3 (+0x18)
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    // Slot 4 (+0x20)
    virtual HRESULT  STDMETHODCALLTYPE GetCurrentReading(
        GameInputKind inputKind,
        void* device,
        IGameInputReading** reading) = 0;
    // Slot 5 (+0x28)
    virtual HRESULT  STDMETHODCALLTYPE GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        void* device,
        IGameInputReading** reading) = 0;
    // Slot 6 (+0x30)
    virtual HRESULT  STDMETHODCALLTYPE GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        void* device,
        IGameInputReading** reading) = 0;
    // Slot 7 (+0x38)
    virtual HRESULT  STDMETHODCALLTYPE GetTemporalReading(
        uint64_t timestamp,
        void* device,
        IGameInputReading** reading) = 0;
    // Slot 8 (+0x40)
    virtual HRESULT  STDMETHODCALLTYPE RegisterReadingCallback(
        void* device,
        GameInputKind inputKind,
        float analogThreshold,
        void* context,
        void* callbackFunc,
        uint64_t* callbackToken) = 0;
    // Slot 9 (+0x48)
    virtual HRESULT  STDMETHODCALLTYPE RegisterDeviceCallback(
        void* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        void* callbackFunc,
        uint64_t* callbackToken) = 0;
    // Slot 10 (+0x50)
    virtual HRESULT  STDMETHODCALLTYPE RegisterSystemButtonCallback(
        void* device,
        uint32_t buttonFilter,
        void* context,
        void* callbackFunc,
        uint64_t* callbackToken) = 0;
    // Slot 11 (+0x58)
    virtual HRESULT  STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(
        void* device,
        void* context,
        void* callbackFunc,
        uint64_t* callbackToken) = 0;
    // Slot 12 (+0x60)
    virtual void     STDMETHODCALLTYPE StopCallback(uint64_t callbackToken) = 0;
    // Slot 13 (+0x68)
    virtual bool     STDMETHODCALLTYPE UnregisterCallback(
        uint64_t callbackToken, uint64_t timeoutInMicroseconds) = 0;
    // Slot 14 (+0x70)
    virtual HRESULT  STDMETHODCALLTYPE CreateDispatcher(void** dispatcher) = 0;
    // Slot 15 (+0x78)
    virtual HRESULT  STDMETHODCALLTYPE CreateAggregateDevice(
        GameInputKind inputKind, void** device) = 0;
    // Slot 16 (+0x80)
    virtual HRESULT  STDMETHODCALLTYPE FindDeviceFromId(
        const void* value, void** device) = 0;
    // Slot 17 (+0x88)
    virtual HRESULT  STDMETHODCALLTYPE FindDeviceFromObject(
        IUnknown* value, void** device) = 0;
    // Slot 18 (+0x90)
    virtual HRESULT  STDMETHODCALLTYPE FindDeviceFromPlatformHandle(
        HANDLE value, void** device) = 0;
    // Slot 19 (+0x98)
    virtual HRESULT  STDMETHODCALLTYPE FindDeviceFromPlatformString(
        LPCWSTR value, void** device) = 0;
    // Slot 20 (+0xA0)
    virtual HRESULT  STDMETHODCALLTYPE EnableOemDeviceSupport(
        uint16_t vendorId, uint16_t productId,
        uint8_t interfaceNumber, uint8_t collectionNumber) = 0;
    // Slot 21 (+0xA8)
    virtual void     STDMETHODCALLTYPE SetFocusPolicy(uint32_t policy) = 0;
};

extern "C" {
    HRESULT WINAPI GameInputCreate(IGameInput** gameInput);
    void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state);
}