#include <unknwn.h>
#include "test_harness.hpp"

using Microsoft::WRL::ComPtr;

bool TestRuntimeAndXUser() {
    SC_EXPECT(SUCCEEDED(XGameRuntimeInitialize()));
    SC_EXPECT(XSystemGetDeviceType() == XSystemDeviceType::XboxScarlettAnaconda);

    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                         XTaskQueueDispatchMode::Manual, &queue)));

    AsyncWaiter waiter{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &waiter;
    async.callback = OnDone;
    SC_EXPECT(SUCCEEDED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async)));
    Pump(queue, waiter);

    XUserHandle user = nullptr;
    SC_EXPECT(SUCCEEDED(XUserAddResult(&async, &user)));
    SC_EXPECT(user != nullptr);

    char tiny[4]{};
    size_t used = 0;
    HRESULT smallHr = XUserGetGamertag(user, XUserGamertagComponent::Classic, sizeof(tiny), tiny, &used);
    SC_EXPECT(smallHr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    SC_EXPECT(used > sizeof(tiny));

    char full[64]{};
    SC_EXPECT(SUCCEEDED(XUserGetGamertag(user, XUserGamertagComponent::Classic, sizeof(full), full, &used)));
    SC_EXPECT(std::strcmp(full, "ScarlettPlayer") == 0);

    XUserCloseHandle(user);
    XTaskQueueCloseHandle(queue);
    return true;
}

bool TestXGameSaveEdgeCases() {
    XTaskQueueHandle queue = nullptr;
    SC_EXPECT(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                         XTaskQueueDispatchMode::Manual, &queue)));

    XGameSaveProviderHandle provider = nullptr;
    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveInitializeProviderAsync(
            nullptr, "00000000-0000-0000-0000-0000TESTSUITE", false, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveInitializeProviderResult(&a, &provider)));
    }

    XGameSaveContainerHandle container = nullptr;
    SC_EXPECT(SUCCEEDED(XGameSaveCreateContainer(provider, "edge_cases", &container)));

    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "nonexistent_blob_xyz" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 1, &a)));
        Pump(queue, w);
        alignas(8) uint8_t buf[128]{};
        uint32_t count = 0;
        HRESULT hr = XGameSaveReadBlobDataResult(&a, sizeof(buf),
                                                 reinterpret_cast<XGameSaveBlob*>(buf), &count);
        SC_EXPECT(hr == E_GS_BLOB_NOT_FOUND);
    }

    const uint8_t payloadA[] = { 0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77 };
    const uint8_t payloadB[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x01 };
    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "EdgeTest", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "alpha", payloadA, sizeof(payloadA))));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "beta", payloadB, sizeof(payloadB))));

        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "alpha", "beta" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 2, &a)));
        Pump(queue, w);

        alignas(8) uint8_t tiny[32]{};
        uint32_t count = 0;
        HRESULT tinyHr = XGameSaveReadBlobDataResult(
            &a, sizeof(tiny), reinterpret_cast<XGameSaveBlob*>(tiny), &count);
        SC_EXPECT(tinyHr == E_GS_PROVIDED_BUFFER_TOO_SMALL);

        alignas(8) uint8_t full[512]{};
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataResult(
            &a, sizeof(full), reinterpret_cast<XGameSaveBlob*>(full), &count)));
        SC_EXPECT(count == 2);

        auto* blobs = reinterpret_cast<XGameSaveBlob*>(full);
        SC_EXPECT(std::strcmp(blobs[0].info.name, "alpha") == 0);
        SC_EXPECT(blobs[0].info.size == sizeof(payloadA));
        SC_EXPECT(std::memcmp(blobs[0].data, payloadA, sizeof(payloadA)) == 0);

        SC_EXPECT(std::strcmp(blobs[1].info.name, "beta") == 0);
        SC_EXPECT(blobs[1].info.size == sizeof(payloadB));
        SC_EXPECT(std::memcmp(blobs[1].data, payloadB, sizeof(payloadB)) == 0);
    }

    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "DeleteAlpha", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobDelete(upd, "alpha")));
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    {
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        const char* names[] = { "alpha" };
        SC_EXPECT(SUCCEEDED(XGameSaveReadBlobDataAsync(container, names, 1, &a)));
        Pump(queue, w);
        alignas(8) uint8_t buf[256]{};
        uint32_t count = 0;
        SC_EXPECT(XGameSaveReadBlobDataResult(
            &a, sizeof(buf), reinterpret_cast<XGameSaveBlob*>(buf), &count) == E_GS_BLOB_NOT_FOUND);
    }

    {
        XGameSaveUpdateHandle upd = nullptr;
        SC_EXPECT(SUCCEEDED(XGameSaveCreateUpdate(container, "CleanupBeta", &upd)));
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitBlobDelete(upd, "beta")));
        AsyncWaiter w{};
        XAsyncBlock a{ queue, &w, OnDone, {} };
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateAsync(upd, &a)));
        Pump(queue, w);
        SC_EXPECT(SUCCEEDED(XGameSaveSubmitUpdateResult(&a)));
        XGameSaveCloseUpdate(upd);
    }

    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(provider);
    XTaskQueueCloseHandle(queue);
    return true;
}

bool TestXAudio2Xma2Rejection() {
    ComPtr<IXAudio2> engine;
    SC_EXPECT(SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)));
    SC_EXPECT(engine != nullptr);

    WAVEFORMATEX xma2Fmt{};
    xma2Fmt.wFormatTag = 0x0166;
    xma2Fmt.nChannels = 2;
    xma2Fmt.nSamplesPerSec = 48000;
    xma2Fmt.wBitsPerSample = 16;

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = engine->CreateSourceVoice(&voice, &xma2Fmt);
    SC_EXPECT(hr == E_NOTIMPL);
    SC_EXPECT(voice == nullptr);
    return true;
}

