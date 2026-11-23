#include "gameinput.hpp"
#include "common/stub.hpp"

#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint64_t QueryTimestampUs() {
    static const uint64_t freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000ULL) / freq);
}

bool IsCurrentProcessForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

float NormalizeAxis(SHORT raw, SHORT deadzone) {
    float val = static_cast<float>(raw);
    if (std::fabs(val) < static_cast<float>(deadzone)) return 0.0f;
    float sign = (val < 0.0f) ? -1.0f : 1.0f;
    float mag = (std::fabs(val) - static_cast<float>(deadzone)) /
                (32767.0f - static_cast<float>(deadzone));
    return sign * std::clamp(mag, 0.0f, 1.0f);
}

uint32_t MapXInputButtons(WORD b) {
    uint32_t out = GameInputGamepadNone;
    if (b & XINPUT_GAMEPAD_START)          out |= GameInputGamepadMenu;
    if (b & XINPUT_GAMEPAD_BACK)           out |= GameInputGamepadView;
    if (b & XINPUT_GAMEPAD_A)              out |= GameInputGamepadA;
    if (b & XINPUT_GAMEPAD_B)              out |= GameInputGamepadB;
    if (b & XINPUT_GAMEPAD_X)              out |= GameInputGamepadX;
    if (b & XINPUT_GAMEPAD_Y)              out |= GameInputGamepadY;
    if (b & XINPUT_GAMEPAD_DPAD_UP)        out |= GameInputGamepadDPadUp;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN)      out |= GameInputGamepadDPadDown;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT)      out |= GameInputGamepadDPadLeft;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     out |= GameInputGamepadDPadRight;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  out |= GameInputGamepadLeftShoulder;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) out |= GameInputGamepadRightShoulder;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB)     out |= GameInputGamepadLeftThumbstick;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    out |= GameInputGamepadRightThumbstick;
    return out;
}

class ScarlettGameInputDevice final : public IGameInputDevice {
public:
    ScarlettGameInputDevice(GameInputKind kind, uint32_t xiSlot, uint16_t vid, uint16_t pid,
                            uint32_t family, const char* idTag, const char* name)
        : m_kind(kind), m_xiSlot(xiSlot), m_name(name ? name : "Scarlett Device") {
        std::memset(&m_info, 0, sizeof(m_info));
        m_info.infoSize = sizeof(GameInputDeviceInfo);
        m_info.vendorId = vid;
        m_info.productId = pid;
        m_info.revisionNumber = 1;
        m_info.hardwareVersion = { 1, 0, 0, 0 };
        m_info.firmwareVersion = { 1, 0, 0, 0 };
        if (idTag) {
            const size_t n = std::min<size_t>(std::strlen(idTag), sizeof(m_info.deviceId));
            std::memcpy(m_info.deviceId, idTag, n);
            std::memcpy(m_info.deviceRootId, idTag, n);
        }
        m_info.deviceFamily = family;
        m_info.supportedInput = kind;
        m_nameStr = { static_cast<uint32_t>(m_name.size()), static_cast<uint32_t>(m_name.size()), m_name.c_str() };
        m_info.displayName = &m_nameStr;

        if ((kind & GameInputKindGamepad) != 0) {
            m_info.usage = { 0x0001, 0x0005 };
            m_info.supportedRumbleMotors = 0x03u;
            m_gpInfo.supportedLayout = 1;
            m_info.gamepadInfo = &m_gpInfo;
        } else if ((kind & GameInputKindKeyboard) != 0) {
            m_info.usage = { 0x0001, 0x0006 };
            m_kbInfo = { 1, 0x04090409u, 104, 12, 16, 0, 0, nullptr };
            m_info.keyboardInfo = &m_kbInfo;
        } else if ((kind & GameInputKindMouse) != 0) {
            m_info.usage = { 0x0001, 0x0002 };
            m_msInfo = { 0x1Fu, 1000, 800, false, true };
            m_info.mouseInfo = &m_msInfo;
        }
    }

