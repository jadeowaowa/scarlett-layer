#include "test_harness.hpp"

using Microsoft::WRL::ComPtr;

bool TestDynamicAndMultiModuleHooks() {
    HMODULE hPix = LoadLibraryW(L"pixevt.dll");
    SC_EXPECT(hPix != nullptr);
    char pixPath[MAX_PATH]{};
    GetModuleFileNameA(hPix, pixPath, MAX_PATH);
    SC_EXPECT(std::strstr(pixPath, "\\shims\\pixevt.dll") != nullptr);

    auto pixBegin = reinterpret_cast<PIXBeginEventFn>(GetProcAddress(hPix, "PIXBeginEvent"));
    auto pixEnd   = reinterpret_cast<PIXEndEventFn>(GetProcAddress(hPix, "PIXEndEvent"));
    auto pixMark  = reinterpret_cast<PIXSetMarkerFn>(GetProcAddress(hPix, "PIXSetMarker"));
    SC_EXPECT(pixBegin && pixEnd && pixMark);
    pixBegin(0xFF00FF00ULL, "CTestScope");
    pixMark(0xFF00FF00ULL, "CTestMarker");
    pixEnd();
    FreeLibrary(hPix);

    HMODULE hPlugin = LoadLibraryW(L"..\\dummy_title\\engine_plugin.dll");
    SC_EXPECT(hPlugin != nullptr);
    using PluginFn = bool(WINAPI*)();
    auto runDiag = reinterpret_cast<PluginFn>(GetProcAddress(hPlugin, "PluginRunDiagnostics"));
    SC_EXPECT(runDiag != nullptr);
    const bool pluginOk = runDiag();
    FreeLibrary(hPlugin);
    SC_EXPECT(pluginOk);
    return true;
}

bool TestDynamicLoaderHooks() {
    // 1. GetModuleHandleW / A redirect to loaded shim in shims dir
    HMODULE hXgW = GetModuleHandleW(L"xgameruntime.dll");
    HMODULE hXgA = GetModuleHandleA("xgameruntime.dll");
    SC_EXPECT(hXgW != nullptr && hXgW == hXgA);

    wchar_t modPath[MAX_PATH]{};
    SC_EXPECT(GetModuleFileNameW(hXgW, modPath, MAX_PATH) > 0);
    std::wstring modPathStr(modPath);
    SC_EXPECT(modPathStr.find(L"shims") != std::wstring::npos);

    // 2. GetModuleHandleExW / A by name
    HMODULE hD3dW = nullptr;
    HMODULE hD3dA = nullptr;
    SC_EXPECT(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"d3d12_xs.dll", &hD3dW) != FALSE);
    SC_EXPECT(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "d3d12_xs.dll", &hD3dA) != FALSE);
    SC_EXPECT(hD3dW != nullptr && hD3dW == hD3dA);

    // 3. GetModuleHandleExW with FROM_ADDRESS must pass through safely
    HMODULE hSelf = nullptr;
    SC_EXPECT(GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&TestDynamicLoaderHooks), &hSelf) != FALSE);
    SC_EXPECT(hSelf == GetModuleHandleW(nullptr));

    // 4. Dynamic LoadLibraryW + GetProcAddress on a Scarlett shim
    HMODULE hGi = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(hGi != nullptr);
    FARPROC pCreate = GetProcAddress(hGi, "GameInputCreate");
    SC_EXPECT(pCreate != nullptr);
    FreeLibrary(hGi);

    return true;
}

bool TestLoaderInspectAndXvdMount() {
    // 1. PE64 import auditor on scarlett_tests.exe itself
    wchar_t selfBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfBuf, MAX_PATH);
    std::filesystem::path selfPath(selfBuf);

    wchar_t shimDirBuf[MAX_PATH]{};
    DWORD shimLen = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDirBuf, MAX_PATH);
    SC_EXPECT(shimLen > 0);
    std::filesystem::path shimDir(shimDirBuf);

    const auto audit = sc::loader::InspectPeImports(selfPath, shimDir);
    SC_EXPECT(audit.validPe64);
    SC_EXPECT(audit.totalImportedSymbols > 0);
    SC_EXPECT(audit.missingImports.empty());

    auto hasShimmed = [&](const char* target) {
        std::string t = sc::loader::detail::ToLowerAscii(target);
        for (const auto& s : audit.shimmedImports) {
            if (sc::loader::detail::ToLowerAscii(s) == t) return true;
        }
        return false;
    };
    SC_EXPECT(hasShimmed("d3d12_xs.dll"));
    SC_EXPECT(hasShimmed("xg_xs.dll"));
    SC_EXPECT(hasShimmed("acphal.dll"));
    SC_EXPECT(hasShimmed("libhttpclient.gdk.dll"));
    SC_EXPECT(hasShimmed("xcurl.dll"));

    // 2. Unencrypted XVD container mount + ReadUserData round-trip
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
    const std::filesystem::path validXvd = tmpDir / L"scarlett_test_valid.xvd";
    const std::filesystem::path encXvd   = tmpDir / L"scarlett_test_enc.xvd";

    const std::string payloadStr = "SCARLETT_XVD_VFS_PAYLOAD_OK";
    std::vector<uint8_t> payload(payloadStr.begin(), payloadStr.end());
    auto validImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, 0u);
    {
        std::ofstream out(validXvd, std::ios::binary);
        out.write(reinterpret_cast<const char*>(validImg.data()), static_cast<std::streamsize>(validImg.size()));
    }

    sc::vfs::XvdContainer xvd;
    std::string err;
    SC_EXPECT(xvd.Open(validXvd, err));
    SC_EXPECT(xvd.Header().magic == "msft-xvd");
    SC_EXPECT(xvd.Header().userDataLength == payload.size());
    std::vector<uint8_t> readBack;
    SC_EXPECT(xvd.ReadUserData(0, payload.size(), readBack, err));
    SC_EXPECT(readBack == payload);

    // 3. Encrypted XVD must be cleanly rejected
    auto encImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagEncrypted);
    {
        std::ofstream out(encXvd, std::ios::binary);
        out.write(reinterpret_cast<const char*>(encImg.data()), static_cast<std::streamsize>(encImg.size()));
    }
    SC_EXPECT(!xvd.Open(encXvd, err));

    std::error_code ec;
    std::filesystem::remove(validXvd, ec);
    std::filesystem::remove(encXvd, ec);
    return true;
}

