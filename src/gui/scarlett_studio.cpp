#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "studio_model.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_STUDIO_LOG_APPEND = WM_APP + 1;
constexpr UINT WM_STUDIO_PROC_DONE  = WM_APP + 2;
constexpr UINT_PTR kAnimTimerId     = 1;

// DWM attributes for Windows 11 Acrylic & Dark Mode (safe no-ops on older builds)
constexpr DWORD kDwmwaUseImmersiveDarkMode   = 20;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaCaptionColor           = 35;
constexpr DWORD kDwmwaSystemBackdropType     = 38;
constexpr DWORD kDwmwcpRound                 = 2;
constexpr DWORD kDwmSbtTransientWindow       = 3; // Acrylic

enum class ActionId {
    None = 0,
    ScanFolder,
    AddFile,
    Refresh,
    Launch,
    Stop,
    Inspect,
    TestGpu,
    TestWarp,
    CopyLog,
    ClearLog
};

enum class ToggleId {
    Warp = 0,
    Headless,
    StrictD3d,
    AutoStub,
    Hotlist,
    Count
};

struct ButtonWidget {
    ActionId     id{ActionId::None};
    std::wstring label;
    D2D1_RECT_F  rect{};
    bool         primary{false};
    bool         danger{false};
    bool         enabled{true};
    float        hoverAnim{0.0f};
};

struct ToggleWidget {
    ToggleId     id{ToggleId::Warp};
    std::wstring label;
    D2D1_RECT_F  rect{};
    bool         checked{false};
    float        anim{0.0f};
};

struct LogLine {
    std::wstring text;
    D2D1_COLOR_F color;
};

struct LiquidStudioApp {
    HWND hwnd{nullptr};
    float dpiScale{1.0f};
    float widthDip{1220.0f};
    float heightDip{760.0f};

    // Graphics stack (D3D11 + DXGI Composition SwapChain + D2D1.1 + DWrite + DComp)
    ComPtr<ID3D11Device>        d3dDevice;
    ComPtr<IDXGIDevice>         dxgiDevice;
    ComPtr<ID2D1Factory1>       d2dFactory;
    ComPtr<ID2D1Device>         d2dDevice;
    ComPtr<ID2D1DeviceContext>  d2dCtx;
    ComPtr<IDXGISwapChain1>     swapChain;
    ComPtr<ID2D1Bitmap1>        targetBitmap;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    ComPtr<IDWriteFactory>      dwriteFactory;

    // Typography
    ComPtr<IDWriteTextFormat> fmtBrand;
    ComPtr<IDWriteTextFormat> fmtSection;
    ComPtr<IDWriteTextFormat> fmtBody;
    ComPtr<IDWriteTextFormat> fmtBodyBold;
    ComPtr<IDWriteTextFormat> fmtSmall;
    ComPtr<IDWriteTextFormat> fmtMono;

    // Paths & model
    std::filesystem::path binDir;
    std::filesystem::path loaderExe;
    std::filesystem::path shimsDir;
    std::filesystem::path configFile;
    std::vector<std::filesystem::path> libraryRoots;
    std::vector<sc::gui::LibraryEntry> entries;
    int selectedIndex{-1};
    int hoveredRow{-1};

    // UI widgets & layout rects
    std::vector<ButtonWidget> buttons;
    std::vector<ToggleWidget> toggles;
    ActionId hoveredButton{ActionId::None};
    ActionId pressedButton{ActionId::None};
    int      hoveredToggle{-1};

    D2D1_RECT_F rcTopBar{};
    D2D1_RECT_F rcToggleBar{};
    D2D1_RECT_F rcLibraryCard{};
    D2D1_RECT_F rcLibraryList{};
    D2D1_RECT_F rcInspectorCard{};
    D2D1_RECT_F rcInspectorBody{};
    D2D1_RECT_F rcConsoleCard{};
    D2D1_RECT_F rcConsoleBody{};

    float libraryScroll{0.0f};
    float inspectorScroll{0.0f};
    float consoleScroll{0.0f};
    bool  consoleAutoScroll{true};

    std::vector<LogLine> logLines;
    std::wstring         statusText{L"Ready"};
    float                pulsePhase{0.0f};

    // Child process runner
    std::mutex        procMtx;
    HANDLE            activeJob{nullptr};
    HANDLE            activeProc{nullptr};
    std::atomic<bool> procRunning{false};
    std::thread       workerThread;
};

LiquidStudioApp g_app;

