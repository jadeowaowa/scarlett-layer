#include "test_harness.hpp"
#include "common/ansi_path.hpp"

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
    auto validImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagEncryptionDisabled);
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
    auto encImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, 0u);
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
        payload, 1u, 0u);
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
    const auto rawImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagEncryptionDisabled);
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
        virtual uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t) = 0;
        virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
        virtual void     STDMETHODCALLTYPE GetDevice(void**) = 0;
        virtual bool     STDMETHODCALLTYPE GetRawReport(void**) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerAxisCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t, float*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerButtonCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t, bool*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, uint32_t*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, LocalKeyState*) = 0;
        virtual bool     STDMETHODCALLTYPE GetMouseState(void*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetTouchCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetMotionState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetArcadeStickState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetFlightStickState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetGamepadState(LocalGamepadState*) = 0;
        virtual bool     STDMETHODCALLTYPE GetRacingWheelState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetUiNavigationState(void*) = 0;
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
    const auto xvdImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(manifestBytes, 1u, sc::vfs::kXvdFlagEncryptionDisabled);
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
        virtual uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t) = 0;
        virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
        virtual void     STDMETHODCALLTYPE GetDevice(void**) = 0;
        virtual bool     STDMETHODCALLTYPE GetRawReport(void**) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerAxisCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t, float*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerButtonCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t, bool*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, uint32_t*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, LocalKey*) = 0;
        virtual bool     STDMETHODCALLTYPE GetMouseState(void*) = 0;
        virtual uint32_t STDMETHODCALLTYPE GetTouchCount() = 0;
        virtual uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetMotionState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetArcadeStickState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetFlightStickState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetGamepadState(LocalPad*) = 0;
        virtual bool     STDMETHODCALLTYPE GetRacingWheelState(void*) = 0;
        virtual bool     STDMETHODCALLTYPE GetUiNavigationState(void*) = 0;
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

namespace {

uint32_t g_s63TlsSlot = TLS_OUT_OF_INDEXES;
int g_s63AttachCount = 0;
int g_s63DetachCount = 0;
bool g_s63SlotMatches = false;
std::vector<int> g_s63AtExitOrder;

void NTAPI S63TlsCallbackA(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ++g_s63AttachCount;
        void* val = TlsGetValue(g_s63TlsSlot);
        if (val && static_cast<const uint8_t*>(val)[0] == 0x5Au) {
            g_s63SlotMatches = true;
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        ++g_s63DetachCount;
    }
}

void NTAPI S63TlsCallbackB(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ++g_s63AttachCount;
    } else if (reason == DLL_PROCESS_DETACH) {
        ++g_s63DetachCount;
    }
}

void __cdecl S63AtExit1() { g_s63AtExitOrder.push_back(1); }
void __cdecl S63AtExit2() { g_s63AtExitOrder.push_back(2); }
void __cdecl S63AtExit3() { g_s63AtExitOrder.push_back(3); }

} // namespace

