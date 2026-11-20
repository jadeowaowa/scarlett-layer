#include "common/stub.hpp"

#include <objbase.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr WORD kWaveFormatXma2 = 0x0166;

using NativeXAudio2CreateFn = HRESULT(WINAPI*)(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    XAUDIO2_PROCESSOR XAudio2Processor);

HMODULE g_selfModule = nullptr;
std::mutex g_sysLoadMtx;
HMODULE g_sysXAudioModule = nullptr;
NativeXAudio2CreateFn g_nativeXAudio2Create = nullptr;

const char* FormatTagName(WORD tag) {
    switch (tag) {
        case WAVE_FORMAT_PCM:        return "PCM";
        case WAVE_FORMAT_IEEE_FLOAT: return "IEEE_FLOAT";
        case WAVE_FORMAT_ADPCM:      return "MS_ADPCM";
        case WAVE_FORMAT_EXTENSIBLE: return "EXTENSIBLE";
        case kWaveFormatXma2:        return "XMA2(Xbox)";
        default:                     return "OTHER";
    }
}

bool EnsureSystemXAudioLoaded() {
    std::lock_guard<std::mutex> lock(g_sysLoadMtx);
    if (g_nativeXAudio2Create) {
        return true;
    }

    wchar_t sysDir[MAX_PATH]{};
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        sc::log::Write(sc::log::Level::Error, "xaudio2_9", "EnsureSystemXAudioLoaded",
                       "-> GetSystemDirectoryW failed");
        return false;
    }

    std::wstring sysDllPath = std::wstring(sysDir, len) + L"\\xaudio2_9.dll";
    HMODULE mod = LoadLibraryW(sysDllPath.c_str());
    if (!mod || mod == g_selfModule) {
        sc::log::Write(sc::log::Level::Error, "xaudio2_9", "EnsureSystemXAudioLoaded",
                       "-> failed to load real System32\\xaudio2_9.dll");
        return false;
    }

    auto createFn = reinterpret_cast<NativeXAudio2CreateFn>(GetProcAddress(mod, "XAudio2Create"));
    if (!createFn) {
        FreeLibrary(mod);
        sc::log::Write(sc::log::Level::Error, "xaudio2_9", "EnsureSystemXAudioLoaded",
                       "-> System32\\xaudio2_9.dll missing XAudio2Create export");
        return false;
    }

    g_sysXAudioModule = mod;
    g_nativeXAudio2Create = createFn;
    sc::log::Write(sc::log::Level::Info, "xaudio2_9", "EnsureSystemXAudioLoaded",
                   "-> bridged to native System32\\xaudio2_9.dll");
    return true;
}

class ScarlettXAudio2 final : public IXAudio2 {
public:
    ScarlettXAudio2(ComPtr<IXAudio2> native, bool ownsComInit)
        : m_native(std::move(native))
        , m_ownsComInit(ownsComInit) {}