bool TestKernelXMemoryAndTopology() {
    TITLEMEMORYSTATUS before{};
    before.dwLength = sizeof(before);
    SC_EXPECT(TitleMemoryStatus(&before) != FALSE);
    SC_EXPECT(before.ullTotalMem == SC_SERIES_X_TITLE_MEM_BYTES);

    void* gfxBuf = VirtualAllocX(
        nullptr, 256 * 1024, MEM_RESERVE | MEM_COMMIT | SC_MEM_GRAPHICS, PAGE_READWRITE);
    SC_EXPECT(gfxBuf != nullptr);
    SC_EXPECT((reinterpret_cast<uintptr_t>(gfxBuf) % 65536ULL) == 0);

    TITLEMEMORYSTATUS during{};
    during.dwLength = sizeof(during);
    SC_EXPECT(TitleMemoryStatus(&during) != FALSE);
    SC_EXPECT(during.ullTitleUsed >= before.ullTitleUsed + 256 * 1024);
    SC_EXPECT(during.ullTitleAvail == during.ullTotalMem - during.ullTitleUsed);

    SC_EXPECT(VirtualFreeX(gfxBuf, 0, MEM_RELEASE) != FALSE);

    TITLEMEMORYSTATUS after{};
    after.dwLength = sizeof(after);
    SC_EXPECT(TitleMemoryStatus(&after) != FALSE);
    SC_EXPECT(after.ullTitleUsed == before.ullTitleUsed);

    SCARLETT_PROCESSOR_INFO cpu{};
    SC_EXPECT(GetConsoleProcessorInfo(&cpu) != FALSE);
    SC_EXPECT(cpu.TitleCoreCount == 7u);
    SC_EXPECT(cpu.TitleLogicalThreadCount == 14u);
    SC_EXPECT(cpu.HostLogicalThreadCount >= 1u);
    SC_EXPECT(cpu.HostProcessAffinityMask != 0);

    DWORD_PTR prevMask = SetThreadAffinityMaskX(GetCurrentThread(), static_cast<DWORD_PTR>(~0ULL));
    SC_EXPECT(prevMask != 0);
    SC_EXPECT(SetThreadAffinityMaskX(GetCurrentThread(), prevMask) != 0);
    return true;
}

bool TestXAudio2ImaAdpcmDecodeBridge() {
    constexpr size_t kSamples = 4800; // 100 ms @ 48 kHz
    std::vector<int16_t> orig(kSamples);
    for (size_t i = 0; i < kSamples; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        orig[i] = static_cast<int16_t>(std::sin(6.28318530718 * 440.0 * t) * 24000.0);
    }

    auto adpcm = sc::codec::EncodeImaAdpcmMono(orig.data(), orig.size());
    SC_EXPECT(!adpcm.empty());
    SC_EXPECT(adpcm.size() < (orig.size() * sizeof(int16_t)) / 3u); // ~3.95x reduction

    std::vector<int16_t> decoded;
    SC_EXPECT(sc::codec::DecodeImaAdpcmMono(adpcm.data(), adpcm.size(),
                                            sc::codec::kImaAdpcmBlockAlign, decoded));
    SC_EXPECT(decoded.size() >= orig.size());

    double signalEnergy = 0.0;
    double noiseEnergy = 0.0;
    for (size_t i = 0; i < orig.size(); ++i) {
        const double s = static_cast<double>(orig[i]);
        const double e = static_cast<double>(orig[i]) - static_cast<double>(decoded[i]);
        signalEnergy += s * s;
        noiseEnergy += e * e;
    }
    const double snrDb = 10.0 * std::log10(signalEnergy / std::max(noiseEnergy, 1.0));
    SC_EXPECT(snrDb > 30.0);

    // Exercise live ScarlettDecodingSourceVoice through xaudio2_9.dll (silent, never started)
    ComPtr<IXAudio2> engine;
    SC_EXPECT(SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)));

    IXAudio2MasteringVoice* master = nullptr;
    if (SUCCEEDED(engine->CreateMasteringVoice(&master)) && master) {
        WAVEFORMATEX adpcmFmt{};
        adpcmFmt.wFormatTag      = sc::codec::kWaveFormatImaAdpcm;
        adpcmFmt.nChannels       = 1;
        adpcmFmt.nSamplesPerSec  = 48000;
        adpcmFmt.wBitsPerSample  = 4;
        adpcmFmt.nBlockAlign     = sc::codec::kImaAdpcmBlockAlign;
        adpcmFmt.nAvgBytesPerSec = (48000 * sc::codec::kImaAdpcmBlockAlign) / sc::codec::kImaAdpcmSamplesPerBlock;

        IXAudio2SourceVoice* voice = nullptr;
        SC_EXPECT(SUCCEEDED(engine->CreateSourceVoice(&voice, &adpcmFmt)));
        SC_EXPECT(voice != nullptr);

        int userSentinel = 0x53434152;
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(adpcm.size());
        buf.pAudioData = adpcm.data();
        buf.Flags = XAUDIO2_END_OF_STREAM;
        buf.pContext = &userSentinel;

        SC_EXPECT(SUCCEEDED(voice->SubmitSourceBuffer(&buf)));

        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st);
        SC_EXPECT(st.BuffersQueued == 1u);
        SC_EXPECT(st.pCurrentBufferContext == &userSentinel);

        SC_EXPECT(SUCCEEDED(voice->FlushSourceBuffers()));
        voice->DestroyVoice();
        master->DestroyVoice();
    }
    return true;
}

// 16. d3d12_xs: SCXS Compute PSO + cache + UAV RWStructuredBuffer GPU dispatch & readback

bool TestXAudio2RuntimePlaybackE2E() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE xaMod = LoadLibraryW(L"shims\\xaudio2_9.dll");
    if (!xaMod) xaMod = LoadLibraryW(L"xaudio2_9.dll");
    SC_EXPECT(xaMod != nullptr);

    using PFN_XAudio2Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    auto pCreate = reinterpret_cast<PFN_XAudio2Create>(GetProcAddress(xaMod, "XAudio2Create"));
    SC_EXPECT(pCreate != nullptr);

    IXAudio2* xa = nullptr;
    SC_EXPECT(SUCCEEDED(pCreate(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)));
    SC_EXPECT(xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    HRESULT hr = xa->CreateMasteringVoice(&master);
    if (SUCCEEDED(hr) && master != nullptr) {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = 96000;

        IXAudio2SourceVoice* src = nullptr;
        SC_EXPECT(SUCCEEDED(xa->CreateSourceVoice(&src, &wfx)));
        SC_EXPECT(src != nullptr);

        // 10 ms silent/low-amplitude 48 kHz PCM buffer (480 samples)
        std::vector<int16_t> pcm(480, 0);
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(pcm.size() * sizeof(int16_t));
        buf.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
        buf.Flags = XAUDIO2_END_OF_STREAM;

        SC_EXPECT(SUCCEEDED(src->SubmitSourceBuffer(&buf)));
        XAUDIO2_VOICE_STATE st{};
        src->GetState(&st);
        SC_EXPECT(st.BuffersQueued == 1);

        SC_EXPECT(SUCCEEDED(src->Start(0)));
        SC_EXPECT(SUCCEEDED(src->Stop(0)));
        SC_EXPECT(SUCCEEDED(src->FlushSourceBuffers()));

        src->DestroyVoice();
        master->DestroyVoice();
    }
    xa->Release();
    FreeLibrary(xaMod);

    // Verify AcpHal shim lifecycle exports
    HMODULE acpMod = LoadLibraryW(L"shims\\AcpHal.dll");
    if (!acpMod) acpMod = LoadLibraryW(L"AcpHal.dll");
    SC_EXPECT(acpMod != nullptr);
    using PFN_AcpHalCreate = HRESULT(WINAPI*)(void**);
    using PFN_AcpHalReleaseCtx = HRESULT(WINAPI*)(void);
    auto pAcpCreate = reinterpret_cast<PFN_AcpHalCreate>(GetProcAddress(acpMod, "AcpHalCreate"));
    auto pAcpRel = reinterpret_cast<PFN_AcpHalReleaseCtx>(GetProcAddress(acpMod, "AcpHalReleaseShapeContexts"));
    SC_EXPECT(pAcpCreate != nullptr && pAcpRel != nullptr);
    void* acpObj = nullptr;
    SC_EXPECT(SUCCEEDED(pAcpCreate(&acpObj)) && acpObj != nullptr);
    SC_EXPECT(SUCCEEDED(pAcpRel()));
    FreeLibrary(acpMod);
    return true;
}