bool TestPe64ImplicitTlsAndGuestAtExitE2E() {
    // 1. Static PE64 IMAGE_DIRECTORY_ENTRY_TLS inspection on a synthetic PE32+ image
    std::vector<uint8_t> pe(0x400, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(pe.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = 0x140000000ULL;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size = sizeof(IMAGE_TLS_DIRECTORY64);

    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sec->Name, ".rdata", 6);
    sec->Misc.VirtualSize = 0x200;
    sec->VirtualAddress = 0x1000;
    sec->SizeOfRawData = 0x200;
    sec->PointerToRawData = 0x200;

    auto* fileTls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(pe.data() + 0x200);
    fileTls->StartAddressOfRawData = 0x140001040ULL;
    fileTls->EndAddressOfRawData   = 0x140001060ULL; // 32 bytes raw template
    fileTls->AddressOfIndex        = 0x140001060ULL;
    fileTls->AddressOfCallBacks    = 0x140001080ULL;
    fileTls->SizeOfZeroFill        = 16;
    fileTls->Characteristics       = 0x00500000u; // 16-byte alignment

    uint64_t fakeCbs[3] = { 0x140001100ULL, 0x140001120ULL, 0ULL };
    std::memcpy(pe.data() + 0x280, fakeCbs, sizeof(fakeCbs));

    const auto probePath = sc::vfs::TranslatePath(L"T:\\sprint63_tls_probe.exe");
    std::filesystem::create_directories(probePath.parent_path());
    {
        std::ofstream out(probePath, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.good());
        out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
    }

    const auto audit = sc::loader::InspectPeImports(probePath, probePath.parent_path());
    std::error_code ec;
    std::filesystem::remove(probePath, ec);

    SC_EXPECT(audit.validPe64);
    SC_EXPECT(audit.hasTlsDirectory);
    SC_EXPECT(audit.tlsRawDataSize == 32u);
    SC_EXPECT(audit.tlsZeroFillSize == 16u);
    SC_EXPECT(audit.tlsCallbackCount == 2u);
    SC_EXPECT(audit.tlsCharacteristics == 0x00500000u);

    // 2. Live implicit TLS template copy + zero-fill + callback dispatch via scarlett_hook.dll
    HMODULE hookMod = GetModuleHandleW(L"scarlett_hook.dll");
    SC_EXPECT(hookMod != nullptr);

    using InitTlsFn = BOOL(WINAPI*)(const IMAGE_TLS_DIRECTORY64*, HMODULE, void**, uint32_t*, uint32_t*);
    using FreeTlsFn = void(WINAPI*)(const IMAGE_TLS_DIRECTORY64*, HMODULE, void*, uint32_t);
    using RegAtExitFn = int(WINAPI*)(void(__cdecl*)());
    using RunAtExitFn = uint32_t(WINAPI*)();

    auto pInitTls   = reinterpret_cast<InitTlsFn>(GetProcAddress(hookMod, "ScarlettHookInitImplicitTls"));
    auto pFreeTls   = reinterpret_cast<FreeTlsFn>(GetProcAddress(hookMod, "ScarlettHookFreeImplicitTls"));
    auto pRegAtExit = reinterpret_cast<RegAtExitFn>(GetProcAddress(hookMod, "ScarlettHookRegisterGuestAtExit"));
    auto pRunAtExit = reinterpret_cast<RunAtExitFn>(GetProcAddress(hookMod, "ScarlettHookRunGuestAtExit"));
    SC_EXPECT(pInitTls && pFreeTls && pRegAtExit && pRunAtExit);

    uint8_t tlsTemplate[16]{};
    for (uint8_t i = 0; i < 16; ++i) {
        tlsTemplate[i] = static_cast<uint8_t>(0x5Au + i);
    }
    PIMAGE_TLS_CALLBACK liveCbs[3] = { &S63TlsCallbackA, &S63TlsCallbackB, nullptr };

    g_s63TlsSlot = TLS_OUT_OF_INDEXES;
    g_s63AttachCount = 0;
    g_s63DetachCount = 0;
    g_s63SlotMatches = false;

    IMAGE_TLS_DIRECTORY64 liveTls{};
    liveTls.StartAddressOfRawData = reinterpret_cast<ULONGLONG>(tlsTemplate);
    liveTls.EndAddressOfRawData   = reinterpret_cast<ULONGLONG>(tlsTemplate + sizeof(tlsTemplate));
    liveTls.AddressOfIndex        = reinterpret_cast<ULONGLONG>(&g_s63TlsSlot);
    liveTls.AddressOfCallBacks    = reinterpret_cast<ULONGLONG>(liveCbs);
    liveTls.SizeOfZeroFill        = 16;

    void* tlsBlock = nullptr;
    uint32_t tlsSlot = TLS_OUT_OF_INDEXES;
    uint32_t cbsRun = 0;
    SC_EXPECT(pInitTls(&liveTls, GetModuleHandleW(nullptr), &tlsBlock, &tlsSlot, &cbsRun) == TRUE);
    SC_EXPECT(tlsBlock != nullptr);
    SC_EXPECT(tlsSlot != TLS_OUT_OF_INDEXES && g_s63TlsSlot == tlsSlot);
    SC_EXPECT(cbsRun == 2u);
    SC_EXPECT(g_s63AttachCount == 2);
    SC_EXPECT(g_s63SlotMatches);

    const auto* blockBytes = static_cast<const uint8_t*>(tlsBlock);
    for (size_t i = 0; i < 16; ++i) {
        SC_EXPECT(blockBytes[i] == tlsTemplate[i]);
        SC_EXPECT(blockBytes[16 + i] == 0u);
    }

    pFreeTls(&liveTls, GetModuleHandleW(nullptr), tlsBlock, tlsSlot);
    SC_EXPECT(g_s63DetachCount == 2);

    // 3. Isolated guest atexit LIFO ordering & idempotent drain
    g_s63AtExitOrder.clear();
    SC_EXPECT(pRegAtExit(nullptr) != 0);
    SC_EXPECT(pRegAtExit(&S63AtExit1) == 0);
    SC_EXPECT(pRegAtExit(&S63AtExit2) == 0);
    SC_EXPECT(pRegAtExit(&S63AtExit3) == 0);

    SC_EXPECT(pRunAtExit() == 3u);
    SC_EXPECT(g_s63AtExitOrder.size() == 3u);
    SC_EXPECT(g_s63AtExitOrder[0] == 3 && g_s63AtExitOrder[1] == 2 && g_s63AtExitOrder[2] == 1);
    SC_EXPECT(pRunAtExit() == 0u);
    return true;
}

bool TestAutoStubThunkSurvivalE2E() {
    HMODULE hookMod = GetModuleHandleW(L"scarlett_hook.dll");
    HMODULE xgrMod  = GetModuleHandleW(L"xgameruntime.dll");
    SC_EXPECT(hookMod != nullptr && xgrMod != nullptr);

    using GetStatsFn  = void(WINAPI*)(uint32_t*, uint64_t*);
    using SetPolicyFn = void(WINAPI*)(const char*, uint64_t);
    using DumpRepFn   = uint32_t(WINAPI*)(const char*);

    auto pGetStats  = reinterpret_cast<GetStatsFn>(GetProcAddress(hookMod, "ScarlettHookGetAutoStubStats"));
    auto pSetPolicy = reinterpret_cast<SetPolicyFn>(GetProcAddress(hookMod, "ScarlettHookSetAutoStubPolicy"));
    auto pDumpRep   = reinterpret_cast<DumpRepFn>(GetProcAddress(hookMod, "ScarlettHookDumpAutoStubReport"));
    SC_EXPECT(pGetStats && pSetPolicy && pDumpRep);

    uint32_t beforeUnique = 0;
    uint64_t beforeCalls  = 0;
    pGetStats(&beforeUnique, &beforeCalls);

    // 1. Missing named export on a shimmed DLL -> synthesized W^X x64 trampoline returning E_NOTIMPL
    using SixArgFn = uint64_t(WINAPI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    FARPROC pMissing1 = GetProcAddress(xgrMod, "XUserGetTokenAndSignatureAsync_S64Probe");
    FARPROC pMissing2 = GetProcAddress(xgrMod, "XUserGetTokenAndSignatureAsync_S64Probe");
    SC_EXPECT(pMissing1 != nullptr);
    SC_EXPECT(pMissing1 == pMissing2); // Deduplicated trampoline pointer

    auto fnMissing = reinterpret_cast<SixArgFn>(pMissing1);
    SC_EXPECT(fnMissing(1, 2, 3, 4, 5, 6) == 0x80004001ULL);
    SC_EXPECT(fnMissing(7, 8, 9, 10, 11, 12) == 0x80004001ULL);

    // 2. Missing ordinal export -> synthesized trampoline + runtime policy override
    FARPROC pOrd = GetProcAddress(xgrMod, MAKEINTRESOURCEA(9999));
    SC_EXPECT(pOrd != nullptr);
    pSetPolicy("xgameruntime.dll!#9999", 0x1337ULL);
    auto fnOrd = reinterpret_cast<SixArgFn>(pOrd);
    SC_EXPECT(fnOrd(0, 0, 0, 0, 0, 0) == 0x1337ULL);

    // 3. PIX-prefixed missing export -> default telemetry policy returns S_OK (0)
    FARPROC pPix = GetProcAddress(xgrMod, "PIXSyntheticAutoStubS64");
    SC_EXPECT(pPix != nullptr);
    auto fnPix = reinterpret_cast<SixArgFn>(pPix);
    SC_EXPECT(fnPix(0, 0, 0, 0, 0, 0) == 0ULL);

    // 4. Host system DLL miss MUST remain nullptr (never auto-stubbed)
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    SC_EXPECT(k32 != nullptr);
    SC_EXPECT(GetProcAddress(k32, "NonExistentKernel32Symbol_S64Probe") == nullptr);

    // 5. Verify hit-count telemetry & report dump
    uint32_t afterUnique = 0;
    uint64_t afterCalls  = 0;
    pGetStats(&afterUnique, &afterCalls);
    SC_EXPECT(afterUnique >= beforeUnique + 3u);
    SC_EXPECT(afterCalls >= beforeCalls + 4u);

    const auto reportPath = sc::vfs::TranslatePath(L"T:\\sprint64_stub_report.tsv");
    std::filesystem::create_directories(reportPath.parent_path());
    const uint32_t hotCount = pDumpRep(reportPath.string().c_str());
    SC_EXPECT(hotCount >= 3u);
    {
        std::ifstream in(reportPath);
        SC_EXPECT(in.good());
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SC_EXPECT(content.find("xgameruntime.dll!XUserGetTokenAndSignatureAsync_S64Probe") != std::string::npos);
        SC_EXPECT(content.find("xgameruntime.dll!#9999") != std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove(reportPath, ec);

    // 6. Verify SCARLETT_AUTO_STUB=0 disables synthesis cleanly
    SetEnvironmentVariableA("SCARLETT_AUTO_STUB", "0");
    FARPROC pDisabled = GetProcAddress(xgrMod, "DisabledWhenEnvZero_S64Probe");
    SetEnvironmentVariableA("SCARLETT_AUTO_STUB", nullptr);
    SC_EXPECT(pDisabled == nullptr);
    return true;
}

bool TestLoaderSymbolCoverageAuditE2E() {
    // Build a synthetic PE64 importing xmem.dll (1 real export + 1 missing named + 1 missing ordinal)
    // and kernel32.dll (host system DLL).
    std::vector<uint8_t> pe(0x600, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_magic  = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(pe.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = 0x140000000ULL;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR) * 3;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sec->Name, ".idata", 6);
    sec->Misc.VirtualSize = 0x400;
    sec->VirtualAddress   = 0x1000;
    sec->SizeOfRawData    = 0x400;
    sec->PointerToRawData = 0x200;

    auto rvaToPtr = [&](uint32_t rva) -> uint8_t* {
        return pe.data() + 0x200 + (rva - 0x1000);
    };

    // Import descriptors at RVA 0x1000 (2 descriptors + null terminator)
    auto* descs = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(rvaToPtr(0x1000));
    descs[0].OriginalFirstThunk = 0x1080;
    descs[0].Name               = 0x1100; // "xmem.dll"
    descs[0].FirstThunk         = 0x1080;

    descs[1].OriginalFirstThunk = 0x10C0;
    descs[1].Name               = 0x1120; // "kernel32.dll"
    descs[1].FirstThunk         = 0x10C0;

    std::memcpy(rvaToPtr(0x1100), "xmem.dll", 9);
    std::memcpy(rvaToPtr(0x1120), "kernel32.dll", 13);

    // IMAGE_IMPORT_BY_NAME entries (2-byte hint + ASCII name)
    std::memcpy(rvaToPtr(0x1140) + 2, "XMemAllocDefault", 17);
    std::memcpy(rvaToPtr(0x1180) + 2, "XMemFutureUnimplementedApi", 27);
    std::memcpy(rvaToPtr(0x11C0) + 2, "GetCurrentProcessId", 20);

    // Thunk table for xmem.dll at 0x1080: implemented name, missing name, missing ordinal #777, null
    uint64_t xmemThunks[4] = {
        0x1140ULL,
        0x1180ULL,
        IMAGE_ORDINAL_FLAG64 | 777ULL,
        0ULL
    };
    std::memcpy(rvaToPtr(0x1080), xmemThunks, sizeof(xmemThunks));

    // Thunk table for kernel32.dll at 0x10C0
    uint64_t k32Thunks[2] = { 0x11C0ULL, 0ULL };
    std::memcpy(rvaToPtr(0x10C0), k32Thunks, sizeof(k32Thunks));

    const auto probePath = sc::vfs::TranslatePath(L"T:\\sprint64_coverage_probe.exe");
    std::filesystem::create_directories(probePath.parent_path());
    {
        std::ofstream out(probePath, std::ios::binary | std::ios::trunc);
        SC_EXPECT(out.good());
        out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
    }

    wchar_t shimDirBuf[MAX_PATH]{};
    const DWORD shimLen = GetEnvironmentVariableW(L"SCARLETT_SHIM_DIR", shimDirBuf, MAX_PATH);
    SC_EXPECT(shimLen > 0 && shimLen < MAX_PATH);
    const std::filesystem::path shimDir(shimDirBuf, shimDirBuf + shimLen);

    const auto cov = sc::loader::InspectPeSymbolCoverage(probePath, shimDir);
    std::error_code ec;
    std::filesystem::remove(probePath, ec);

    SC_EXPECT(cov.audit.validPe64);
    SC_EXPECT(cov.audit.totalImportedSymbols == 4u);
    SC_EXPECT(cov.audit.ordinalImportCount == 1u);
    SC_EXPECT(cov.implementedSymbols.size() == 1u);
    SC_EXPECT(cov.implementedSymbols[0] == "xmem.dll!XMemAllocDefault");
    SC_EXPECT(cov.autoStubSymbols.size() == 2u);
    SC_EXPECT(std::find(cov.autoStubSymbols.begin(), cov.autoStubSymbols.end(),
                        "xmem.dll!XMemFutureUnimplementedApi") != cov.autoStubSymbols.end());
    SC_EXPECT(std::find(cov.autoStubSymbols.begin(), cov.autoStubSymbols.end(),
                        "xmem.dll!#777") != cov.autoStubSymbols.end());
    return true;
}

bool TestXvdFlagsAndBoundsNegative() {
    const auto tmpDir = std::filesystem::temp_directory_path() / L"scarlett_s66_xvd_tests";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    auto writeImg = [&](const wchar_t* name, const std::vector<uint8_t>& bytes) {
        const auto p = tmpDir / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return p;
    };
    auto writeU32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFFu);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto writeU64 = [&](uint8_t* p, uint64_t v) {
        writeU32(p, static_cast<uint32_t>(v & 0xFFFFFFFFull));
        writeU32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
    };

    const std::vector<uint8_t> payload = { 'X', 'V', 'D', '_', 'S', '6', '6' };
    sc::vfs::XvdContainer xvd;
    std::string err;

    // 1. Flag matrix per emoose/xvdtool (EncryptionDisabled = 0x2, ReadOnly = 0x1)
    {
        auto p0 = writeImg(L"flags_0x0.xvd", sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, 0x0u));
        SC_EXPECT(!xvd.Open(p0, err)); // Encrypted -> must reject

        auto p1 = writeImg(L"flags_0x1.xvd", sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagReadOnly));
        SC_EXPECT(!xvd.Open(p1, err)); // ReadOnly + Encrypted -> must reject

        auto p2 = writeImg(L"flags_0x2.xvd", sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, sc::vfs::kXvdFlagEncryptionDisabled));
        SC_EXPECT(xvd.Open(p2, err));
        SC_EXPECT(!xvd.Header().IsEncrypted());

        auto p3 = writeImg(L"flags_0x3.xvd", sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(
            payload, 1u, sc::vfs::kXvdFlagReadOnly | sc::vfs::kXvdFlagEncryptionDisabled));
        SC_EXPECT(xvd.Open(p3, err));
        SC_EXPECT(!xvd.Header().IsEncrypted());
    }

    // 2. Truncated image smaller than real 12 KiB (0x3000) XVD header
    {
        std::vector<uint8_t> trunc(0x1000, 0u);
        std::memcpy(trunc.data() + sc::vfs::kXvdMagicOffset, "msft-xvd", 8);
        writeU32(trunc.data() + 0x208, sc::vfs::kXvdFlagEncryptionDisabled);
        auto p = writeImg(L"truncated_4k.xvd", trunc);
        SC_EXPECT(!xvd.Open(p, err));
    }

    // 3. Out-of-bounds real uint32 UserDataLength (0x28C) and MutableDataPageCount (0x470)
    {
        auto imgLen = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload);
        writeU32(imgLen.data() + 0x28C, 0xFFFFFFFFu);
        auto pLen = writeImg(L"oob_userdata_len.xvd", imgLen);
        SC_EXPECT(!xvd.Open(pLen, err));

        auto imgMdu = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload);
        imgMdu[0x470] = 0xFFu;
        auto pMdu = writeImg(L"oob_mdu.xvd", imgMdu);
        SC_EXPECT(!xvd.Open(pMdu, err));
    }

    // 4. Out-of-bounds driveSize (0x218) and real EmbeddedXVDLength (0x288)
    {
        auto imgDrive = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload);
        writeU64(imgDrive.data() + 0x218, UINT64_MAX);
        auto pDrive = writeImg(L"oob_drivesize.xvd", imgDrive);
        SC_EXPECT(!xvd.Open(pDrive, err));

        auto imgEmb = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload);
        writeU32(imgEmb.data() + 0x288, 0xFFFFFFFFu);
        auto pEmb = writeImg(L"oob_embedded.xvd", imgEmb);
        SC_EXPECT(!xvd.Open(pEmb, err));
    }

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