std::filesystem::path GetExeDir() {
    wchar_t buf[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
}

bool PtInRectF(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

D2D1_COLOR_F ClassifyLogColor(const std::string& line) {
    if (line.find("[ERR") != std::string::npos || line.find("FAIL") != std::string::npos ||
        line.find("ERROR") != std::string::npos) {
        return D2D1::ColorF(1.0f, 0.45f, 0.48f, 0.98f); // Soft coral red
    }
    if (line.find("[WARN") != std::string::npos || line.find("[STUB") != std::string::npos ||
        line.find("auto_stub") != std::string::npos) {
        return D2D1::ColorF(1.0f, 0.78f, 0.38f, 0.98f); // Warm amber
    }
    if (line.find("[studio]") != std::string::npos || line.find("100% tests passed") != std::string::npos ||
        line.find("0 failed") != std::string::npos) {
        return D2D1::ColorF(0.32f, 0.95f, 0.64f, 0.98f); // Emerald mint
    }
    if (line.find("===") != std::string::npos) {
        return D2D1::ColorF(0.95f, 0.98f, 1.0f, 1.0f);   // Crisp white
    }
    if (line.find("[INFO]") != std::string::npos) {
        return D2D1::ColorF(0.55f, 0.85f, 1.0f, 0.94f);  // Ice cyan
    }
    return D2D1::ColorF(0.82f, 0.87f, 0.93f, 0.90f);
}

void AppendLogUi(const std::string& chunk) {
    size_t start = 0;
    while (start < chunk.size()) {
        size_t nl = chunk.find('\n', start);
        std::string line = (nl == std::string::npos) ? chunk.substr(start) : chunk.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() || nl != std::string::npos) {
            g_app.logLines.push_back(LogLine{ sc::gui::Utf8ToWide(line), ClassifyLogColor(line) });
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (g_app.logLines.size() > 4000) {
        g_app.logLines.erase(g_app.logLines.begin(), g_app.logLines.begin() + 1000);
    }
    if (g_app.consoleAutoScroll) {
        const float lineH = 18.0f;
        const float viewH = std::max(1.0f, g_app.rcConsoleBody.bottom - g_app.rcConsoleBody.top);
        const float totalH = static_cast<float>(g_app.logLines.size()) * lineH;
        g_app.consoleScroll = std::max(0.0f, totalH - viewH);
    }
}

void PostLogAsync(const std::string& msg) {
    if (!g_app.hwnd) return;
    auto* heapStr = new std::string(msg);
    if (!PostMessageW(g_app.hwnd, WM_STUDIO_LOG_APPEND, 0, reinterpret_cast<LPARAM>(heapStr))) {
        delete heapStr;
    }
}

void UpdateButtonStates() {
    const bool running = g_app.procRunning.load(std::memory_order_acquire);
    const bool hasSel = (g_app.selectedIndex >= 0 && static_cast<size_t>(g_app.selectedIndex) < g_app.entries.size());
    const bool canLaunch = hasSel && g_app.entries[static_cast<size_t>(g_app.selectedIndex)].launchable;

    for (auto& b : g_app.buttons) {
        switch (b.id) {
        case ActionId::Launch:   b.enabled = !running && canLaunch; break;
        case ActionId::Stop:     b.enabled = running; break;
        case ActionId::Inspect:  b.enabled = !running && hasSel; break;
        case ActionId::TestGpu:
        case ActionId::TestWarp: b.enabled = !running; break;
        default:                 b.enabled = true; break;
        }
    }
}

void RefreshLibrary() {
    g_app.entries = sc::gui::ScanLibraryRoots(g_app.libraryRoots, g_app.shimsDir);
    if (g_app.entries.empty()) {
        g_app.selectedIndex = -1;
    } else if (g_app.selectedIndex < 0 || static_cast<size_t>(g_app.selectedIndex) >= g_app.entries.size()) {
        g_app.selectedIndex = 0;
    }
    g_app.libraryScroll = 0.0f;
    g_app.inspectorScroll = 0.0f;
    UpdateButtonStates();

    wchar_t buf[256];
    std::swprintf(buf, std::size(buf), L"%zu title(s) indexed  |  %zu root(s)  |  Shims armed",
                  g_app.entries.size(), g_app.libraryRoots.size());
    g_app.statusText = buf;
}

void StopActiveProcess() {
    std::lock_guard<std::mutex> lock(g_app.procMtx);
    if (g_app.activeJob) {
        TerminateJobObject(g_app.activeJob, 1);
    } else if (g_app.activeProc) {
        TerminateProcess(g_app.activeProc, 1);
    }
}

void LaunchLoaderCommand(
    const std::wstring& args,
    bool forceWarp,
    bool forceHeadless,
    const std::wstring& banner) {
    if (g_app.procRunning.load(std::memory_order_acquire)) return;
    if (g_app.workerThread.joinable()) g_app.workerThread.join();

    const bool useWarp    = forceWarp     || g_app.toggles[static_cast<size_t>(ToggleId::Warp)].checked;
    const bool headless   = forceHeadless || g_app.toggles[static_cast<size_t>(ToggleId::Headless)].checked;
    const bool strictD3d  = g_app.toggles[static_cast<size_t>(ToggleId::StrictD3d)].checked;
    const bool autoStub   = g_app.toggles[static_cast<size_t>(ToggleId::AutoStub)].checked;
    const bool stubReport = g_app.toggles[static_cast<size_t>(ToggleId::Hotlist)].checked;

    const std::wstring cmdLine = L"\"" + g_app.loaderExe.wstring() + L"\" " + args;
    g_app.procRunning.store(true, std::memory_order_release);
    g_app.consoleAutoScroll = true;
    g_app.statusText = L"Running: " + banner;
    UpdateButtonStates();

    AppendLogUi("=== " + sc::gui::WideToUtf8(banner) + " ===");
    AppendLogUi("[studio] cmd: " + sc::gui::WideToUtf8(cmdLine));

    g_app.workerThread = std::thread([cmdLine, useWarp, headless, strictD3d, autoStub, stubReport]() {
        SetEnvironmentVariableW(L"SCARLETT_USE_WARP",     useWarp ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_HEADLESS",     headless ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_STRICT_D3D12", strictD3d ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_AUTO_STUB",    autoStub ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_STUB_REPORT",  stubReport ? L"1" : L"0");

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hRead = nullptr, hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            PostLogAsync("[studio] ERROR: CreatePipe failed\n");
            PostMessageW(g_app.hwnd, WM_STUDIO_PROC_DONE, 1, 0);
            return;
        }
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWrite;
        si.hStdError  = hWrite;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::wstring mutableCmd = cmdLine;
        const BOOL ok = CreateProcessW(
            nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, g_app.binDir.c_str(), &si, &pi);

        CloseHandle(hWrite);

        if (!ok) {
            const DWORD err = GetLastError();
            if (hJob) CloseHandle(hJob);
            CloseHandle(hRead);
            char ebuf[128];
            std::snprintf(ebuf, sizeof(ebuf), "[studio] ERROR: CreateProcessW failed (err=%lu)\n", err);
            PostLogAsync(ebuf);
            PostMessageW(g_app.hwnd, WM_STUDIO_PROC_DONE, err, 0);
            return;
        }

        if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
        {
            std::lock_guard<std::mutex> lock(g_app.procMtx);
            g_app.activeJob = hJob;
            g_app.activeProc = pi.hProcess;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        char chunk[2048];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, chunk, sizeof(chunk) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            chunk[bytesRead] = '\0';
            PostLogAsync(std::string(chunk, bytesRead));
        }
        CloseHandle(hRead);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        {
            std::lock_guard<std::mutex> lock(g_app.procMtx);
            g_app.activeProc = nullptr;
            g_app.activeJob = nullptr;
        }
        CloseHandle(pi.hProcess);
        if (hJob) CloseHandle(hJob);

        char doneBuf[128];
        std::snprintf(doneBuf, sizeof(doneBuf), "[studio] Process finished with exit code %lu (0x%08lX)\n", exitCode, exitCode);
        PostLogAsync(doneBuf);
        PostMessageW(g_app.hwnd, WM_STUDIO_PROC_DONE, static_cast<WPARAM>(exitCode), 0);
    });
}

void CopyLogToClipboard() {
    std::wstring all;
    for (const auto& l : g_app.logLines) {
        all += l.text;
        all += L"\r\n";
    }
    if (all.empty() || !OpenClipboard(g_app.hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (all.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        if (dst) {
            std::memcpy(dst, all.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
    g_app.statusText = L"Copied console log to clipboard";
}

void ExecuteAction(ActionId id) {
    switch (id) {
    case ActionId::ScanFolder: {
        BROWSEINFOW bi{};
        bi.hwndOwner = g_app.hwnd;
        bi.lpszTitle = L"Select a folder containing Xbox GDK packages, executables, or XVD/XVC containers";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t pathBuf[MAX_PATH * 4]{};
            if (SHGetPathFromIDListW(pidl, pathBuf)) {
                g_app.libraryRoots.emplace_back(pathBuf);
                sc::gui::SaveRootsConfig(g_app.configFile, g_app.libraryRoots);
                RefreshLibrary();
            }
            CoTaskMemFree(pidl);
        }
        break;
    }
    case ActionId::AddFile: {
        wchar_t fileBuf[MAX_PATH * 4]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_app.hwnd;
        ofn.lpstrFilter = L"Scarlett Targets (*.exe;MicrosoftGame.Config;*.xvd;*.xvc)\0*.exe;*.config;*.mgc;*.xvd;*.xvc\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuf));
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            g_app.libraryRoots.emplace_back(fileBuf);
            sc::gui::SaveRootsConfig(g_app.configFile, g_app.libraryRoots);
            RefreshLibrary();
        }
        break;
    }
    case ActionId::Refresh:
        RefreshLibrary();
        break;
    case ActionId::Launch: {
        if (g_app.selectedIndex < 0 || static_cast<size_t>(g_app.selectedIndex) >= g_app.entries.size()) break;
        const auto& e = g_app.entries[static_cast<size_t>(g_app.selectedIndex)];
        if (!e.launchable) break;
        if (e.kind == sc::gui::EntryKind::GdkPackage) {
            LaunchLoaderCommand(L"--boot \"" + e.targetPath.wstring() + L"\"", false, false, L"Boot " + e.displayName);
        } else {
            LaunchLoaderCommand(L"\"" + e.executablePath.wstring() + L"\"", false, false, L"Launch " + e.displayName);
        }
        break;
    }
    case ActionId::Stop:
        StopActiveProcess();
        break;
    case ActionId::Inspect: {
        if (g_app.selectedIndex < 0 || static_cast<size_t>(g_app.selectedIndex) >= g_app.entries.size()) break;
        const auto& e = g_app.entries[static_cast<size_t>(g_app.selectedIndex)];
        if (e.kind == sc::gui::EntryKind::GdkPackage) {
            LaunchLoaderCommand(L"--inspect-package \"" + e.targetPath.wstring() + L"\"", false, true, L"Inspect Package " + e.displayName);
        } else if (e.kind == sc::gui::EntryKind::Executable) {
            LaunchLoaderCommand(L"--inspect \"" + e.executablePath.wstring() + L"\"", false, true, L"Inspect PE64 " + e.displayName);
        } else {
            LaunchLoaderCommand(L"--audit-dir \"" + e.targetPath.parent_path().wstring() + L"\"", false, true, L"Audit XVD/XVC " + e.displayName);
        }
        break;
    }
    case ActionId::TestGpu: {
        const auto testsExe = g_app.binDir / L"titles" / L"scarlett_tests" / L"scarlett_tests.exe";
        LaunchLoaderCommand(L"\"" + testsExe.wstring() + L"\"", false, true, L"Verification Suite (Hardware GPU)");
        break;
    }
    case ActionId::TestWarp: {
        const auto testsExe = g_app.binDir / L"titles" / L"scarlett_tests" / L"scarlett_tests.exe";
        LaunchLoaderCommand(L"\"" + testsExe.wstring() + L"\"", true, true, L"Verification Suite (Microsoft WARP)");
        break;
    }
    case ActionId::CopyLog:
        CopyLogToClipboard();
        break;
    case ActionId::ClearLog:
        g_app.logLines.clear();
        g_app.consoleScroll = 0.0f;
        g_app.consoleAutoScroll = true;
        break;
    default:
        break;
    }
}

void ComputeLayout() {
    const float w = std::max(640.0f, g_app.widthDip);
    const float h = std::max(460.0f, g_app.heightDip);
    const float pad = 16.0f;

    g_app.rcTopBar = D2D1::RectF(pad, pad, w - pad, pad + 56.0f);

    // Lay out pill buttons right-to-left inside the top bar
    const float btnH = 34.0f;
    const float btnY = g_app.rcTopBar.top + (56.0f - btnH) * 0.5f;
    float rx = g_app.rcTopBar.right - 14.0f;
    auto placeRight = [&](ActionId id, float bw) {
        for (auto& b : g_app.buttons) {
            if (b.id == id) {
                b.rect = D2D1::RectF(rx - bw, btnY, rx, btnY + btnH);
                rx -= (bw + 8.0f);
                break;
            }
        }
    };
    placeRight(ActionId::ClearLog,   68.0f);
    placeRight(ActionId::CopyLog,    82.0f);
    placeRight(ActionId::TestWarp,   108.0f);
    placeRight(ActionId::TestGpu,    98.0f);
    placeRight(ActionId::Inspect,    78.0f);
    placeRight(ActionId::Refresh,    78.0f);
    placeRight(ActionId::AddFile,    96.0f);
    placeRight(ActionId::ScanFolder, 108.0f);
    placeRight(ActionId::Stop,       68.0f);
    placeRight(ActionId::Launch,     96.0f);

    // Toggle capsule bar
    const float togTop = g_app.rcTopBar.bottom + 10.0f;
    g_app.rcToggleBar = D2D1::RectF(pad, togTop, w - pad, togTop + 40.0f);
    float tx = g_app.rcToggleBar.left + 16.0f;
    const float ty = g_app.rcToggleBar.top + 8.0f;
    const float widths[] = { 164.0f, 134.0f, 136.0f, 152.0f, 150.0f };
    for (size_t i = 0; i < g_app.toggles.size() && i < std::size(widths); ++i) {
        g_app.toggles[i].rect = D2D1::RectF(tx, ty, tx + widths[i], ty + 24.0f);
        tx += widths[i] + 12.0f;
    }

    // Main split cards
    const float bodyTop = g_app.rcToggleBar.bottom + 12.0f;
    const float footerH = 26.0f;
    const float availH = std::max(240.0f, h - bodyTop - footerH - pad);
    const float upperH = availH * 0.57f;
    const float lowerH = availH - upperH - 12.0f;

    const float availW = w - pad * 2.0f;
    const float leftW = (availW - 12.0f) * 0.57f;

    g_app.rcLibraryCard = D2D1::RectF(pad, bodyTop, pad + leftW, bodyTop + upperH);
    g_app.rcLibraryList = D2D1::RectF(g_app.rcLibraryCard.left + 12.0f, g_app.rcLibraryCard.top + 44.0f,
                                      g_app.rcLibraryCard.right - 12.0f, g_app.rcLibraryCard.bottom - 12.0f);

    g_app.rcInspectorCard = D2D1::RectF(g_app.rcLibraryCard.right + 12.0f, bodyTop, w - pad, bodyTop + upperH);
    g_app.rcInspectorBody = D2D1::RectF(g_app.rcInspectorCard.left + 16.0f, g_app.rcInspectorCard.top + 44.0f,
                                        g_app.rcInspectorCard.right - 16.0f, g_app.rcInspectorCard.bottom - 12.0f);

    g_app.rcConsoleCard = D2D1::RectF(pad, g_app.rcLibraryCard.bottom + 12.0f, w - pad, g_app.rcLibraryCard.bottom + 12.0f + lowerH);
    g_app.rcConsoleBody = D2D1::RectF(g_app.rcConsoleCard.left + 16.0f, g_app.rcConsoleCard.top + 40.0f,
                                      g_app.rcConsoleCard.right - 16.0f, g_app.rcConsoleCard.bottom - 10.0f);
}

bool CreateTargetBitmapForSwapChain() {
    g_app.d2dCtx->SetTarget(nullptr);
    g_app.targetBitmap.Reset();

    ComPtr<IDXGISurface> surface;
    if (FAILED(g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * g_app.dpiScale,
        96.0f * g_app.dpiScale);
    if (FAILED(g_app.d2dCtx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &g_app.targetBitmap))) return false;
    g_app.d2dCtx->SetTarget(g_app.targetBitmap.Get());
    g_app.d2dCtx->SetDpi(96.0f * g_app.dpiScale, 96.0f * g_app.dpiScale);
    return true;
}

bool InitGraphics(HWND hwnd) {
    D3D_FEATURE_LEVEL fl{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &g_app.d3dDevice, &fl, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION, &g_app.d3dDevice, &fl, nullptr);
    }
    if (FAILED(hr)) return false;

    if (FAILED(g_app.d3dDevice.As(&g_app.dxgiDevice))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&g_app.d2dFactory)))) return false;
    if (FAILED(g_app.d2dFactory->CreateDevice(g_app.dxgiDevice.Get(), &g_app.d2dDevice))) return false;
    if (FAILED(g_app.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_app.d2dCtx))) return false;

    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)))) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT pxW = std::max<UINT>(1, static_cast<UINT>(rc.right - rc.left));
    const UINT pxH = std::max<UINT>(1, static_cast<UINT>(rc.bottom - rc.top));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = pxW;
    scd.Height = pxH;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(g_app.dxgiDevice.Get(), &scd, nullptr, &g_app.swapChain))) {
        return false;
    }

    if (FAILED(DCompositionCreateDevice(g_app.dxgiDevice.Get(), IID_PPV_ARGS(&g_app.dcompDevice)))) return false;
    if (FAILED(g_app.dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_app.dcompTarget))) return false;
    if (FAILED(g_app.dcompDevice->CreateVisual(&g_app.dcompVisual))) return false;
    if (FAILED(g_app.dcompVisual->SetContent(g_app.swapChain.Get()))) return false;
    if (FAILED(g_app.dcompTarget->SetRoot(g_app.dcompVisual.Get()))) return false;
    if (FAILED(g_app.dcompDevice->Commit())) return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_app.dwriteFactory.GetAddressOf())))) {
        return false;
    }

    auto mkFmt = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        ComPtr<IDWriteTextFormat> f;
        g_app.dwriteFactory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f) {
            f->SetTextAlignment(align);
            f->SetParagraphAlignment(valign);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return f;
    };

    g_app.fmtBrand    = mkFmt(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 17.0f);
    g_app.fmtSection  = mkFmt(L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f);
    g_app.fmtBody     = mkFmt(L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,    13.0f);
    g_app.fmtBodyBold = mkFmt(L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f);
    g_app.fmtSmall    = mkFmt(L"Segoe UI Variable Small",   DWRITE_FONT_WEIGHT_MEDIUM,    11.5f);
    g_app.fmtMono     = mkFmt(L"Cascadia Mono",             DWRITE_FONT_WEIGHT_NORMAL,    12.5f,
                              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    return CreateTargetBitmapForSwapChain();
}