bool TestNetworkingShimsE2E() {
    // 1. libHttpClient shim E2E
    HMODULE hcMod = LoadLibraryW(L"shims\\libHttpClient.dll");
    if (!hcMod) hcMod = LoadLibraryW(L"shims\\libHttpClient.GDK.dll");
    if (!hcMod) hcMod = LoadLibraryW(L"libHttpClient.dll");
    SC_EXPECT(hcMod != nullptr);

    using PFN_HCInitialize = HRESULT(WINAPI*)(void*);
    using PFN_HCCleanup = void(WINAPI*)(void);
    using PFN_HCIsInitialized = uint32_t(WINAPI*)(void);
    using PFN_HCHttpCallCreate = HRESULT(WINAPI*)(void**);
    using PFN_HCHttpCallCloseHandle = HRESULT(WINAPI*)(void*);
    using PFN_HCHttpCallRequestSetUrl = HRESULT(WINAPI*)(void*, const char*, const char*);
    using PFN_HCHttpCallRequestSetHeader = HRESULT(WINAPI*)(void*, const char*, const char*, uint32_t);
    using PFN_HCHttpCallPerformAsync = HRESULT(WINAPI*)(void*, void*);
    using PFN_HCHttpCallResponseGetStatusCode = HRESULT(WINAPI*)(void*, uint32_t*);
    using PFN_HCHttpCallResponseGetResponseString = HRESULT(WINAPI*)(void*, const char**);
    using PFN_HCHttpCallRegisterMock = HRESULT(WINAPI*)(const char*, const char*, uint32_t, const char*);
    using PFN_HCHttpCallClearMocks = void(WINAPI*)(void);

    auto pHCInit = reinterpret_cast<PFN_HCInitialize>(GetProcAddress(hcMod, "HCInitialize"));
    auto pHCCleanup = reinterpret_cast<PFN_HCCleanup>(GetProcAddress(hcMod, "HCCleanup"));
    auto pHCIsInit = reinterpret_cast<PFN_HCIsInitialized>(GetProcAddress(hcMod, "HCIsInitialized"));
    auto pHCCreate = reinterpret_cast<PFN_HCHttpCallCreate>(GetProcAddress(hcMod, "HCHttpCallCreate"));
    auto pHCClose = reinterpret_cast<PFN_HCHttpCallCloseHandle>(GetProcAddress(hcMod, "HCHttpCallCloseHandle"));
    auto pHCSetUrl = reinterpret_cast<PFN_HCHttpCallRequestSetUrl>(GetProcAddress(hcMod, "HCHttpCallRequestSetUrl"));
    auto pHCSetHdr = reinterpret_cast<PFN_HCHttpCallRequestSetHeader>(GetProcAddress(hcMod, "HCHttpCallRequestSetHeader"));
    auto pHCPerform = reinterpret_cast<PFN_HCHttpCallPerformAsync>(GetProcAddress(hcMod, "HCHttpCallPerformAsync"));
    auto pHCStatus = reinterpret_cast<PFN_HCHttpCallResponseGetStatusCode>(GetProcAddress(hcMod, "HCHttpCallResponseGetStatusCode"));
    auto pHCRespStr = reinterpret_cast<PFN_HCHttpCallResponseGetResponseString>(GetProcAddress(hcMod, "HCHttpCallResponseGetResponseString"));
    auto pHCMock = reinterpret_cast<PFN_HCHttpCallRegisterMock>(GetProcAddress(hcMod, "HCHttpCallRegisterMock"));
    auto pHCClearMocks = reinterpret_cast<PFN_HCHttpCallClearMocks>(GetProcAddress(hcMod, "HCHttpCallClearMocks"));
    SC_EXPECT(pHCInit && pHCCleanup && pHCIsInit && pHCCreate && pHCClose &&
              pHCSetUrl && pHCSetHdr && pHCPerform && pHCStatus && pHCRespStr && pHCMock && pHCClearMocks);

    SC_EXPECT(SUCCEEDED(pHCInit(nullptr)));
    SC_EXPECT(pHCIsInit() != 0);
    pHCClearMocks();
    SC_EXPECT(SUCCEEDED(pHCMock("GET", "https://scarlett.local/title", 200, "{\"ok\":true}")));

    void* hcCall = nullptr;
    SC_EXPECT(SUCCEEDED(pHCCreate(&hcCall)) && hcCall != nullptr);
    SC_EXPECT(SUCCEEDED(pHCSetUrl(hcCall, "GET", "https://scarlett.local/title/config")));
    SC_EXPECT(SUCCEEDED(pHCSetHdr(hcCall, "X-Scarlett-Test", "1", 0)));
    SC_EXPECT(SUCCEEDED(pHCPerform(hcCall, nullptr)));

    uint32_t hcStatus = 0;
    const char* hcBody = nullptr;
    SC_EXPECT(SUCCEEDED(pHCStatus(hcCall, &hcStatus)) && hcStatus == 200);
    SC_EXPECT(SUCCEEDED(pHCRespStr(hcCall, &hcBody)) && hcBody != nullptr);
    SC_EXPECT(std::string(hcBody) == "{\"ok\":true}");

    SC_EXPECT(SUCCEEDED(pHCClose(hcCall)));
    pHCClearMocks();
    pHCCleanup();
    FreeLibrary(hcMod);

    // 2. XCurl shim E2E
    HMODULE curlMod = LoadLibraryW(L"shims\\XCurl.dll");
    if (!curlMod) curlMod = LoadLibraryW(L"XCurl.dll");
    SC_EXPECT(curlMod != nullptr);

    using PFN_curl_global_init = int(*)(int64_t);
    using PFN_curl_global_cleanup = void(*)(void);
    using PFN_curl_easy_init = void*(*)(void);
    using PFN_curl_easy_cleanup = void(*)(void*);
    using PFN_curl_easy_setopt = int(*)(void*, int, ...);
    using PFN_curl_easy_perform = int(*)(void*);
    using PFN_curl_easy_getinfo = int(*)(void*, int, ...);
    using PFN_curl_slist_append = void*(*)(void*, const char*);
    using PFN_curl_slist_free_all = void(*)(void*);
    using PFN_xcurl_register_mock = int(*)(const char*, long, const char*);
    using PFN_xcurl_clear_mocks = void(*)(void);

    auto pGlobalInit = reinterpret_cast<PFN_curl_global_init>(GetProcAddress(curlMod, "curl_global_init"));
    auto pGlobalCleanup = reinterpret_cast<PFN_curl_global_cleanup>(GetProcAddress(curlMod, "curl_global_cleanup"));
    auto pEasyInit = reinterpret_cast<PFN_curl_easy_init>(GetProcAddress(curlMod, "curl_easy_init"));
    auto pEasyCleanup = reinterpret_cast<PFN_curl_easy_cleanup>(GetProcAddress(curlMod, "curl_easy_cleanup"));
    auto pEasySetopt = reinterpret_cast<PFN_curl_easy_setopt>(GetProcAddress(curlMod, "curl_easy_setopt"));
    auto pEasyPerform = reinterpret_cast<PFN_curl_easy_perform>(GetProcAddress(curlMod, "curl_easy_perform"));
    auto pEasyGetinfo = reinterpret_cast<PFN_curl_easy_getinfo>(GetProcAddress(curlMod, "curl_easy_getinfo"));
    auto pSlistAppend = reinterpret_cast<PFN_curl_slist_append>(GetProcAddress(curlMod, "curl_slist_append"));
    auto pSlistFree = reinterpret_cast<PFN_curl_slist_free_all>(GetProcAddress(curlMod, "curl_slist_free_all"));
    auto pRegMock = reinterpret_cast<PFN_xcurl_register_mock>(GetProcAddress(curlMod, "xcurl_register_mock_response"));
    auto pClearMocks = reinterpret_cast<PFN_xcurl_clear_mocks>(GetProcAddress(curlMod, "xcurl_clear_mocks"));
    SC_EXPECT(pGlobalInit && pGlobalCleanup && pEasyInit && pEasyCleanup &&
              pEasySetopt && pEasyPerform && pEasyGetinfo && pSlistAppend && pSlistFree &&
              pRegMock && pClearMocks);

    SC_EXPECT(pGlobalInit(0) == 0);
    pClearMocks();
    SC_EXPECT(pRegMock("https://scarlett.local/curl", 200L, "xcurl-ok") == 0);

    void* curl = pEasyInit();
    SC_EXPECT(curl != nullptr);
    void* hdrs = pSlistAppend(nullptr, "Accept: application/json");
    std::string recvBody;
    SC_EXPECT(pEasySetopt(curl, 10002, "https://scarlett.local/curl/ping") == 0); // CURLOPT_URL
    SC_EXPECT(pEasySetopt(curl, 10023, hdrs) == 0);                               // CURLOPT_HTTPHEADER
    SC_EXPECT(pEasySetopt(curl, 20011, &CurlStringWriteCb) == 0);                   // CURLOPT_WRITEFUNCTION
    SC_EXPECT(pEasySetopt(curl, 10001, &recvBody) == 0);                          // CURLOPT_WRITEDATA
    SC_EXPECT(pEasyPerform(curl) == 0);

    long httpCode = 0;
    SC_EXPECT(pEasyGetinfo(curl, 0x200002, &httpCode) == 0);                      // CURLINFO_RESPONSE_CODE
    SC_EXPECT(httpCode == 200L);
    SC_EXPECT(recvBody == "xcurl-ok");

    pSlistFree(hdrs);
    pEasyCleanup(curl);
    pClearMocks();
    pGlobalCleanup();
    FreeLibrary(curlMod);
    return true;
}