bool TestNonAsciiShimAndFreeLibraryHook() {
    // Non-CP1252 folder suffix using ASCII \u escapes: U+30C6 U+30B9 U+30C8 (Katakana) + U+03B2 (beta)
    const std::wstring kNonAsciiFolder = L"scarlett_s66_\u30C6\u30B9\u30C8_\u03B2_shims";

    // 1. Lossless codepage helper must reject unrepresentable characters instead of corrupting them
    {
        std::string outAnsi;
        SC_EXPECT(sc::path::WideToCodePageLossless(1252u, L"C:\\shims\\pixevt.dll", outAnsi));
        SC_EXPECT(outAnsi == "C:\\shims\\pixevt.dll");

        const std::wstring sampleBad = L"C:\\" + kNonAsciiFolder + L"\\pixevt.dll";
        SC_EXPECT(!sc::path::WideToCodePageLossless(1252u, sampleBad, outAnsi));
        SC_EXPECT(outAnsi.empty());

        if (GetACP() != CP_UTF8) {
            const std::wstring missingBad = L"Z:\\nonexistent_" + kNonAsciiFolder + L"\\pixevt.dll";
            SC_EXPECT(!sc::path::WidePathToAnsiLossless(missingBad, outAnsi));
            SC_EXPECT(outAnsi.empty());
        }
    }

    HMODULE hookMod = GetModuleHandleW(L"scarlett_hook.dll");
    SC_EXPECT(hookMod != nullptr);

    using PFN_RegisterShim   = BOOL(WINAPI*)(const wchar_t*, const wchar_t*);
    using PFN_IsTracked      = BOOL(WINAPI*)(HMODULE);
    using PFN_TrackAndPatch  = uint32_t(WINAPI*)(HMODULE);
    using PFN_Untrack        = void(WINAPI*)(HMODULE);

    auto pRegisterShim  = reinterpret_cast<PFN_RegisterShim>(GetProcAddress(hookMod, "ScarlettHookRegisterTestShim"));
    auto pIsTracked     = reinterpret_cast<PFN_IsTracked>(GetProcAddress(hookMod, "ScarlettHookIsModuleTracked"));
    auto pTrackAndPatch = reinterpret_cast<PFN_TrackAndPatch>(GetProcAddress(hookMod, "ScarlettHookTrackAndPatchForTest"));
    auto pUntrack       = reinterpret_cast<PFN_Untrack>(GetProcAddress(hookMod, "ScarlettHookUntrackModuleForTest"));
    SC_EXPECT(pRegisterShim && pIsTracked && pTrackAndPatch && pUntrack);

    // 2. Non-ASCII shim directory (characters outside CP1252) loaded via LoadLibraryA / GetModuleHandleA
    {
        auto shimDirOpt = sc::config::GetShimDir();
        SC_EXPECT(shimDirOpt.has_value());
        const auto srcPix = *shimDirOpt / L"pixevt.dll";
        SC_EXPECT(std::filesystem::exists(srcPix));

        const auto nonAsciiDir = std::filesystem::temp_directory_path() / kNonAsciiFolder;
        std::error_code ec;
        std::filesystem::remove_all(nonAsciiDir, ec);
        std::filesystem::create_directories(nonAsciiDir, ec);
        const auto dstPix = nonAsciiDir / L"pixevt.dll";
        std::filesystem::copy_file(srcPix, dstPix, std::filesystem::copy_options::overwrite_existing, ec);
        SC_EXPECT(!ec && std::filesystem::exists(dstPix));

        SC_EXPECT(pRegisterShim(L"s66_nonascii_probe.dll", dstPix.c_str()) == TRUE);
        HMODULE hModA = LoadLibraryA("s66_nonascii_probe.dll");
        SC_EXPECT(hModA != nullptr);

        HMODULE hQueryA = GetModuleHandleA("s66_nonascii_probe.dll");
        HMODULE hQueryExA = nullptr;
        SC_EXPECT(hQueryA == hModA);
        SC_EXPECT(GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "s66_nonascii_probe.dll", &hQueryExA) != FALSE);
        SC_EXPECT(hQueryExA == hModA);

        wchar_t loadedPath[1024]{};
        const DWORD len = GetModuleFileNameW(hModA, loadedPath, 1024);
        SC_EXPECT(len > 0);
        SC_EXPECT(std::wstring(loadedPath, len).find(kNonAsciiFolder) != std::wstring::npos);
        SC_EXPECT(GetProcAddress(hModA, "PIXBeginEvent") != nullptr);

        FreeLibrary(hModA);
        pRegisterShim(L"s66_nonascii_probe.dll", nullptr);
        std::filesystem::remove_all(nonAsciiDir, ec);
    }

    // 3. FreeLibrary hook untracks unloaded DLL and re-tracks + re-patches on reload
    {
        HMODULE hPlugin1 = LoadLibraryW(L"..\\dummy_title\\engine_plugin.dll");
        SC_EXPECT(hPlugin1 != nullptr);
        SC_EXPECT(pIsTracked(hPlugin1) == TRUE);

        using PluginFn = bool(WINAPI*)();
        auto runDiag1 = reinterpret_cast<PluginFn>(GetProcAddress(hPlugin1, "PluginRunDiagnostics"));
        SC_EXPECT(runDiag1 != nullptr && runDiag1());

        SC_EXPECT(FreeLibrary(hPlugin1) != FALSE);
        SC_EXPECT(pIsTracked(hPlugin1) == FALSE);

        HMODULE hPlugin2 = LoadLibraryW(L"..\\dummy_title\\engine_plugin.dll");
        SC_EXPECT(hPlugin2 != nullptr);
        SC_EXPECT(pIsTracked(hPlugin2) == TRUE);

        auto runDiag2 = reinterpret_cast<PluginFn>(GetProcAddress(hPlugin2, "PluginRunDiagnostics"));
        SC_EXPECT(runDiag2 != nullptr && runDiag2());

        SC_EXPECT(FreeLibrary(hPlugin2) != FALSE);
        SC_EXPECT(pIsTracked(hPlugin2) == FALSE);
    }

    // 4. Stale handle reuse at identical base address with new PE identity (TimeDateStamp / SizeOfImage)
    {
        uint8_t* img = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        SC_EXPECT(img != nullptr);

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img);
        dos->e_magic  = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.TimeDateStamp = 0x11111111u;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = 0x2000u;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x1000;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR) * 2;

        std::strcpy(reinterpret_cast<char*>(img + 0x1300), "kernel32.dll");
        auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(img + 0x1320);
        std::strcpy(ibn->Name, "FreeLibrary");

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(img + 0x1000);
        desc[0].OriginalFirstThunk = 0x1100;
        desc[0].Name               = 0x1300;
        desc[0].FirstThunk         = 0x1200;

        auto* intThunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(img + 0x1100);
        intThunks[0].u1.AddressOfData = 0x1320;
        intThunks[1].u1.AddressOfData = 0;

        auto* iatThunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(img + 0x1200);
        iatThunks[0].u1.Function = 0xDEAD0001ULL;
        iatThunks[1].u1.Function = 0;

        HMODULE fakeMod = reinterpret_cast<HMODULE>(img);
        SC_EXPECT(pTrackAndPatch(fakeMod) == 1u);
        SC_EXPECT(iatThunks[0].u1.Function != 0xDEAD0001ULL);

        // Same base + same (TimeDateStamp, SizeOfImage) -> already seen, no re-patch
        SC_EXPECT(pTrackAndPatch(fakeMod) == 0u);

        // Simulate unhooked unload + reload of different module at the exact same base address
        iatThunks[0].u1.Function = 0xDEAD0002ULL;
        nt->FileHeader.TimeDateStamp = 0x22222222u;
        SC_EXPECT(pTrackAndPatch(fakeMod) == 1u);
        SC_EXPECT(iatThunks[0].u1.Function != 0xDEAD0002ULL);

        pUntrack(fakeMod);
        VirtualFree(img, 0, MEM_RELEASE);
    }

    return true;
}

