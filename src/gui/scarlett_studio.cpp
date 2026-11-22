#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "studio_model.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT WM_STUDIO_LOG_APPEND = WM_APP + 1;
constexpr UINT WM_STUDIO_PROC_DONE  = WM_APP + 2;

enum ControlId : int {
    IDC_BTN_SCAN_DIR   = 101,
    IDC_BTN_ADD_FILE   = 102,
    IDC_BTN_REFRESH    = 103,
    IDC_BTN_LAUNCH     = 104,
    IDC_BTN_STOP       = 105,
    IDC_BTN_INSPECT    = 106,
    IDC_BTN_TEST_GPU   = 107,
    IDC_BTN_TEST_WARP  = 108,
    IDC_BTN_CLEAR_LOG  = 109,
    IDC_CHK_WARP       = 201,
    IDC_CHK_HEADLESS   = 202,
    IDC_CHK_STRICT     = 203,
    IDC_CHK_AUTOSTUB   = 204,
    IDC_CHK_HOTLIST    = 205,
    IDC_LIST_TITLES    = 301,
    IDC_EDIT_INSPECTOR = 302,
    IDC_EDIT_CONSOLE   = 303,
    IDC_STATUS_BAR     = 304
};

struct StudioApp {
    HWND hwndMain{nullptr};
    HWND hwndList{nullptr};
    HWND hwndInspector{nullptr};
    HWND hwndConsole{nullptr};
    HWND hwndStatus{nullptr};

    HWND btnScanDir{nullptr};
    HWND btnAddFile{nullptr};
    HWND btnRefresh{nullptr};
    HWND btnLaunch{nullptr};
    HWND btnStop{nullptr};
    HWND btnInspect{nullptr};
    HWND btnTestGpu{nullptr};
    HWND btnTestWarp{nullptr};
    HWND btnClearLog{nullptr};

    HWND chkWarp{nullptr};
    HWND chkHeadless{nullptr};
    HWND chkStrict{nullptr};
    HWND chkAutoStub{nullptr};
    HWND chkHotlist{nullptr};

    HFONT uiFont{nullptr};
    HFONT monoFont{nullptr};

    std::filesystem::path binDir;
    std::filesystem::path loaderExe;
    std::filesystem::path shimsDir;
    std::filesystem::path configFile;

    std::vector<std::filesystem::path> libraryRoots;
    std::vector<sc::gui::LibraryEntry> entries;

    std::mutex procMtx;
    HANDLE     activeJob{nullptr};
    HANDLE     activeProc{nullptr};
    std::atomic<bool> procRunning{false};
    std::thread       workerThread;
};

StudioApp g_app;

std::filesystem::path GetExeDir() {
    wchar_t buf[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
}

void AppendLogUi(const std::string& text) {
    if (!g_app.hwndConsole || text.empty()) return;
    // Normalize bare \n to \r\n for Win32 multiline EDIT control
    std::string norm;
    norm.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            norm.push_back('\r');
        }
        norm.push_back(text[i]);
    }
    const std::wstring wide = sc::gui::Utf8ToWide(norm);
    const int len = GetWindowTextLengthW(g_app.hwndConsole);
    SendMessageW(g_app.hwndConsole, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(g_app.hwndConsole, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wide.c_str()));
}

void PostLogAsync(const std::string& msg) {
    if (!g_app.hwndMain) return;
    auto* heapStr = new std::string(msg);
    if (!PostMessageW(g_app.hwndMain, WM_STUDIO_LOG_APPEND, 0, reinterpret_cast<LPARAM>(heapStr))) {
        delete heapStr;
    }
}

void SetStatusText(const std::wstring& msg) {
    if (g_app.hwndStatus) {
        SendMessageW(g_app.hwndStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(msg.c_str()));
    }
}

void UpdateActionButtons() {
    const bool running = g_app.procRunning.load(std::memory_order_acquire);
    const int sel = ListView_GetNextItem(g_app.hwndList, -1, LVNI_SELECTED);
    const bool hasSel = (sel >= 0 && static_cast<size_t>(sel) < g_app.entries.size());
    const bool canLaunch = hasSel && g_app.entries[static_cast<size_t>(sel)].launchable;

    EnableWindow(g_app.btnLaunch, (!running && canLaunch) ? TRUE : FALSE);
    EnableWindow(g_app.btnInspect, (!running && hasSel) ? TRUE : FALSE);
    EnableWindow(g_app.btnTestGpu, !running ? TRUE : FALSE);
    EnableWindow(g_app.btnTestWarp, !running ? TRUE : FALSE);
    EnableWindow(g_app.btnStop, running ? TRUE : FALSE);
}