bool TestGameConfigBootParser() {
    // 1. Synthetic MicrosoftGame.Config round-trip
    const auto tmpDir = std::filesystem::temp_directory_path() / "scarlett_gameconfig_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const auto cfgPath = tmpDir / "MicrosoftGame.Config";
    {
        std::ofstream ofs(cfgPath);
        ofs << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            << "<Game configVersion=\"1\">\n"
            << "  <Identity Name=\"ScarlettTestTitle\" Publisher=\"CN=Test\" Version=\"1.2.3.0\" />\n"
            << "  <TitleId>1234ABCD</TitleId>\n"
            << "  <ExecutableList>\n"
            << "    <Executable Name=\"game_main.exe\" Id=\"Game\" />\n"
            << "  </ExecutableList>\n"
            << "  <ShellVisuals DefaultDisplayName=\"Scarlett Test Title\" />\n"
            << "</Game>\n";
    }

    const auto parsed = sc::loader::ParseGameConfig(tmpDir);
    SC_EXPECT(parsed.valid);
    SC_EXPECT(parsed.identityName == "ScarlettTestTitle");
    SC_EXPECT(parsed.version == "1.2.3.0");
    SC_EXPECT(parsed.titleId == "1234ABCD");
    SC_EXPECT(parsed.displayName == "Scarlett Test Title");
    SC_EXPECT(parsed.executableName == "game_main.exe");
    SC_EXPECT(parsed.executableId == "Game");
    SC_EXPECT(parsed.executablePath == tmpDir / "game_main.exe");

    // 2. Reject missing directory cleanly
    const auto bad = sc::loader::ParseGameConfig(tmpDir / "does_not_exist");
    SC_EXPECT(!bad.valid && !bad.error.empty());

    std::filesystem::remove_all(tmpDir, ec);

    // 3. Validate all 4 ScarlettX synthetic game manifests when sibling repo is present
    const std::filesystem::path sxSynthRoot = "E:\\gta\\test\\ScarlettX\\tests\\synthetic_games";
    if (std::filesystem::exists(sxSynthRoot)) {
        const char* kTitles[] = { "hello_scarlett", "iat_probe", "rt_showcase", "scarlett_arena" };
        for (const char* title : kTitles) {
            const auto pkg = sc::loader::ParseGameConfig(sxSynthRoot / title);
            SC_EXPECT(pkg.valid);
            SC_EXPECT(!pkg.executableName.empty());
        }
    }

    return true;
}

bool TestLoaderXvdAndGameConfigE2E() {
    const auto tmpDir = std::filesystem::temp_directory_path() / "scarlett_sprint49_xvd_pkg";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string xmlManifest =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Game configVersion=\"1\">\n"
        "  <Identity Name=\"ScarlettPackageE2E\" Publisher=\"CN=Scarlett\" Version=\"4.9.0.0\" />\n"
        "  <TitleId>778899AA</TitleId>\n"
        "  <ShellVisuals DefaultDisplayName=\"Scarlett E2E Title\" />\n"
        "  <ExecutableList>\n"
        "    <Executable Name=\"ScarlettDemo.exe\" Id=\"Game\" />\n"
        "  </ExecutableList>\n"
        "</Game>\n";

    std::vector<uint8_t> payload(xmlManifest.begin(), xmlManifest.end());

    // 1. Verify encrypted XVD rejection
    const auto encImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(
        payload, 1u, sc::vfs::kXvdFlagEncrypted);
    const auto encPath = tmpDir / "encrypted_reject.xvd";
    {
        std::ofstream ofs(encPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(encImg.data()), static_cast<std::streamsize>(encImg.size()));
    }
    {
        sc::vfs::XvdContainer encXvd;
        std::string encErr;
        SC_EXPECT(!encXvd.Open(encPath, encErr));
    }

    // 2. Build & open unencrypted XVD, extract embedded MicrosoftGame.Config
    const auto rawImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, 0u);
    const auto xvdPath = tmpDir / "title_pkg.xvd";
    {
        std::ofstream ofs(xvdPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(rawImg.data()), static_cast<std::streamsize>(rawImg.size()));
    }

    sc::vfs::XvdContainer xvd;
    std::string err;
    SC_EXPECT(xvd.Open(xvdPath, err));
    SC_EXPECT(!xvd.Header().IsEncrypted());

    std::vector<uint8_t> extracted;
    SC_EXPECT(xvd.ReadUserData(0, payload.size(), extracted, err));
    SC_EXPECT(extracted.size() == payload.size());
    SC_EXPECT(std::memcmp(extracted.data(), payload.data(), payload.size()) == 0);

    // 3. Stage extracted manifest + target executable and parse via ParseGameConfig
    const auto pkgRoot = tmpDir / "mounted_pkg";
    std::filesystem::create_directories(pkgRoot, ec);
    {
        std::ofstream ofs(pkgRoot / "MicrosoftGame.Config", std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(extracted.data()), static_cast<std::streamsize>(extracted.size()));
    }
    {
        std::ofstream ofs(pkgRoot / "ScarlettDemo.exe", std::ios::binary);
        ofs << "MZ";
    }

    const auto cfg = sc::loader::ParseGameConfig(pkgRoot);
    SC_EXPECT(cfg.valid);
    SC_EXPECT(cfg.identityName == "ScarlettPackageE2E");
    SC_EXPECT(cfg.version == "4.9.0.0");
    SC_EXPECT(cfg.titleId == "778899AA");
    SC_EXPECT(cfg.displayName == "Scarlett E2E Title");
    SC_EXPECT(cfg.executableName == "ScarlettDemo.exe");
    SC_EXPECT(cfg.executableId == "Game");
    SC_EXPECT(std::filesystem::exists(cfg.executablePath));

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