bool TestWarpAdapterRenderReadback() {
    using Microsoft::WRL::ComPtr;

    char prevWarp[8]{};
    const DWORD prevLen = GetEnvironmentVariableA("SCARLETT_USE_WARP", prevWarp, static_cast<DWORD>(sizeof(prevWarp)));
    struct RestoreWarp {
        bool hadPrev;
        std::string val;
        ~RestoreWarp() {
            SetEnvironmentVariableA("SCARLETT_USE_WARP", hadPrev ? val.c_str() : nullptr);
        }
    } guard{ prevLen > 0, std::string(prevWarp) };

    SC_EXPECT(SetEnvironmentVariableA("SCARLETT_USE_WARP", "1") != FALSE);
    SC_EXPECT(sc::config::UseWarp());

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12DeviceX> devX;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(devX.GetAddressOf()))));

    ComPtr<ID3D12Device> nativeDev;
    SC_EXPECT(SUCCEEDED(devX->QueryInterface(
        IID_ScarlettNativeD3D12Device, reinterpret_cast<void**>(nativeDev.GetAddressOf()))));

    // Confirm the selected adapter is genuinely Microsoft WARP (VendorId 0x1414, SOFTWARE flag)
    const LUID luid = nativeDev->GetAdapterLuid();
    HMODULE hDxgi = LoadLibraryW(L"dxgi.dll");
    SC_EXPECT(hDxgi != nullptr);
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto pCreateFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(hDxgi, "CreateDXGIFactory1"));
    SC_EXPECT(pCreateFactory1 != nullptr);

    ComPtr<IDXGIFactory4> factory4;
    SC_EXPECT(SUCCEEDED(pCreateFactory1(IID_PPV_ARGS(&factory4))));
    ComPtr<IDXGIAdapter1> adapter;
    SC_EXPECT(SUCCEEDED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))));
    DXGI_ADAPTER_DESC1 desc{};
    SC_EXPECT(SUCCEEDED(adapter->GetDesc1(&desc)));
    SC_EXPECT(desc.VendorId == 0x1414u);
    SC_EXPECT((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u);
    FreeLibrary(hDxgi);

    // Execute a real 4x4 render-target clear + readback on WARP through d3d12_xs
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queueX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandQueueX(&qd, queueX.GetAddressOf())));

    ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    ComPtr<ID3D12GraphicsCommandListX> clX;
    SC_EXPECT(SUCCEEDED(devX->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
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

    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearVal.Color[0] = 12.0f / 255.0f;
    clearVal.Color[1] = 34.0f / 255.0f;
    clearVal.Color[2] = 56.0f / 255.0f;
    clearVal.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> rtTex;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&rtTex))));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    nativeDev->CreateRenderTargetView(rtTex.Get(), nullptr, rtvH);

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = 256 * 4; rbDesc.Height = 1; rbDesc.DepthOrArraySize = 1; rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1; rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rbBuf;
    SC_EXPECT(SUCCEEDED(devX->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
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
    const bool pixelOk = (pRb[0] == 12 && pRb[1] == 34 && pRb[2] == 56 && pRb[3] == 255);
    rbBuf->Unmap(0, nullptr);
    SC_EXPECT(pixelOk);
    return true;
}

bool TestLocalCorpusReadOnlyAudit() {
    // 1. Always verify synthetic encrypted XVC rejection (flags=0x41: ReadOnly | RegionIdInXts)
    const auto tmpDir = std::filesystem::temp_directory_path() / L"scarlett_s67_corpus_probe";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::vector<uint8_t> payload = { 'S', '6', '7' };
    const auto retailStyleFlags = sc::vfs::kXvdFlagReadOnly | sc::vfs::kXvdFlagRegionIdInXts; // 0x41
    const auto encImg = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(payload, 1u, retailStyleFlags);
    const auto encPath = tmpDir / L"retail_flags_0x41.xvc";
    {
        std::ofstream ofs(encPath, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(encImg.data()), static_cast<std::streamsize>(encImg.size()));
    }
    {
        sc::vfs::XvdContainer xvd;
        std::string err;
        SC_EXPECT(!xvd.Open(encPath, err));
        SC_EXPECT(err.find("Encrypted") != std::string::npos);
    }
    std::filesystem::remove_all(tmpDir, ec);

    // 2. Opt-in local encrypted XVC negative test (never committed; skipped when env var unset)
    if (auto localXvc = sc::config::GetEnvPath(L"SCARLETT_LOCAL_XVC_PATH")) {
        if (std::filesystem::exists(*localXvc, ec)) {
            sc::vfs::XvdContainer xvd;
            std::string err;
            SC_EXPECT(!xvd.Open(*localXvc, err));
            SC_EXPECT(err.find("Encrypted") != std::string::npos);
        }
    }

    // 3. Opt-in local unpacked corpus directory read-only audit (skipped when env var unset)
    if (auto corpusDir = sc::config::GetEnvPath(L"SCARLETT_LOCAL_CORPUS_DIR")) {
        if (std::filesystem::exists(*corpusDir, ec) && std::filesystem::is_directory(*corpusDir, ec)) {
            auto shimDir = sc::config::GetShimDir().value_or(std::filesystem::current_path());
            std::filesystem::recursive_directory_iterator it(
                *corpusDir, std::filesystem::directory_options::skip_permission_denied, ec);
            std::filesystem::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = sc::loader::detail::ToLowerAscii(
                    sc::path::WideToUtf8(it->path().extension().wstring()));
                if (ext == ".xvd" || ext == ".xvc") {
                    sc::vfs::XvdContainer xvd;
                    std::string err;
                    if (!xvd.Open(it->path(), err)) {
                        SC_EXPECT(!err.empty());
                    }
                } else if (ext == ".exe") {
                    const auto cov = sc::loader::InspectPeSymbolCoverage(it->path(), shimDir);
                    SC_EXPECT(cov.audit.validPe64 || !cov.audit.error.empty());
                }
            }
        }
    }
    return true;
}