bool TestXUserAndGameSaveProfileE2E() {
    HMODULE xgrMod = LoadLibraryW(L"shims\\xgameruntime.dll");
    if (!xgrMod) xgrMod = LoadLibraryW(L"xgameruntime.dll");
    SC_EXPECT(xgrMod != nullptr);

    struct LocalAsyncBlock {
        void* queue;
        void* context;
        void (CALLBACK* callback)(LocalAsyncBlock*);
        unsigned char internal[sizeof(void*) * 4];
    };
    struct LocalBlobInfo { const char* name; uint32_t size; };
    struct LocalBlob { LocalBlobInfo info; uint8_t* data; };

    using PFN_Init = HRESULT(WINAPI*)(void);
    using PFN_Uninit = void(WINAPI*)(void);
    using PFN_QueueCreate = HRESULT(WINAPI*)(uint32_t, uint32_t, void**);
    using PFN_QueueDispatch = bool(WINAPI*)(void*, uint32_t, uint32_t);
    using PFN_QueueClose = void(WINAPI*)(void*);
    using PFN_UserAddAsync = HRESULT(WINAPI*)(uint32_t, LocalAsyncBlock*);
    using PFN_UserAddResult = HRESULT(WINAPI*)(LocalAsyncBlock*, void**);
    using PFN_UserGetId = HRESULT(WINAPI*)(void*, uint64_t*);
    using PFN_UserGetState = HRESULT(WINAPI*)(void*, uint32_t*);
    using PFN_UserGetLocalId = HRESULT(WINAPI*)(void*, uint64_t*);
    using PFN_UserGetGamertag = HRESULT(WINAPI*)(void*, uint32_t, size_t, char*, size_t*);
    using PFN_UserClose = void(WINAPI*)(void*);
    using PFN_GsInitAsync = HRESULT(WINAPI*)(void*, const char*, bool, LocalAsyncBlock*);
    using PFN_GsInitResult = HRESULT(WINAPI*)(LocalAsyncBlock*, void**);
    using PFN_GsCloseProv = void(WINAPI*)(void*);
    using PFN_GsCreateCont = HRESULT(WINAPI*)(void*, const char*, void**);
    using PFN_GsCloseCont = void(WINAPI*)(void*);
    using PFN_GsCreateUpd = HRESULT(WINAPI*)(void*, const char*, void**);
    using PFN_GsCloseUpd = void(WINAPI*)(void*);
    using PFN_GsWriteBlob = HRESULT(WINAPI*)(void*, const char*, const uint8_t*, size_t);
    using PFN_GsSubmitAsync = HRESULT(WINAPI*)(void*, LocalAsyncBlock*);
    using PFN_GsSubmitResult = HRESULT(WINAPI*)(LocalAsyncBlock*);
    using PFN_GsReadAsync = HRESULT(WINAPI*)(void*, const char**, uint32_t, LocalAsyncBlock*);
    using PFN_GsReadResult = HRESULT(WINAPI*)(LocalAsyncBlock*, size_t, LocalBlob*, uint32_t*);

    auto pInit = reinterpret_cast<PFN_Init>(GetProcAddress(xgrMod, "XGameRuntimeInitialize"));
    auto pUninit = reinterpret_cast<PFN_Uninit>(GetProcAddress(xgrMod, "XGameRuntimeUninitialize"));
    auto pQCreate = reinterpret_cast<PFN_QueueCreate>(GetProcAddress(xgrMod, "XTaskQueueCreate"));
    auto pQDispatch = reinterpret_cast<PFN_QueueDispatch>(GetProcAddress(xgrMod, "XTaskQueueDispatch"));
    auto pQClose = reinterpret_cast<PFN_QueueClose>(GetProcAddress(xgrMod, "XTaskQueueCloseHandle"));
    auto pUserAdd = reinterpret_cast<PFN_UserAddAsync>(GetProcAddress(xgrMod, "XUserAddAsync"));
    auto pUserAddRes = reinterpret_cast<PFN_UserAddResult>(GetProcAddress(xgrMod, "XUserAddResult"));
    auto pUserGetId = reinterpret_cast<PFN_UserGetId>(GetProcAddress(xgrMod, "XUserGetId"));
    auto pUserGetState = reinterpret_cast<PFN_UserGetState>(GetProcAddress(xgrMod, "XUserGetState"));
    auto pUserGetLocalId = reinterpret_cast<PFN_UserGetLocalId>(GetProcAddress(xgrMod, "XUserGetLocalId"));
    auto pUserGetTag = reinterpret_cast<PFN_UserGetGamertag>(GetProcAddress(xgrMod, "XUserGetGamertag"));
    auto pUserClose = reinterpret_cast<PFN_UserClose>(GetProcAddress(xgrMod, "XUserCloseHandle"));
    auto pGsInit = reinterpret_cast<PFN_GsInitAsync>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderAsync"));
    auto pGsInitRes = reinterpret_cast<PFN_GsInitResult>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderResult"));
    auto pGsCloseProv = reinterpret_cast<PFN_GsCloseProv>(GetProcAddress(xgrMod, "XGameSaveCloseProvider"));
    auto pGsCreateCont = reinterpret_cast<PFN_GsCreateCont>(GetProcAddress(xgrMod, "XGameSaveCreateContainer"));
    auto pGsCloseCont = reinterpret_cast<PFN_GsCloseCont>(GetProcAddress(xgrMod, "XGameSaveCloseContainer"));
    auto pGsCreateUpd = reinterpret_cast<PFN_GsCreateUpd>(GetProcAddress(xgrMod, "XGameSaveCreateUpdate"));
    auto pGsCloseUpd = reinterpret_cast<PFN_GsCloseUpd>(GetProcAddress(xgrMod, "XGameSaveCloseUpdate"));
    auto pGsWriteBlob = reinterpret_cast<PFN_GsWriteBlob>(GetProcAddress(xgrMod, "XGameSaveSubmitBlobWrite"));
    auto pGsSubmit = reinterpret_cast<PFN_GsSubmitAsync>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateAsync"));
    auto pGsSubmitRes = reinterpret_cast<PFN_GsSubmitResult>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateResult"));
    auto pGsRead = reinterpret_cast<PFN_GsReadAsync>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataAsync"));
    auto pGsReadRes = reinterpret_cast<PFN_GsReadResult>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataResult"));

    SC_EXPECT(pInit && pUninit && pQCreate && pQDispatch && pQClose &&
              pUserAdd && pUserAddRes && pUserGetId && pUserGetState && pUserGetLocalId &&
              pUserGetTag && pUserClose && pGsInit && pGsInitRes && pGsCloseProv &&
              pGsCreateCont && pGsCloseCont && pGsCreateUpd && pGsCloseUpd &&
              pGsWriteBlob && pGsSubmit && pGsSubmitRes && pGsRead && pGsReadRes);

    SC_EXPECT(SUCCEEDED(pInit()));
    void* queue = nullptr;
    SC_EXPECT(SUCCEEDED(pQCreate(0, 0, &queue)) && queue != nullptr); // Manual / Manual

    LocalAsyncBlock asyncUser{};
    asyncUser.queue = queue;
    SC_EXPECT(SUCCEEDED(pUserAdd(1, &asyncUser))); // AddDefaultUserSilently
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}

    void* user = nullptr;
    SC_EXPECT(SUCCEEDED(pUserAddRes(&asyncUser, &user)) && user != nullptr);

    uint64_t xuid = 0, localId = 0;
    uint32_t state = 99;
    char gamertag[64]{};
    size_t tagUsed = 0;
    SC_EXPECT(SUCCEEDED(pUserGetId(user, &xuid)) && xuid != 0);
    SC_EXPECT(SUCCEEDED(pUserGetState(user, &state)) && state == 0);
    SC_EXPECT(SUCCEEDED(pUserGetLocalId(user, &localId)) && localId == xuid);
    SC_EXPECT(SUCCEEDED(pUserGetTag(user, 0, sizeof(gamertag), gamertag, &tagUsed)) && tagUsed > 1);

    LocalAsyncBlock asyncProv{};
    asyncProv.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsInit(user, "00000000-0000-0000-0000-000000000043", false, &asyncProv)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}

    void* provider = nullptr;
    SC_EXPECT(SUCCEEDED(pGsInitRes(&asyncProv, &provider)) && provider != nullptr);

    void* container = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCreateCont(provider, "sprint43_profile", &container)) && container != nullptr);

    void* upd = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCreateUpd(container, "Sprint 43 Save", &upd)) && upd != nullptr);

    const std::string payload = "scarlett-sprint43-save-ok";
    SC_EXPECT(SUCCEEDED(pGsWriteBlob(
        upd, "slot0", reinterpret_cast<const uint8_t*>(payload.data()), payload.size())));

    LocalAsyncBlock asyncUpd{};
    asyncUpd.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsSubmit(upd, &asyncUpd)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}
    SC_EXPECT(SUCCEEDED(pGsSubmitRes(&asyncUpd)));
    pGsCloseUpd(upd);

    const char* blobNames[] = { "slot0" };
    LocalAsyncBlock asyncRead{};
    asyncRead.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsRead(container, blobNames, 1, &asyncRead)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}

    std::vector<uint8_t> readBuf(1024, 0);
    uint32_t blobCount = 0;
    SC_EXPECT(SUCCEEDED(pGsReadRes(
        &asyncRead, readBuf.size(), reinterpret_cast<LocalBlob*>(readBuf.data()), &blobCount)));
    SC_EXPECT(blobCount == 1);
    const auto* blob = reinterpret_cast<const LocalBlob*>(readBuf.data());
    SC_EXPECT(blob->info.size == payload.size());
    SC_EXPECT(std::memcmp(blob->data, payload.data(), payload.size()) == 0);

    pGsCloseCont(container);
    pGsCloseProv(provider);
    pUserClose(user);
    pQClose(queue);
    pUninit();
    FreeLibrary(xgrMod);
    return true;
}