bool TestPeDelayLoadAndOrdinalAuditE2E() {
    const auto tmpDir = std::filesystem::temp_directory_path() / "scarlett_sprint51_pe_audit";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    // Synthesize a minimal PE64 with 1 .rdata section (RVA 0x1000, RawOffset 0x200, Size 0x1000)
    std::vector<uint8_t> pe(0x1200, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(pe.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    // Import Directory at RVA 0x1000, Delay-Load Directory at RVA 0x1100
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR) * 2;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress = 0x1100;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size = sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) * 3;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sec->Name, ".rdata", 6);
    sec->Misc.VirtualSize = 0x1000;
    sec->VirtualAddress = 0x1000;
    sec->SizeOfRawData = 0x1000;
    sec->PointerToRawData = 0x200;

    // Helper: write at RVA (0x1000 + off -> file offset 0x200 + off)
    auto atRva = [&](uint32_t rva) -> uint8_t* { return pe.data() + 0x200 + (rva - 0x1000); };

    // Strings at 0x1300+
    std::strcpy(reinterpret_cast<char*>(atRva(0x1300)), "d3d12_xs.dll");
    std::strcpy(reinterpret_cast<char*>(atRva(0x1320)), "xgameruntime.dll");
    std::strcpy(reinterpret_cast<char*>(atRva(0x1340)), "custom_missing_delay.dll");

    // Standard import: d3d12_xs.dll -> 1 named thunk + 1 ordinal thunk (#42)
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(atRva(0x1000));
    imp[0].OriginalFirstThunk = 0x1200;
    imp[0].Name = 0x1300;
    auto* t0 = reinterpret_cast<uint64_t*>(atRva(0x1200));
    t0[0] = 0x1380ULL;                       // named RVA
    t0[1] = IMAGE_ORDINAL_FLAG64 | 42ULL;    // ordinal #42
    t0[2] = 0;

    // Delay-load imports: xgameruntime.dll (1 named) + custom_missing_delay.dll (1 ordinal #7)
    auto* dimp = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(atRva(0x1100));
    dimp[0].Attributes.AllAttributes = 1;
    dimp[0].DllNameRVA = 0x1320;
    dimp[0].ImportNameTableRVA = 0x1220;
    auto* t1 = reinterpret_cast<uint64_t*>(atRva(0x1220));
    t1[0] = 0x13A0ULL;
    t1[1] = 0;

    dimp[1].Attributes.AllAttributes = 1;
    dimp[1].DllNameRVA = 0x1340;
    dimp[1].ImportNameTableRVA = 0x1240;
    auto* t2 = reinterpret_cast<uint64_t*>(atRva(0x1240));
    t2[0] = IMAGE_ORDINAL_FLAG64 | 7ULL;     // ordinal #7
    t2[1] = 0;

    const auto pePath = tmpDir / "synthetic_delay_ordinal.exe";
    {
        std::ofstream ofs(pePath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
    }

    wchar_t shimDirBuf[MAX_PATH]{};
    DWORD shimLen = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDirBuf, MAX_PATH);
    SC_EXPECT(shimLen > 0);
    const auto audit = sc::loader::InspectPeImports(pePath, std::filesystem::path(shimDirBuf));

    SC_EXPECT(audit.validPe64);
    SC_EXPECT(audit.delayLoadImports.size() == 2);
    SC_EXPECT(audit.shimmedImports.size() == 2);
    SC_EXPECT(audit.missingImports.size() == 1 && audit.missingImports[0] == "custom_missing_delay.dll");
    SC_EXPECT(audit.totalImportedSymbols == 4);
    SC_EXPECT(audit.ordinalImportCount == 2);

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

bool TestRuntimeDelayLoadAndOrdinalHookE2E() {
    HMODULE hookMod = GetModuleHandleW(L"scarlett_hook.dll");
    bool loadedByTest = false;
    if (!hookMod) {
        hookMod = LoadLibraryW(L"scarlett_hook.dll");
        loadedByTest = (hookMod != nullptr);
    }
    SC_EXPECT(hookMod != nullptr);

    using PFN_PatchTest = uint32_t(WINAPI*)(HMODULE);
    using PFN_GetStats  = void(WINAPI*)(uint32_t*, uint32_t*);
    auto pPatchTest = reinterpret_cast<PFN_PatchTest>(GetProcAddress(hookMod, "ScarlettHookPatchModuleForTest"));
    auto pGetStats  = reinterpret_cast<PFN_GetStats>(GetProcAddress(hookMod, "ScarlettHookGetRuntimeStats"));
    SC_EXPECT(pPatchTest != nullptr && pGetStats != nullptr);

    uint32_t delayBefore = 0, ordBefore = 0;
    pGetStats(&delayBefore, &ordBefore);

    // Synthesize an in-memory PE64 image with a Delay-Load descriptor importing LoadLibraryW & GetProcAddress
    uint8_t* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    SC_EXPECT(img != nullptr);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size = sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) * 2;

    std::strcpy(reinterpret_cast<char*>(img + 0x1300), "kernel32.dll");
    auto* ibn0 = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(img + 0x1320);
    std::strcpy(ibn0->Name, "LoadLibraryW");
    auto* ibn1 = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(img + 0x1350);
    std::strcpy(ibn1->Name, "GetProcAddress");

    auto* ddesc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(img + 0x1000);
    ddesc[0].Attributes.AllAttributes = 1;
    ddesc[0].DllNameRVA = 0x1300;
    ddesc[0].ImportNameTableRVA = 0x1100;
    ddesc[0].ImportAddressTableRVA = 0x1200;

    auto* intThunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(img + 0x1100);
    intThunks[0].u1.AddressOfData = 0x1320;
    intThunks[1].u1.AddressOfData = 0x1350;
    intThunks[2].u1.AddressOfData = 0;

    auto* iatThunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(img + 0x1200);
    iatThunks[0].u1.Function = 0xDEADBEEFULL;
    iatThunks[1].u1.Function = 0xCAFEBABEULL;
    iatThunks[2].u1.Function = 0;

    const uint32_t patched = pPatchTest(reinterpret_cast<HMODULE>(img));
    SC_EXPECT(patched == 2);
    SC_EXPECT(iatThunks[0].u1.Function != 0xDEADBEEFULL);
    SC_EXPECT(iatThunks[1].u1.Function != 0xCAFEBABEULL);

    // Invoke the patched GetProcAddress hook with an ordinal lookup on d3d12_xs.dll
    using PFN_GetProc = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    auto hookedGetProc = reinterpret_cast<PFN_GetProc>(iatThunks[1].u1.Function);
    HMODULE d3dMod = LoadLibraryW(L"shims\\d3d12_xs.dll");
    if (!d3dMod) d3dMod = LoadLibraryW(L"d3d12_xs.dll");
    SC_EXPECT(d3dMod != nullptr);
    FARPROC ordProc = hookedGetProc(d3dMod, MAKEINTRESOURCEA(1));
    SC_EXPECT(ordProc != nullptr);
    FreeLibrary(d3dMod);

    uint32_t delayAfter = 0, ordAfter = 0;
    pGetStats(&delayAfter, &ordAfter);
    SC_EXPECT(delayAfter >= delayBefore + 2);
    SC_EXPECT(ordAfter >= ordBefore + 1);

    VirtualFree(img, 0, MEM_RELEASE);
    if (loadedByTest) {
        FreeLibrary(hookMod);
    }
    return true;
}

