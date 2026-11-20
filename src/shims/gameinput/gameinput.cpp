#include "gameinput.hpp"
#include "common/stub.hpp"

#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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
    if (!fg) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

float NormalizeAxis(SHORT raw, SHORT deadzone) {
    float val = static_cast<float>(raw);
    if (std::fabs(val) < static_cast<float>(deadzone)) {
        return 0.0f;
    }
    float sign = (val < 0.0f) ? -1.0f : 1.0f;
    float mag = (std::fabs(val) - static_cast<float>(deadzone)) /
                (32767.0f - static_cast<float>(deadzone));
    return sign * std::clamp(mag, 0.0f, 1.0f);
}

uint32_t MapXInputButtons(WORD xiButtons) {
    uint32_t out = GameInputGamepadNone;
    if (xiButtons & XINPUT_GAMEPAD_START)          out |= GameInputGamepadMenu;
    if (xiButtons & XINPUT_GAMEPAD_BACK)           out |= GameInputGamepadView;
    if (xiButtons & XINPUT_GAMEPAD_A)              out |= GameInputGamepadA;
    if (xiButtons & XINPUT_GAMEPAD_B)              out |= GameInputGamepadB;
    if (xiButtons & XINPUT_GAMEPAD_X)              out |= GameInputGamepadX;
    if (xiButtons & XINPUT_GAMEPAD_Y)              out |= GameInputGamepadY;
    if (xiButtons & XINPUT_GAMEPAD_DPAD_UP)        out |= GameInputGamepadDPadUp;
    if (xiButtons & XINPUT_GAMEPAD_DPAD_DOWN)      out |= GameInputGamepadDPadDown;
    if (xiButtons & XINPUT_GAMEPAD_DPAD_LEFT)      out |= GameInputGamepadDPadLeft;
    if (xiButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     out |= GameInputGamepadDPadRight;
    if (xiButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  out |= GameInputGamepadLeftShoulder;
    if (xiButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) out |= GameInputGamepadRightShoulder;
    if (xiButtons & XINPUT_GAMEPAD_LEFT_THUMB)     out |= GameInputGamepadLeftThumbstick;
    if (xiButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    out |= GameInputGamepadRightThumbstick;
    return out;
}

class ScarlettGameInputReading final : public IGameInputReading {
public:
    ScarlettGameInputReading(
        GameInputKind kind,
        uint64_t timestamp,
        bool hasGamepad,
        const GameInputGamepadState& gamepad,
        std::vector<GameInputKeyState> keys)
        : m_kind(kind)
        , m_timestamp(timestamp)
        , m_hasGamepad(hasGamepad)
        , m_gamepad(gamepad)
        , m_keys(std::move(keys)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IGameInputReading)) {
            *ppvObject = static_cast<IGameInputReading*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    GameInputKind STDMETHODCALLTYPE GetInputKind() override {
        return m_kind;
    }

    uint64_t STDMETHODCALLTYPE GetTimestamp() override {
        return m_timestamp;
    }

    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) override {
        if (!state || !m_hasGamepad) {
            return false;
        }
        *state = m_gamepad;
        return true;
    }

    uint32_t STDMETHODCALLTYPE GetKeyCount() override {
        return static_cast<uint32_t>(m_keys.size());
    }

    uint32_t STDMETHODCALLTYPE GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) override {
        if (!stateArray || stateArrayCount == 0 || m_keys.empty()) {
            return 0;
        }
        uint32_t count = std::min(stateArrayCount, static_cast<uint32_t>(m_keys.size()));
        std::memcpy(stateArray, m_keys.data(), count * sizeof(GameInputKeyState));
        return count;
    }

private:
    std::atomic<ULONG> m_refs{1};
    GameInputKind m_kind{GameInputKindUnknown};
    uint64_t m_timestamp{0};
    bool m_hasGamepad{false};
    GameInputGamepadState m_gamepad{};
    std::vector<GameInputKeyState> m_keys;
};

class ScarlettGameInput final : public IGameInput {
public:
    ScarlettGameInput() {
        sc::log::Write(sc::log::Level::Info, "gameinput", "ScarlettGameInput",
                       "-> initialized (XInput gamepad + Win32 keyboard backend)");
    }

    ~ScarlettGameInput() {
        sc::log::Write(sc::log::Level::Info, "gameinput", "~ScarlettGameInput", "-> released IGameInput");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IGameInput)) {
            *ppvObject = static_cast<IGameInput*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override {
        return QueryTimestampUs();
    }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(
        GameInputKind inputKind,
        void* /*device*/,
        IGameInputReading** reading) override {
        if (!reading) {
            return E_POINTER;
        }
        *reading = nullptr;

        uint32_t resolvedKind = GameInputKindUnknown;
        bool hasGamepad = false;
        GameInputGamepadState gp{};
        std::vector<GameInputKeyState> keys;

        if ((inputKind & GameInputKindGamepad) != 0) {
            XINPUT_STATE xi{};
            if (XInputGetState(0, &xi) == ERROR_SUCCESS) {
                hasGamepad = true;
                resolvedKind |= GameInputKindGamepad;
                gp.buttons          = MapXInputButtons(xi.Gamepad.wButtons);
                gp.leftTrigger      = static_cast<float>(xi.Gamepad.bLeftTrigger) / 255.0f;
                gp.rightTrigger     = static_cast<float>(xi.Gamepad.bRightTrigger) / 255.0f;
                gp.leftThumbstickX  = NormalizeAxis(xi.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                gp.leftThumbstickY  = NormalizeAxis(xi.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                gp.rightThumbstickX = NormalizeAxis(xi.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                gp.rightThumbstickY = NormalizeAxis(xi.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);

                if (!m_loggedGamepad.exchange(true, std::memory_order_relaxed)) {
                    sc::log::Write(sc::log::Level::Info, "gameinput", "GetCurrentReading",
                                   "-> detected active XInput controller #0");
                }
            }
        }

        if ((inputKind & GameInputKindKeyboard) != 0) {
            resolvedKind |= GameInputKindKeyboard;
            if (IsCurrentProcessForeground()) {
                static constexpr uint8_t kTrackedVks[] = {
                    'W', 'A', 'S', 'D', 'R',
                    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                    VK_SPACE, VK_SHIFT
                };
                for (uint8_t vk : kTrackedVks) {
                    if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                        GameInputKeyState ks{};
                        ks.virtualKey = vk;
                        ks.scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
                        ks.codePoint = vk;
                        ks.isDeadKey = false;
                        keys.push_back(ks);
                    }
                }
            }
        }

        if (resolvedKind == GameInputKindUnknown) {
            return GAMEINPUT_E_READING_NOT_FOUND;
        }

        *reading = new ScarlettGameInputReading(
            static_cast<GameInputKind>(resolvedKind),
            QueryTimestampUs(),
            hasGamepad,
            gp,
            std::move(keys));
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::atomic<bool> m_loggedGamepad{false};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "gameinput", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI GameInputCreate(IGameInput** gameInput) {
    if (!gameInput) {
        return E_POINTER;
    }
    *gameInput = new ScarlettGameInput();
    return S_OK;
}

} // extern "C"