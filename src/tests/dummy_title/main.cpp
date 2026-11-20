#include "common/vfs.hpp"
#include "shims/xgameruntime/gdk_types.hpp"
#include "shims/d3d12_xs/d3d12_xs.hpp"
#include "shims/gameinput/gameinput.hpp"
#include "shims/xg_xs/xg_xs.hpp"
#include "shims/dstorage_xs/dstorage_xs.hpp"
#include "shims/pixevt/pixevt.hpp"
#include "shims/xmem/xmem.hpp"

#include <d3dcompiler.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
    __declspec(dllimport) HRESULT WINAPI XGameRuntimeInitialize();
    __declspec(dllimport) void    WINAPI XGameRuntimeUninitialize();
    __declspec(dllimport) XSystemDeviceType WINAPI XSystemGetDeviceType();
    __declspec(dllimport) HRESULT WINAPI XTaskQueueCreate(
        XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue);
    __declspec(dllimport) bool    WINAPI XTaskQueueDispatch(
        XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeoutInMs);
    __declspec(dllimport) void    WINAPI XTaskQueueCloseHandle(XTaskQueueHandle queue);
    __declspec(dllimport) HRESULT WINAPI XUserAddAsync(XUserAddOptions options, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser);
    __declspec(dllimport) HRESULT WINAPI XUserGetGamertag(
        XUserHandle user, XUserGamertagComponent comp, size_t size, char* out, size_t* used);
    __declspec(dllimport) void    WINAPI XUserCloseHandle(XUserHandle user);

    __declspec(dllimport) HRESULT WINAPI XGameSaveInitializeProviderAsync(
        XUserHandle requestingUser, const char* configurationId, bool syncOnDemand, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveInitializeProviderResult(
        XAsyncBlock* async, XGameSaveProviderHandle* provider);
    __declspec(dllimport) void    WINAPI XGameSaveCloseProvider(XGameSaveProviderHandle provider);
    __declspec(dllimport) HRESULT WINAPI XGameSaveCreateContainer(
        XGameSaveProviderHandle provider, const char* name, XGameSaveContainerHandle* result);
    __declspec(dllimport) void    WINAPI XGameSaveCloseContainer(XGameSaveContainerHandle context);
    __declspec(dllimport) HRESULT WINAPI XGameSaveCreateUpdate(
        XGameSaveContainerHandle container, const char* displayName, XGameSaveUpdateHandle* updateContext);
    __declspec(dllimport) void    WINAPI XGameSaveCloseUpdate(XGameSaveUpdateHandle context);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitBlobWrite(
        XGameSaveUpdateHandle updateContext, const char* blobName, const uint8_t* data, size_t byteCount);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitUpdateAsync(
        XGameSaveUpdateHandle updateContext, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveSubmitUpdateResult(XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveReadBlobDataAsync(
        XGameSaveContainerHandle container, const char** blobNames, uint32_t countOfBlobs, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XGameSaveReadBlobDataResult(
        XAsyncBlock* async, size_t blobsSize, XGameSaveBlob* blobData, uint32_t* countOfBlobs);
}

namespace {

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kTexSize = 256;
constexpr uint32_t kAudioRate = 48000;
constexpr float kTwoPi = 6.28318530718f;
constexpr char kTitleScid[] = "00000000-0000-0000-0000-0000SCARLETT";

using PluginRunDiagnosticsFn = bool(WINAPI*)();

template <typename T>
class XMemBuffer {
public:
    XMemBuffer() = default;
    ~XMemBuffer() { Reset(); }

    XMemBuffer(const XMemBuffer&) = delete;
    XMemBuffer& operator=(const XMemBuffer&) = delete;

    bool Allocate(size_t elementCount, uint32_t attrs) {
        Reset();
        m_attrs = attrs;
        m_count = elementCount;
        m_ptr = static_cast<T*>(XMemAlloc(elementCount * sizeof(T), attrs));
        if (!m_ptr) {
            m_count = 0;
            return false;
        }
        return true;
    }

    void Reset() {
        if (m_ptr) {
            XMemFree(m_ptr, m_attrs);
            m_ptr = nullptr;
            m_count = 0;
        }
    }

    T*       Data()       { return m_ptr; }
    const T* Data() const { return m_ptr; }
    size_t   Count() const { return m_count; }
    size_t   ByteSize() const { return m_count * sizeof(T); }
    size_t   QueriedByteSize() const { return XMemSize(m_ptr, m_attrs); }
    T&       operator[](size_t i) { return m_ptr[i]; }

private:
    T*       m_ptr{nullptr};
    size_t   m_count{0};
    uint32_t m_attrs{0};
};

class TitleAudio {
public:
    bool Init() {
        if (FAILED(XAudio2Create(&m_engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            std::printf("[TITLE] XAudio2Create failed; continuing muted.\n");
            return false;
        }
        if (FAILED(m_engine->CreateMasteringVoice(&m_master))) {
            std::printf("[TITLE] No audio output device available; continuing muted.\n");
            Shutdown();
            return false;
        }

        if (!BuildBootChordPcm() || !BuildThrusterHumPcm() || !BuildSaveChimePcm()) {
            Shutdown();
            return false;
        }

        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = kAudioRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = wf.nChannels * (wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        if (FAILED(m_engine->CreateSourceVoice(&m_bootVoice, &wf, 0, 2.0f)) ||
            FAILED(m_engine->CreateSourceVoice(&m_humVoice, &wf, 0, 4.0f)) ||
            FAILED(m_engine->CreateSourceVoice(&m_saveVoice, &wf, 0, 2.0f))) {
            Shutdown();
            return false;
        }

        XAUDIO2_BUFFER humBuf{};
        humBuf.AudioBytes = static_cast<UINT32>(m_humPcm.ByteSize());
        humBuf.pAudioData = reinterpret_cast<const BYTE*>(m_humPcm.Data());
        humBuf.LoopCount = XAUDIO2_LOOP_INFINITE;
        m_humVoice->SetVolume(0.03f);
        m_humVoice->SetFrequencyRatio(0.85f);
        m_humVoice->SubmitSourceBuffer(&humBuf);
        m_humVoice->Start(0);

        XAUDIO2_BUFFER bootBuf{};
        bootBuf.AudioBytes = static_cast<UINT32>(m_bootPcm.ByteSize());
        bootBuf.pAudioData = reinterpret_cast<const BYTE*>(m_bootPcm.Data());
        bootBuf.Flags = XAUDIO2_END_OF_STREAM;
        m_bootVoice->SetVolume(0.28f);
        m_bootVoice->SubmitSourceBuffer(&bootBuf);
        m_bootVoice->Start(0);

        m_active = true;
        return true;
    }

    void UpdateThruster(float moveMag, bool turbo, float dt) {
        if (!m_active || !m_humVoice) {
            return;
        }
        const float targetVol = turbo ? 0.20f : (0.03f + 0.08f * std::clamp(moveMag, 0.0f, 1.0f));
        const float targetPitch = turbo ? 1.65f : (0.85f + 0.30f * std::clamp(moveMag, 0.0f, 1.0f));
        const float blend = std::clamp(dt * 8.0f, 0.0f, 1.0f);

        m_curVol += (targetVol - m_curVol) * blend;
        m_curPitch += (targetPitch - m_curPitch) * blend;

        m_humVoice->SetVolume(m_curVol);
        m_humVoice->SetFrequencyRatio(m_curPitch);
    }

    void PlaySaveChime() {
        if (!m_active || !m_saveVoice) {
            return;
        }
        m_saveVoice->Stop(0);
        m_saveVoice->FlushSourceBuffers();

        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(m_savePcm.ByteSize());
        buf.pAudioData = reinterpret_cast<const BYTE*>(m_savePcm.Data());
        buf.Flags = XAUDIO2_END_OF_STREAM;
        m_saveVoice->SetVolume(0.32f);
        m_saveVoice->SubmitSourceBuffer(&buf);
        m_saveVoice->Start(0);
    }

    void Shutdown() {
        m_active = false;
        auto destroyVoice = [](IXAudio2SourceVoice*& v) {
            if (v) {
                v->Stop(0);
                v->FlushSourceBuffers();
                v->DestroyVoice();
                v = nullptr;
            }
        };
        destroyVoice(m_bootVoice);
        destroyVoice(m_humVoice);
        destroyVoice(m_saveVoice);
        if (m_master) {
            m_master->DestroyVoice();
            m_master = nullptr;
        }
        m_engine.Reset();
        m_bootPcm.Reset();
        m_humPcm.Reset();
        m_savePcm.Reset();
    }

    ~TitleAudio() {
        Shutdown();
    }

private:
    static int16_t FloatToPcm16(float sample) {
        float clamped = std::clamp(sample, -1.0f, 1.0f);
        return static_cast<int16_t>(clamped * 32000.0f);
    }

    static constexpr uint32_t kAudioPoolAttrs = MakeXAllocAttributes(
        XALLOC_ID_AUDIO_PCM, XALLOC_ALIGNMENT_64, XALLOC_MEMTYPE_HEAP, true);

    bool BuildBootChordPcm() {
        const uint32_t samples = static_cast<uint32_t>(kAudioRate * 0.90f);
        if (!m_bootPcm.Allocate(samples, kAudioPoolAttrs)) {
            return false;
        }
        constexpr float freqs[4] = { 261.63f, 392.00f, 587.33f, 659.25f };
        constexpr float starts[4] = { 0.00f, 0.07f, 0.14f, 0.21f };

        for (uint32_t i = 0; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kAudioRate);
            float mix = 0.0f;
            for (int n = 0; n < 4; ++n) {
                if (t < starts[n]) {
                    continue;
                }
                const float nt = t - starts[n];
                const float env = std::min(1.0f, nt / 0.04f) * std::expf(-nt * 3.8f);
                const float tone = std::sinf(kTwoPi * freqs[n] * nt)
                                 + 0.25f * std::sinf(kTwoPi * freqs[n] * 2.0f * nt);
                mix += tone * env * 0.24f;
            }
            m_bootPcm[i] = FloatToPcm16(mix);
        }
        return true;
    }

    bool BuildThrusterHumPcm() {
        const uint32_t samples = kAudioRate;
        if (!m_humPcm.Allocate(samples, kAudioPoolAttrs)) {
            return false;
        }
        for (uint32_t i = 0; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kAudioRate);
            const float lfo = 0.85f + 0.15f * std::sinf(kTwoPi * 3.0f * t);
            const float wave = 0.50f * std::sinf(kTwoPi * 84.0f * t)
                             + 0.30f * std::sinf(kTwoPi * 126.0f * t)
                             + 0.15f * std::sinf(kTwoPi * 168.0f * t)
                             + 0.05f * std::sinf(kTwoPi * 252.0f * t);
            m_humPcm[i] = FloatToPcm16(wave * lfo);
        }
        return true;
    }

    bool BuildSaveChimePcm() {
        const uint32_t samples = static_cast<uint32_t>(kAudioRate * 0.38f);
        if (!m_savePcm.Allocate(samples, kAudioPoolAttrs)) {
            return false;
        }
        for (uint32_t i = 0; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kAudioRate);
            float mix = 0.0f;
            {
                const float env1 = std::min(1.0f, t / 0.006f) * std::expf(-t * 14.0f);
                mix += std::sinf(kTwoPi * 880.0f * t) * env1 * 0.45f;
            }
            if (t >= 0.08f) {
                const float t2 = t - 0.08f;
                const float env2 = std::min(1.0f, t2 / 0.006f) * std::expf(-t2 * 10.0f);
                mix += (std::sinf(kTwoPi * 1318.51f * t2) + 0.20f * std::sinf(kTwoPi * 2637.02f * t2))
                     * env2 * 0.45f;
            }
            m_savePcm[i] = FloatToPcm16(mix);
        }
        return true;
    }

    XMemBuffer<int16_t>     m_bootPcm;
    XMemBuffer<int16_t>     m_humPcm;
    XMemBuffer<int16_t>     m_savePcm;

    ComPtr<IXAudio2>        m_engine;
    IXAudio2MasteringVoice* m_master{nullptr};
    IXAudio2SourceVoice*    m_bootVoice{nullptr};
    IXAudio2SourceVoice*    m_humVoice{nullptr};
    IXAudio2SourceVoice*    m_saveVoice{nullptr};
    float                   m_curVol{0.03f};
    float                   m_curPitch{0.85f};
    bool                    m_active{false};
};

struct StarSaveData {
    uint32_t magic;
    uint32_t version;
    float    posX;
    float    posY;
    float    angle;
    uint32_t saveCount;
};
constexpr uint32_t kSaveMagic = 0x53434152U;

struct ScarlettTexAssetHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat;
    uint32_t tileMode;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t fnv1aChecksum;
};
static_assert(sizeof(ScarlettTexAssetHeader) == 32, "Asset header must be 32 bytes");
constexpr uint32_t kAssetMagic = 0x58544353U;

uint32_t ComputeFnv1a32(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

struct AsyncWaiter {
    std::atomic<bool> done{false};
};

void CALLBACK OnSimpleComplete(XAsyncBlock* async) {
    auto* w = static_cast<AsyncWaiter*>(async->context);
    w->done.store(true, std::memory_order_release);
}

void PumpUntilDone(XTaskQueueHandle queue, AsyncWaiter& waiter) {
    while (!waiter.done.load(std::memory_order_acquire)) {
        XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 50);
    }
}

bool LoadStarState(
    XTaskQueueHandle queue,
    XGameSaveContainerHandle container,
    StarSaveData& outData) {
    AsyncWaiter waiter{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &waiter;
    async.callback = OnSimpleComplete;

    const char* blobNames[] = { "star_transform" };
    if (FAILED(XGameSaveReadBlobDataAsync(container, blobNames, 1, &async))) {
        return false;
    }
    PumpUntilDone(queue, waiter);

    alignas(8) uint8_t buffer[512]{};
    uint32_t blobCount = 0;
    HRESULT hr = XGameSaveReadBlobDataResult(
        &async, sizeof(buffer), reinterpret_cast<XGameSaveBlob*>(buffer), &blobCount);
    if (FAILED(hr) || blobCount == 0) {
        return false;
    }

    auto* blob = reinterpret_cast<XGameSaveBlob*>(buffer);
    if (blob->info.size != sizeof(StarSaveData) || !blob->data) {
        return false;
    }

    StarSaveData loaded{};
    std::memcpy(&loaded, blob->data, sizeof(StarSaveData));
    if (loaded.magic != kSaveMagic || loaded.version != 1) {
        return false;
    }

    outData = loaded;
    return true;
}

bool SaveStarState(
    XTaskQueueHandle queue,
    XGameSaveContainerHandle container,
    float posX,
    float posY,
    float angle,
    uint32_t& saveCount) {
    XGameSaveUpdateHandle upd = nullptr;
    if (FAILED(XGameSaveCreateUpdate(container, "Star Transform", &upd))) {
        return false;
    }

    StarSaveData payload{};
    payload.magic = kSaveMagic;
    payload.version = 1;
    payload.posX = posX;
    payload.posY = posY;
    payload.angle = angle;
    payload.saveCount = saveCount + 1;

    HRESULT hr = XGameSaveSubmitBlobWrite(
        upd, "star_transform",
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    if (FAILED(hr)) {
        XGameSaveCloseUpdate(upd);
        return false;
    }

    AsyncWaiter waiter{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &waiter;
    async.callback = OnSimpleComplete;

    hr = XGameSaveSubmitUpdateAsync(upd, &async);
    if (SUCCEEDED(hr)) {
        PumpUntilDone(queue, waiter);
        hr = XGameSaveSubmitUpdateResult(&async);
    }
    XGameSaveCloseUpdate(upd);

    if (SUCCEEDED(hr)) {
        saveCount = payload.saveCount;
        std::printf("[TITLE] Saved state #%u -> pos=(%.2f, %.2f), angle=%.2f rad\n",
                    saveCount, posX, posY, angle);
        return true;
    }
    return false;
}

struct Vertex {
    float x, y;
    float r, g, b;
    float u, v;
};

struct DrawConstants {
    float offsetX;
    float offsetY;
    float angle;
    float pulse;
    float aspect;
    float scale;
    float turbo;
    float timeSec;
};
static_assert(sizeof(DrawConstants) == 32, "DrawConstants must be 8x 32-bit values");

const char* kShaderSource = R"(
Texture2D<float4> gCrestTex : register(t0);
SamplerState      gSampler  : register(s0);

cbuffer DrawParams : register(b0) {
    float2 uOffset;
    float  uAngle;
    float  uPulse;
    float  uAspect;
    float  uScale;
    float  uTurbo;
    float  uTime;
};

struct VSIn {
    float2 pos : POSITION;
    float3 col : COLOR;
    float2 uv  : TEXCOORD0;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float3 col : COLOR;
    float2 uv  : TEXCOORD0;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    float s = sin(uAngle);
    float c = cos(uAngle);
    float2 rot = float2(
        input.pos.x * c - input.pos.y * s,
        input.pos.x * s + input.pos.y * c
    );
    rot *= uScale;
    rot.x /= uAspect;
    o.pos = float4(rot + uOffset, 0.0f, 1.0f);
    o.col = input.col;
    o.uv  = input.uv;
    return o;
}

float4 PSMain(PSIn input) : SV_TARGET {
    float2 animUv = input.uv + float2(uTime * 0.08f, -uTime * 0.05f);
    float3 texCol = gCrestTex.Sample(gSampler, animUv).rgb;
    float3 turboTint = float3(0.18f, 0.96f, 1.00f);
    float3 combined = lerp(input.col * (0.45f + 0.90f * texCol), turboTint * (0.55f + 0.75f * texCol), uTurbo * 0.55f);
    float3 lit = saturate(combined * (0.85f + 0.30f * uPulse));
    return float4(lit, 1.0f);
}
)";

const Vertex kStarVertices[] = {
    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    {  0.00f,  0.55f,  0.10f, 0.95f, 0.35f,  0.50f, 0.00f },
    {  0.14f,  0.14f,  0.06f, 0.65f, 0.32f,  0.63f, 0.37f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    {  0.14f,  0.14f,  0.06f, 0.65f, 0.32f,  0.63f, 0.37f },
    {  0.55f,  0.00f,  0.08f, 0.88f, 0.78f,  1.00f, 0.50f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    {  0.55f,  0.00f,  0.08f, 0.88f, 0.78f,  1.00f, 0.50f },
    {  0.14f, -0.14f,  0.06f, 0.55f, 0.55f,  0.63f, 0.63f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    {  0.14f, -0.14f,  0.06f, 0.55f, 0.55f,  0.63f, 0.63f },
    {  0.00f, -0.55f,  0.18f, 0.96f, 0.40f,  0.50f, 1.00f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    {  0.00f, -0.55f,  0.18f, 0.96f, 0.40f,  0.50f, 1.00f },
    { -0.14f, -0.14f,  0.06f, 0.65f, 0.32f,  0.37f, 0.63f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    { -0.14f, -0.14f,  0.06f, 0.65f, 0.32f,  0.37f, 0.63f },
    { -0.55f,  0.00f,  0.08f, 0.88f, 0.78f,  0.00f, 0.50f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    { -0.55f,  0.00f,  0.08f, 0.88f, 0.78f,  0.00f, 0.50f },
    { -0.14f,  0.14f,  0.06f, 0.55f, 0.55f,  0.37f, 0.37f },

    {  0.00f,  0.00f,  0.92f, 1.00f, 0.95f,  0.50f, 0.50f },
    { -0.14f,  0.14f,  0.06f, 0.55f, 0.55f,  0.37f, 0.37f },
    {  0.00f,  0.55f,  0.10f, 0.95f, 0.35f,  0.50f, 0.00f },
};

std::vector<uint8_t> BuildProceduralCrestTexture(uint32_t size) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t cell = 32;
            const bool gridLine = ((x % cell) < 3) || ((y % cell) < 3);
            const bool checker = (((x / cell) + (y / cell)) & 1u) != 0;
            const float fx = (static_cast<float>(x) / static_cast<float>(size)) * 2.0f - 1.0f;
            const float fy = (static_cast<float>(y) / static_cast<float>(size)) * 2.0f - 1.0f;
            const float ring = 0.5f + 0.5f * std::sinf(std::sqrtf(fx * fx + fy * fy) * 18.0f);

            uint8_t r = gridLine ? 210 : (checker ? 42 : 24);
            uint8_t g = gridLine ? 255 : static_cast<uint8_t>(110 + 115 * ring);
            uint8_t b = gridLine ? 235 : static_cast<uint8_t>(checker ? (145 + 75 * ring) : (85 + 65 * ring));

            const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
            pixels[idx + 3] = 255;
        }
    }
    return pixels;
}

bool EnsureBakedConsoleTextureAsset(
    const std::filesystem::path& assetPath,
    IXGTextureAddressComputer* tiledComputer,
    const XG_RESOURCE_LAYOUT& tiledLayout) {
    std::error_code ec;
    if (std::filesystem::exists(assetPath, ec)) {
        return true;
    }

    std::filesystem::create_directories(assetPath.parent_path(), ec);
    std::vector<uint8_t> linearSrc = BuildProceduralCrestTexture(kTexSize);

    XMemBuffer<uint8_t> swizzled;
    const uint32_t physAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    if (!swizzled.Allocate(static_cast<size_t>(tiledLayout.SizeBytes), physAttrs)) {
        return false;
    }

    if (FAILED(tiledComputer->CopyIntoSubresource(
            swizzled.Data(), 0, linearSrc.data(), kTexSize * 4, 0))) {
        return false;
    }

    ScarlettTexAssetHeader hdr{};
    hdr.magic = kAssetMagic;
    hdr.width = kTexSize;
    hdr.height = kTexSize;
    hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    hdr.tileMode = static_cast<uint32_t>(XG_TILE_MODE_2D_THIN);
    hdr.dataOffset = sizeof(ScarlettTexAssetHeader);
    hdr.dataSize = static_cast<uint32_t>(swizzled.ByteSize());
    hdr.fnv1aChecksum = ComputeFnv1a32(swizzled.Data(), swizzled.ByteSize());

    std::ofstream out(assetPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(swizzled.Data()),
              static_cast<std::streamsize>(swizzled.ByteSize()));
    out.flush();
    if (!out.good()) {
        return false;
    }

    std::printf("[TITLE] Baked pre-swizzled console texture asset: %s (%u B payload, crc=0x%08X)\n",
                assetPath.string().c_str(), hdr.dataSize, hdr.fnv1aChecksum);
    return true;
}

bool CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        kShaderSource, std::strlen(kShaderSource),
        "scarlett_star_textured.hlsl", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &outBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            std::printf("[TITLE] Shader compile error (%s): %s\n",
                        entry, static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("[TITLE][T:%05lu] Booting synthetic Series X title...\n", GetCurrentThreadId());

    // 0a. Exercise direct runtime LoadLibraryA/W IAT hooks
    HMODULE hPix = LoadLibraryW(L"pixevt.dll");
    PIXBeginEventFn pixBegin = nullptr;
    PIXEndEventFn   pixEnd   = nullptr;
    PIXSetMarkerFn  pixMark  = nullptr;
    if (hPix) {
        pixBegin = reinterpret_cast<PIXBeginEventFn>(GetProcAddress(hPix, "PIXBeginEvent"));
        pixEnd   = reinterpret_cast<PIXEndEventFn>(GetProcAddress(hPix, "PIXEndEvent"));
        pixMark  = reinterpret_cast<PIXSetMarkerFn>(GetProcAddress(hPix, "PIXSetMarker"));
        if (pixMark) {
            pixMark(0xFF00FF66ULL, "BootDynamicHooksVerified");
        }
    }

    // 0b. Exercise multi-module hooking: load middleware DLL that dynamically loads Xbox shims!
    HMODULE hPlugin = LoadLibraryW(L"engine_plugin.dll");
    if (hPlugin) {
        auto runDiag = reinterpret_cast<PluginRunDiagnosticsFn>(
            GetProcAddress(hPlugin, "PluginRunDiagnostics"));
        if (runDiag) {
            runDiag();
        }
        FreeLibrary(hPlugin);
    }

    if (FAILED(XGameRuntimeInitialize())) {
        return 1;
    }

    auto deviceType = XSystemGetDeviceType();
    std::printf("[TITLE] Device type: 0x%02X\n", static_cast<uint32_t>(deviceType));

    XTaskQueueHandle queue = nullptr;
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                XTaskQueueDispatchMode::Manual, &queue))) {
        return 1;
    }

    // 1. Async offline profile sign-in
    XUserHandle user = nullptr;
    {
        AsyncWaiter waiter{};
        XAsyncBlock async{};
        async.queue = queue;
        async.context = &waiter;
        async.callback = OnSimpleComplete;
        if (SUCCEEDED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async))) {
            PumpUntilDone(queue, waiter);
            if (SUCCEEDED(XUserAddResult(&async, &user)) && user) {
                char tag[64]{};
                size_t used = 0;
                if (SUCCEEDED(XUserGetGamertag(user, XUserGamertagComponent::Classic,
                                               sizeof(tag), tag, &used))) {
                    std::printf("[TITLE][T:%05lu] Signed in as: %s\n", GetCurrentThreadId(), tag);
                }
            }
        }
    }

    // 2. Async Connected Storage (XGameSave) mount & restore
    XGameSaveProviderHandle saveProvider = nullptr;
    XGameSaveContainerHandle saveContainer = nullptr;
    float posX = 0.0f;
    float posY = 0.0f;
    float angle = 0.0f;
    uint32_t saveCount = 0;

    {
        AsyncWaiter waiter{};
        XAsyncBlock async{};
        async.queue = queue;
        async.context = &waiter;
        async.callback = OnSimpleComplete;

        if (SUCCEEDED(XGameSaveInitializeProviderAsync(user, kTitleScid, false, &async))) {
            PumpUntilDone(queue, waiter);
            if (SUCCEEDED(XGameSaveInitializeProviderResult(&async, &saveProvider)) && saveProvider) {
                if (SUCCEEDED(XGameSaveCreateContainer(saveProvider, "player_state", &saveContainer))) {
                    StarSaveData restored{};
                    if (LoadStarState(queue, saveContainer, restored)) {
                        posX = restored.posX;
                        posY = restored.posY;
                        angle = restored.angle;
                        saveCount = restored.saveCount;
                        std::printf("[TITLE] Restored save #%u -> pos=(%.2f, %.2f), angle=%.2f rad\n",
                                    saveCount, posX, posY, angle);
                    } else {
                        std::printf("[TITLE] No existing save found; starting at origin.\n");
                    }
                }
            }
        }
    }

    // 3. Initialize XAudio2 through our xaudio2_9.dll + xmem.dll shims
    TitleAudio audio;
    audio.Init();

    // 4. Initialize GameInput
    ComPtr<IGameInput> gameInput;
    if (FAILED(GameInputCreate(&gameInput))) {
        return 1;
    }

    // 5. Scarlett D3D12.x graphics bootstrap
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;

    ComPtr<ID3D12DeviceX> deviceX;
    if (FAILED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX,
                                     reinterpret_cast<void**>(deviceX.GetAddressOf())))) {
        return 1;
    }

    deviceX->SetFrameIntervalX(nullptr, 16667, 1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE);

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> cmdQueue;
    if (FAILED(deviceX->CreateCommandQueueX(&qDesc, &cmdQueue))) {
        return 1;
    }

    ComPtr<ID3D12Fence> ioFence;
    if (FAILED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ioFence)))) {
        return 1;
    }
    HANDLE ioFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 6. Velocity asset streaming into 64 KB-aligned xmem.dll Physical memory -> xg_xs detile -> GPU
    XG_TEXTURE2D_DESC xgTiledDesc{};
    xgTiledDesc.Width = kTexSize;
    xgTiledDesc.Height = kTexSize;
    xgTiledDesc.MipLevels = 1;
    xgTiledDesc.ArraySize = 1;
    xgTiledDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    xgTiledDesc.TileMode = XG_TILE_MODE_2D_THIN;

    ComPtr<IXGTextureAddressComputer> tiledComputer;
    XG_RESOURCE_LAYOUT tiledLayout{};
    if (FAILED(XGCreateTexture2DComputer(&xgTiledDesc, &tiledComputer)) ||
        FAILED(tiledComputer->GetResourceLayout(&tiledLayout))) {
        return 1;
    }

    XG_TEXTURE2D_DESC xgLinearDesc = xgTiledDesc;
    xgLinearDesc.TileMode = XG_TILE_MODE_LINEAR;
    XG_RESOURCE_LAYOUT linearLayout{};
    if (FAILED(XGComputeTexture2DLayout(&xgLinearDesc, &linearLayout))) {
        return 1;
    }

    const wchar_t* kVirtualAssetPath = L"G:\\assets\\crest_2dthin.scx";
    const std::filesystem::path assetPath = sc::vfs::TranslatePath(kVirtualAssetPath);
    if (!EnsureBakedConsoleTextureAsset(assetPath, tiledComputer.Get(), tiledLayout)) {
        return 1;
    }

    DSTORAGE_CONFIGURATION dsCfg{};
    dsCfg.NumSubmitThreads = 1;
    dsCfg.NumBuiltInCpuDecompressionThreads = 1;
    DStorageSetConfiguration(&dsCfg);

    ComPtr<IDStorageFactoryX> dsFactory;
    if (FAILED(DStorageGetFactory(IID_IDStorageFactoryX,
                                  reinterpret_cast<void**>(dsFactory.GetAddressOf())))) {
        return 1;
    }

    DSTORAGE_QUEUE_DESC dsQueueDesc{};
    dsQueueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    dsQueueDesc.Capacity = 64;
    dsQueueDesc.Priority = DSTORAGE_PRIORITY_HIGH;
    dsQueueDesc.Name = "AssetStreamQueue";
    dsQueueDesc.Device = deviceX.Get();

    ComPtr<IDStorageQueueX> dsQueue;
    ComPtr<IDStorageFileX> dsFile;
    ComPtr<IDStorageStatusArrayX> dsStatus;
    if (FAILED(dsFactory->CreateQueue(&dsQueueDesc, IID_IDStorageQueueX,
                                      reinterpret_cast<void**>(dsQueue.GetAddressOf()))) ||
        FAILED(dsFactory->OpenFile(kVirtualAssetPath, IID_IDStorageFileX,
                                   reinterpret_cast<void**>(dsFile.GetAddressOf()))) ||
        FAILED(dsFactory->CreateStatusArray(2, "AssetStatus", IID_IDStorageStatusArrayX,
                                            reinterpret_cast<void**>(dsStatus.GetAddressOf())))) {
        return 1;
    }

    ScarlettTexAssetHeader streamedHdr{};
    XMemBuffer<uint8_t> streamedTiledMem;
    const uint32_t tiledPhysAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_TILED, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, true);
    if (!streamedTiledMem.Allocate(static_cast<size_t>(tiledLayout.SizeBytes), tiledPhysAttrs)) {
        return 1;
    }

    const bool aligned64k = (reinterpret_cast<uintptr_t>(streamedTiledMem.Data()) % 65536ULL) == 0;
    const bool sizeOk = (streamedTiledMem.QueriedByteSize() == static_cast<size_t>(tiledLayout.SizeBytes));
    std::printf("[TITLE] xmem Physical buffer: 64KB-aligned=%s, XMemSize=%zu B (%s)\n",
                aligned64k ? "YES" : "NO",
                streamedTiledMem.QueriedByteSize(),
                sizeOk ? "EXACT" : "MISMATCH");

    DSTORAGE_REQUEST hdrReq{};
    hdrReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    hdrReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    hdrReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    hdrReq.Source.File.Source = dsFile.Get();
    hdrReq.Source.File.Offset = 0;
    hdrReq.Source.File.Size = sizeof(ScarlettTexAssetHeader);
    hdrReq.Destination.Memory.Buffer = &streamedHdr;
    hdrReq.Destination.Memory.Size = sizeof(ScarlettTexAssetHeader);
    hdrReq.UncompressedSize = sizeof(ScarlettTexAssetHeader);
    hdrReq.Name = "crest_header";

    DSTORAGE_REQUEST bodyReq{};
    bodyReq.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    bodyReq.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    bodyReq.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    bodyReq.Source.File.Source = dsFile.Get();
    bodyReq.Source.File.Offset = sizeof(ScarlettTexAssetHeader);
    bodyReq.Source.File.Size = static_cast<uint32_t>(streamedTiledMem.ByteSize());
    bodyReq.Destination.Memory.Buffer = streamedTiledMem.Data();
    bodyReq.Destination.Memory.Size = static_cast<uint32_t>(streamedTiledMem.ByteSize());
    bodyReq.UncompressedSize = static_cast<uint32_t>(streamedTiledMem.ByteSize());
    bodyReq.Name = "crest_2dthin_payload";

    dsQueue->EnqueueRequest(&hdrReq);
    dsQueue->EnqueueRequest(&bodyReq);
    dsQueue->EnqueueStatus(dsStatus.Get(), 0);
    dsQueue->EnqueueSignal(ioFence.Get(), 1);
    dsQueue->Submit();

    if (ioFence->GetCompletedValue() < 1) {
        ioFence->SetEventOnCompletion(1, ioFenceEvent);
        WaitForSingleObject(ioFenceEvent, INFINITE);
    }

    const bool statusOk = dsStatus->IsComplete(0) && SUCCEEDED(dsStatus->GetHResult(0));
    const uint32_t actualCrc = ComputeFnv1a32(streamedTiledMem.Data(), streamedTiledMem.ByteSize());
    if (!statusOk || streamedHdr.magic != kAssetMagic || actualCrc != streamedHdr.fnv1aChecksum) {
        return 1;
    }

    std::printf("[TITLE] DirectStorage streamed & verified '%s' via ID3D12Fence (crc=0x%08X MATCH)\n",
                assetPath.string().c_str(), actualCrc);
    if (pixMark) {
        pixMark(0xFF00CCFFULL, "DirectStorageAssetReady");
    }

    XMemBuffer<uint8_t> detiledUploadMem;
    const uint32_t uploadHeapAttrs = MakeXAllocAttributes(
        XALLOC_ID_TEXTURE_UPLOAD, XALLOC_ALIGNMENT_256, XALLOC_MEMTYPE_HEAP, true);
    if (!detiledUploadMem.Allocate(static_cast<size_t>(linearLayout.SizeBytes), uploadHeapAttrs)) {
        return 1;
    }

    if (FAILED(tiledComputer->CopyFromSubresource(
            detiledUploadMem.Data(), 0, streamedTiledMem.Data(),
            linearLayout.Plane[0].MipLayout[0].PitchBytes, 0))) {
        return 1;
    }

    streamedTiledMem.Reset();

    dsFile->Close();
    dsFile.Reset();
    dsStatus.Reset();
    dsQueue.Reset();
    dsFactory.Reset();
    CloseHandle(ioFenceEvent);
    ioFence.Reset();

    // 7. Build Root Signature, PSO (cached by d3d12_xs!), Vertex Buffer, Texture SRV, and Render Target
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = sizeof(DrawConstants) / sizeof(uint32_t);
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MaxAnisotropy = 1;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        return 1;
    }

    ComPtr<ID3D12RootSignature> rootSig;
    if (FAILED(deviceX->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSig)))) {
        return 1;
    }

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader("VSMain", "vs_5_0", vsBlob) || !CompileShader("PSMain", "ps_5_0", psBlob)) {
        return 1;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(_countof(inputLayout)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(deviceX->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) {
        return 1;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = sizeof(kStarVertices);
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.Format = DXGI_FORMAT_UNKNOWN;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> vertexBuffer;
    if (FAILED(deviceX->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer)))) {
        return 1;
    }

    void* mappedVb = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(vertexBuffer->Map(0, &readRange, &mappedVb))) {
        return 1;
    }
    std::memcpy(mappedVb, kStarVertices, sizeof(kStarVertices));
    vertexBuffer->Unmap(0, nullptr);

    D3D12_VERTEX_BUFFER_VIEW vbView{};
    vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vbView.StrideInBytes = sizeof(Vertex);
    vbView.SizeInBytes = sizeof(kStarVertices);

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kTexSize;
    texDesc.Height = kTexSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> crestTexture;
    if (FAILED(deviceX->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&crestTexture)))) {
        return 1;
    }

    D3D12_RESOURCE_DESC texUploadDesc = vbDesc;
    texUploadDesc.Width = linearLayout.SizeBytes;

    ComPtr<ID3D12Resource> texUpload;
    if (FAILED(deviceX->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &texUploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texUpload)))) {
        return 1;
    }

    void* mappedTex = nullptr;
    if (FAILED(texUpload->Map(0, &readRange, &mappedTex))) {
        return 1;
    }
    std::memcpy(mappedTex, detiledUploadMem.Data(), detiledUploadMem.ByteSize());
    texUpload->Unmap(0, nullptr);
    detiledUploadMem.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    if (FAILED(deviceX->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)))) {
        return 1;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    deviceX->CreateShaderResourceView(crestTexture.Get(), &srvDesc,
                                      srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kWidth;
    rtDesc.Height = kHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> renderTarget;
    if (FAILED(deviceX->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&renderTarget)))) {
        return 1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(deviceX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
        return 1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    deviceX->CreateRenderTargetView(renderTarget.Get(), nullptr, rtvHandle);

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(deviceX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc))) ||
        FAILED(deviceX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList))) ||
        FAILED(deviceX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return 1;
    }

    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;

    {
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = crestTexture.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = texUpload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = linearLayout.Plane[0].MipLayout[0].OffsetBytes;
        srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width = kTexSize;
        srcLoc.PlacedFootprint.Footprint.Height = kTexSize;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = linearLayout.Plane[0].MipLayout[0].PitchBytes;

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER texBarrier{};
        texBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        texBarrier.Transition.pResource = crestTexture.Get();
        texBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        texBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        texBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &texBarrier);
        cmdList->Close();

        ID3D12CommandList* initLists[] = { cmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, initLists);

        ++fenceVal;
        cmdQueue->Signal(fence.Get(), fenceVal);
        if (fence->GetCompletedValue() < fenceVal) {
            fence->SetEventOnCompletion(fenceVal, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        texUpload.Reset();
    }

    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };

    std::printf("[TITLE] Controls: WASD/Stick = Move | Space/A = Turbo Hum | F5/Menu = Save Chime | R/B = Reset | ESC = Auto-Save & Exit\n");

    float elapsed = 0.0f;
    bool prevSavePressed = false;
    uint64_t lastTs = gameInput->GetCurrentTimestamp();
    uint64_t frame = 0;

    while (true) {
        if (pixBegin) {
            pixBegin(0xFF00AA44ULL, "MainFrame");
        }

        uint64_t nowTs = gameInput->GetCurrentTimestamp();
        float dt = static_cast<float>(nowTs - lastTs) / 1000000.0f;
        dt = std::clamp(dt, 0.0001f, 0.05f);
        lastTs = nowTs;
        elapsed += dt;

        float moveX = 0.0f;
        float moveY = 0.0f;
        bool turbo = false;
        bool reset = false;
        bool savePressed = false;

        ComPtr<IGameInputReading> gpReading;
        if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &gpReading))) {
            GameInputGamepadState gp{};
            if (gpReading->GetGamepadState(&gp)) {
                moveX += gp.leftThumbstickX;
                moveY += gp.leftThumbstickY;
                if ((gp.buttons & GameInputGamepadDPadLeft) != 0)  moveX -= 1.0f;
                if ((gp.buttons & GameInputGamepadDPadRight) != 0) moveX += 1.0f;
                if ((gp.buttons & GameInputGamepadDPadUp) != 0)    moveY += 1.0f;
                if ((gp.buttons & GameInputGamepadDPadDown) != 0)  moveY -= 1.0f;
                if ((gp.buttons & GameInputGamepadA) != 0 || gp.rightTrigger > 0.25f) turbo = true;
                if ((gp.buttons & GameInputGamepadB) != 0) reset = true;
                if ((gp.buttons & GameInputGamepadMenu) != 0) savePressed = true;
            }
        }

        ComPtr<IGameInputReading> kbReading;
        if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, &kbReading))) {
            GameInputKeyState keys[16]{};
            uint32_t count = kbReading->GetKeyState(16, keys);
            for (uint32_t i = 0; i < count; ++i) {
                switch (keys[i].virtualKey) {
                    case 'W': case VK_UP:    moveY += 1.0f; break;
                    case 'S': case VK_DOWN:  moveY -= 1.0f; break;
                    case 'A': case VK_LEFT:  moveX -= 1.0f; break;
                    case 'D': case VK_RIGHT: moveX += 1.0f; break;
                    case VK_SPACE: case VK_SHIFT: turbo = true; break;
                    case 'R': reset = true; break;
                    case VK_F5: savePressed = true; break;
                    default: break;
                }
            }
        }

        if (savePressed && !prevSavePressed && saveContainer) {
            if (SaveStarState(queue, saveContainer, posX, posY, angle, saveCount)) {
                audio.PlaySaveChime();
            }
        }
        prevSavePressed = savePressed;

        const float clampedX = std::clamp(moveX, -1.0f, 1.0f);
        const float clampedY = std::clamp(moveY, -1.0f, 1.0f);
        const float moveMag = std::sqrtf(clampedX * clampedX + clampedY * clampedY);
        audio.UpdateThruster(moveMag, turbo, dt);

        if (reset) {
            posX = 0.0f;
            posY = 0.0f;
        } else {
            const float speed = turbo ? 1.85f : 1.15f;
            posX = std::clamp(posX + clampedX * speed * dt, -0.80f, 0.80f);
            posY = std::clamp(posY + clampedY * speed * dt, -0.75f, 0.75f);
        }

        angle += (turbo ? 4.8f : 1.1f) * dt;

        D3D12XBOX_FRAME_PIPELINE_TOKEN token = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
        deviceX->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, &token);

        cmdAlloc->Reset();
        cmdList->Reset(cmdAlloc.Get(), pso.Get());

        D3D12_RESOURCE_BARRIER toRT{};
        toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource = renderTarget.Get();
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmdList->ResourceBarrier(1, &toRT);

        const float bgColor[4] = { 0.03f, 0.06f, 0.08f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, bgColor, 0, nullptr);

        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->SetGraphicsRootSignature(rootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        DrawConstants dc{};
        dc.offsetX = posX;
        dc.offsetY = posY;
        dc.angle   = angle;
        dc.pulse   = 0.5f + 0.5f * std::sinf(elapsed * (turbo ? 12.0f : 3.0f));
        dc.aspect  = static_cast<float>(kWidth) / static_cast<float>(kHeight);
        dc.scale   = turbo ? 0.65f : 0.52f;
        dc.turbo   = turbo ? 1.0f : 0.0f;
        dc.timeSec = elapsed;

        cmdList->SetGraphicsRoot32BitConstants(0, sizeof(DrawConstants) / sizeof(uint32_t), &dc, 0);
        cmdList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &vbView);
        cmdList->DrawInstanced(static_cast<UINT>(_countof(kStarVertices)), 1, 0, 0);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = renderTarget.Get();
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &toPresent);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        ID3D12Resource* planeResources[] = { renderTarget.Get() };
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.ResourceCount = 1;
        plane.ppResources = planeResources;

        D3D12XBOX_PRESENT_PARAMETERS presentParams{};
        HRESULT presentHr = cmdQueue->PresentX(1, &plane, &presentParams);

        ++fenceVal;
        cmdQueue->Signal(fence.Get(), fenceVal);
        if (fence->GetCompletedValue() < fenceVal) {
            fence->SetEventOnCompletion(fenceVal, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        if (pixEnd) {
            pixEnd();
        }

        if (presentHr == S_FALSE || FAILED(presentHr)) {
            break;
        }
        ++frame;
    }

    if (saveContainer) {
        SaveStarState(queue, saveContainer, posX, posY, angle, saveCount);
        XGameSaveCloseContainer(saveContainer);
    }
    if (saveProvider) {
        XGameSaveCloseProvider(saveProvider);
    }

    audio.Shutdown();

    std::printf("[TITLE] Rendered %llu frames via PresentX. Shutting down...\n",
                static_cast<unsigned long long>(frame));

    CloseHandle(fenceEvent);
    fence.Reset();
    cmdList.Reset();
    cmdAlloc.Reset();
    rtvHeap.Reset();
    srvHeap.Reset();
    crestTexture.Reset();
    renderTarget.Reset();
    vertexBuffer.Reset();
    pso.Reset();
    rootSig.Reset();
    cmdQueue.Reset();
    deviceX.Reset();
    gameInput.Reset();
    tiledComputer.Reset();

    if (user) {
        XUserCloseHandle(user);
    }
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    if (hPix) {
        FreeLibrary(hPix);
    }

    std::printf("[TITLE] Shutdown complete.\n");
    return 0;
}