bool TestScarlettTitleBringUpE2E() {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 1. GDK Runtime + Offline XUser + XGameSave Provider
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

    auto pInit        = reinterpret_cast<HRESULT(WINAPI*)(void)>(GetProcAddress(xgrMod, "XGameRuntimeInitialize"));
    auto pUninit      = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(xgrMod, "XGameRuntimeUninitialize"));
    auto pQCreate     = reinterpret_cast<HRESULT(WINAPI*)(uint32_t, uint32_t, void**)>(GetProcAddress(xgrMod, "XTaskQueueCreate"));
    auto pQDispatch   = reinterpret_cast<bool(WINAPI*)(void*, uint32_t, uint32_t)>(GetProcAddress(xgrMod, "XTaskQueueDispatch"));
    auto pQClose      = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XTaskQueueCloseHandle"));
    auto pUserAdd     = reinterpret_cast<HRESULT(WINAPI*)(uint32_t, LocalAsyncBlock*)>(GetProcAddress(xgrMod, "XUserAddAsync"));
    auto pUserAddRes  = reinterpret_cast<HRESULT(WINAPI*)(LocalAsyncBlock*, void**)>(GetProcAddress(xgrMod, "XUserAddResult"));
    auto pUserGetId   = reinterpret_cast<HRESULT(WINAPI*)(void*, uint64_t*)>(GetProcAddress(xgrMod, "XUserGetId"));
    auto pUserClose   = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XUserCloseHandle"));
    auto pGsInit      = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, bool, LocalAsyncBlock*)>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderAsync"));
    auto pGsInitRes   = reinterpret_cast<HRESULT(WINAPI*)(LocalAsyncBlock*, void**)>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderResult"));
    auto pGsCloseProv = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseProvider"));
    auto pGsCreateC   = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, void**)>(GetProcAddress(xgrMod, "XGameSaveCreateContainer"));
    auto pGsCloseC    = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseContainer"));
    auto pGsCreateU   = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, void**)>(GetProcAddress(xgrMod, "XGameSaveCreateUpdate"));
    auto pGsCloseU    = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseUpdate"));
    auto pGsWrite     = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, const uint8_t*, size_t)>(GetProcAddress(xgrMod, "XGameSaveSubmitBlobWrite"));
    auto pGsSubmit    = reinterpret_cast<HRESULT(WINAPI*)(void*, LocalAsyncBlock*)>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateAsync"));
    auto pGsSubmitRes = reinterpret_cast<HRESULT(WINAPI*)(LocalAsyncBlock*)>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateResult"));
    auto pGsRead      = reinterpret_cast<HRESULT(WINAPI*)(void*, const char**, uint32_t, LocalAsyncBlock*)>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataAsync"));
    auto pGsReadRes   = reinterpret_cast<HRESULT(WINAPI*)(LocalAsyncBlock*, size_t, LocalBlob*, uint32_t*)>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataResult"));
    SC_EXPECT(pInit && pUninit && pQCreate && pQDispatch && pQClose &&
              pUserAdd && pUserAddRes && pUserGetId && pUserClose &&
              pGsInit && pGsInitRes && pGsCloseProv && pGsCreateC && pGsCloseC &&
              pGsCreateU && pGsCloseU && pGsWrite && pGsSubmit && pGsSubmitRes && pGsRead && pGsReadRes);

    SC_EXPECT(SUCCEEDED(pInit()));
    void* queue = nullptr;
    SC_EXPECT(SUCCEEDED(pQCreate(0, 0, &queue)) && queue != nullptr);

    LocalAsyncBlock abUser{}; abUser.queue = queue;
    SC_EXPECT(SUCCEEDED(pUserAdd(1, &abUser)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}
    void* user = nullptr;
    SC_EXPECT(SUCCEEDED(pUserAddRes(&abUser, &user)) && user != nullptr);
    uint64_t xuid = 0;
    SC_EXPECT(SUCCEEDED(pUserGetId(user, &xuid)) && xuid != 0);

    // 2. Networking (libHttpClient title config fetch)
    HMODULE hcMod = LoadLibraryW(L"shims\\libHttpClient.dll");
    if (!hcMod) hcMod = LoadLibraryW(L"shims\\libHttpClient.GDK.dll");
    if (!hcMod) hcMod = LoadLibraryW(L"libHttpClient.dll");
    SC_EXPECT(hcMod != nullptr);
    auto pHCInit    = reinterpret_cast<HRESULT(WINAPI*)(void*)>(GetProcAddress(hcMod, "HCInitialize"));
    auto pHCCleanup = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(hcMod, "HCCleanup"));
    auto pHCCreate  = reinterpret_cast<HRESULT(WINAPI*)(void**)>(GetProcAddress(hcMod, "HCHttpCallCreate"));
    auto pHCClose   = reinterpret_cast<HRESULT(WINAPI*)(void*)>(GetProcAddress(hcMod, "HCHttpCallCloseHandle"));
    auto pHCSetUrl  = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, const char*)>(GetProcAddress(hcMod, "HCHttpCallRequestSetUrl"));
    auto pHCPerform = reinterpret_cast<HRESULT(WINAPI*)(void*, void*)>(GetProcAddress(hcMod, "HCHttpCallPerformAsync"));
    auto pHCStatus  = reinterpret_cast<HRESULT(WINAPI*)(void*, uint32_t*)>(GetProcAddress(hcMod, "HCHttpCallResponseGetStatusCode"));
    auto pHCMock    = reinterpret_cast<HRESULT(WINAPI*)(const char*, const char*, uint32_t, const char*)>(GetProcAddress(hcMod, "HCHttpCallRegisterMock"));
    auto pHCClear   = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(hcMod, "HCHttpCallClearMocks"));
    SC_EXPECT(pHCInit && pHCCleanup && pHCCreate && pHCClose && pHCSetUrl && pHCPerform && pHCStatus && pHCMock && pHCClear);

    SC_EXPECT(SUCCEEDED(pHCInit(nullptr)));
    pHCClear();
    SC_EXPECT(SUCCEEDED(pHCMock("GET", "https://scarlett.local/bringup", 200, "{\"title\":\"scarlett-demo\"}")));
    void* hcCall = nullptr;
    SC_EXPECT(SUCCEEDED(pHCCreate(&hcCall)));
    SC_EXPECT(SUCCEEDED(pHCSetUrl(hcCall, "GET", "https://scarlett.local/bringup/manifest")));
    SC_EXPECT(SUCCEEDED(pHCPerform(hcCall, nullptr)));
    uint32_t httpStatus = 0;
    SC_EXPECT(SUCCEEDED(pHCStatus(hcCall, &httpStatus)) && httpStatus == 200);
    pHCClose(hcCall);
    pHCClear();
    pHCCleanup();
    FreeLibrary(hcMod);

    // 3. Input (GameInput synthetic gamepad poll)
    HMODULE giMod = LoadLibraryW(L"shims\\gameinput.dll");
    if (!giMod) giMod = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(giMod != nullptr);
    struct LocalGamepadState { uint32_t buttons; float lt, rt, lx, ly, rx, ry; };
    struct LocalKeyState { uint32_t scanCode, codePoint; uint8_t vk; bool isDead; };
    struct ILocalGameInputReading : public IUnknown {
        virtual uint32_t STDMETHODCALLTYPE GetInputKind() = 0;
        virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
        virtual bool     STDMETHODCALLTYPE GetGamepadState(LocalGamepadState*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, LocalKeyState*) = 0;
    };
    struct ILocalGameInput : public IUnknown {
        virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
        virtual HRESULT  STDMETHODCALLTYPE GetCurrentReading(uint32_t, void*, ILocalGameInputReading**) = 0;
    };
    auto pGICreate   = reinterpret_cast<HRESULT(WINAPI*)(ILocalGameInput**)>(GetProcAddress(giMod, "GameInputCreate"));
    auto pGISetSynth = reinterpret_cast<void(WINAPI*)(bool, const LocalGamepadState*)>(GetProcAddress(giMod, "ScarlettGameInputSetSyntheticGamepad"));
    SC_EXPECT(pGICreate && pGISetSynth);

    ILocalGameInput* gi = nullptr;
    SC_EXPECT(SUCCEEDED(pGICreate(&gi)) && gi != nullptr);
    LocalGamepadState padIn{ 0x00000004u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f }; // A button pressed
    pGISetSynth(true, &padIn);
    ILocalGameInputReading* reading = nullptr;
    SC_EXPECT(SUCCEEDED(gi->GetCurrentReading(0x00020000u, nullptr, &reading)) && reading != nullptr);
    LocalGamepadState padOut{};
    SC_EXPECT(reading->GetGamepadState(&padOut) && (padOut.buttons & 0x00000004u) != 0);
    pGISetSynth(false, nullptr);
    reading->Release();
    gi->Release();
    FreeLibrary(giMod);

    // 4. Audio (XAudio2 engine init alongside GPU)
    HMODULE xaMod = LoadLibraryW(L"shims\\xaudio2_9.dll");
    if (!xaMod) xaMod = LoadLibraryW(L"xaudio2_9.dll");
    SC_EXPECT(xaMod != nullptr);
    auto pXACreate = reinterpret_cast<HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR)>(GetProcAddress(xaMod, "XAudio2Create"));
    SC_EXPECT(pXACreate != nullptr);
    IXAudio2* xa = nullptr;
    SC_EXPECT(SUCCEEDED(pXACreate(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) && xa != nullptr);

    // 5. Graphics (D3D12_XS input-driven render-target clear + GPU readback)
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));
    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device, reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(devX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))));

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 4; rtDesc.Height = 4; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Green clear triggered by GameInput A-button press
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearVal.Color[0] = 0.0f; clearVal.Color[1] = 1.0f; clearVal.Color[2] = 0.0f; clearVal.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&rtTex))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvHandle);

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 4; rbDesc.Height = 1; rbDesc.DepthOrArraySize = 1; rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1; rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    clX->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    clX->ClearRenderTargetView(rtvHandle, clearVal.Color, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rtTex.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    clX->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{ rtTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { 0 } };
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rbBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256 };
    clX->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    SC_EXPECT(SUCCEEDED(clX->Close()));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(ev != nullptr);
    ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
    queueX->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(queueX->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
    SC_EXPECT(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);

    uint8_t* pRb = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&pRb))));
    SC_EXPECT(pRb[0] == 0 && pRb[1] == 255 && pRb[2] == 0 && pRb[3] == 255);
    rbBuf->Unmap(0, nullptr);

    xa->Release();
    FreeLibrary(xaMod);

    // 6. Persist verified frame state via XGameSave
    LocalAsyncBlock abProv{}; abProv.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsInit(user, "00000000-0000-0000-0000-000000000045", false, &abProv)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}
    void* provider = nullptr;
    SC_EXPECT(SUCCEEDED(pGsInitRes(&abProv, &provider)) && provider != nullptr);

    void* container = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCreateC(provider, "bringup_frame", &container)) && container != nullptr);
    void* upd = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCreateU(container, "Frame 1", &upd)) && upd != nullptr);
    const std::string stateStr = "frame1:green:ok";
    SC_EXPECT(SUCCEEDED(pGsWrite(upd, "state", reinterpret_cast<const uint8_t*>(stateStr.data()), stateStr.size())));

    LocalAsyncBlock abSub{}; abSub.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsSubmit(upd, &abSub)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}
    SC_EXPECT(SUCCEEDED(pGsSubmitRes(&abSub)));
    pGsCloseU(upd);

    const char* names[] = { "state" };
    LocalAsyncBlock abRead{}; abRead.queue = queue;
    SC_EXPECT(SUCCEEDED(pGsRead(container, names, 1, &abRead)));
    while (pQDispatch(queue, 0, 0)) {}
    while (pQDispatch(queue, 1, 0)) {}
    std::vector<uint8_t> readBuf(512, 0);
    uint32_t blobCount = 0;
    SC_EXPECT(SUCCEEDED(pGsReadRes(&abRead, readBuf.size(), reinterpret_cast<LocalBlob*>(readBuf.data()), &blobCount)));
    SC_EXPECT(blobCount == 1);
    const auto* blob = reinterpret_cast<const LocalBlob*>(readBuf.data());
    SC_EXPECT(blob->info.size == stateStr.size() && std::memcmp(blob->data, stateStr.data(), stateStr.size()) == 0);

    pGsCloseC(container);
    pGsCloseProv(provider);
    pUserClose(user);
    pQClose(queue);
    pUninit();
    FreeLibrary(xgrMod);
    return true;
}