bool TestScarlettStudioSelfTest() {
    wchar_t exeBuf[MAX_PATH * 4]{};
    SC_EXPECT(GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf))) > 0);
    // scarlett_tests.exe lives at build/bin/titles/scarlett_tests/scarlett_tests.exe
    const std::filesystem::path binDir = std::filesystem::path(exeBuf).parent_path().parent_path().parent_path();
    const std::filesystem::path studioExe = binDir / L"scarlett_studio.exe";
    SC_EXPECT(std::filesystem::exists(studioExe));

    std::wstring cmd = L"\"" + studioExe.wstring() + L"\" --self-test";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    SC_EXPECT(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, binDir.c_str(), &si, &pi) != FALSE);
    SC_EXPECT(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0);
    DWORD exitCode = 999;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SC_EXPECT(exitCode == 0);
    return true;
}

bool TestXvdRealOffsetsAndGeometry() {
    const auto tmpDir = std::filesystem::temp_directory_path() / L"scarlett_s70_xvd_geometry";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    auto writeImg = [&](const wchar_t* name, const std::vector<uint8_t>& bytes) {
        const auto p = tmpDir / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return p;
    };

    const std::vector<uint8_t> payload = { 'S', 'C', 'A', 'R', 'L', 'E', 'T', 'T', '_', '7', '0' };
    sc::vfs::XvdContainer xvd;
    std::string err;

    // 1. Real offsets (0x284/0x288/0x28C/0x470) + untouched TopHashBlockHash (0x240..0x25F)
    {
        auto img = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(
            payload, 7u, sc::vfs::kXvdFlagEncryptionDisabled, 0x1500u, 2u);
        // Confirm legacy synthetic offsets 0x240..0x25F are zero and corrupting them has no effect
        for (size_t off = 0x240; off < 0x260; ++off) {
            SC_EXPECT(img[off] == 0u);
            img[off] = 0xABu;
        }
        const auto p = writeImg(L"real_layout.xvd", img);
        SC_EXPECT(xvd.Open(p, err));
        SC_EXPECT(xvd.Header().xvdType == 0u);
        SC_EXPECT(xvd.Header().contentType == 7u);
        SC_EXPECT(xvd.Header().embeddedXvdLen == 0x1500u);
        SC_EXPECT(xvd.Header().mutableDataPageCount == 2u);
        // EmbeddedXVD (0x1500 -> 2 pages = 0x2000) + MDU (2 pages = 0x2000) => hashTreeOffset = 0x7000
        SC_EXPECT(xvd.Header().hashTreeOffset == 0x7000u);
        SC_EXPECT(xvd.Header().hashTreeLength == 0x1000u);
        SC_EXPECT(xvd.Header().userDataOffset == 0x8000u);
        SC_EXPECT(xvd.Header().userDataLength == payload.size());

        std::vector<uint8_t> out;
        SC_EXPECT(xvd.ReadUserData(0, payload.size(), out, err));
        SC_EXPECT(out == payload);
    }

    // 2. DataIntegrityDisabled (0x4) omits hash tree completely
    {
        auto img = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(
            payload, 1u,
            sc::vfs::kXvdFlagEncryptionDisabled | sc::vfs::kXvdFlagDataIntegrityDisabled,
            0x0800u, 1u);
        const auto p = writeImg(L"no_hash_tree.xvd", img);
        SC_EXPECT(xvd.Open(p, err));
        SC_EXPECT(!xvd.Header().IsDataIntegrityEnabled());
        SC_EXPECT(xvd.Header().hashTreeOffset == 0x5000u);
        SC_EXPECT(xvd.Header().hashTreeLength == 0u);
        SC_EXPECT(xvd.Header().userDataOffset == 0x5000u);
    }

    // 3. ResiliencyEnabled (0x10) doubles hash-tree pages
    {
        auto img = sc::vfs::XvdContainer::BuildSyntheticUnencryptedXvd(
            payload, 1u,
            sc::vfs::kXvdFlagEncryptionDisabled | sc::vfs::kXvdFlagResiliencyEnabled,
            0u, 0u);
        const auto p = writeImg(L"resilient_hash_tree.xvd", img);
        SC_EXPECT(xvd.Open(p, err));
        SC_EXPECT(xvd.Header().hashTreeOffset == 0x3000u);
        SC_EXPECT(xvd.Header().hashTreeLength == 0x2000u);
        SC_EXPECT(xvd.Header().userDataOffset == 0x5000u);

        std::vector<uint8_t> out;
        SC_EXPECT(xvd.ReadUserData(0, payload.size(), out, err));
        SC_EXPECT(out == payload);
    }

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

bool TestMalformedPeAndConfigHardening() {
    const auto tmpDir = std::filesystem::temp_directory_path() / L"scarlett_s70_parser_hardening";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    auto writeText = [&](const wchar_t* name, std::string_view text) {
        const auto p = tmpDir / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    };
    auto writeBin = [&](const wchar_t* name, const std::vector<uint8_t>& bytes) {
        const auto p = tmpDir / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return p;
    };

    // 1. MicrosoftGame.Config path containment checks
    const char* badExes[] = {
        "..\\escape.exe",
        "sub\\..\\..\\escape.exe",
        "/rooted.exe",
        "\\rooted.exe",
        "C:\\Windows\\System32\\cmd.exe",
        "game.exe:ads",
        "bad\x01name.exe"
    };
    int idx = 0;
    for (const char* badExe : badExes) {
        std::string xml = "<Game><Identity Name=\"Bad\"/><ExecutableList><Executable Name=\"";
        xml += badExe;
        xml += "\" Id=\"Game\"/></ExecutableList></Game>";
        const auto cfgPath = writeText((L"bad_cfg_" + std::to_wstring(idx++) + L".xml").c_str(), xml);
        const auto parsed = sc::loader::ParseGameConfig(cfgPath);
        SC_EXPECT(!parsed.valid);
    }

    // Valid nested relative executable stays inside packageRoot
    {
        const auto okPath = writeText(
            L"good_cfg.xml",
            "<Game><Identity Name=\"Good\"/><ExecutableList><Executable Name=\"Bin\\Title.exe\" Id=\"Game\"/></ExecutableList></Game>");
        const auto parsed = sc::loader::ParseGameConfig(okPath);
        SC_EXPECT(parsed.valid);
        SC_EXPECT(parsed.executablePath.filename() == L"Title.exe");
    }

    // 2. Malformed PE64 inputs must fail safely with no crash or OOB read
    const auto emptyShimDir = tmpDir / L"empty_shims";
    std::filesystem::create_directories(emptyShimDir, ec);

    // 2a. Truncated non-PE
    {
        const auto p = writeBin(L"trunc.exe", {'M', 'Z', 0x00, 0x01});
        SC_EXPECT(!sc::loader::InspectPeImports(p, emptyShimDir).validPe64);
        const auto sym = sc::loader::InspectPeSymbolCoverage(p, emptyShimDir);
        SC_EXPECT(!sym.audit.validPe64 && sym.implementedSymbols.empty() && sym.autoStubSymbols.empty());
        const auto exps = sc::loader::detail::ParsePeExports(p);
        SC_EXPECT(exps.names.empty());
    }

    // 2b. Negative e_lfanew
    {
        std::vector<uint8_t> buf(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64), 0u);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = -64;
        const auto p = writeBin(L"neg_lfanew.exe", buf);
        SC_EXPECT(!sc::loader::InspectPeImports(p, emptyShimDir).validPe64);
        SC_EXPECT(!sc::loader::InspectPeSymbolCoverage(p, emptyShimDir).audit.validPe64);
    }

    // 2c. NumberOfSections claiming 5000 sections inside tiny file
    {
        std::vector<uint8_t> buf(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64) + 64u, 0u);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = static_cast<LONG>(sizeof(IMAGE_DOS_HEADER));
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(buf.data() + sizeof(IMAGE_DOS_HEADER));
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 5000u;
        nt->FileHeader.SizeOfOptionalHeader = static_cast<WORD>(sizeof(IMAGE_OPTIONAL_HEADER64));
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        const auto p = writeBin(L"huge_sections.exe", buf);
        SC_EXPECT(!sc::loader::InspectPeImports(p, emptyShimDir).validPe64);
        SC_EXPECT(sc::loader::detail::ParsePeExports(p).names.empty());
    }

    // 2d. Oversized SizeOfOptionalHeader exceeding file
    {
        std::vector<uint8_t> buf(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64) + 64u, 0u);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = static_cast<LONG>(sizeof(IMAGE_DOS_HEADER));
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(buf.data() + sizeof(IMAGE_DOS_HEADER));
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 1u;
        nt->FileHeader.SizeOfOptionalHeader = 0xFF00u;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        const auto p = writeBin(L"huge_opthdr.exe", buf);
        SC_EXPECT(!sc::loader::InspectPeImports(p, emptyShimDir).validPe64);
    }

    // 2e. Valid headers + out-of-bounds Import Directory RVA
    {
        std::vector<uint8_t> buf(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64) + sizeof(IMAGE_SECTION_HEADER), 0u);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = static_cast<LONG>(sizeof(IMAGE_DOS_HEADER));
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(buf.data() + sizeof(IMAGE_DOS_HEADER));
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 1u;
        nt->FileHeader.SizeOfOptionalHeader = static_cast<WORD>(sizeof(IMAGE_OPTIONAL_HEADER64));
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x7FFF0000u;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 0x40u;
        auto* sec = reinterpret_cast<IMAGE_SECTION_HEADER*>(
            buf.data() + sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64));
        sec->VirtualAddress = 0x1000u;
        sec->SizeOfRawData = 0x200u;
        sec->PointerToRawData = 0x400u;
        const auto p = writeBin(L"oob_import_rva.exe", buf);
        SC_EXPECT(!sc::loader::InspectPeImports(p, emptyShimDir).validPe64);
    }

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

