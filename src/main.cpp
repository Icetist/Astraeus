#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <memory>
#include <string>

#include "config.h"
#include "drawer.h"
#include "gfx.h"
#include "model.h"
#include "tray.h"

namespace {

constexpr UINT kTrayId = 1;
constexpr UINT_PTR kSecTimer = 1;
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; // per-user autostart
constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
using namespace tray; // menu command ids

struct App {
    std::wstring dir, cfgPath, exePath;
    Config cfg;
    std::unique_ptr<Model> model;
    std::unique_ptr<Drawer> drawer;
    HWND host = nullptr; // hidden window owning tray + timer
    UINT tbMsg = 0; // TaskbarCreated, explorer restarted
    bool appbar = false; // registered for fullscreen notices
    HANDLE mutex = nullptr; // single instance guard
};

App* app = nullptr; // global so window procs can reach it

void Log(const std::wstring& s) {
    std::wstring line = s + L"\r\n";
    HANDLE f = CreateFileW((app->dir + L"\\astraeus.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr); // append, create if missing
    if (f == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr); // size pass, then convert
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
    DWORD written;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
}

bool StartupOn() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    if (_wcsicmp(value, (L"\"" + app->exePath + L"\"").c_str()) != 0) return false; // stale path, exe moved
    BYTE flags[12] = {};
    size = sizeof(flags);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, L"Astraeus", RRF_RT_REG_BINARY, nullptr, flags, &size) == ERROR_SUCCESS)
        return (flags[0] & 1) == 0; // settings > startup toggle: odd first byte = disabled
    return true;
}

void SetStartup(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + app->exePath + L"\""; // quoted for paths with spaces
        RegSetValueExW(key, L"Astraeus", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t))); // byte size incl null
        BYTE flags[12] = {2}; // 2 = enabled, clears a disable from settings
        RegSetKeyValueW(HKEY_CURRENT_USER, kApprovedKey, L"Astraeus", REG_BINARY, flags, sizeof(flags));
    } else {
        RegDeleteValueW(key, L"Astraeus");
    }
    RegCloseKey(key);
}

void AddTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = app->host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY; // clicks come back to HostProc
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(app->host)),
                                  GetSystemMetricsForDpi(SM_CYSMICON, GetDpiForWindow(app->host)), 0);
    wcscpy_s(nid.szTip, L"Astraeus");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = app->host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Notify(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = app->host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND; // silent, we beep ourselves
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowMenu() {
    tray::Show(app->host, app->drawer->Shell(), StartupOn()); // themed popup, picks arrive as WM_COMMAND
}

void Restart() {
    if (app->mutex) {
        ReleaseMutex(app->mutex); // free guard so new instance can start
        CloseHandle(app->mutex);
        app->mutex = nullptr;
    }
    ShellExecuteW(nullptr, L"open", app->exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // relaunch self, then quit
    PostQuitMessage(0);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (app && app->tbMsg != 0 && msg == app->tbMsg) { // explorer restarted, icon lost
        AddTray();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == kSecTimer && app->model) {
            app->model->Tick(); // refresh stats, clock, timer
            app->drawer->OnSecond();
            if (app->model->TimerDone()) { // countdown just hit zero
                std::wstring detail = std::to_wstring(app->model->TimerMins()) + L" min timer is up";
                MessageBeep(MB_ICONASTERISK);
                app->drawer->Peek(L"Timer finished", detail); // in-app drop banner
                Notify(L"Timer finished", detail.c_str()); // plus system toast
            }
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) app->drawer->Toggle(); // left click opens/closes
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(); // right click or keyboard menu
        return 0;
    case WM_APP_APPBAR:
        if (wp == ABN_FULLSCREENAPP && app->cfg.hideFull) app->drawer->SetFull(lp != 0); // hide while game/video fullscreen
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kOpen: app->drawer->Toggle(); break;
        case kConfig: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + app->cfgPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break; // edit ini in notepad
        case kStartup: SetStartup(!StartupOn()); break;
        case kRestart: Restart(); break;
        case kExit: PostQuitMessage(0); break;
        }
        return 0;
    case WM_ENDSESSION:
        if (wp) PostQuitMessage(0); // logoff/shutdown, exit cleanly
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateHost(HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"AstraeusHost";
    RegisterClassExW(&wc);
    app->host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Astraeus", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr); // invisible, no taskbar button
    if (!app->host) return false;
    app->tbMsg = RegisterWindowMessageW(L"TaskbarCreated"); // broadcast when explorer restarts
    AddTray();
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = app->host;
    abd.uCallbackMessage = WM_APP_APPBAR;
    app->appbar = SHAppBarMessage(ABM_NEW, &abd) != FALSE; // appbar only to get fullscreen notices
    SetCoalescableTimer(app->host, kSecTimer, 1000, nullptr, 250); // 1s tick drives model
    return true;
}

void DestroyHost() {
    if (!app->host) return;
    if (app->appbar) {
        APPBARDATA abd = {sizeof(abd)};
        abd.hWnd = app->host;
        SHAppBarMessage(ABM_REMOVE, &abd); // unregister or shell keeps stale entry
    }
    RemoveTray();
    DestroyWindow(app->host);
}

void Run() {
    MSG msg;
    for (;;) {
        Drawer& d = *app->drawer;
        if (d.NeedsFrame() && !d.FrameWait()) d.Frame(); // no waitable yet, draw once to create it
        HANDLE wait = d.NeedsFrame() ? d.FrameWait() : nullptr; // only wait on vsync while animating
        DWORD r = MsgWaitForMultipleObjectsEx(wait ? 1 : 0, wait ? &wait : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE); // sleep until vsync or input, 0 cpu idle
        if (wait && r == WAIT_OBJECT_0) {
            d.Frame();
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { // drain all queued input
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HEAP_OPTIMIZE_RESOURCES_INFORMATION heap = {HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &heap, sizeof(heap)); // heaps hand freed memory back sooner
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // crisp on mixed dpi monitors
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // smoother animation under load
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Astraeus.Native");
    if (!mutex || WaitForSingleObject(mutex, 3000) == WAIT_TIMEOUT) return 0; // wait for old instance on restart, else bail
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1; // sta for wic/shell apis

    App state;
    app = &state;
    state.mutex = mutex;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    state.exePath = exe;
    PathRemoveFileSpecW(exe);
    state.dir = exe;
    state.cfgPath = state.dir + L"\\astraeus.ini";
    state.cfg = Config::Load(state.cfgPath);
    for (const auto& w : state.cfg.warns) Log(w); // bad ini keys go to log

    if (!gfx::Init(state.dir + L"\\fonts")) { // shared d2d/dwrite factories + bundled fonts
        Log(L"Graphics initialisation failed");
        return 1;
    }
    state.model = std::make_unique<Model>(state.cfg);
    state.drawer = std::make_unique<Drawer>(state.cfg, *state.model);
    if (!CreateHost(instance) || !state.drawer->Create(instance)) {
        Log(L"Window creation failed");
        return 1;
    }
    state.model->Start(state.drawer->Hwnd()); // background pollers post to drawer
    if (!StartupOn()) tray::Ask(app->host, state.drawer->Shell()); // asks every launch while off
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0); // trim startup memory

    Run();

    if (state.model->awake) sys::SetAwake(false); // restore normal sleep
    state.drawer.reset(); // drawer before model, it holds a ref
    state.model.reset();
    DestroyHost();
    gfx::Shutdown();
    CoUninitialize();
    if (state.mutex) {
        ReleaseMutex(state.mutex);
        CloseHandle(state.mutex);
    }
    return 0;
}