bool TestScarlettFullStack50SprintCapstoneE2E() {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 1. Profiler (pixevt) + OS Topology & Budget (kernelx)
    HMODULE pixMod = LoadLibraryW(L"shims\\pixevt.dll");
    if (!pixMod) pixMod = LoadLibraryW(L"pixevt.dll");
    SC_EXPECT(pixMod != nullptr);
    auto pPixBegin  = reinterpret_cast<void(WINAPI*)(uint64_t, const char*)>(GetProcAddress(pixMod, "PIXBeginEvent"));
    auto pPixEnd    = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(pixMod, "PIXEndEvent"));
    auto pPixMarker = reinterpret_cast<void(WINAPI*)(uint64_t, const char*)>(GetProcAddress(pixMod, "PIXSetMarker"));
    SC_EXPECT(pPixBegin && pPixEnd && pPixMarker);

    pPixBegin(0xFFFF0040ULL, "Sprint50::GoldenMasterCapstone");

    HMODULE kxMod = LoadLibraryW(L"shims\\kernelx.dll");
    if (!kxMod) kxMod = LoadLibraryW(L"kernelx.dll");
    SC_EXPECT(kxMod != nullptr);
    struct LocalMemStat { DWORD len, res; uint64_t tot, avail, lu, lp, la, tu, tp, ta; };
    struct LocalProcInfo { uint32_t tc, tt, ht; DWORD_PTR mask; };
    auto pTitleMem = reinterpret_cast<BOOL(WINAPI*)(LocalMemStat*)>(GetProcAddress(kxMod, "TitleMemoryStatus"));
    auto pProcInfo = reinterpret_cast<BOOL(WINAPI*)(LocalProcInfo*)>(GetProcAddress(kxMod, "GetConsoleProcessorInfo"));
    SC_EXPECT(pTitleMem && pProcInfo);
    LocalMemStat ms{}; ms.len = sizeof(ms);
    SC_EXPECT(pTitleMem(&ms) != FALSE && ms.tot == 14495514624ULL);
    LocalProcInfo pi{};
    SC_EXPECT(pProcInfo(&pi) != FALSE && pi.tc == 7 && pi.tt == 14);

    // 2. Unencrypted XVD + MicrosoftGame.Config Packaging
    const auto tmpDir = std::filesystem::temp_directory_path() / "scarlett_sprint50_capstone";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string xmlManifest =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Game configVersion=\"1\">\n"
        "  <Identity Name=\"ScarlettGoldenMaster\" Publisher=\"CN=Scarlett\" Version=\"1.0.50.0\" />\n"
        "  <TitleId>50505050</TitleId>\n"
        "  <ShellVisuals DefaultDisplayName=\"Scarlett Layer v1.0\" />\n"
        "  <ExecutableList><Executable Name=\"ScarlettCapstone.exe\" Id=\"Game\" /></ExecutableList>\n"
        "</Game>\n";
    std::vector<uint8_t> manifestBytes(xmlManifest.begin(), xmlManifest.end());
    const auto xvdImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(manifestBytes, 1u, 0u);
    const auto xvdPath = tmpDir / "golden_master.xvd";
    { std::ofstream ofs(xvdPath, std::ios::binary); ofs.write(reinterpret_cast<const char*>(xvdImg.data()), static_cast<std::streamsize>(xvdImg.size())); }

    sc::vfs::XvdContainer xvd;
    std::string xvdErr;
    SC_EXPECT(xvd.Open(xvdPath, xvdErr) && !xvd.Header().IsEncrypted());
    std::vector<uint8_t> extracted;
    SC_EXPECT(xvd.ReadUserData(0, manifestBytes.size(), extracted, xvdErr));
    const auto pkgRoot = tmpDir / "pkg";
    std::filesystem::create_directories(pkgRoot, ec);
    { std::ofstream ofs(pkgRoot / "MicrosoftGame.Config", std::ios::binary); ofs.write(reinterpret_cast<const char*>(extracted.data()), static_cast<std::streamsize>(extracted.size())); }
    { std::ofstream ofs(pkgRoot / "ScarlettCapstone.exe", std::ios::binary); ofs << "MZ"; }
    const auto cfg = sc::loader::ParseGameConfig(pkgRoot);
    SC_EXPECT(cfg.valid && cfg.identityName == "ScarlettGoldenMaster" && cfg.titleId == "50505050");

    // 3. Physical Memory (xmem) + 64KB_S BC7 Swizzle (xg_xs) + Velocity Streaming (dstorage_xs)
    pPixMarker(0xFF00CCFFULL, "Sprint50::AssetSwizzleAndStream");
    HMODULE xmemMod = LoadLibraryW(L"shims\\xmem.dll");
    if (!xmemMod) xmemMod = LoadLibraryW(L"xmem.dll");
    HMODULE xgMod = LoadLibraryW(L"shims\\xg_xs.dll");
    if (!xgMod) xgMod = LoadLibraryW(L"xg_xs.dll");
    HMODULE dsMod = LoadLibraryW(L"shims\\dstorage_xs.dll");
    if (!dsMod) dsMod = LoadLibraryW(L"dstorage_xs.dll");
    SC_EXPECT(xmemMod && xgMod && dsMod);

    auto pXMemAlloc = reinterpret_cast<void*(WINAPI*)(size_t, uint32_t)>(GetProcAddress(xmemMod, "XMemAlloc"));
    auto pXMemFree  = reinterpret_cast<void(WINAPI*)(void*, uint32_t)>(GetProcAddress(xmemMod, "XMemFree"));
    SC_EXPECT(pXMemAlloc && pXMemFree);

    struct LocalXgDesc { uint32_t w, h, mips, arr; DXGI_FORMAT fmt; uint32_t mode; };
    struct LocalXgMip { uint64_t off, sz; uint32_t pitch, pw, ph, mode; };
    struct LocalXgPlane { uint64_t baseOff, sz; uint32_t bpe, mips; LocalXgMip ml[15]; };
    struct LocalXgLayout { uint64_t sz, align; uint32_t planes; LocalXgPlane pl[1]; };
    struct ILocalXgComp : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetResourceLayout(LocalXgLayout*) = 0;
        virtual HRESULT STDMETHODCALLTYPE CopyIntoSubresource(void*, uint32_t, const void*, uint32_t, uint32_t) = 0;
        virtual HRESULT STDMETHODCALLTYPE CopyFromSubresource(void*, uint32_t, const void*, uint32_t, uint32_t) = 0;
    };
    auto pXgCreate = reinterpret_cast<HRESULT(WINAPI*)(const LocalXgDesc*, ILocalXgComp**)>(GetProcAddress(xgMod, "XGCreateTexture2DComputer"));
    SC_EXPECT(pXgCreate != nullptr);

    LocalXgDesc xgDesc{ 256, 256, 1, 1, DXGI_FORMAT_BC7_UNORM, 2u };
    ILocalXgComp* xgComp = nullptr;
    SC_EXPECT(SUCCEEDED(pXgCreate(&xgDesc, &xgComp)) && xgComp != nullptr);
    LocalXgLayout xgLayout{};
    SC_EXPECT(SUCCEEDED(xgComp->GetResourceLayout(&xgLayout)) && xgLayout.sz >= 65536ULL);

    const uint32_t physAttrs = (10u) | (15u << 8) | (1u << 12) | (1u << 14); // TextureTiled, 64K, Physical, ZeroInit
    void* tiledSrc = pXMemAlloc(static_cast<size_t>(xgLayout.sz), physAttrs);
    void* tiledDst = pXMemAlloc(static_cast<size_t>(xgLayout.sz), physAttrs);
    SC_EXPECT(tiledSrc && tiledDst);

    constexpr uint32_t kRowPitch = 64u * 16u;
    constexpr size_t   kLinearBytes = static_cast<size_t>(kRowPitch) * 64u;
    std::vector<uint8_t> linearIn(kLinearBytes), linearOut(kLinearBytes, 0);
    for (size_t i = 0; i < kLinearBytes; ++i) linearIn[i] = static_cast<uint8_t>((i * 50u + 7u) & 0xFFu);
    SC_EXPECT(SUCCEEDED(xgComp->CopyIntoSubresource(tiledSrc, 0, linearIn.data(), kRowPitch, static_cast<uint32_t>(kLinearBytes))));

    auto pDsGetFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dsMod, "DStorageGetFactory"));
    SC_EXPECT(pDsGetFactory != nullptr);
    ComPtr<IDStorageFactoryX> dsFactory;
    SC_EXPECT(SUCCEEDED(pDsGetFactory(IID_IDStorageFactoryX, reinterpret_cast<void**>(dsFactory.GetAddressOf()))));
    DSTORAGE_QUEUE_DESC dsQd{ DSTORAGE_REQUEST_SOURCE_MEMORY, 16, DSTORAGE_PRIORITY_REALTIME, "Sprint50CapstoneQ", nullptr };
    ComPtr<IDStorageQueueX> dsQueue;
    SC_EXPECT(SUCCEEDED(dsFactory->CreateQueue(&dsQd, IID_IDStorageQueueX, reinterpret_cast<void**>(dsQueue.GetAddressOf()))));
    ComPtr<IDStorageStatusArrayX> dsStatus;
    SC_EXPECT(SUCCEEDED(dsFactory->CreateStatusArray(1, "Sprint50Status", IID_IDStorageStatusArrayX, reinterpret_cast<void**>(dsStatus.GetAddressOf()))));

    DSTORAGE_REQUEST dsReq{};
    dsReq.Options = { DSTORAGE_COMPRESSION_FORMAT_NONE, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_REQUEST_DESTINATION_MEMORY };
    dsReq.Source.Memory = { tiledSrc, static_cast<uint32_t>(xgLayout.sz) };
    dsReq.Destination.Memory = { tiledDst, static_cast<uint32_t>(xgLayout.sz) };
    dsReq.UncompressedSize = static_cast<uint32_t>(xgLayout.sz);
    dsReq.Name = "Sprint50::BC7Asset";
    dsQueue->EnqueueRequest(&dsReq);
    dsQueue->EnqueueStatus(dsStatus.Get(), 0);
    dsQueue->Submit();
    for (uint32_t s = 0; !dsStatus->IsComplete(0) && s < 5000; ++s) Sleep(1);
    SC_EXPECT(dsStatus->IsComplete(0) && SUCCEEDED(dsStatus->GetHResult(0)));

    SC_EXPECT(SUCCEEDED(xgComp->CopyFromSubresource(linearOut.data(), 0, tiledDst, kRowPitch, static_cast<uint32_t>(kLinearBytes))));
    SC_EXPECT(std::memcmp(linearIn.data(), linearOut.data(), kLinearBytes) == 0);

    dsStatus.Reset(); dsQueue.Reset(); dsFactory.Reset();
    xgComp->Release();
    pXMemFree(tiledDst, physAttrs);
    pXMemFree(tiledSrc, physAttrs);
    FreeLibrary(dsMod); FreeLibrary(xgMod); FreeLibrary(xmemMod);

    // 4. Input (gameinput) + Audio (xaudio2_9) + GPU Render Clear (d3d12_xs)
    pPixMarker(0xFFFF0040ULL, "Sprint50::InputAudioGpuFrame");
    HMODULE giMod = LoadLibraryW(L"shims\\gameinput.dll");
    if (!giMod) giMod = LoadLibraryW(L"gameinput.dll");
    SC_EXPECT(giMod != nullptr);
    struct LocalPad { uint32_t buttons; float lt, rt, lx, ly, rx, ry; };
    struct LocalKey { uint32_t sc, cp; uint8_t vk; bool dead; };
    struct ILocalGIRead : public IUnknown {
        virtual uint32_t STDMETHODCALLTYPE GetInputKind() = 0;
        virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
        virtual bool     STDMETHODCALLTYPE GetGamepadState(LocalPad*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, LocalKey*) = 0;
    };
    struct ILocalGI : public IUnknown {
        virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
        virtual HRESULT  STDMETHODCALLTYPE GetCurrentReading(uint32_t, void*, ILocalGIRead**) = 0;
    };
    auto pGICreate = reinterpret_cast<HRESULT(WINAPI*)(ILocalGI**)>(GetProcAddress(giMod, "GameInputCreate"));
    auto pGISynth  = reinterpret_cast<void(WINAPI*)(bool, const LocalPad*)>(GetProcAddress(giMod, "ScarlettGameInputSetSyntheticGamepad"));
    SC_EXPECT(pGICreate && pGISynth);
    ILocalGI* gi = nullptr;
    SC_EXPECT(SUCCEEDED(pGICreate(&gi)) && gi != nullptr);
    LocalPad padIn{ 0x00000004u, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    pGISynth(true, &padIn);
    ILocalGIRead* rd = nullptr;
    SC_EXPECT(SUCCEEDED(gi->GetCurrentReading(0x00020000u, nullptr, &rd)) && rd != nullptr);
    LocalPad padOut{};
    SC_EXPECT(rd->GetGamepadState(&padOut) && (padOut.buttons & 0x00000004u) != 0);
    pGISynth(false, nullptr);
    rd->Release(); gi->Release(); FreeLibrary(giMod);

    HMODULE xaMod = LoadLibraryW(L"shims\\xaudio2_9.dll");
    if (!xaMod) xaMod = LoadLibraryW(L"xaudio2_9.dll");
    SC_EXPECT(xaMod != nullptr);
    auto pXACreate = reinterpret_cast<HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR)>(GetProcAddress(xaMod, "XAudio2Create"));
    IXAudio2* xa = nullptr;
    SC_EXPECT(pXACreate && SUCCEEDED(pXACreate(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) && xa != nullptr);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{}; devParams.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));
    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(IID_ScarlettNativeD3D12Device, reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(clX.GetAddressOf()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHD{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(devX->CreateDescriptorHeap(&rtvHD, IID_PPV_ARGS(&rtvHeap))));

    D3D12_HEAP_PROPERTIES defHeap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES rbHeap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 4; rtDesc.Height = 4; rtDesc.DepthOrArraySize = 1; rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Scarlett Crimson (255, 0, 64, 255)
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearVal.Color[0] = 1.0f; clearVal.Color[1] = 0.0f; clearVal.Color[2] = 64.0f / 255.0f; clearVal.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&rtTex))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvH);

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 4; rbDesc.Height = 1; rbDesc.DepthOrArraySize = 1; rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1; rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf))));

    clX->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);
    clX->ClearRenderTargetView(rtvH, clearVal.Color, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { rtTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    clX->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION srcL{ rtTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { 0 } };
    D3D12_TEXTURE_COPY_LOCATION dstL{};
    dstL.pResource = rbBuf.Get();
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstL.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256 };
    clX->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, nullptr);
    SC_EXPECT(SUCCEEDED(clX->Close()));

    ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(nativeDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SC_EXPECT(ev != nullptr);
    ID3D12CommandList* lists[] = { clX->GetNativeCommandList() };
    queueX->ExecuteCommandLists(1, lists);
    SC_EXPECT(SUCCEEDED(queueX->Signal(fence.Get(), 1)));
    SC_EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
    SC_EXPECT(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);

    uint8_t* pRb = nullptr;
    SC_EXPECT(SUCCEEDED(rbBuf->Map(0, nullptr, reinterpret_cast<void**>(&pRb))));
    SC_EXPECT(pRb[0] == 255 && pRb[1] == 0 && pRb[2] == 64 && pRb[3] == 255);
    rbBuf->Unmap(0, nullptr);

    xa->Release();
    FreeLibrary(xaMod);

    // 5. GDK Runtime + XUser + XGameSave Golden Master Checkpoint
    pPixMarker(0xFF00FF88ULL, "Sprint50::PersistCheckpoint");
    HMODULE xgrMod = LoadLibraryW(L"shims\\xgameruntime.dll");
    if (!xgrMod) xgrMod = LoadLibraryW(L"xgameruntime.dll");
    SC_EXPECT(xgrMod != nullptr);
    struct LocalAB { void* q; void* ctx; void (CALLBACK* cb)(LocalAB*); unsigned char in[sizeof(void*) * 4]; };
    struct LocalBI { const char* name; uint32_t size; };
    struct LocalBlob { LocalBI info; uint8_t* data; };

    auto pInit   = reinterpret_cast<HRESULT(WINAPI*)(void)>(GetProcAddress(xgrMod, "XGameRuntimeInitialize"));
    auto pUninit = reinterpret_cast<void(WINAPI*)(void)>(GetProcAddress(xgrMod, "XGameRuntimeUninitialize"));
    auto pQCr    = reinterpret_cast<HRESULT(WINAPI*)(uint32_t, uint32_t, void**)>(GetProcAddress(xgrMod, "XTaskQueueCreate"));
    auto pQDisp  = reinterpret_cast<bool(WINAPI*)(void*, uint32_t, uint32_t)>(GetProcAddress(xgrMod, "XTaskQueueDispatch"));
    auto pQCl    = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XTaskQueueCloseHandle"));
    auto pUAdd   = reinterpret_cast<HRESULT(WINAPI*)(uint32_t, LocalAB*)>(GetProcAddress(xgrMod, "XUserAddAsync"));
    auto pUAddR  = reinterpret_cast<HRESULT(WINAPI*)(LocalAB*, void**)>(GetProcAddress(xgrMod, "XUserAddResult"));
    auto pUCl    = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XUserCloseHandle"));
    auto pGsIn   = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, bool, LocalAB*)>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderAsync"));
    auto pGsInR  = reinterpret_cast<HRESULT(WINAPI*)(LocalAB*, void**)>(GetProcAddress(xgrMod, "XGameSaveInitializeProviderResult"));
    auto pGsClP  = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseProvider"));
    auto pGsCrC  = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, void**)>(GetProcAddress(xgrMod, "XGameSaveCreateContainer"));
    auto pGsClC  = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseContainer"));
    auto pGsCrU  = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, void**)>(GetProcAddress(xgrMod, "XGameSaveCreateUpdate"));
    auto pGsClU  = reinterpret_cast<void(WINAPI*)(void*)>(GetProcAddress(xgrMod, "XGameSaveCloseUpdate"));
    auto pGsWr   = reinterpret_cast<HRESULT(WINAPI*)(void*, const char*, const uint8_t*, size_t)>(GetProcAddress(xgrMod, "XGameSaveSubmitBlobWrite"));
    auto pGsSub  = reinterpret_cast<HRESULT(WINAPI*)(void*, LocalAB*)>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateAsync"));
    auto pGsSubR = reinterpret_cast<HRESULT(WINAPI*)(LocalAB*)>(GetProcAddress(xgrMod, "XGameSaveSubmitUpdateResult"));
    auto pGsRd   = reinterpret_cast<HRESULT(WINAPI*)(void*, const char**, uint32_t, LocalAB*)>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataAsync"));
    auto pGsRdR  = reinterpret_cast<HRESULT(WINAPI*)(LocalAB*, size_t, LocalBlob*, uint32_t*)>(GetProcAddress(xgrMod, "XGameSaveReadBlobDataResult"));
    SC_EXPECT(pInit && pUninit && pQCr && pQDisp && pQCl && pUAdd && pUAddR && pUCl &&
              pGsIn && pGsInR && pGsClP && pGsCrC && pGsClC && pGsCrU && pGsClU && pGsWr && pGsSub && pGsSubR && pGsRd && pGsRdR);

    SC_EXPECT(SUCCEEDED(pInit()));
    void* tq = nullptr;
    SC_EXPECT(SUCCEEDED(pQCr(0, 0, &tq)) && tq != nullptr);
    LocalAB abU{}; abU.q = tq;
    SC_EXPECT(SUCCEEDED(pUAdd(1, &abU)));
    while (pQDisp(tq, 0, 0)) {} while (pQDisp(tq, 1, 0)) {}
    void* user = nullptr;
    SC_EXPECT(SUCCEEDED(pUAddR(&abU, &user)) && user != nullptr);

    LocalAB abP{}; abP.q = tq;
    SC_EXPECT(SUCCEEDED(pGsIn(user, "00000000-0000-0000-0000-000000000050", false, &abP)));
    while (pQDisp(tq, 0, 0)) {} while (pQDisp(tq, 1, 0)) {}
    void* prov = nullptr;
    SC_EXPECT(SUCCEEDED(pGsInR(&abP, &prov)) && prov != nullptr);

    void* cont = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCrC(prov, "sprint50_golden", &cont)) && cont != nullptr);
    void* upd = nullptr;
    SC_EXPECT(SUCCEEDED(pGsCrU(cont, "GoldenMaster", &upd)) && upd != nullptr);
    const std::string checkpoint = "scarlett:v1.0:sprint50:all9shims:ok";
    SC_EXPECT(SUCCEEDED(pGsWr(upd, "milestone", reinterpret_cast<const uint8_t*>(checkpoint.data()), checkpoint.size())));

    LocalAB abS{}; abS.q = tq;
    SC_EXPECT(SUCCEEDED(pGsSub(upd, &abS)));
    while (pQDisp(tq, 0, 0)) {} while (pQDisp(tq, 1, 0)) {}
    SC_EXPECT(SUCCEEDED(pGsSubR(&abS)));
    pGsClU(upd);

    const char* bNames[] = { "milestone" };
    LocalAB abR{}; abR.q = tq;
    SC_EXPECT(SUCCEEDED(pGsRd(cont, bNames, 1, &abR)));
    while (pQDisp(tq, 0, 0)) {} while (pQDisp(tq, 1, 0)) {}
    std::vector<uint8_t> rBuf(512, 0);
    uint32_t bCnt = 0;
    SC_EXPECT(SUCCEEDED(pGsRdR(&abR, rBuf.size(), reinterpret_cast<LocalBlob*>(rBuf.data()), &bCnt)));
    SC_EXPECT(bCnt == 1);
    const auto* blob = reinterpret_cast<const LocalBlob*>(rBuf.data());
    SC_EXPECT(blob->info.size == checkpoint.size() && std::memcmp(blob->data, checkpoint.data(), checkpoint.size()) == 0);

    pGsClC(cont); pGsClP(prov); pUCl(user); pQCl(tq); pUninit();
    FreeLibrary(xgrMod);
    FreeLibrary(kxMod);
    pPixEnd();
    FreeLibrary(pixMod);
    std::filesystem::remove_all(tmpDir, ec);
    return true;
}