    GameInputKind Kind() const { return m_kind; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IGameInputDevice) ||
            IsEqualGUID(riid, IID_IGameInputDeviceGdk)) {
            *ppv = static_cast<IGameInputDevice*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    const GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() override { return &m_info; }
    GameInputDeviceStatus STDMETHODCALLTYPE GetDeviceStatus() override {
        return static_cast<GameInputDeviceStatus>(
            GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled);
    }
    void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* s) override {
        if (!s) return;
        std::memset(s, 0, sizeof(*s));
        s->remainingCapacity = 1.0f;
        s->fullChargeCapacity = 1.0f;
    }
    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void** e) override { if (e) *e = nullptr; return E_NOTIMPL; }
    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) override { return false; }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) override {}
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetRumbleState(const GameInputRumbleParams* p) override {
        if ((m_kind & GameInputKindGamepad) == 0 || m_xiSlot >= 4) return;
        XINPUT_VIBRATION v{};
        if (p) {
            v.wLeftMotorSpeed = static_cast<WORD>(std::clamp(p->lowFrequency, 0.0f, 1.0f) * 65535.0f);
            v.wRightMotorSpeed = static_cast<WORD>(std::clamp(p->highFrequency, 0.0f, 1.0f) * 65535.0f);
        }
        XInputSetState(m_xiSlot, &v);
    }
    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {}
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {}
    void STDMETHODCALLTYPE PowerOff() override {}
    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, uint32_t, void** r) override { if (r) *r = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void** r) override { if (r) *r = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void** r) override { if (r) *r = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t* outSz) override {
        if (outSz) *outSz = 0;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {}

private:
    std::atomic<ULONG> m_refs{1};
    GameInputKind m_kind{GameInputKindUnknown};
    uint32_t m_xiSlot{0};
    std::string m_name;
    GameInputString m_nameStr{};
    GameInputKeyboardInfo m_kbInfo{};
    GameInputMouseInfo m_msInfo{};
    GameInputGamepadInfo m_gpInfo{};
    GameInputDeviceInfo m_info{};
};

class ScarlettGameInputReading final : public IGameInputReading {
public:
    ScarlettGameInputReading(GameInputKind kind, uint64_t seq, uint64_t ts, IGameInputDevice* dev,
                             bool hasGp, const GameInputGamepadState& gp,
                             bool hasMs, const GameInputMouseState& ms,
                             std::vector<GameInputKeyState> keys)
        : m_kind(kind), m_seq(seq), m_ts(ts), m_dev(dev),
          m_hasGp(hasGp), m_gp(gp), m_hasMs(hasMs), m_ms(ms), m_keys(std::move(keys)) {
        if (m_dev) m_dev->AddRef();
    }
    ~ScarlettGameInputReading() { if (m_dev) m_dev->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IGameInputReading) ||
            IsEqualGUID(riid, IID_IGameInputReadingGdk)) {
            *ppv = static_cast<IGameInputReading*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    GameInputKind STDMETHODCALLTYPE GetInputKind() override { return m_kind; }
    uint64_t STDMETHODCALLTYPE GetSequenceNumber(GameInputKind) override { return m_seq; }
    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return m_ts; }
    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** dev) override {
        if (!dev) return;
        *dev = m_dev;
        if (m_dev) m_dev->AddRef();
    }
    bool STDMETHODCALLTYPE GetRawReport(void** r) override { if (r) *r = nullptr; return false; }
    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t, float*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t, bool*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, uint32_t*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyCount() override { return static_cast<uint32_t>(m_keys.size()); }
    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t cap, GameInputKeyState* arr) override {
        if (!arr || cap == 0 || m_keys.empty()) return 0;
        const uint32_t n = std::min(cap, static_cast<uint32_t>(m_keys.size()));
        std::memcpy(arr, m_keys.data(), n * sizeof(GameInputKeyState));
        return n;
    }
    bool STDMETHODCALLTYPE GetMouseState(GameInputMouseState* s) override {
        if (!s || !m_hasMs) return false;
        *s = m_ms;
        return true;
    }
    uint32_t STDMETHODCALLTYPE GetTouchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMotionState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetArcadeStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetFlightStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* s) override {
        if (!s || !m_hasGp) return false;
        *s = m_gp;
        return true;
    }
    bool STDMETHODCALLTYPE GetRacingWheelState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetUiNavigationState(void*) override { return false; }

private:
    std::atomic<ULONG> m_refs{1};
    GameInputKind m_kind{GameInputKindUnknown};
    uint64_t m_seq{1}, m_ts{0};
    IGameInputDevice* m_dev{nullptr};
    bool m_hasGp{false};
    GameInputGamepadState m_gp{};
    bool m_hasMs{false};
    GameInputMouseState m_ms{};
    std::vector<GameInputKeyState> m_keys;
};

SRWLOCK g_synthLock = SRWLOCK_INIT;
bool g_synthEnabled = false;
GameInputGamepadState g_synthState{};