void DrawGlassCard(const D2D1_RECT_F& rect, float radius, float tintAlpha = 0.34f) {
    auto* ctx = g_app.d2dCtx.Get();
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);

    // Frosted vertical glass gradient
    D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, D2D1::ColorF(0.18f, 0.24f, 0.34f, tintAlpha + 0.08f) },
        { 1.0f, D2D1::ColorF(0.05f, 0.08f, 0.13f, tintAlpha) }
    };
    ComPtr<ID2D1GradientStopCollection> gsc;
    ComPtr<ID2D1LinearGradientBrush> fillBrush;
    ctx->CreateGradientStopCollection(stops, 2, &gsc);
    ctx->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
        gsc.Get(), &fillBrush);
    ctx->FillRoundedRectangle(rr, fillBrush.Get());

    // Liquid Glass top specular sheen (upper 32% subtle light-catch band)
    const float sheenH = std::min(46.0f, (rect.bottom - rect.top) * 0.34f);
    const D2D1_RECT_F sheenRc = D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.top + sheenH);
    D2D1_GRADIENT_STOP sheenStops[2] = {
        { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) }
    };
    ComPtr<ID2D1GradientStopCollection> sheenGsc;
    ComPtr<ID2D1LinearGradientBrush> sheenBrush;
    ctx->CreateGradientStopCollection(sheenStops, 2, &sheenGsc);
    ctx->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(sheenRc.left, sheenRc.top), D2D1::Point2F(sheenRc.left, sheenRc.bottom)),
        sheenGsc.Get(), &sheenBrush);
    ctx->FillRoundedRectangle(D2D1::RoundedRect(sheenRc, radius - 1.0f, radius - 1.0f), sheenBrush.Get());

    // Prismatic specular rim (crisp top-left refraction fading into bottom-right shadow)
    D2D1_GRADIENT_STOP rimStops[3] = {
        { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.44f) },
        { 0.42f, D2D1::ColorF(0.62f, 0.86f, 1.0f, 0.18f) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) }
    };
    ComPtr<ID2D1GradientStopCollection> rimGsc;
    ComPtr<ID2D1LinearGradientBrush> rimBrush;
    ctx->CreateGradientStopCollection(rimStops, 3, &rimGsc);
    ctx->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.right, rect.bottom)),
        rimGsc.Get(), &rimBrush);
    ctx->DrawRoundedRectangle(rr, rimBrush.Get(), 1.25f);
}