bool TestGameInputRuntimePollingE2E() {
    HMODULE giMod = LoadLibraryW(L"shims\\gameinput.dll");
    if (!giMod) giMod = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(giMod != nullptr);

    using PFN_GameInputCreate = HRESULT(WINAPI*)(IGameInput**);
    using PFN_SetSynthetic = void(WINAPI*)(bool, const GameInputGamepadState*);
    auto pCreate = reinterpret_cast<PFN_GameInputCreate>(GetProcAddress(giMod, "GameInputCreate"));
    auto pSetSynth = reinterpret_cast<PFN_SetSynthetic>(GetProcAddress(giMod, "ScarlettGameInputSetSyntheticGamepad"));
    SC_EXPECT(pCreate != nullptr && pSetSynth != nullptr);

    IGameInput* gi = nullptr;
    SC_EXPECT(SUCCEEDED(pCreate(&gi)) && gi != nullptr);
    const uint64_t t0 = gi->GetCurrentTimestamp();
    SC_EXPECT(t0 > 0);

    uint32_t seenKinds = 0;
    auto enumCb = +[](uint64_t, void* ctx, IGameInputDevice* dev, uint64_t, GameInputDeviceStatus cur, GameInputDeviceStatus) {
        if (ctx && dev && (cur & GameInputDeviceConnected) != 0)
            *static_cast<uint32_t*>(ctx) |= static_cast<uint32_t>(dev->GetDeviceInfo()->supportedInput);
    };
    uint64_t tok = 0;
    SC_EXPECT(SUCCEEDED(gi->RegisterDeviceCallback(nullptr, static_cast<GameInputKind>(0x0FFFFFFFu),
        GameInputDeviceAnyStatus, GameInputBlockingEnumeration, &seenKinds, reinterpret_cast<void*>(enumCb), &tok)));
    SC_EXPECT((seenKinds & GameInputKindGamepad) && (seenKinds & GameInputKindKeyboard) && (seenKinds & GameInputKindMouse));

    GameInputGamepadState injected{ GameInputGamepadA | GameInputGamepadRightShoulder, 0.25f, 0.75f, -0.5f, 0.5f, 0.25f, -0.25f };
    pSetSynth(true, &injected);

    IGameInputReading* reading = nullptr;
    SC_EXPECT(SUCCEEDED(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &reading)) && reading != nullptr);
    SC_EXPECT(reading->GetInputKind() == GameInputKindGamepad && reading->GetTimestamp() >= t0);
    GameInputGamepadState polled{};
    SC_EXPECT(reading->GetGamepadState(&polled) && std::memcmp(&polled, &injected, sizeof(polled)) == 0);

    IGameInputReading* mr = nullptr;
    SC_EXPECT(SUCCEEDED(gi->GetCurrentReading(GameInputKindMouse, nullptr, &mr)) && mr != nullptr);
    GameInputMouseState ms{};
    SC_EXPECT(mr->GetMouseState(&ms));
    mr->Release();

    pSetSynth(false, nullptr);
    reading->Release();
    gi->Release();
    FreeLibrary(giMod);
    return true;
}