class ScarlettGameInput final : public IGameInput {
public:
    ScarlettGameInput() {
        m_gpDev = new ScarlettGameInputDevice(
            GameInputKindGamepad, 0, 0x045E, 0x02FF, 2u, "SCARLETT-XBOXONE-GAMEPAD-000000", "Xbox One Controller");
        m_kbDev = new ScarlettGameInputDevice(
            GameInputKindKeyboard, 0xFFFFFFFFu, 0x045E, 0x0750, 4u, "SCARLETT-HOST-KEYBOARD-00000000", "Keyboard");
        m_msDev = new ScarlettGameInputDevice(
            GameInputKindMouse, 0xFFFFFFFFu, 0x045E, 0x00CB, 4u, "SCARLETT-HOST-MOUSE-00000000000", "Mouse");
        sc::log::Write(sc::log::Level::Info, "gameinput", "ScarlettGameInput",
                       "-> initialized GDK v0 GameInput (XInput gamepad + Win32 keyboard/mouse)");
    }
    ~ScarlettGameInput() {
        if (m_gpDev) m_gpDev->Release();
        if (m_kbDev) m_kbDev->Release();
        if (m_msDev) m_msDev->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IGameInput) ||
            IsEqualGUID(riid, IID_IGameInputGdk)) {
            *ppv = static_cast<IGameInput*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override { return QueryTimestampUs(); }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(
        GameInputKind inputKind, IGameInputDevice* device, IGameInputReading** reading) override {
        if (!reading) return E_POINTER;
        *reading = nullptr;

        uint32_t resolved = GameInputKindUnknown;
        IGameInputDevice* rDev = device;
        bool hasGp = false, hasMs = false;
        GameInputGamepadState gp{};
        GameInputMouseState ms{};
        std::vector<GameInputKeyState> keys;

        if ((inputKind & GameInputKindGamepad) != 0 && (!device || device == m_gpDev)) {
            bool usedSynth = false;
            AcquireSRWLockShared(&g_synthLock);
            if (g_synthEnabled) { gp = g_synthState; usedSynth = true; }
            ReleaseSRWLockShared(&g_synthLock);

            hasGp = true;
            resolved |= GameInputKindGamepad;
            if (!rDev) rDev = m_gpDev;

            if (!usedSynth) {
                const uint64_t nowUs = QueryTimestampUs();
                bool xiOk = false;
                XINPUT_STATE xi{};
                if (m_xiConnected.load(std::memory_order_relaxed) ||
                    (nowUs - m_lastXiProbeUs.load(std::memory_order_relaxed)) >= 250000ULL) {
                    m_lastXiProbeUs.store(nowUs, std::memory_order_relaxed);
                    xiOk = (XInputGetState(0, &xi) == ERROR_SUCCESS);
                    m_xiConnected.store(xiOk, std::memory_order_relaxed);
                }
                if (xiOk) {
                    gp.buttons          = MapXInputButtons(xi.Gamepad.wButtons);
                    gp.leftTrigger      = static_cast<float>(xi.Gamepad.bLeftTrigger) / 255.0f;
                    gp.rightTrigger     = static_cast<float>(xi.Gamepad.bRightTrigger) / 255.0f;
                    gp.leftThumbstickX  = NormalizeAxis(xi.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                    gp.leftThumbstickY  = NormalizeAxis(xi.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                    gp.rightThumbstickX = NormalizeAxis(xi.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                    gp.rightThumbstickY = NormalizeAxis(xi.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                    if (!m_loggedGp.exchange(true, std::memory_order_relaxed)) {
                        sc::log::Write(sc::log::Level::Info, "gameinput", "GetCurrentReading",
                                       "-> polling live XInput controller #0");
                    }
                    const uint32_t prevBtns = m_lastGpButtons.exchange(gp.buttons, std::memory_order_relaxed);
                    if (gp.buttons != prevBtns) {
                        char bmsg[160];
                        std::snprintf(bmsg, sizeof(bmsg),
                                      "-> gp.buttons 0x%04X -> 0x%04X (xi=0x%04X) LT=%.2f RT=%.2f",
                                      static_cast<unsigned>(prevBtns),
                                      static_cast<unsigned>(gp.buttons),
                                      static_cast<unsigned>(xi.Gamepad.wButtons),
                                      gp.leftTrigger, gp.rightTrigger);
                        sc::log::Write(sc::log::Level::Info, "gameinput", "GetCurrentReading", bmsg);
                    }

                } else if (IsCurrentProcessForeground()) {
                    if ((GetAsyncKeyState('W') & 0x8000) != 0) gp.leftThumbstickY += 1.0f;
                    if ((GetAsyncKeyState('S') & 0x8000) != 0) gp.leftThumbstickY -= 1.0f;
                    if ((GetAsyncKeyState('D') & 0x8000) != 0) gp.leftThumbstickX += 1.0f;
                    if ((GetAsyncKeyState('A') & 0x8000) != 0) gp.leftThumbstickX -= 1.0f;
                    if ((GetAsyncKeyState(VK_UP) & 0x8000) != 0)    gp.buttons |= GameInputGamepadDPadUp;
                    if ((GetAsyncKeyState(VK_DOWN) & 0x8000) != 0)  gp.buttons |= GameInputGamepadDPadDown;
                    if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0)  gp.buttons |= GameInputGamepadDPadLeft;
                    if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0) gp.buttons |= GameInputGamepadDPadRight;
                    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 ||
                        (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) gp.buttons |= GameInputGamepadA;
                    if ((GetAsyncKeyState(VK_BACK) & 0x8000) != 0)   gp.buttons |= GameInputGamepadB;
                    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) gp.buttons |= GameInputGamepadMenu;
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)  gp.rightTrigger = 1.0f;
                }
            }
        }

        if ((inputKind & GameInputKindKeyboard) != 0 && (!device || device == m_kbDev)) {
            resolved |= GameInputKindKeyboard;
            if (!rDev) rDev = m_kbDev;
            if (!m_loggedKb.exchange(true, std::memory_order_relaxed)) {
                sc::log::Write(sc::log::Level::Info, "gameinput", "GetCurrentReading", "-> polling Win32 keyboard");
            }
            if (IsCurrentProcessForeground()) {
                for (uint32_t vk = 0x08; vk <= 0xFE && keys.size() < 16; ++vk) {
                    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= 0xC3 && vk <= 0xDA)) continue;
                    if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0) {
                        GameInputKeyState ks{};
                        ks.virtualKey = static_cast<uint8_t>(vk);
                        uint32_t sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
                        if ((sc & 0xE000u) == 0u && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
                            vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR ||
                            vk == VK_NEXT || vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_DIVIDE ||
                            vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS)) { sc |= 0xE000u; }
                        ks.scanCode = sc;
                        ks.codePoint = (vk >= 0x20 && vk <= 0x7E) ? vk : 0u;
                        keys.push_back(ks);
                    }
                }
            }
        }

        if ((inputKind & GameInputKindMouse) != 0 && (!device || device == m_msDev)) {
            hasMs = true;
            resolved |= GameInputKindMouse;
            if (!rDev) rDev = m_msDev;
            if (!m_loggedMs.exchange(true, std::memory_order_relaxed)) {
                sc::log::Write(sc::log::Level::Info, "gameinput", "GetCurrentReading", "-> polling Win32 mouse");
            }
            if (IsCurrentProcessForeground()) {
                POINT pt{};
                if (GetCursorPos(&pt)) { ms.positionX = pt.x; ms.positionY = pt.y; }
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)  ms.buttons |= GameInputMouseLeftButton;
                if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)  ms.buttons |= GameInputMouseRightButton;
                if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0)  ms.buttons |= GameInputMouseMiddleButton;
                if ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0) ms.buttons |= GameInputMouseButton4;
                if ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) ms.buttons |= GameInputMouseButton5;
            }
        }

        if (resolved == GameInputKindUnknown) return GAMEINPUT_E_READING_NOT_FOUND;

        *reading = new ScarlettGameInputReading(
            static_cast<GameInputKind>(resolved),
            m_seq.fetch_add(1, std::memory_order_relaxed),
            QueryTimestampUs(), rDev, hasGp, gp, hasMs, ms, std::move(keys));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading** r) override {
        if (r) *r = nullptr; return GAMEINPUT_E_READING_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading** r) override {
        if (r) *r = nullptr; return GAMEINPUT_E_READING_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading** r) override {
        if (r) *r = nullptr; return GAMEINPUT_E_READING_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice*, GameInputKind kind, float, void*, void*, uint64_t* tokOut) override {
        uint64_t tok = m_nextTok.fetch_add(1, std::memory_order_relaxed);
        if (tokOut) *tokOut = tok;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "-> kind=0x%X token=%llu", static_cast<unsigned>(kind), static_cast<unsigned long long>(tok));
        sc::log::Write(sc::log::Level::Info, "gameinput", "RegisterReadingCallback", msg);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(
        IGameInputDevice* filter, GameInputKind kind, uint32_t statusFilter, uint32_t enumKind,
        void* ctx, void* cbPtr, uint64_t* tokOut) override {
        uint64_t tok = m_nextTok.fetch_add(1, std::memory_order_relaxed);
        if (tokOut) *tokOut = tok;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "-> kind=0x%X status=0x%X enum=%u token=%llu cb=%p",
                      static_cast<unsigned>(kind), statusFilter, enumKind, static_cast<unsigned long long>(tok), cbPtr);
        sc::log::Write(sc::log::Level::Info, "gameinput", "RegisterDeviceCallback", msg);

        if (cbPtr && enumKind != GameInputNoEnumeration) {
            auto cb = reinterpret_cast<GameInputDeviceCallback>(cbPtr);
            const uint64_t ts = QueryTimestampUs();
            const auto st = static_cast<GameInputDeviceStatus>(
                GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled);
            ScarlettGameInputDevice* devs[] = { m_gpDev, m_kbDev, m_msDev };
            for (ScarlettGameInputDevice* d : devs) {
                if (!d || (filter && filter != d)) continue;
                if ((d->Kind() & kind) == 0 || (static_cast<uint32_t>(st) & statusFilter) == 0) continue;
                char dm[128];
                std::snprintf(dm, sizeof(dm), "-> enumerating '%s' kind=0x%X",
                              d->GetDeviceInfo()->displayName->data, static_cast<unsigned>(d->Kind()));
                sc::log::Write(sc::log::Level::Info, "gameinput", "RegisterDeviceCallback", dm);
                cb(tok, ctx, d, ts, st, GameInputDeviceNoStatus);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterSystemButtonCallback(IGameInputDevice*, uint32_t f, void*, void*, uint64_t* tokOut) override {
        uint64_t tok = m_nextTok.fetch_add(1, std::memory_order_relaxed);
        if (tokOut) *tokOut = tok;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "-> filter=0x%X token=%llu", f, static_cast<unsigned long long>(tok));
        sc::log::Write(sc::log::Level::Info, "gameinput", "RegisterSystemButtonCallback", msg);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, void*, uint64_t* tokOut) override {
        if (tokOut) *tokOut = m_nextTok.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    void STDMETHODCALLTYPE StopCallback(uint64_t) override {}
    bool STDMETHODCALLTYPE UnregisterCallback(uint64_t, uint64_t) override { return true; }
    HRESULT STDMETHODCALLTYPE CreateDispatcher(void** d) override { if (d) *d = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(GameInputKind, IGameInputDevice** d) override { if (d) *d = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindDeviceFromId(const void* id, IGameInputDevice** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!id) return E_INVALIDARG;
        ScarlettGameInputDevice* devs[] = { m_gpDev, m_kbDev, m_msDev };
        for (ScarlettGameInputDevice* d : devs) {
            if (d && std::memcmp(d->GetDeviceInfo()->deviceId, id, 32) == 0) {
                d->AddRef();
                *out = d;
                return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice** d) override { if (d) *d = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** d) override { if (d) *d = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice** d) override { if (d) *d = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override { return S_OK; }
    void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) override {}

private:
    std::atomic<ULONG> m_refs{1};
    std::atomic<uint64_t> m_nextTok{1}, m_seq{1}, m_lastXiProbeUs{0};
    std::atomic<bool> m_xiConnected{false}, m_loggedGp{false}, m_loggedKb{false}, m_loggedMs{false};
    std::atomic<uint32_t> m_lastGpButtons{0};
    ScarlettGameInputDevice *m_gpDev{nullptr}, *m_kbDev{nullptr}, *m_msDev{nullptr};
};

} // namespace

extern "C" void WINAPI ScarlettGameInputSetSyntheticGamepad(bool enabled, const GameInputGamepadState* state) {
    AcquireSRWLockExclusive(&g_synthLock);
    g_synthEnabled = enabled;
    if (enabled && state) g_synthState = *state;
    else if (!enabled) std::memset(&g_synthState, 0, sizeof(g_synthState));
    ReleaseSRWLockExclusive(&g_synthLock);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "gameinput", "DllMain", path);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI GameInputCreate(IGameInput** gameInput) {
    if (!gameInput) return E_POINTER;
    *gameInput = new ScarlettGameInput();
    return S_OK;
}