void RenderFrame() {
    if (!g_app.d2dCtx || !g_app.targetBitmap) return;
    auto* ctx = g_app.d2dCtx.Get();
    ctx->BeginDraw();

    // Translucent dark glass veil so Windows 11 DWM Acrylic blurs the desktop cleanly behind us
    ctx->Clear(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.36f));

    // Ambient liquid aurora glows (Xbox Emerald + Ice Cyan)
    auto drawAurora = [&](float cx, float cy, float rx, float ry, D2D1_COLOR_F centerCol) {
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, centerCol },
            { 1.0f, D2D1::ColorF(centerCol.r, centerCol.g, centerCol.b, 0.0f) }
        };
        ComPtr<ID2D1GradientStopCollection> gsc;
        ComPtr<ID2D1RadialGradientBrush> br;
        ctx->CreateGradientStopCollection(stops, 2, &gsc);
        ctx->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry),
            gsc.Get(), &br);
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), br.Get());
    };
    drawAurora(g_app.widthDip * 0.16f, 40.0f, 360.0f, 220.0f, D2D1::ColorF(0.06f, 0.78f, 0.45f, 0.16f));
    drawAurora(g_app.widthDip * 0.84f, g_app.heightDip * 0.22f, 380.0f, 260.0f, D2D1::ColorF(0.18f, 0.58f, 1.0f, 0.13f));
    drawAurora(g_app.widthDip * 0.50f, g_app.heightDip * 0.92f, 420.0f, 220.0f, D2D1::ColorF(0.42f, 0.28f, 0.92f, 0.10f));

    ComPtr<ID2D1SolidColorBrush> solid;
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &solid);

    // 1. Top Floating Glass Bar
    DrawGlassCard(g_app.rcTopBar, 16.0f, 0.48f);

    // Brand orb + title
    const float pulse = 0.5f + 0.5f * std::sin(g_app.pulsePhase);
    const float orbX = g_app.rcTopBar.left + 26.0f;
    const float orbY = (g_app.rcTopBar.top + g_app.rcTopBar.bottom) * 0.5f;
    const bool running = g_app.procRunning.load(std::memory_order_acquire);
    const D2D1_COLOR_F orbCol = running
        ? D2D1::ColorF(0.20f, 0.85f, 1.0f, 0.95f)
        : D2D1::ColorF(0.10f, 0.88f, 0.46f, 0.95f);
    solid->SetColor(D2D1::ColorF(orbCol.r, orbCol.g, orbCol.b, 0.22f + 0.12f * pulse));
    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(orbX, orbY), 12.0f, 12.0f), solid.Get());
    solid->SetColor(orbCol);
    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(orbX, orbY), 5.5f, 5.5f), solid.Get());

    solid->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.96f));
    D2D1_RECT_F rcTitle = D2D1::RectF(orbX + 18.0f, g_app.rcTopBar.top, g_app.rcTopBar.left + 250.0f, g_app.rcTopBar.bottom);
    ctx->DrawTextW(L"SCARLETT STUDIO", 15, g_app.fmtBrand.Get(), rcTitle, solid.Get());

    // Pill Buttons
    g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (const auto& b : g_app.buttons) {
        const D2D1_ROUNDED_RECT br = D2D1::RoundedRect(b.rect, 17.0f, 17.0f);
        const float h = b.enabled ? b.hoverAnim : 0.0f;
        const float alphaMul = b.enabled ? 1.0f : 0.38f;

        if (b.primary) {
            D2D1_GRADIENT_STOP bs[2] = {
                { 0.0f, D2D1::ColorF(0.10f + 0.08f * h, 0.82f + 0.10f * h, 0.45f + 0.08f * h, 0.88f * alphaMul) },
                { 1.0f, D2D1::ColorF(0.05f + 0.06f * h, 0.58f + 0.10f * h, 0.34f + 0.06f * h, 0.92f * alphaMul) }
            };
            ComPtr<ID2D1GradientStopCollection> gsc;
            ComPtr<ID2D1LinearGradientBrush> lb;
            ctx->CreateGradientStopCollection(bs, 2, &gsc);
            ctx->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(b.rect.left, b.rect.top), D2D1::Point2F(b.rect.left, b.rect.bottom)),
                gsc.Get(), &lb);
            ctx->FillRoundedRectangle(br, lb.Get());
            solid->SetColor(D2D1::ColorF(0.70f, 1.0f, 0.85f, (0.45f + 0.30f * h) * alphaMul));
            ctx->DrawRoundedRectangle(br, solid.Get(), 1.1f);
        } else if (b.danger && b.enabled) {
            solid->SetColor(D2D1::ColorF(0.86f, 0.20f, 0.28f, 0.72f + 0.18f * h));
            ctx->FillRoundedRectangle(br, solid.Get());
            solid->SetColor(D2D1::ColorF(1.0f, 0.55f, 0.60f, 0.55f));
            ctx->DrawRoundedRectangle(br, solid.Get(), 1.0f);
        } else {
            solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, (0.08f + 0.09f * h) * alphaMul));
            ctx->FillRoundedRectangle(br, solid.Get());
            solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, (0.18f + 0.22f * h) * alphaMul));
            ctx->DrawRoundedRectangle(br, solid.Get(), 1.0f);
        }

        solid->SetColor(D2D1::ColorF(0.97f, 0.99f, 1.0f, 0.96f * alphaMul));
        ctx->DrawTextW(b.label.c_str(), static_cast<UINT32>(b.label.size()), g_app.fmtSmall.Get(), b.rect, solid.Get());
    }
    g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // 2. Capsule Toggle Bar
    DrawGlassCard(g_app.rcToggleBar, 13.0f, 0.36f);
    for (size_t i = 0; i < g_app.toggles.size(); ++i) {
        const auto& t = g_app.toggles[i];
        const float swW = 36.0f, swH = 20.0f;
        const float sy = (t.rect.top + t.rect.bottom - swH) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(t.rect.left, sy, t.rect.left + swW, sy + swH);
        const D2D1_ROUNDED_RECT tr = D2D1::RoundedRect(track, 10.0f, 10.0f);

        // Interpolate track color from glass-slate to emerald
        const float a = t.anim;
        solid->SetColor(D2D1::ColorF(
            0.22f * (1.0f - a) + 0.09f * a,
            0.26f * (1.0f - a) + 0.78f * a,
            0.34f * (1.0f - a) + 0.45f * a,
            0.78f));
        ctx->FillRoundedRectangle(tr, solid.Get());
        solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f));
        ctx->DrawRoundedRectangle(tr, solid.Get(), 1.0f);

        const float knobX = track.left + 10.0f + a * (swW - 20.0f);
        const float knobY = (track.top + track.bottom) * 0.5f;
        solid->SetColor(D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.96f));
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), 7.5f, 7.5f), solid.Get());

        solid->SetColor(D2D1::ColorF(0.88f, 0.92f, 0.97f, 0.92f));
        D2D1_RECT_F lblRc = D2D1::RectF(track.right + 8.0f, t.rect.top, t.rect.right, t.rect.bottom);
        ctx->DrawTextW(t.label.c_str(), static_cast<UINT32>(t.label.size()), g_app.fmtSmall.Get(), lblRc, solid.Get());
    }

    // 3. Title Library Glass Card
    DrawGlassCard(g_app.rcLibraryCard, 16.0f, 0.42f);
    solid->SetColor(D2D1::ColorF(0.74f, 0.83f, 0.94f, 0.92f));
    ctx->DrawTextW(L"TITLE LIBRARY & CORPUS", 22, g_app.fmtSection.Get(),
                   D2D1::RectF(g_app.rcLibraryCard.left + 16.0f, g_app.rcLibraryCard.top + 10.0f,
                               g_app.rcLibraryCard.right - 16.0f, g_app.rcLibraryCard.top + 36.0f),
                   solid.Get());

    ctx->PushAxisAlignedClip(g_app.rcLibraryList, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float rowH = 52.0f;
    const float rowGap = 6.0f;
    for (size_t i = 0; i < g_app.entries.size(); ++i) {
        const auto& e = g_app.entries[i];
        const float ry = g_app.rcLibraryList.top + static_cast<float>(i) * (rowH + rowGap) - g_app.libraryScroll;
        if (ry + rowH < g_app.rcLibraryList.top || ry > g_app.rcLibraryList.bottom) continue;

        const D2D1_RECT_F rowRc = D2D1::RectF(g_app.rcLibraryList.left, ry, g_app.rcLibraryList.right, ry + rowH);
        const D2D1_ROUNDED_RECT rowRr = D2D1::RoundedRect(rowRc, 11.0f, 11.0f);
        const bool isSel = (static_cast<int>(i) == g_app.selectedIndex);
        const bool isHov = (static_cast<int>(i) == g_app.hoveredRow);

        if (isSel) {
            solid->SetColor(D2D1::ColorF(0.10f, 0.78f, 0.46f, 0.22f));
            ctx->FillRoundedRectangle(rowRr, solid.Get());
            solid->SetColor(D2D1::ColorF(0.24f, 0.92f, 0.58f, 0.55f));
            ctx->DrawRoundedRectangle(rowRr, solid.Get(), 1.2f);
        } else if (isHov) {
            solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
            ctx->FillRoundedRectangle(rowRr, solid.Get());
            solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
            ctx->DrawRoundedRectangle(rowRr, solid.Get(), 1.0f);
        } else {
            solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.035f));
            ctx->FillRoundedRectangle(rowRr, solid.Get());
        }

        // Type pill badge
        const wchar_t* tag = (e.kind == sc::gui::EntryKind::GdkPackage) ? L"GDK" :
                             (e.kind == sc::gui::EntryKind::XvdContainer) ? L"XVD" : L"EXE";
        const D2D1_COLOR_F tagCol = (e.kind == sc::gui::EntryKind::GdkPackage)
            ? D2D1::ColorF(0.14f, 0.80f, 0.48f, 0.85f)
            : (e.kind == sc::gui::EntryKind::XvdContainer)
                ? D2D1::ColorF(0.95f, 0.58f, 0.24f, 0.85f)
                : D2D1::ColorF(0.26f, 0.65f, 1.0f, 0.85f);
        const D2D1_RECT_F badgeRc = D2D1::RectF(rowRc.left + 12.0f, rowRc.top + 14.0f, rowRc.left + 52.0f, rowRc.top + 36.0f);
        solid->SetColor(D2D1::ColorF(tagCol.r, tagCol.g, tagCol.b, 0.20f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(badgeRc, 6.0f, 6.0f), solid.Get());
        solid->SetColor(tagCol);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(badgeRc, 6.0f, 6.0f), solid.Get(), 1.0f);
        g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx->DrawTextW(tag, static_cast<UINT32>(std::wcslen(tag)), g_app.fmtSmall.Get(), badgeRc, solid.Get());
        g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // Title + Path
        solid->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.96f));
        ctx->DrawTextW(e.displayName.c_str(), static_cast<UINT32>(e.displayName.size()), g_app.fmtBodyBold.Get(),
                       D2D1::RectF(rowRc.left + 64.0f, rowRc.top + 6.0f, rowRc.right - 160.0f, rowRc.top + 26.0f),
                       solid.Get());

        const std::wstring pathW = e.targetPath.wstring();
        solid->SetColor(D2D1::ColorF(0.64f, 0.72f, 0.82f, 0.82f));
        ctx->DrawTextW(pathW.c_str(), static_cast<UINT32>(pathW.size()), g_app.fmtSmall.Get(),
                       D2D1::RectF(rowRc.left + 64.0f, rowRc.top + 26.0f, rowRc.right - 160.0f, rowRc.top + 46.0f),
                       solid.Get());

        // Right-side status & coverage
        const std::wstring statW = sc::gui::Utf8ToWide(e.statusText);
        const std::wstring covW  = sc::gui::Utf8ToWide(e.coverageText);
        const D2D1_COLOR_F statCol = (e.statusText.find("Ready") != std::string::npos)
            ? D2D1::ColorF(0.30f, 0.95f, 0.62f, 0.95f)
            : (e.statusText.find("Encrypted") != std::string::npos || e.statusText.find("Missing") != std::string::npos)
                ? D2D1::ColorF(1.0f, 0.76f, 0.36f, 0.95f)
                : D2D1::ColorF(0.82f, 0.88f, 0.96f, 0.90f);

        g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        solid->SetColor(statCol);
        ctx->DrawTextW(statW.c_str(), static_cast<UINT32>(statW.size()), g_app.fmtSmall.Get(),
                       D2D1::RectF(rowRc.right - 154.0f, rowRc.top + 7.0f, rowRc.right - 14.0f, rowRc.top + 25.0f),
                       solid.Get());
        solid->SetColor(D2D1::ColorF(0.74f, 0.85f, 0.96f, 0.88f));
        ctx->DrawTextW(covW.c_str(), static_cast<UINT32>(covW.size()), g_app.fmtSmall.Get(),
                       D2D1::RectF(rowRc.right - 154.0f, rowRc.top + 23.0f, rowRc.right - 14.0f, rowRc.top + 39.0f),
                       solid.Get());
        g_app.fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // Mini coverage progress bar
        float covRatio = (e.coverageText.find("100%") != std::string::npos) ? 1.0f :
                         (e.coverageText.find("Encrypted") != std::string::npos) ? 0.15f : 0.75f;
        const D2D1_RECT_F barBg = D2D1::RectF(rowRc.right - 118.0f, rowRc.top + 41.0f, rowRc.right - 14.0f, rowRc.top + 45.0f);
        solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(barBg, 2.0f, 2.0f), solid.Get());
        const D2D1_RECT_F barFg = D2D1::RectF(barBg.left, barBg.top, barBg.left + (barBg.right - barBg.left) * covRatio, barBg.bottom);
        solid->SetColor(statCol);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(barFg, 2.0f, 2.0f), solid.Get());
    }
    ctx->PopAxisAlignedClip();

    // 4. Pre-Flight Inspector Glass Card
    DrawGlassCard(g_app.rcInspectorCard, 16.0f, 0.42f);
    solid->SetColor(D2D1::ColorF(0.74f, 0.83f, 0.94f, 0.92f));
    ctx->DrawTextW(L"PRE-FLIGHT INSPECTOR", 20, g_app.fmtSection.Get(),
                   D2D1::RectF(g_app.rcInspectorCard.left + 16.0f, g_app.rcInspectorCard.top + 10.0f,
                               g_app.rcInspectorCard.right - 16.0f, g_app.rcInspectorCard.top + 36.0f),
                   solid.Get());

    ctx->PushAxisAlignedClip(g_app.rcInspectorBody, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float inspTextTop = g_app.rcInspectorBody.top;
    std::wstring inspText = L"Select a title, GDK package, or XVD/XVC container to inspect.";
    if (g_app.selectedIndex >= 0 && static_cast<size_t>(g_app.selectedIndex) < g_app.entries.size()) {
        const auto& sel = g_app.entries[static_cast<size_t>(g_app.selectedIndex)];
        const D2D1_RECT_F heroRc = D2D1::RectF(g_app.rcInspectorBody.left, inspTextTop,
                                               g_app.rcInspectorBody.right, inspTextTop + 52.0f);
        solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(heroRc, 11.0f, 11.0f), solid.Get());
        solid->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(heroRc, 11.0f, 11.0f), solid.Get(), 1.0f);

        solid->SetColor(D2D1::ColorF(0.97f, 0.99f, 1.0f, 0.98f));
        ctx->DrawTextW(sel.displayName.c_str(), static_cast<UINT32>(sel.displayName.size()), g_app.fmtBrand.Get(),
                       D2D1::RectF(heroRc.left + 14.0f, heroRc.top + 4.0f, heroRc.right - 140.0f, heroRc.top + 28.0f),
                       solid.Get());
        const std::wstring subW = sc::gui::Utf8ToWide(sel.coverageText + "  |  " + sel.statusText);
        solid->SetColor(D2D1::ColorF(0.32f, 0.95f, 0.64f, 0.92f));
        ctx->DrawTextW(subW.c_str(), static_cast<UINT32>(subW.size()), g_app.fmtSmall.Get(),
                       D2D1::RectF(heroRc.left + 14.0f, heroRc.top + 28.0f, heroRc.right - 14.0f, heroRc.top + 48.0f),
                       solid.Get());

        inspTextTop = heroRc.bottom + 12.0f;
        inspText = sc::gui::Utf8ToWide(sel.detailReport);
    }
    solid->SetColor(D2D1::ColorF(0.87f, 0.92f, 0.98f, 0.94f));
    g_app.fmtMono->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ctx->DrawTextW(inspText.c_str(), static_cast<UINT32>(inspText.size()), g_app.fmtMono.Get(),
                   D2D1::RectF(g_app.rcInspectorBody.left, inspTextTop - g_app.inspectorScroll,
                               g_app.rcInspectorBody.right, g_app.rcInspectorBody.bottom + 2000.0f),
                   solid.Get());
    g_app.fmtMono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ctx->PopAxisAlignedClip();

    // 5. Live Console Glass Card
    DrawGlassCard(g_app.rcConsoleCard, 16.0f, 0.50f);
    solid->SetColor(D2D1::ColorF(0.74f, 0.83f, 0.94f, 0.92f));
    ctx->DrawTextW(L"LIVE LOADER & SHIM CONSOLE", 26, g_app.fmtSection.Get(),
                   D2D1::RectF(g_app.rcConsoleCard.left + 16.0f, g_app.rcConsoleCard.top + 8.0f,
                               g_app.rcConsoleCard.right - 16.0f, g_app.rcConsoleCard.top + 34.0f),
                   solid.Get());

    ctx->PushAxisAlignedClip(g_app.rcConsoleBody, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float lineH = 18.0f;
    for (size_t i = 0; i < g_app.logLines.size(); ++i) {
        const float ly = g_app.rcConsoleBody.top + static_cast<float>(i) * lineH - g_app.consoleScroll;
        if (ly + lineH < g_app.rcConsoleBody.top || ly > g_app.rcConsoleBody.bottom) continue;
        solid->SetColor(g_app.logLines[i].color);
        ctx->DrawTextW(g_app.logLines[i].text.c_str(), static_cast<UINT32>(g_app.logLines[i].text.size()),
                       g_app.fmtMono.Get(),
                       D2D1::RectF(g_app.rcConsoleBody.left, ly, g_app.rcConsoleBody.right + 1200.0f, ly + lineH),
                       solid.Get());
    }
    ctx->PopAxisAlignedClip();

    // 6. Subtle Footer Status Strip
    solid->SetColor(D2D1::ColorF(0.72f, 0.80f, 0.90f, 0.85f));
    ctx->DrawTextW(g_app.statusText.c_str(), static_cast<UINT32>(g_app.statusText.size()), g_app.fmtSmall.Get(),
                   D2D1::RectF(20.0f, g_app.heightDip - 26.0f, g_app.widthDip - 20.0f, g_app.heightDip - 4.0f),
                   solid.Get());

    if (SUCCEEDED(ctx->EndDraw())) {
        g_app.swapChain->Present(1, 0);
    }
}