bool TestGameInputLivePolling() {
    IGameInput* gi = nullptr;
    SC_EXPECT(SUCCEEDED(GameInputCreate(&gi)) && gi != nullptr);

    const uint64_t t0 = gi->GetCurrentTimestamp();
    SC_EXPECT(t0 > 0);

    GameInputGamepadState injected{};
    injected.buttons = 0x00000010u | 0x00000100u; // A + DPadUp
    injected.leftTrigger = 0.25f;
    injected.rightTrigger = 0.75f;
    injected.leftThumbstickX = -0.5f;
    injected.leftThumbstickY = 1.0f;
    injected.rightThumbstickX = 0.5f;
    injected.rightThumbstickY = -0.25f;

    ScarlettGameInputSetSyntheticGamepad(true, &injected);

    IGameInputReading* reading = nullptr;
    const HRESULT hr = gi->GetCurrentReading(GameInputKindGamepad, nullptr, &reading);
    SC_EXPECT(SUCCEEDED(hr) && reading != nullptr);
    SC_EXPECT(reading->GetTimestamp() >= t0);

    GameInputGamepadState out{};
    SC_EXPECT(reading->GetGamepadState(&out));
    SC_EXPECT(out.buttons == injected.buttons);
    SC_EXPECT(std::fabs(out.leftTrigger - 0.25f) < 1e-5f);
    SC_EXPECT(std::fabs(out.rightTrigger - 0.75f) < 1e-5f);
    SC_EXPECT(std::fabs(out.leftThumbstickX - (-0.5f)) < 1e-5f);
    SC_EXPECT(std::fabs(out.leftThumbstickY - 1.0f) < 1e-5f);
    SC_EXPECT(std::fabs(out.rightThumbstickX - 0.5f) < 1e-5f);
    SC_EXPECT(std::fabs(out.rightThumbstickY - (-0.25f)) < 1e-5f);

    reading->Release();
    ScarlettGameInputSetSyntheticGamepad(false, nullptr);
    gi->Release();
    return true;
}