    ~ScarlettXAudio2() {
        if (m_native) {
            XAUDIO2_PERFORMANCE_DATA perf{};
            m_native->GetPerformanceData(&perf);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "-> teardown (voicesCreated=%u, glitches=%u, activeAtExit=%u)",
                          m_sourceVoicesCreated.load(),
                          perf.GlitchesSinceEngineStarted,
                          perf.ActiveSourceVoiceCount);
            sc::log::Write(sc::log::Level::Info, "xaudio2_9", "~ScarlettXAudio2", buf);
            m_native.Reset();
        }
        if (m_ownsComInit) {
            CoUninitialize();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvInterface) override {
        if (!ppvInterface) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IXAudio2))) {
            *ppvInterface = static_cast<IXAudio2*>(this);
            AddRef();
            return S_OK;
        }
        *ppvInterface = nullptr;
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

    HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback* pCallback) override {
        return m_native->RegisterForCallbacks(pCallback);
    }

    void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback* pCallback) override {
        m_native->UnregisterForCallbacks(pCallback);
    }

    HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const WAVEFORMATEX* pSourceFormat,
        UINT32 Flags,
        float MaxFrequencyRatio,
        IXAudio2VoiceCallback* pCallback,
        const XAUDIO2_VOICE_SENDS* pSendList,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain) override {
        if (!pSourceFormat) {
            return E_INVALIDARG;
        }

        if (pSourceFormat->wFormatTag == kWaveFormatXma2) {
            // Future Xbox XMA2 hardware audio decoder bridge plugs in right here
            sc::log::Write(sc::log::Level::Warn, "xaudio2_9", "CreateSourceVoice",
                           "-> WAVE_FORMAT_XMA2 (0x0166) requested; hardware XMA2 decode not yet implemented");
            return E_NOTIMPL;
        }

        HRESULT hr = m_native->CreateSourceVoice(
            ppSourceVoice, pSourceFormat, Flags, MaxFrequencyRatio,
            pCallback, pSendList, pEffectChain);
        if (SUCCEEDED(hr)) {
            uint32_t n = m_sourceVoicesCreated.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "-> voice #%u: %s (0x%04X), %u ch, %lu Hz, %u-bit",
                              n, FormatTagName(pSourceFormat->wFormatTag),
                              pSourceFormat->wFormatTag,
                              pSourceFormat->nChannels,
                              static_cast<unsigned long>(pSourceFormat->nSamplesPerSec),
                              pSourceFormat->wBitsPerSample);
                sc::log::Write(sc::log::Level::Info, "xaudio2_9", "CreateSourceVoice", buf);
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32 Flags,
        UINT32 ProcessingStage,
        const XAUDIO2_VOICE_SENDS* pSendList,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain) override {
        return m_native->CreateSubmixVoice(
            ppSubmixVoice, InputChannels, InputSampleRate, Flags,
            ProcessingStage, pSendList, pEffectChain);
    }

    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32 Flags,
        LPCWSTR szDeviceId,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain,
        AUDIO_STREAM_CATEGORY StreamCategory) override {
        HRESULT hr = m_native->CreateMasteringVoice(
            ppMasteringVoice, InputChannels, InputSampleRate, Flags,
            szDeviceId, pEffectChain, StreamCategory);
        if (SUCCEEDED(hr) && ppMasteringVoice && *ppMasteringVoice) {
            XAUDIO2_VOICE_DETAILS details{};
            (*ppMasteringVoice)->GetVoiceDetails(&details);
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "-> mastering voice active (%u ch @ %u Hz)",
                          details.InputChannels, details.InputSampleRate);
            sc::log::Write(sc::log::Level::Info, "xaudio2_9", "CreateMasteringVoice", buf);
        } else {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "-> CreateMasteringVoice failed (hr=0x%08lX)",
                          static_cast<unsigned long>(hr));
            sc::log::Write(sc::log::Level::Warn, "xaudio2_9", "CreateMasteringVoice", buf);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE StartEngine() override {
        return m_native->StartEngine();
    }

    void STDMETHODCALLTYPE StopEngine() override {
        m_native->StopEngine();
    }

    HRESULT STDMETHODCALLTYPE CommitChanges(UINT32 OperationSet) override {
        return m_native->CommitChanges(OperationSet);
    }

    void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* pPerfData) override {
        m_native->GetPerformanceData(pPerfData);
    }

    void STDMETHODCALLTYPE SetDebugConfiguration(
        const XAUDIO2_DEBUG_CONFIGURATION* pDebugConfiguration,
        void* pReserved) override {
        m_native->SetDebugConfiguration(pDebugConfiguration, pReserved);
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::atomic<uint32_t> m_sourceVoicesCreated{0};
    ComPtr<IXAudio2> m_native;
    bool m_ownsComInit{false};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xaudio2_9", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI ScarlettXAudio2Create(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    XAUDIO2_PROCESSOR XAudio2Processor) {
    if (!ppXAudio2) {
        return E_POINTER;
    }
    *ppXAudio2 = nullptr;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = (coHr == S_OK);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        return coHr;
    }

    if (!EnsureSystemXAudioLoaded()) {
        if (ownsCom) {
            CoUninitialize();
        }
        return E_FAIL;
    }

    ComPtr<IXAudio2> nativeEngine;
    HRESULT hr = g_nativeXAudio2Create(&nativeEngine, Flags, XAudio2Processor);
    if (FAILED(hr)) {
        if (ownsCom) {
            CoUninitialize();
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "-> native XAudio2Create failed (hr=0x%08lX)",
                      static_cast<unsigned long>(hr));
        sc::log::Write(sc::log::Level::Error, "xaudio2_9", "XAudio2Create", buf);
        return hr;
    }

    sc::log::Write(sc::log::Level::Info, "xaudio2_9", "XAudio2Create",
                   "-> created wrapped IXAudio2 instance");
    *ppXAudio2 = new ScarlettXAudio2(std::move(nativeEngine), ownsCom);
    return S_OK;
}

} // extern "C"