void HandleMouseMove(float x, float y) {
    g_app.hoveredButton = ActionId::None;
    for (const auto& b : g_app.buttons) {
        if (PtInRectF(b.rect, x, y)) {
            g_app.hoveredButton = b.id;
            break;
        }
    }
    g_app.hoveredToggle = -1;
    for (size_t i = 0; i < g_app.toggles.size(); ++i) {
        if (PtInRectF(g_app.toggles[i].rect, x, y)) {
            g_app.hoveredToggle = static_cast<int>(i);
            break;
        }
    }
    g_app.hoveredRow = -1;
    if (PtInRectF(g_app.rcLibraryList, x, y)) {
        const float relY = (y - g_app.rcLibraryList.top) + g_app.libraryScroll;
        const int idx = static_cast<int>(relY / 58.0f);
        if (idx >= 0 && static_cast<size_t>(idx) < g_app.entries.size()) {
            g_app.hoveredRow = idx;
        }
    }
}

LRESULT CALLBACK StudioWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;
        g_app.dpiScale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;

        // Enable Windows 11 Dark Mode + Rounded Corners + Acrylic Liquid Glass backdrop
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
        const DWORD cornerPref = kDwmwcpRound;
        DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &cornerPref, sizeof(cornerPref));
        const COLORREF captionCol = RGB(12, 18, 28);
        DwmSetWindowAttribute(hwnd, kDwmwaCaptionColor, &captionCol, sizeof(captionCol));
        const DWORD backdrop = kDwmSbtTransientWindow;
        DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop));
        const MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        g_app.buttons = {
            { ActionId::Launch,     L"Launch",       {}, true,  false, true, 0.0f },
            { ActionId::Stop,       L"Stop",         {}, false, true,  false, 0.0f },
            { ActionId::ScanFolder, L"Scan Folder",  {}, false, false, true, 0.0f },
            { ActionId::AddFile,    L"Add Target",   {}, false, false, true, 0.0f },
            { ActionId::Refresh,    L"Refresh",      {}, false, false, true, 0.0f },
            { ActionId::Inspect,    L"Inspect",      {}, false, false, true, 0.0f },
            { ActionId::TestGpu,    L"Tests (GPU)",  {}, false, false, true, 0.0f },
            { ActionId::TestWarp,   L"Tests (WARP)", {}, false, false, true, 0.0f },
            { ActionId::CopyLog,    L"Copy Log",     {}, false, false, true, 0.0f },
            { ActionId::ClearLog,   L"Clear",        {}, false, false, true, 0.0f },
        };
        g_app.toggles = {
            { ToggleId::Warp,      L"WARP Software GPU", {}, false, 0.0f },
            { ToggleId::Headless,  L"Headless Mode",     {}, false, 0.0f },
            { ToggleId::StrictD3d, L"Strict D3D12",      {}, false, 0.0f },
            { ToggleId::AutoStub,  L"Auto-Stub Engine",  {}, true,  1.0f },
            { ToggleId::Hotlist,   L"Stub Hotlist Log",  {}, true,  1.0f },
        };

        RECT rc{};
        GetClientRect(hwnd, &rc);
        g_app.widthDip  = static_cast<float>(rc.right - rc.left) / g_app.dpiScale;
        g_app.heightDip = static_cast<float>(rc.bottom - rc.top) / g_app.dpiScale;
        ComputeLayout();

        if (!InitGraphics(hwnd)) {
            return -1;
        }
        RefreshLibrary();
        AppendLogUi("[studio] Scarlett Studio Liquid Glass ready. Double-click any title to launch via loader.exe.");
        SetTimer(hwnd, kAnimTimerId, 16, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        g_app.dpiScale = static_cast<float>(HIWORD(wp)) / 96.0f;
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SIZE: {
        const UINT pxW = std::max<UINT>(1, LOWORD(lp));
        const UINT pxH = std::max<UINT>(1, HIWORD(lp));
        g_app.widthDip  = static_cast<float>(pxW) / g_app.dpiScale;
        g_app.heightDip = static_cast<float>(pxH) / g_app.dpiScale;
        ComputeLayout();
        if (g_app.swapChain && g_app.d2dCtx) {
            g_app.d2dCtx->SetTarget(nullptr);
            g_app.targetBitmap.Reset();
            if (SUCCEEDED(g_app.swapChain->ResizeBuffers(0, pxW, pxH, DXGI_FORMAT_UNKNOWN, 0))) {
                CreateTargetBitmapForSwapChain();
            }
        }
        RenderFrame();
        return 0;
    }
    case WM_TIMER: {
        if (wp == kAnimTimerId) {
            g_app.pulsePhase += 0.06f;
            for (auto& b : g_app.buttons) {
                const float target = (b.id == g_app.hoveredButton && b.enabled) ? 1.0f : 0.0f;
                b.hoverAnim += (target - b.hoverAnim) * 0.22f;
            }
            for (auto& t : g_app.toggles) {
                const float target = t.checked ? 1.0f : 0.0f;
                t.anim += (target - t.anim) * 0.26f;
            }
            RenderFrame();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / g_app.dpiScale;
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / g_app.dpiScale;
        HandleMouseMove(x, y);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / g_app.dpiScale;
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / g_app.dpiScale;
        HandleMouseMove(x, y);

        for (const auto& b : g_app.buttons) {
            if (b.enabled && PtInRectF(b.rect, x, y)) {
                ExecuteAction(b.id);
                return 0;
            }
        }
        for (auto& t : g_app.toggles) {
            if (PtInRectF(t.rect, x, y)) {
                t.checked = !t.checked;
                return 0;
            }
        }
        if (g_app.hoveredRow >= 0 && static_cast<size_t>(g_app.hoveredRow) < g_app.entries.size()) {
            g_app.selectedIndex = g_app.hoveredRow;
            g_app.inspectorScroll = 0.0f;
            UpdateButtonStates();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / g_app.dpiScale;
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / g_app.dpiScale;
        HandleMouseMove(x, y);
        if (g_app.hoveredRow >= 0 && static_cast<size_t>(g_app.hoveredRow) < g_app.entries.size()) {
            g_app.selectedIndex = g_app.hoveredRow;
            UpdateButtonStates();
            ExecuteAction(ActionId::Launch);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / g_app.dpiScale;
        const float y = static_cast<float>(pt.y) / g_app.dpiScale;
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f * 42.0f;

        if (PtInRectF(g_app.rcConsoleCard, x, y)) {
            const float viewH = std::max(1.0f, g_app.rcConsoleBody.bottom - g_app.rcConsoleBody.top);
            const float maxScroll = std::max(0.0f, static_cast<float>(g_app.logLines.size()) * 18.0f - viewH);
            g_app.consoleScroll = std::clamp(g_app.consoleScroll - delta, 0.0f, maxScroll);
            g_app.consoleAutoScroll = (g_app.consoleScroll >= maxScroll - 4.0f);
        } else if (PtInRectF(g_app.rcLibraryCard, x, y)) {
            const float viewH = std::max(1.0f, g_app.rcLibraryList.bottom - g_app.rcLibraryList.top);
            const float maxScroll = std::max(0.0f, static_cast<float>(g_app.entries.size()) * 58.0f - viewH);
            g_app.libraryScroll = std::clamp(g_app.libraryScroll - delta, 0.0f, maxScroll);
        } else if (PtInRectF(g_app.rcInspectorCard, x, y)) {
            g_app.inspectorScroll = std::clamp(g_app.inspectorScroll - delta, 0.0f, 600.0f);
        }
        return 0;
    }
    case WM_STUDIO_LOG_APPEND: {
        auto* heapStr = reinterpret_cast<std::string*>(lp);
        if (heapStr) {
            AppendLogUi(*heapStr);
            delete heapStr;
        }
        return 0;
    }
    case WM_STUDIO_PROC_DONE: {
        g_app.procRunning.store(false, std::memory_order_release);
        UpdateButtonStates();
        wchar_t sbuf[128];
        std::swprintf(sbuf, std::size(sbuf), L"Completed (exit code %u)", static_cast<unsigned>(wp));
        g_app.statusText = sbuf;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        RenderFrame();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, kAnimTimerId);
        StopActiveProcess();
        if (g_app.workerThread.joinable()) g_app.workerThread.join();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int RunSelfTest(const std::filesystem::path& binDir) {
    const auto titlesDir = binDir / L"titles";
    const auto shimsDir  = binDir / L"shims";
    const auto entries   = sc::gui::ScanLibraryRoots({ titlesDir }, shimsDir);
    if (entries.empty()) return 1;

    bool foundDummy = false;
    bool foundTests = false;
    for (const auto& e : entries) {
        if (e.displayName == L"dummy_title" && e.launchable) foundDummy = true;
        if (e.displayName == L"scarlett_tests" && e.launchable) foundTests = true;
    }
    if (!foundDummy || !foundTests) return 2;

    const auto tmpXvd = std::filesystem::temp_directory_path() / L"scarlett_studio_selftest.xvc";
    {
        std::vector<uint8_t> buf(0x220, 0);
        std::memcpy(buf.data() + 0x200, "msft-xvd", 8);
        const uint32_t encFlags = 0x00000041u;
        std::memcpy(buf.data() + 0x208, &encFlags, sizeof(encFlags));
        std::ofstream f(tmpXvd, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    const auto xvdEntry = sc::gui::InspectXvdFileReadOnly(tmpXvd);
    std::error_code ec;
    std::filesystem::remove(tmpXvd, ec);
    if (xvdEntry.launchable || xvdEntry.statusText.find("Encrypted") == std::string::npos) return 3;
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_app.binDir     = GetExeDir();
    g_app.loaderExe  = g_app.binDir / L"loader.exe";
    g_app.shimsDir   = g_app.binDir / L"shims";
    g_app.configFile = g_app.binDir / L"scarlett_studio.cfg";

    if (lpCmdLine && std::wcsstr(lpCmdLine, L"--self-test") != nullptr) {
        return RunSelfTest(g_app.binDir);
    }

    g_app.libraryRoots = sc::gui::LoadRootsConfig(g_app.configFile);
    if (g_app.libraryRoots.empty()) {
        g_app.libraryRoots.push_back(g_app.binDir / L"titles");
        std::error_code ec;
        const std::filesystem::path extBuild = L"E:\\gta\\external\\build";
        if (std::filesystem::exists(extBuild, ec)) {
            g_app.libraryRoots.push_back(extBuild);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StudioWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ScarlettStudioLiquidGlassWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Scarlett Studio",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1260, 780,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