bool TestKernelXAndPixEvtE2E() {
    // 1. kernelx OS shim E2E
    HMODULE kxMod = LoadLibraryW(L"shims\\kernelx.dll");
    if (!kxMod) kxMod = LoadLibraryW(L"kernelx.dll");
    SC_EXPECT(kxMod != nullptr);

    struct LocalTitleMemoryStatus {
        DWORD    dwLength;
        DWORD    dwReserved;
        uint64_t ullTotalMem;
        uint64_t ullAvailMem;
        uint64_t ullLegacyUsed;
        uint64_t ullLegacyPeak;
        uint64_t ullLegacyAvail;
        uint64_t ullTitleUsed;
        uint64_t ullTitlePeak;
        uint64_t ullTitleAvail;
    };
    struct LocalProcessorInfo {
        uint32_t  TitleCoreCount;
        uint32_t  TitleLogicalThreadCount;
        uint32_t  HostLogicalThreadCount;
        DWORD_PTR HostProcessAffinityMask;
    };

    auto pTitleMem  = reinterpret_cast<BOOL(WINAPI*)(LocalTitleMemoryStatus*)>(GetProcAddress(kxMod, "TitleMemoryStatus"));
    auto pVAllocX   = reinterpret_cast<void*(WINAPI*)(void*, size_t, DWORD, DWORD)>(GetProcAddress(kxMod, "VirtualAllocX"));
    auto pVFreeX    = reinterpret_cast<BOOL(WINAPI*)(void*, size_t, DWORD)>(GetProcAddress(kxMod, "VirtualFreeX"));
    auto pProcInfo  = reinterpret_cast<BOOL(WINAPI*)(LocalProcessorInfo*)>(GetProcAddress(kxMod, "GetConsoleProcessorInfo"));
    auto pSetAffX   = reinterpret_cast<DWORD_PTR(WINAPI*)(HANDLE, DWORD_PTR)>(GetProcAddress(kxMod, "SetThreadAffinityMaskX"));
    SC_EXPECT(pTitleMem && pVAllocX && pVFreeX && pProcInfo && pSetAffX);

    LocalTitleMemoryStatus memBefore{};
    memBefore.dwLength = sizeof(memBefore);
    SC_EXPECT(pTitleMem(&memBefore) != FALSE);
    SC_EXPECT(memBefore.ullTotalMem == 14495514624ULL); // 13.5 GiB Series X budget
    SC_EXPECT(memBefore.ullAvailMem <= memBefore.ullTotalMem);

    // Allocate 64 KiB with SC_MEM_TITLE_BUDGET (0x08000000)
    constexpr size_t kAllocSize = 64 * 1024;
    void* mem = pVAllocX(nullptr, kAllocSize, MEM_COMMIT | MEM_RESERVE | 0x08000000u, PAGE_READWRITE);
    SC_EXPECT(mem != nullptr);
    std::memset(mem, 0xA5, kAllocSize);
    SC_EXPECT(static_cast<uint8_t*>(mem)[0] == 0xA5 && static_cast<uint8_t*>(mem)[kAllocSize - 1] == 0xA5);

    LocalTitleMemoryStatus memDuring{};
    memDuring.dwLength = sizeof(memDuring);
    SC_EXPECT(pTitleMem(&memDuring) != FALSE);
    SC_EXPECT(memDuring.ullTitleUsed >= memBefore.ullTitleUsed);

    SC_EXPECT(pVFreeX(mem, 0, MEM_RELEASE) != FALSE);

    LocalProcessorInfo procInfo{};
    SC_EXPECT(pProcInfo(&procInfo) != FALSE);
    SC_EXPECT(procInfo.TitleCoreCount == 7);
    SC_EXPECT(procInfo.TitleLogicalThreadCount == 14);
    SC_EXPECT(procInfo.HostLogicalThreadCount >= 1);
    SC_EXPECT(procInfo.HostProcessAffinityMask != 0);

    const DWORD_PTR prevMask = pSetAffX(GetCurrentThread(), procInfo.HostProcessAffinityMask);
    SC_EXPECT(prevMask != 0);
    SC_EXPECT(pSetAffX(GetCurrentThread(), prevMask) != 0);

    FreeLibrary(kxMod);

    // 2. pixevt profiler shim E2E
    HMODULE pixMod = LoadLibraryW(L"shims\\pixevt.dll");
    if (!pixMod) pixMod = LoadLibraryW(L"pixevt.dll");
    SC_EXPECT(pixMod != nullptr);

    auto pBegin  = reinterpret_cast<void(WINAPI*)(uint64_t, const char*)>(GetProcAddress(pixMod, "PIXBeginEvent"));
    auto pEnd    = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(pixMod, "PIXEndEvent"));
    auto pMarker = reinterpret_cast<void(WINAPI*)(uint64_t, const char*)>(GetProcAddress(pixMod, "PIXSetMarker"));
    SC_EXPECT(pBegin && pEnd && pMarker);

    pBegin(0xFF00FF00ULL, "Sprint46::Frame");
    pBegin(0xFF0088FFULL, "Sprint46::ShadowPass");
    pMarker(0xFFFFAA00ULL, "Sprint46::DispatchSync");
    pEnd();
    pEnd();

    FreeLibrary(pixMod);
    return true;
}

bool TestRetailBringUpShims() {
    // 1. GameInput.dll ordinal #1 export check
    HMODULE hGi = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(hGi != nullptr);
    FARPROC ord1 = GetProcAddress(hGi, MAKEINTRESOURCEA(1));
    FARPROC byName = GetProcAddress(hGi, "GameInputCreate");
    SC_EXPECT(ord1 != nullptr && ord1 == byName);
    FreeLibrary(hGi);

    // 2. AcpHal.dll vtable lifecycle + ApuAlloc alignment
    void* rawAcp = nullptr;
    SC_EXPECT(SUCCEEDED(AcpHalCreate(&rawAcp)) && rawAcp != nullptr);
    auto* acp = static_cast<IAcpHal*>(rawAcp);
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->Connect(acp, 64, 64)));
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->SubmitCommand(acp, 1, 1, 0, nullptr, 0)));
    uint8_t msg[16]{};
    SC_EXPECT(acp->lpVtbl->PopMessage(acp, msg) == 0);
    SC_EXPECT(SUCCEEDED(acp->lpVtbl->Disconnect(acp)));
    SC_EXPECT(acp->lpVtbl->PopMessage(acp, msg) == 1);
    SC_EXPECT(msg[0] == 0x80u);

    ACP_SHAPE_CONTEXT_COUNTS counts{ 4, 2, 2, 2, 4, 2 };
    SC_EXPECT(SUCCEEDED(AcpHalAllocateShapeContexts(&counts)));
    SC_EXPECT(SUCCEEDED(AcpHalReleaseShapeContexts()));

    void* apuPtr = nullptr;
    uint64_t apuPhys = 0;
    SC_EXPECT(SUCCEEDED(ApuAlloc(&apuPtr, &apuPhys, 4096, 256)));
    SC_EXPECT(apuPtr != nullptr && (reinterpret_cast<uintptr_t>(apuPtr) % 256u) == 0u);
    SC_EXPECT(apuPhys == reinterpret_cast<uint64_t>(apuPtr));
    SC_EXPECT(SUCCEEDED(ApuFree(apuPtr)));

    // 3. libHttpClient.GDK.dll offline mock round-trip
    SC_EXPECT(SUCCEEDED(HCInitialize(nullptr)));
    SC_EXPECT(HCIsInitialized() == 1u);
    SC_EXPECT(SUCCEEDED(HCHttpCallRegisterMock("GET", "https://xbox.local/status", 200u, "{\"ok\":true}")));

    HCCallHandle hc = nullptr;
    SC_EXPECT(SUCCEEDED(HCHttpCallCreate(&hc)) && hc != nullptr);
    SC_EXPECT(SUCCEEDED(HCHttpCallRequestSetUrl(hc, "GET", "https://xbox.local/status")));
    SC_EXPECT(SUCCEEDED(HCHttpCallPerformAsync(hc, nullptr)));

    uint32_t hcStatus = 0;
    const char* hcBody = nullptr;
    SC_EXPECT(SUCCEEDED(HCHttpCallResponseGetStatusCode(hc, &hcStatus)) && hcStatus == 200u);
    SC_EXPECT(SUCCEEDED(HCHttpCallResponseGetResponseString(hc, &hcBody)) && std::strcmp(hcBody, "{\"ok\":true}") == 0);
    SC_EXPECT(SUCCEEDED(HCHttpCallCloseHandle(hc)));
    HCCleanup();

    // 4. XCurl.dll offline mock round-trip
    SC_EXPECT(curl_global_init(0) == CURLE_OK);
    SC_EXPECT(xcurl_register_mock_response("https://cdn.local/patch", 200, "scarlett-ok") == CURLE_OK);
    CURL* curl = curl_easy_init();
    SC_EXPECT(curl != nullptr);
    std::string captured;
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_URL, "https://cdn.local/patch") == CURLE_OK);
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlStringWriteCb) == CURLE_OK);
    SC_EXPECT(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &captured) == CURLE_OK);
    SC_EXPECT(curl_easy_perform(curl) == CURLE_OK);
    long httpCode = 0;
    SC_EXPECT(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode) == CURLE_OK && httpCode == 200);
    SC_EXPECT(captured == "scarlett-ok");
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    return true;
}

