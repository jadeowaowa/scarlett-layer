#include "common/codecs.hpp"
#include "common/stub.hpp"

#include <objbase.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

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
        case WAVE_FORMAT_PCM:                return "PCM";
        case WAVE_FORMAT_IEEE_FLOAT:         return "IEEE_FLOAT";
        case WAVE_FORMAT_ADPCM:              return "MS_ADPCM";
        case sc::codec::kWaveFormatImaAdpcm: return "IMA_ADPCM";
        case WAVE_FORMAT_EXTENSIBLE:         return "EXTENSIBLE";
        case sc::codec::kWaveFormatXma2:     return "XMA2(Xbox)";
        default:                             return "OTHER";
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

struct AudioDecodeTelemetry {
    std::atomic<uint64_t> buffersDecoded{0};
    std::atomic<uint64_t> compressedBytesIn{0};
    std::atomic<uint64_t> pcmBytesOut{0};
};

// Transparent IXAudio2SourceVoice wrapper that decodes compressed IMA-ADPCM blocks into
// 16-bit PCM on SubmitSourceBuffer while managing decoded buffer lifetimes via callback proxy.
class ScarlettDecodingSourceVoice final : public IXAudio2SourceVoice {
public:
    ScarlettDecodingSourceVoice(
        uint16_t blockAlign,
        IXAudio2VoiceCallback* userCallback,
        std::shared_ptr<AudioDecodeTelemetry> telemetry)
        : m_blockAlign(blockAlign)
        , m_userCallback(userCallback)
        , m_telemetry(std::move(telemetry))
        , m_cbProxy(this) {}

    void AttachNativeVoice(IXAudio2SourceVoice* nativeVoice) {
        m_native = nativeVoice;
    }

    IXAudio2VoiceCallback* GetInternalCallback() {
        return &m_cbProxy;
    }

    // ---- IXAudio2Voice methods ----
    void STDMETHODCALLTYPE GetVoiceDetails(XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        m_native->GetVoiceDetails(pVoiceDetails);
    }

    HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS* pSendList) override {
        return m_native->SetOutputVoices(pSendList);
    }

    HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN* pEffectChain) override {
        return m_native->SetEffectChain(pEffectChain);
    }

    HRESULT STDMETHODCALLTYPE EnableEffect(UINT32 EffectIndex, UINT32 OperationSet) override {
        return m_native->EnableEffect(EffectIndex, OperationSet);
    }

    HRESULT STDMETHODCALLTYPE DisableEffect(UINT32 EffectIndex, UINT32 OperationSet) override {
        return m_native->DisableEffect(EffectIndex, OperationSet);
    }

    void STDMETHODCALLTYPE GetEffectState(UINT32 EffectIndex, BOOL* pEnabled) override {
        m_native->GetEffectState(EffectIndex, pEnabled);
    }

    HRESULT STDMETHODCALLTYPE SetEffectParameters(
        UINT32 EffectIndex, const void* pParameters,
        UINT32 ParametersByteSize, UINT32 OperationSet) override {
        return m_native->SetEffectParameters(EffectIndex, pParameters, ParametersByteSize, OperationSet);
    }

    HRESULT STDMETHODCALLTYPE GetEffectParameters(
        UINT32 EffectIndex, void* pParameters, UINT32 ParametersByteSize) override {
        return m_native->GetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    }

    HRESULT STDMETHODCALLTYPE SetFilterParameters(
        const XAUDIO2_FILTER_PARAMETERS* pParameters, UINT32 OperationSet) override {
        return m_native->SetFilterParameters(pParameters, OperationSet);
    }

    void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS* pParameters) override {
        m_native->GetFilterParameters(pParameters);
    }

    HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(
        IXAudio2Voice* pDestinationVoice,
        const XAUDIO2_FILTER_PARAMETERS* pParameters,
        UINT32 OperationSet) override {
        return m_native->SetOutputFilterParameters(pDestinationVoice, pParameters, OperationSet);
    }

    void STDMETHODCALLTYPE GetOutputFilterParameters(
        IXAudio2Voice* pDestinationVoice, XAUDIO2_FILTER_PARAMETERS* pParameters) override {
        m_native->GetOutputFilterParameters(pDestinationVoice, pParameters);
    }

    HRESULT STDMETHODCALLTYPE SetVolume(float Volume, UINT32 OperationSet) override {
        return m_native->SetVolume(Volume, OperationSet);
    }

    void STDMETHODCALLTYPE GetVolume(float* pVolume) override {
        m_native->GetVolume(pVolume);
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolumes(
        UINT32 Channels, const float* pVolumes, UINT32 OperationSet) override {
        return m_native->SetChannelVolumes(Channels, pVolumes, OperationSet);
    }

    void STDMETHODCALLTYPE GetChannelVolumes(UINT32 Channels, float* pVolumes) override {
        m_native->GetChannelVolumes(Channels, pVolumes);
    }

    HRESULT STDMETHODCALLTYPE SetOutputMatrix(
        IXAudio2Voice* pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, const float* pLevelMatrix, UINT32 OperationSet) override {
        return m_native->SetOutputMatrix(
            pDestinationVoice, SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);
    }

    void STDMETHODCALLTYPE GetOutputMatrix(
        IXAudio2Voice* pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, float* pLevelMatrix) override {
        m_native->GetOutputMatrix(pDestinationVoice, SourceChannels, DestinationChannels, pLevelMatrix);
    }

    void STDMETHODCALLTYPE DestroyVoice() override {
        if (m_native) {
            m_native->DestroyVoice();
            m_native = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(m_blockMtx);
            m_liveBlocks.clear();
        }
        delete this;
    }

    // ---- IXAudio2SourceVoice methods ----
    HRESULT STDMETHODCALLTYPE Start(UINT32 Flags, UINT32 OperationSet) override {
        return m_native->Start(Flags, OperationSet);
    }

    HRESULT STDMETHODCALLTYPE Stop(UINT32 Flags, UINT32 OperationSet) override {
        return m_native->Stop(Flags, OperationSet);
    }

    HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(
        const XAUDIO2_BUFFER* pBuffer, const XAUDIO2_BUFFER_WMA* /*pBufferWMA*/) override {
        if (!pBuffer || !pBuffer->pAudioData || pBuffer->AudioBytes == 0) {
            return E_INVALIDARG;
        }

        auto block = std::make_unique<DecodedBlock>();
        block->userContext = pBuffer->pContext;
        if (!sc::codec::DecodeImaAdpcmMono(
                pBuffer->pAudioData, pBuffer->AudioBytes, m_blockAlign, block->pcm)) {
            return E_FAIL;
        }

        const size_t pcmBytes = block->pcm.size() * sizeof(int16_t);
        XAUDIO2_BUFFER nativeBuf{};
        nativeBuf.Flags = pBuffer->Flags;
        nativeBuf.AudioBytes = static_cast<UINT32>(pcmBytes);
        nativeBuf.pAudioData = reinterpret_cast<const BYTE*>(block->pcm.data());
        nativeBuf.PlayBegin = pBuffer->PlayBegin;
        nativeBuf.PlayLength = pBuffer->PlayLength;
        nativeBuf.LoopBegin = pBuffer->LoopBegin;
        nativeBuf.LoopLength = pBuffer->LoopLength;
        nativeBuf.LoopCount = pBuffer->LoopCount;

        DecodedBlock* rawKey = block.get();
        nativeBuf.pContext = rawKey;

        {
            std::lock_guard<std::mutex> lock(m_blockMtx);
            m_liveBlocks.emplace(rawKey, std::move(block));
        }

        HRESULT hr = m_native->SubmitSourceBuffer(&nativeBuf, nullptr);
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(m_blockMtx);
            m_liveBlocks.erase(rawKey);
            return hr;
        }

        if (m_telemetry) {
            m_telemetry->buffersDecoded.fetch_add(1, std::memory_order_relaxed);
            m_telemetry->compressedBytesIn.fetch_add(pBuffer->AudioBytes, std::memory_order_relaxed);
            m_telemetry->pcmBytesOut.fetch_add(pcmBytes, std::memory_order_relaxed);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FlushSourceBuffers() override {
        return m_native->FlushSourceBuffers();
    }

    HRESULT STDMETHODCALLTYPE Discontinuity() override {
        return m_native->Discontinuity();
    }

    HRESULT STDMETHODCALLTYPE ExitLoop(UINT32 OperationSet) override {
        return m_native->ExitLoop(OperationSet);
    }

    void STDMETHODCALLTYPE GetState(XAUDIO2_VOICE_STATE* pVoiceState, UINT32 Flags) override {
        m_native->GetState(pVoiceState, Flags);
        if (pVoiceState && pVoiceState->pCurrentBufferContext) {
            std::lock_guard<std::mutex> lock(m_blockMtx);
            auto it = m_liveBlocks.find(static_cast<DecodedBlock*>(pVoiceState->pCurrentBufferContext));
            if (it != m_liveBlocks.end()) {
                pVoiceState->pCurrentBufferContext = it->second->userContext;
            }
        }
    }

    HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float Ratio, UINT32 OperationSet) override {
        return m_native->SetFrequencyRatio(Ratio, OperationSet);
    }

    void STDMETHODCALLTYPE GetFrequencyRatio(float* pRatio) override {
        m_native->GetFrequencyRatio(pRatio);
    }

    HRESULT STDMETHODCALLTYPE SetSourceSampleRate(UINT32 NewSourceSampleRate) override {
        return m_native->SetSourceSampleRate(NewSourceSampleRate);
    }

private:
    struct DecodedBlock {
        std::vector<int16_t> pcm;
        void*                userContext{nullptr};
    };

    class CallbackProxy final : public IXAudio2VoiceCallback {
    public:
        explicit CallbackProxy(ScarlettDecodingSourceVoice* owner) : m_owner(owner) {}

        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 BytesRequired) override {
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnVoiceProcessingPassStart(BytesRequired);
            }
        }
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnVoiceProcessingPassEnd();
            }
        }
        void STDMETHODCALLTYPE OnStreamEnd() override {
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnStreamEnd();
            }
        }
        void STDMETHODCALLTYPE OnBufferStart(void* pBufferContext) override {
            void* userCtx = m_owner->TranslateContext(pBufferContext);
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnBufferStart(userCtx);
            }
        }
        void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override {
            void* userCtx = m_owner->ReclaimBlock(pBufferContext);
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnBufferEnd(userCtx);
            }
        }
        void STDMETHODCALLTYPE OnLoopEnd(void* pBufferContext) override {
            void* userCtx = m_owner->TranslateContext(pBufferContext);
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnLoopEnd(userCtx);
            }
        }
        void STDMETHODCALLTYPE OnVoiceError(void* pBufferContext, HRESULT Error) override {
            void* userCtx = m_owner->TranslateContext(pBufferContext);
            if (m_owner->m_userCallback) {
                m_owner->m_userCallback->OnVoiceError(userCtx, Error);
            }
        }

    private:
        ScarlettDecodingSourceVoice* m_owner;
    };

    void* TranslateContext(void* pBufferContext) {
        std::lock_guard<std::mutex> lock(m_blockMtx);
        auto it = m_liveBlocks.find(static_cast<DecodedBlock*>(pBufferContext));
        return (it != m_liveBlocks.end()) ? it->second->userContext : pBufferContext;
    }

    void* ReclaimBlock(void* pBufferContext) {
        std::unique_ptr<DecodedBlock> reclaimed;
        void* userCtx = pBufferContext;
        {
            std::lock_guard<std::mutex> lock(m_blockMtx);
            auto it = m_liveBlocks.find(static_cast<DecodedBlock*>(pBufferContext));
            if (it != m_liveBlocks.end()) {
                userCtx = it->second->userContext;
                reclaimed = std::move(it->second);
                m_liveBlocks.erase(it);
            }
        }
        return userCtx;
    }

    uint16_t                                                      m_blockAlign{sc::codec::kImaAdpcmBlockAlign};
    IXAudio2SourceVoice*                                          m_native{nullptr};
    IXAudio2VoiceCallback*                                        m_userCallback{nullptr};
    std::shared_ptr<AudioDecodeTelemetry>                         m_telemetry;
    CallbackProxy                                                 m_cbProxy;
    std::mutex                                                    m_blockMtx;
    std::unordered_map<DecodedBlock*, std::unique_ptr<DecodedBlock>> m_liveBlocks;
};