void RefreshInspectorForSelection() {
    const int sel = ListView_GetNextItem(g_app.hwndList, -1, LVNI_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= g_app.entries.size()) {
        SetWindowTextW(g_app.hwndInspector, L"Select a title, GDK package, or XVD/XVC container to inspect.");
        UpdateActionButtons();
        return;
    }
    const auto& e = g_app.entries[static_cast<size_t>(sel)];
    const std::wstring w = sc::gui::Utf8ToWide(e.detailReport);
    SetWindowTextW(g_app.hwndInspector, w.c_str());
    UpdateActionButtons();
}

void RefreshLibrary() {
    g_app.entries = sc::gui::ScanLibraryRoots(g_app.libraryRoots, g_app.shimsDir);
    ListView_DeleteAllItems(g_app.hwndList);

    for (size_t i = 0; i < g_app.entries.size(); ++i) {
        const auto& e = g_app.entries[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(e.displayName.c_str());
        ListView_InsertItem(g_app.hwndList, &item);

        const wchar_t* kindStr = L"Executable";
        if (e.kind == sc::gui::EntryKind::GdkPackage) kindStr = L"GDK Package";
        else if (e.kind == sc::gui::EntryKind::XvdContainer) kindStr = L"XVD/XVC";

        const std::wstring covW = sc::gui::Utf8ToWide(e.coverageText);
        const std::wstring statW = sc::gui::Utf8ToWide(e.statusText);
        const std::wstring pathW = e.targetPath.wstring();

        ListView_SetItemText(g_app.hwndList, static_cast<int>(i), 1, const_cast<LPWSTR>(kindStr));
        ListView_SetItemText(g_app.hwndList, static_cast<int>(i), 2, const_cast<LPWSTR>(covW.c_str()));
        ListView_SetItemText(g_app.hwndList, static_cast<int>(i), 3, const_cast<LPWSTR>(statW.c_str()));
        ListView_SetItemText(g_app.hwndList, static_cast<int>(i), 4, const_cast<LPWSTR>(pathW.c_str()));
    }

    if (!g_app.entries.empty()) {
        ListView_SetItemState(g_app.hwndList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    RefreshInspectorForSelection();

    wchar_t sbuf[256];
    std::swprintf(sbuf, std::size(sbuf),
                  L"Ready | %zu item(s) across %zu root(s) | Shims: %ls",
                  g_app.entries.size(), g_app.libraryRoots.size(), g_app.shimsDir.c_str());
    SetStatusText(sbuf);
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
    if (g_app.procRunning.load(std::memory_order_acquire)) {
        return;
    }
    if (g_app.workerThread.joinable()) {
        g_app.workerThread.join();
    }

    const bool useWarp    = forceWarp || (SendMessageW(g_app.chkWarp, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool headless   = forceHeadless || (SendMessageW(g_app.chkHeadless, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool strictD3d  = (SendMessageW(g_app.chkStrict, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool autoStub   = (SendMessageW(g_app.chkAutoStub, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool stubReport = (SendMessageW(g_app.chkHotlist, BM_GETCHECK, 0, 0) == BST_CHECKED);

    const std::wstring cmdLine = L"\"" + g_app.loaderExe.wstring() + L"\" " + args;
    g_app.procRunning.store(true, std::memory_order_release);
    UpdateActionButtons();
    SetStatusText(L"Running: " + banner);
    AppendLogUi("\n=== " + sc::gui::WideToUtf8(banner) + " ===\n");
    AppendLogUi("[studio] cmd: " + sc::gui::WideToUtf8(cmdLine) + "\n");

    g_app.workerThread = std::thread([cmdLine, useWarp, headless, strictD3d, autoStub, stubReport]() {
        // Apply per-run environment variables cleanly in this child setup scope
        SetEnvironmentVariableW(L"SCARLETT_USE_WARP",      useWarp ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_HEADLESS",      headless ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_STRICT_D3D12",  strictD3d ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_AUTO_STUB",     autoStub ? L"1" : L"0");
        SetEnvironmentVariableW(L"SCARLETT_STUB_REPORT",   stubReport ? L"1" : L"0");

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hRead = nullptr, hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            PostLogAsync("[studio] ERROR: CreatePipe failed\n");
            PostMessageW(g_app.hwndMain, WM_STUDIO_PROC_DONE, 1, 0);
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
            PostMessageW(g_app.hwndMain, WM_STUDIO_PROC_DONE, err, 0);
            return;
        }

        if (hJob) {
            AssignProcessToJobObject(hJob, pi.hProcess);
        }
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
        PostMessageW(g_app.hwndMain, WM_STUDIO_PROC_DONE, static_cast<WPARAM>(exitCode), 0);
    });
}

void LaunchSelectedEntry() {
    const int sel = ListView_GetNextItem(g_app.hwndList, -1, LVNI_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= g_app.entries.size()) return;
    const auto& e = g_app.entries[static_cast<size_t>(sel)];
    if (!e.launchable) return;

    if (e.kind == sc::gui::EntryKind::GdkPackage) {
        LaunchLoaderCommand(L"--boot \"" + e.targetPath.wstring() + L"\"", false, false, L"Boot " + e.displayName);
    } else {
        LaunchLoaderCommand(L"\"" + e.executablePath.wstring() + L"\"", false, false, L"Launch " + e.displayName);
    }
}

void InspectSelectedEntry() {
    const int sel = ListView_GetNextItem(g_app.hwndList, -1, LVNI_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= g_app.entries.size()) return;
    const auto& e = g_app.entries[static_cast<size_t>(sel)];

    if (e.kind == sc::gui::EntryKind::GdkPackage) {
        LaunchLoaderCommand(L"--inspect-package \"" + e.targetPath.wstring() + L"\"", false, true, L"Inspect Package " + e.displayName);
    } else if (e.kind == sc::gui::EntryKind::Executable) {
        LaunchLoaderCommand(L"--inspect \"" + e.executablePath.wstring() + L"\"", false, true, L"Inspect PE64 " + e.displayName);
    } else {
        LaunchLoaderCommand(L"--audit-dir \"" + e.targetPath.parent_path().wstring() + L"\"", false, true, L"Audit XVD/XVC " + e.displayName);
    }
}

void BrowseAddFolder() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_app.hwndMain;
    bi.lpszTitle = L"Select a folder containing Xbox GDK packages, executables, or XVD/XVC containers";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t pathBuf[MAX_PATH * 4]{};
    if (SHGetPathFromIDListW(pidl, pathBuf)) {
        g_app.libraryRoots.emplace_back(pathBuf);
        sc::gui::SaveRootsConfig(g_app.configFile, g_app.libraryRoots);
        RefreshLibrary();
    }
    CoTaskMemFree(pidl);
}

void BrowseAddFile() {
    wchar_t fileBuf[MAX_PATH * 4]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwndMain;
    ofn.lpstrFilter = L"Scarlett Targets (*.exe;MicrosoftGame.Config;*.xvd;*.xvc)\0*.exe;*.config;*.mgc;*.xvd;*.xvc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuf));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        g_app.libraryRoots.emplace_back(fileBuf);
        sc::gui::SaveRootsConfig(g_app.configFile, g_app.libraryRoots);
        RefreshLibrary();
    }
}

void LayoutControls(int w, int h) {
    const int pad = 8;
    const int btnH = 28;
    const int row1Y = pad;
    int x = pad;
    auto placeBtn = [&](HWND btn, int bw) {
        MoveWindow(btn, x, row1Y, bw, btnH, TRUE);
        x += bw + 6;
    };
    placeBtn(g_app.btnScanDir,  104);
    placeBtn(g_app.btnAddFile,  96);
    placeBtn(g_app.btnRefresh,  72);
    placeBtn(g_app.btnLaunch,   80);
    placeBtn(g_app.btnStop,     64);
    placeBtn(g_app.btnInspect,  76);
    placeBtn(g_app.btnTestGpu,  122);
    placeBtn(g_app.btnTestWarp, 132);
    placeBtn(g_app.btnClearLog, 80);

    const int row2Y = row1Y + btnH + 6;
    const int chkH = 22;
    x = pad;
    auto placeChk = [&](HWND chk, int cw) {
        MoveWindow(chk, x, row2Y, cw, chkH, TRUE);
        x += cw + 10;
    };
    placeChk(g_app.chkWarp,     148);
    placeChk(g_app.chkHeadless, 108);
    placeChk(g_app.chkStrict,   116);
    placeChk(g_app.chkAutoStub, 126);
    placeChk(g_app.chkHotlist,  128);

    const int statusH = 22;
    const int topY = row2Y + chkH + 8;
    const int availH = (h - topY - statusH - pad * 2 > 120) ? (h - topY - statusH - pad * 2) : 120;
    const int upperH = (availH * 56) / 100;
    const int lowerH = availH - upperH - pad;

    const int availW = (w - pad * 3 > 200) ? (w - pad * 3) : 200;
    const int leftW = (availW * 58) / 100;
    const int rightW = availW - leftW;

    MoveWindow(g_app.hwndList, pad, topY, leftW, upperH, TRUE);
    MoveWindow(g_app.hwndInspector, pad * 2 + leftW, topY, rightW, upperH, TRUE);
    MoveWindow(g_app.hwndConsole, pad, topY + upperH + pad, w - pad * 2, lowerH, TRUE);
    SendMessageW(g_app.hwndStatus, WM_SIZE, 0, 0);
}

LRESULT CALLBACK StudioWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hwndMain = hwnd;
        g_app.uiFont = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_app.monoFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        auto mkBtn = [&](const wchar_t* text, int id) {
            HWND h = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 80, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
            return h;
        };
        auto mkChk = [&](const wchar_t* text, int id, bool checked) {
            HWND h = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                0, 0, 120, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
            if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
            return h;
        };

        g_app.btnScanDir  = mkBtn(L"Scan Folder...", IDC_BTN_SCAN_DIR);
        g_app.btnAddFile  = mkBtn(L"Add Target...",  IDC_BTN_ADD_FILE);
        g_app.btnRefresh  = mkBtn(L"Refresh",        IDC_BTN_REFRESH);
        g_app.btnLaunch   = mkBtn(L"Launch",         IDC_BTN_LAUNCH);
        g_app.btnStop     = mkBtn(L"Stop",           IDC_BTN_STOP);
        g_app.btnInspect  = mkBtn(L"Inspect",        IDC_BTN_INSPECT);
        g_app.btnTestGpu  = mkBtn(L"Run Tests (GPU)",  IDC_BTN_TEST_GPU);
        g_app.btnTestWarp = mkBtn(L"Run Tests (WARP)", IDC_BTN_TEST_WARP);
        g_app.btnClearLog = mkBtn(L"Clear Log",      IDC_BTN_CLEAR_LOG);

        g_app.chkWarp     = mkChk(L"WARP Software GPU", IDC_CHK_WARP,     false);
        g_app.chkHeadless = mkChk(L"Headless Mode",     IDC_CHK_HEADLESS, false);
        g_app.chkStrict   = mkChk(L"Strict D3D12",      IDC_CHK_STRICT,   false);
        g_app.chkAutoStub = mkChk(L"Auto-Stub Engine",  IDC_CHK_AUTOSTUB, true);
        g_app.chkHotlist  = mkChk(L"Stub Hotlist Log",  IDC_CHK_HOTLIST,  true);

        g_app.hwndList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 500, 300, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_TITLES)), nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_app.hwndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        SendMessageW(g_app.hwndList, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

        auto addCol = [&](int idx, const wchar_t* title, int width) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = width;
            col.pszText = const_cast<LPWSTR>(title);
            ListView_InsertColumn(g_app.hwndList, idx, &col);
        };
        addCol(0, L"Title / Item",   180);
        addCol(1, L"Type",           95);
        addCol(2, L"Shim Coverage",  115);
        addCol(3, L"Status",         130);
        addCol(4, L"Path",           260);

        g_app.hwndInspector = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, 350, 300, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_INSPECTOR)), nullptr, nullptr);
        SendMessageW(g_app.hwndInspector, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.monoFont), TRUE);

        g_app.hwndConsole = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, 800, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_CONSOLE)), nullptr, nullptr);
        SendMessageW(g_app.hwndConsole, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.monoFont), TRUE);
        SendMessageW(g_app.hwndConsole, EM_SETLIMITTEXT, 0, 0);

        g_app.hwndStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, L"Initializing Scarlett Studio...",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_BAR)), nullptr, nullptr);

        RefreshLibrary();
        AppendLogUi("Scarlett Studio ready. Double-click any title to launch through loader.exe.\n");
        return 0;
    }
    case WM_SIZE: {
        LayoutControls(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_BTN_SCAN_DIR:  BrowseAddFolder(); break;
        case IDC_BTN_ADD_FILE:  BrowseAddFile(); break;
        case IDC_BTN_REFRESH:   RefreshLibrary(); break;
        case IDC_BTN_LAUNCH:    LaunchSelectedEntry(); break;
        case IDC_BTN_STOP:      StopActiveProcess(); break;
        case IDC_BTN_INSPECT:   InspectSelectedEntry(); break;
        case IDC_BTN_TEST_GPU: {
            const auto testsExe = g_app.binDir / L"titles" / L"scarlett_tests" / L"scarlett_tests.exe";
            LaunchLoaderCommand(L"\"" + testsExe.wstring() + L"\"", false, true, L"Verification Suite (Hardware GPU)");
            break;
        }
        case IDC_BTN_TEST_WARP: {
            const auto testsExe = g_app.binDir / L"titles" / L"scarlett_tests" / L"scarlett_tests.exe";
            LaunchLoaderCommand(L"\"" + testsExe.wstring() + L"\"", true, true, L"Verification Suite (Microsoft WARP)");
            break;
        }
        case IDC_BTN_CLEAR_LOG:
            SetWindowTextW(g_app.hwndConsole, L"");
            break;
        default: break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (hdr && hdr->idFrom == IDC_LIST_TITLES) {
            if (hdr->code == LVN_ITEMCHANGED) {
                RefreshInspectorForSelection();
            } else if (hdr->code == NM_DBLCLK) {
                LaunchSelectedEntry();
            }
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
        UpdateActionButtons();
        wchar_t sbuf[128];
        std::swprintf(sbuf, std::size(sbuf), L"Completed (exit code %u)", static_cast<unsigned>(wp));
        SetStatusText(sbuf);
        return 0;
    }
    case WM_CLOSE:
        StopActiveProcess();
        if (g_app.workerThread.joinable()) {
            g_app.workerThread.join();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_app.uiFont) DeleteObject(g_app.uiFont);
        if (g_app.monoFont) DeleteObject(g_app.monoFont);
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
    if (entries.empty()) {
        std::fprintf(stderr, "[scarlett_studio] self-test FAIL: no titles discovered under %s\n",
                     titlesDir.string().c_str());
        return 1;
    }

    bool foundDummy = false;
    bool foundTests = false;
    for (const auto& e : entries) {
        if (e.displayName == L"dummy_title" && e.launchable) foundDummy = true;
        if (e.displayName == L"scarlett_tests" && e.launchable) foundTests = true;
    }
    if (!foundDummy || !foundTests) {
        std::fprintf(stderr, "[scarlett_studio] self-test FAIL: expected dummy_title and scarlett_tests\n");
        return 2;
    }

    // Verify read-only XVD encrypted header rejection in studio_model
    const auto tmpXvd = std::filesystem::temp_directory_path() / L"scarlett_studio_selftest.xvc";
    {
        std::vector<uint8_t> buf(0x220, 0);
        std::memcpy(buf.data() + 0x200, "msft-xvd", 8);
        const uint32_t encFlags = 0x00000041u; // bit 0x2 clear -> encrypted
        std::memcpy(buf.data() + 0x208, &encFlags, sizeof(encFlags));
        std::ofstream f(tmpXvd, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    const auto xvdEntry = sc::gui::InspectXvdFileReadOnly(tmpXvd);
    std::error_code ec;
    std::filesystem::remove(tmpXvd, ec);
    if (xvdEntry.launchable || xvdEntry.statusText.find("Encrypted") == std::string::npos) {
        std::fprintf(stderr, "[scarlett_studio] self-test FAIL: encrypted XVD not rejected\n");
        return 3;
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR lpCmdLine, int nCmdShow) {
    g_app.binDir     = GetExeDir();
    g_app.loaderExe  = g_app.binDir / L"loader.exe";
    g_app.shimsDir   = g_app.binDir / L"shims";
    g_app.configFile = g_app.binDir / L"scarlett_studio.cfg";

    if (lpCmdLine && std::wcsstr(lpCmdLine, L"--self-test") != nullptr) {
        return RunSelfTest(g_app.binDir);
    }

    // Load saved roots or initialize defaults (built-in titles + optional external GDK build dir)
    g_app.libraryRoots = sc::gui::LoadRootsConfig(g_app.configFile);
    if (g_app.libraryRoots.empty()) {
        g_app.libraryRoots.push_back(g_app.binDir / L"titles");
        std::error_code ec;
        const std::filesystem::path extBuild = L"E:\\gta\\external\\build";
        if (std::filesystem::exists(extBuild, ec)) {
            g_app.libraryRoots.push_back(extBuild);
        }
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StudioWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScarlettStudioMainWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Scarlett Studio - Xbox Series Compatibility Launcher & Test Hub",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 740,
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