bool TestXGameRuntimeComBrokerE2E() {
    HMODULE mod = GetModuleHandleW(L"xgameruntime.dll");
    if (!mod) {
        mod = LoadLibraryW(L"xgameruntime.dll");
    }
    SC_EXPECT(mod != nullptr);

    using PFN_InitEx2  = HRESULT(WINAPI*)(uint64_t, uint64_t, uint64_t, void*);
    using PFN_Query    = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
    using PFN_Uninit   = void(WINAPI*)();
    using PFN_CanUnload = HRESULT(WINAPI*)();

    auto pInitEx2   = reinterpret_cast<PFN_InitEx2>(GetProcAddress(mod, "InitializeApiImplEx2"));
    auto pQuery     = reinterpret_cast<PFN_Query>(GetProcAddress(mod, "QueryApiImpl"));
    auto pUninit    = reinterpret_cast<PFN_Uninit>(GetProcAddress(mod, "UninitializeApiImpl"));
    auto pCanUnload = reinterpret_cast<PFN_CanUnload>(GetProcAddress(mod, "DllCanUnloadNow"));
    SC_EXPECT(pInitEx2 && pQuery && pUninit && pCanUnload);

    SC_EXPECT(SUCCEEDED(pInitEx2(0x6336, 0x2712, 1, nullptr)));
    SC_EXPECT(pCanUnload() == S_FALSE);

    // 1. XSystemAnalyticsInfo hidden-sret (0x90 bytes)
    const GUID kAnalytics = { 0xb884675d, 0xb738, 0x4a9c, { 0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b } };
    struct IAnalyticsVtbl : public IUnknown {
        virtual XSystemAnalyticsInfo STDMETHODCALLTYPE GetAnalyticsInfo() = 0;
    };
    IAnalyticsVtbl* analytics = nullptr;
    SC_EXPECT(SUCCEEDED(pQuery(&kAnalytics, &kAnalytics, reinterpret_cast<void**>(&analytics))) && analytics);
    XSystemAnalyticsInfo info = analytics->GetAnalyticsInfo();
    SC_EXPECT(info.osVersion.major == 10 && info.osVersion.build >= 20000);
    SC_EXPECT(std::strcmp(info.family, "Windows.Xbox") == 0);
    analytics->Release();

    // 2. XSystem (ConsoleId + SandboxId)
    const GUID kSysClsid = { 0xe349bd1a, 0xfc20, 0x4e40, { 0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f } };
    const GUID kSysIid   = { 0x1861cf2e, 0xe18b, 0x4834, { 0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf } };
    struct ISystemVtbl : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetConsoleId(size_t, char*, size_t*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetXboxLiveSandboxId(size_t, char*, size_t*) = 0;
    };
    ISystemVtbl* sys = nullptr;
    SC_EXPECT(SUCCEEDED(pQuery(&kSysClsid, &kSysIid, reinterpret_cast<void**>(&sys))) && sys);
    char cid[64]{}; size_t cidUsed = 0;
    char sbx[32]{}; size_t sbxUsed = 0;
    SC_EXPECT(SUCCEEDED(sys->GetConsoleId(sizeof(cid), cid, &cidUsed)) && cidUsed > 0);
    SC_EXPECT(SUCCEEDED(sys->GetXboxLiveSandboxId(sizeof(sbx), sbx, &sbxUsed)) && std::strcmp(sbx, "RETAIL") == 0);
    sys->Release();

    // 3. XGame (TitleId)
    const GUID kGameClsid = { 0x973a344e, 0x24bf, 0x4d0f, { 0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29 } };
    const GUID kGameIid   = { 0x2549f142, 0x6419, 0x4a06, { 0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34 } };
    struct IGameVtbl : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetXboxTitleId(uint32_t*) = 0;
    };
    IGameVtbl* game = nullptr;
    SC_EXPECT(SUCCEEDED(pQuery(&kGameClsid, &kGameIid, reinterpret_cast<void**>(&game))) && game);
    uint32_t titleId = 0;
    SC_EXPECT(SUCCEEDED(game->GetXboxTitleId(&titleId)) && titleId != 0);
    game->Release();

    // 4. XAsync::Run (slot 6) invokes work + completion callback
    const GUID kAsyncClsid = { 0x073b7dcb, 0x1fcf, 0x4030, { 0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28 } };
    struct IAsyncVtbl : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetStatus(XAsyncBlock*, bool) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetResultSize(XAsyncBlock*, size_t*) = 0;
        virtual void    STDMETHODCALLTYPE Cancel(XAsyncBlock*) = 0;
        virtual HRESULT STDMETHODCALLTYPE Run(XAsyncBlock*, XAsyncWork) = 0;
    };
    IAsyncVtbl* asyncSvc = nullptr;
    SC_EXPECT(SUCCEEDED(pQuery(&kAsyncClsid, &kAsyncClsid, reinterpret_cast<void**>(&asyncSvc))) && asyncSvc);
    struct RunState { bool workRan{false}; bool cbRan{false}; } st{};
    XAsyncBlock ab{};
    ab.context = &st;
    ab.callback = [](XAsyncBlock* b) { static_cast<RunState*>(b->context)->cbRan = true; };
    SC_EXPECT(SUCCEEDED(asyncSvc->Run(&ab, [](XAsyncBlock* b) -> HRESULT {
        static_cast<RunState*>(b->context)->workRan = true;
        return S_OK;
    })));
    SC_EXPECT(st.workRan && st.cbRan);
    asyncSvc->Release();

    // 5. Unknown GUID cleanly returns REGDB_E_CLASSNOTREG (0x80040154)
    const GUID kUnknown = { 0x11111111, 0x2222, 0x3333, { 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb } };
    void* dummy = nullptr;
    SC_EXPECT(pQuery(&kUnknown, &kUnknown, &dummy) == static_cast<HRESULT>(0x80040154L));

    pUninit();
    return true;
}