class ScarlettXAudio2 final : public IXAudio2 {
public:
    ScarlettXAudio2(ComPtr<IXAudio2> native, bool ownsComInit)
        : m_native(std::move(native))
        , m_ownsComInit(ownsComInit)
        , m_decodeTelemetry(std::make_shared<AudioDecodeTelemetry>()) {}

    ~ScarlettXAudio2() {
        if (m_native) {
            XAUDIO2_PERFORMANCE_DATA perf{};
            m_native->GetPerformanceData(&perf);
            const uint64_t decBufs = m_decodeTelemetry->buffersDecoded.load();
            const uint64_t decIn   = m_decodeTelemetry->compressedBytesIn.load();
            const uint64_t decOut  = m_decodeTelemetry->pcmBytesOut.load();
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "-> teardown (voicesCreated=%u, glitches=%u, activeAtExit=%u | adpcmDecoded=%llu bufs, %llu B -> %llu B PCM)",
                          m_sourceVoicesCreated.load(),
                          perf.GlitchesSinceEngineStarted,
                          perf.ActiveSourceVoiceCount,
                          static_cast<unsigned long long>(decBufs),
                          static_cast<unsigned long long>(decIn),
                          static_cast<unsigned long long>(decOut));
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
        if (!ppSourceVoice || !pSourceFormat) {
            return E_INVALIDARG;
        }
        *ppSourceVoice = nullptr;

        if (pSourceFormat->wFormatTag == sc::codec::kWaveFormatXma2) {
            sc::log::Write(sc::log::Level::Warn, "xaudio2_9", "CreateSourceVoice",
                           "-> WAVE_FORMAT_XMA2 (0x0166) requested; hardware XMA2 decode not yet implemented");
            return E_NOTIMPL;
        }

        // Live 4:1 IMA-ADPCM -> 16-bit PCM decoding voice bridge
        if (pSourceFormat->wFormatTag == sc::codec::kWaveFormatImaAdpcm) {
            if (pSourceFormat->nChannels != 1) {
                return E_NOTIMPL;
            }
            const uint16_t blockAlign = (pSourceFormat->nBlockAlign >= 8u)
                ? pSourceFormat->nBlockAlign
                : sc::codec::kImaAdpcmBlockAlign;

            WAVEFORMATEX pcmFmt{};
            pcmFmt.wFormatTag      = WAVE_FORMAT_PCM;
            pcmFmt.nChannels       = pSourceFormat->nChannels;
            pcmFmt.nSamplesPerSec  = pSourceFormat->nSamplesPerSec;
            pcmFmt.wBitsPerSample  = 16;
            pcmFmt.nBlockAlign     = pcmFmt.nChannels * (pcmFmt.wBitsPerSample / 8);
            pcmFmt.nAvgBytesPerSec = pcmFmt.nSamplesPerSec * pcmFmt.nBlockAlign;

            auto* wrapper = new ScarlettDecodingSourceVoice(blockAlign, pCallback, m_decodeTelemetry);
            IXAudio2SourceVoice* nativeVoice = nullptr;
            HRESULT hr = m_native->CreateSourceVoice(
                &nativeVoice, &pcmFmt, Flags, MaxFrequencyRatio,
                wrapper->GetInternalCallback(), pSendList, pEffectChain);
            if (FAILED(hr)) {
                delete wrapper;
                return hr;
            }

            wrapper->AttachNativeVoice(nativeVoice);
            *ppSourceVoice = wrapper;

            uint32_t n = m_sourceVoicesCreated.fetch_add(1, std::memory_order_relaxed) + 1;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "-> voice #%u: IMA_ADPCM (0x0011) -> PCM16 decode bridge (%u ch, %lu Hz, blockAlign=%u B)",
                          n, pSourceFormat->nChannels,
                          static_cast<unsigned long>(pSourceFormat->nSamplesPerSec),
                          static_cast<unsigned>(blockAlign));
            sc::log::Write(sc::log::Level::Info, "xaudio2_9", "CreateSourceVoice", buf);
            return S_OK;
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
    std::shared_ptr<AudioDecodeTelemetry> m_decodeTelemetry;
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

HRESULT WINAPI ScarlettXAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    XAUDIO2_PROCESSOR XAudio2Processor,
    DWORD /*ntddiVersion*/) {
    return ScarlettXAudio2Create(ppXAudio2, Flags, XAudio2Processor);
}

} // extern "C"