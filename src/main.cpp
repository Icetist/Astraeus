#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "backup.h"
#include "config.h"
#include "drawer.h"
#include "gfx.h"
#include "model.h"
#include "glyphs.h"
#include "tray.h"
#include "version.h"

namespace {

constexpr UINT kTrayId = 1;
constexpr int kHotId = 1; // the open-the-panel shortcut
constexpr UINT_PTR kSecTimer = 1;
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; // per-user autostart
constexpr const wchar_t* kBootArg = L"--boot"; // on the startup entry: a launch at sign-in, which greets with the home page
constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr int kBootTries = 20; // at sign-in the gpu, the shell and the screens can lag behind us: ~a minute of tries before giving up
constexpr DWORD kBootWaitMs = 3000;
constexpr unsigned kShellFastTicks = 120; // tray icon and fullscreen notices: retried every second for two minutes, then every half minute
constexpr unsigned kShellSlowTicks = 30;
constexpr LONGLONG kLogMax = 256 << 10; // astraeus.log starts over past this
constexpr UINT_PTR kIniTimer = 2; // astraeus.ini changed: read it once the editor is done writing
constexpr UINT kIniSettleMs = 300;
constexpr int kIniTries = 20; // ~6 s of a file that can't be opened, then it waits for the next change
using namespace tray; // menu command ids

struct App {
    std::wstring dir, cfgPath, exePath;
    Config cfg;
    std::unique_ptr<Model> model;
    std::unique_ptr<Drawer> drawer;
    HWND host = nullptr; // hidden window owning tray + timer
    UINT tbMsg = 0; // TaskbarCreated, explorer restarted
    HICON trayIcon = nullptr; // ours to free when the taskbar's theme swaps it
    bool trayOn = false; // the shell took the icon; early at sign-in it may not be there yet
    bool appbar = false; // registered for fullscreen notices
    HANDLE mutex = nullptr; // single instance guard
    sys::Updater updater;
    unsigned ticks = 0; // seconds since launch, schedules update checks
    bool restartPending = false; // new exe swapped in, restart once idle
    HANDLE iniWatch = INVALID_HANDLE_VALUE; // the exe's folder, told when a file in it is written
    FILETIME iniTime{}; // astraeus.ini as last read
    int iniTries = 0; // reads put off because the file couldn't be opened
};

App* app = nullptr; // global so window procs can reach it

void Log(const std::wstring& s) {
    std::wstring line = s + L"\r\n", path = app->dir + L"\\astraeus.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && (((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > kLogMax)
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING); // one generation kept, never grows without end
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr); // append, create if missing
    if (f == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr); // size pass, then convert
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
    DWORD written;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
}

std::wstring InstallDir() { // %LOCALAPPDATA%\Programs\Astraeus, per-user, no admin needed
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &base))) dir = std::wstring(base) + L"\\Astraeus";
    CoTaskMemFree(base);
    return dir;
}

bool CopyIfChanged(const std::wstring& from, const std::wstring& to) {
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &a)) return false;
    if (GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &b) && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) <= 0)
        return true; // target same or newer (e.g. auto-updated), keep it
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
}

bool Install(const std::wstring& dst) { // copy the exe (fonts are inside it) so boot never depends on the build folder
    if (dst.empty()) return false;
    int r = SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr); // creates parents too
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) return false;
    if (!CopyIfChanged(app->exePath, dst + L"\\Astraeus.exe")) return false;
    DeleteFileW((dst + L"\\Astraeus.exe:Zone.Identifier").c_str()); // the download's internet mark came along with the copy: at sign-in windows may hold it for a security prompt nobody sees
    std::wstring ini = dst + L"\\astraeus.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(app->cfgPath.c_str(), ini.c_str(), TRUE); // carry settings over once, never clobber
    return true;
}

void UndoRename() { // 1.2.11 called itself astraeus island for a day: its startup entry, its switch and its shortcut go back to plain astraeus
    constexpr const wchar_t* kRenamed = L"Astraeus Island";
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRenamed, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", REG_SZ, value, size);
        BYTE flags[12] = {};
        DWORD fsize = sizeof(flags);
        if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRenamed, RRF_RT_REG_BINARY, nullptr, flags, &fsize) == ERROR_SUCCESS)
            RegSetKeyValueW(HKEY_CURRENT_USER, kApprovedKey, L"Astraeus", REG_BINARY, flags, fsize); // on or off, as the user left it
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRenamed);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRenamed);
    }
    PWSTR programs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        DeleteFileW((std::wstring(programs) + L"\\Astraeus Island.lnk").c_str()); // StartShortcut writes the plain one
        CoTaskMemFree(programs);
    }
}

std::wstring BootExe() { // exe the run key launches at sign-in, unquoted, empty if none
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return L"";
    std::wstring v = value;
    if (v.size() >= 2 && v.front() == L'"') v = v.substr(1, v.find(L'"', 1) - 1); // strip quotes
    return v;
}

bool StartupOn() { // any existing copy counts, wherever it lives
    std::wstring exe = BootExe();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false; // no entry, or exe deleted
    BYTE flags[12] = {};
    DWORD size = sizeof(flags);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, L"Astraeus", RRF_RT_REG_BINARY, nullptr, flags, &size) == ERROR_SUCCESS)
        return (flags[0] & 1) == 0; // settings > startup toggle: odd first byte = disabled
    return true;
}

void SetBoot(const std::wstring& exe) { // point the run key at exe and clear any settings disable
    std::wstring cmd = L"\"" + exe + L"\" " + kBootArg; // quoted for paths with spaces
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    BYTE flags[12] = {2}; // 2 = enabled
    RegSetKeyValueW(HKEY_CURRENT_USER, kApprovedKey, L"Astraeus", REG_BINARY, flags, sizeof(flags));
}

void Restart(const std::wstring& exe);
void SetStartup(bool on) {
    std::wstring dst = InstallDir();
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus");
        return;
    }
    if (!Install(dst)) { // nothing safe to point at
        Log(L"Could not copy Astraeus to " + dst);
        return;
    }
    SetBoot(dst + L"\\Astraeus.exe");
    if (_wcsicmp(app->dir.c_str(), dst.c_str()) != 0) Restart(dst + L"\\Astraeus.exe"); // switch over to the installed copy
}

void StartShortcut() { // a shortcut in the start menu, so astraeus can be found again after it's closed; rewritten only when it points somewhere else
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
    std::wstring lnk = std::wstring(programs) + L"\\Astraeus.lnk";
    CoTaskMemFree(programs);
    std::wstring target = app->exePath, inst = InstallDir() + L"\\Astraeus.exe";
    if (_wcsicmp(BootExe().c_str(), inst.c_str()) == 0 && GetFileAttributesW(inst.c_str()) != INVALID_FILE_ATTRIBUTES) target = inst; // the copy that starts with windows and gets updated
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file))) return;
    wchar_t now[MAX_PATH] = {};
    if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(link->GetPath(now, MAX_PATH, nullptr, SLGP_RAWPATH)) && _wcsicmp(now, target.c_str()) == 0)
        return; // already right: nothing written
    std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(dir.c_str());
    link->SetDescription(L"Astraeus");
    link->SetIconLocation(target.c_str(), 0);
    file->Save(lnk.c_str(), TRUE);
}

void MarkBoot() { // entries written before the greeting existed get the flag once
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return;
    std::wstring v = value;
    if (v.empty() || v.front() != L'"' || v.find(kBootArg) != std::wstring::npos) return; // unquoted: a path with words after it wouldn't parse
    v = v + L" " + kBootArg;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Astraeus", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); // startup approval untouched: on or off as the user left it
}

void SyncBoot() { // launch-time housekeeping once startup is known to be on
    std::wstring dst = InstallDir(), inst = dst + L"\\Astraeus.exe", boot = BootExe();
    if (_wcsicmp(app->dir.c_str(), dst.c_str()) == 0) { // we are the installed copy
        DeleteFileW((app->exePath + L":Zone.Identifier").c_str()); // copies installed before this kept the download's mark
        return;
    }
    if (_wcsicmp(boot.c_str(), app->exePath.c_str()) == 0) { // entry points at this build folder: move it somewhere safe
        if (Install(dst)) SetBoot(inst);
    } else if (_wcsicmp(boot.c_str(), inst.c_str()) == 0) {
        Install(dst); // newer build run: refresh the boot copy
    } // else a copy the user placed elsewhere, leave it alone
}

bool LightTaskbar() { // the taskbar's own light/dark setting, not the panel's look or the apps' mode
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD,
                 nullptr, &v, &size);
    return v != 0;
}

HICON TrayIcon() { // the white mark, or the same mark in near-black on a light taskbar
    UINT dpi = GetDpiForWindow(app->host);
    HICON white = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                    GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
    if (!white || !LightTaskbar()) return white;
    ICONINFO ii{};
    if (!GetIconInfo(white, &ii)) return white;
    if (!ii.hbmColor) {
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return white;
    }
    BITMAP bm{};
    GetObjectW(ii.hbmColor, sizeof(bm), &bm);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
    HICON dark = nullptr;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color && GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, bits, &bi, DIB_RGB_COLORS)) {
        auto* px = static_cast<BYTE*>(bits);
        for (int i = 0; i < bm.bmWidth * bm.bmHeight; ++i) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0x1C; // keep the alpha, darken the ink
        ICONINFO di = ii;
        di.hbmColor = color;
        dark = CreateIconIndirect(&di);
    }
    ReleaseDC(nullptr, dc);
    if (color) DeleteObject(color);
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (!dark) return white;
    DestroyIcon(white);
    return dark;
}

void SetTrayIcon(NOTIFYICONDATAW& nid) {
    HICON old = app->trayIcon;
    app->trayIcon = TrayIcon();
    nid.hIcon = app->trayIcon;
    nid.uFlags |= NIF_ICON;
    if (old) DestroyIcon(old); // the shell copied it
}

void AddTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = app->host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY; // clicks come back to HostProc
    SetTrayIcon(nid);
    wcscpy_s(nid.szTip, L"Astraeus");
    app->trayOn = Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid); // already there (a tray host's re-add call): kept as it is
}

void AddAppbar() { // only for the shell's fullscreen notices; a restarted explorer forgets it, so it's made again
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = app->host;
    abd.uCallbackMessage = WM_APP_APPBAR;
    if (app->appbar) SHAppBarMessage(ABM_REMOVE, &abd); // the old registration, if the shell still has it
    app->appbar = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
}

void RetryShell() { // early at sign-in explorer may not take the icon or the appbar yet: asked again until it does
    if (app->trayOn && app->appbar) return;
    if (app->ticks > kShellFastTicks && app->ticks % kShellSlowTicks) return; // a shell that never comes: a calm retry
    if (!app->trayOn) AddTray();
    if (!app->appbar) AddAppbar();
    if (app->trayOn && app->appbar) Log(L"Shell ready after " + std::to_wstring(app->ticks) + L" s");
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

FILETIME IniTime() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(app->cfgPath.c_str(), GetFileExInfoStandard, &fa);
    return fa.ftLastWriteTime;
}

void ReloadIni() { // astraeus.ini was saved: read it again, no restart needed
    FILETIME now = IniTime();
    if (CompareFileTime(&now, &app->iniTime) == 0) return; // the log or another file in the folder, not the config
    HANDLE probe = CreateFileW(app->cfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) { // mid-save (renamed away) or held by the editor: looked at again shortly, never read as missing and reset to defaults
        if (++app->iniTries < kIniTries) SetTimer(app->host, kIniTimer, kIniSettleMs, nullptr);
        return;
    }
    CloseHandle(probe);
    app->iniTries = 0;
    app->iniTime = now;
    UINT mods = app->cfg.hotMods, key = app->cfg.hotKey;
    app->cfg = Config::Load(app->cfgPath); // the drawer and model hold a reference to this same object
    for (const auto& w : app->cfg.warns) Log(w);
    if (mods != app->cfg.hotMods || key != app->cfg.hotKey) { // a new shortcut
        UnregisterHotKey(app->host, kHotId);
        if (app->cfg.hotKey && !RegisterHotKey(app->host, kHotId, app->cfg.hotMods | MOD_NOREPEAT, app->cfg.hotKey)) Log(L"The shortcut is taken by another app");
    }
    if (app->drawer) app->drawer->Reconfig();
    Log(L"Reloaded astraeus.ini");
}

void Restart(const std::wstring& exe) {
    if (app->mutex) {
        ReleaseMutex(app->mutex); // free guard so new instance can start
        CloseHandle(app->mutex);
        app->mutex = nullptr;
    }
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // relaunch self, then quit
    PostQuitMessage(0);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (app && msg == WM_SETTINGCHANGE && wp == 0 && lp && !wcsncmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet", 18)) { // taskbar went light or dark
        NOTIFYICONDATAW nid = {sizeof(nid)};
        nid.hWnd = app->host;
        nid.uID = kTrayId;
        SetTrayIcon(nid);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        return 0;
    }
    if (app && app->tbMsg != 0 && msg == app->tbMsg) { // explorer restarted, icon and appbar lost
        AddTray();
        AddAppbar();
        if (app->drawer) app->drawer->SetFull(false); // its "fullscreen is over" died with it
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == kIniTimer) {
            KillTimer(hwnd, kIniTimer);
            ReloadIni();
            return 0;
        }
        if (wp == kSecTimer && app->model) {
            app->model->Tick(); // refresh stats, clock, timer
            ++app->ticks;
            RetryShell();
            if (app->ticks == 3) app->model->Prefetch(); // weather is ready before the panel first opens
            if (app->cfg.autoUpdate && (app->ticks == 30 || app->ticks % 10800 == 0)) // 30s after launch, then every 3h
                app->updater.Check(app->host, WM_APP_UPDATE);
            if (app->restartPending && !app->drawer->Busy()) Restart(app->exePath); // never yank the panel away mid-use
            app->drawer->OnSecond();
            if (int ev = app->model->focusEvent) { // a focus block or a break just ended
                app->model->focusEvent = 0;
                const wchar_t* title = ev == 1 ? L"Focus done" : L"Break's over";
                std::wstring detail = ev == 1 ? std::to_wstring(app->model->breakMin) + L" min break, then round " + std::to_wstring(app->model->focusRound + 1)
                                              : L"Round " + std::to_wstring(app->model->focusRound) + L", " + std::to_wstring(app->model->focusMin) + L" min";
                MessageBeep(MB_ICONASTERISK);
                app->drawer->Peek(title, detail);
                Notify(title, detail.c_str());
            }
            if (app->model->TimerDone()) { // countdown just hit zero
                std::wstring detail = std::to_wstring(app->model->TimerMins()) + L" min timer is up";
                MessageBeep(MB_ICONASTERISK);
                app->drawer->Peek(L"Timer finished", detail); // in-app drop banner
                Notify(L"Timer finished", detail.c_str()); // plus system toast
            }
        }
        return 0;
    case WM_APP_UPDATE:
        switch (wp) {
        case sys::Updater::kInstalled:
            tray::SetUpdate(tray::Upd::Installed, app->updater.Applied());
            app->restartPending = true;
            break;
        case sys::Updater::kDownloading: tray::SetUpdate(tray::Upd::Downloading, app->updater.Applied()); break;
        case sys::Updater::kCurrent: tray::SetUpdate(tray::Upd::Current); break;
        default: tray::SetUpdate(tray::Upd::Failed); break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == kHotId && app->drawer) app->drawer->KeyOpen(); // the shortcut
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
        case kSettings: app->drawer->ShowSettings(); break;
        case kConfig: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + app->cfgPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break; // edit ini in notepad
        case kStartup: SetStartup(!StartupOn()); break;
        case kRestart: Restart(app->exePath); break;
        case kUpdate: app->updater.Check(app->host, WM_APP_UPDATE, true); break; // the tray row shows the outcome
        case kExit: PostQuitMessage(0); break;
        case kWake: // started again by hand: show up, whatever sign-in left behind
            app->drawer->Wake();
            if (!app->trayOn) AddTray();
            break;
        case kPlayFull: app->drawer->PlayFull(); break; // halobar's album cover
        case kExport: // from settings: windows' own dialogs, owned by the host so the panel can go
            SetForegroundWindow(hwnd); // we were just clicked: the dialog may come to the front
            if (backup::Export(hwnd, app->cfgPath)) Notify(L"Settings exported", L"Import the file on any PC to bring them back");
            break;
        case kImport:
            SetForegroundWindow(hwnd);
            if (backup::Import(hwnd, app->cfgPath)) Restart(app->exePath); // a clean start on the imported settings
            break;
        case kReset:
            backup::Reset();
            Restart(app->exePath);
            break;
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
    ChangeWindowMessageFilterEx(app->host, WM_COMMAND, MSGFLT_ALLOW, nullptr); // halobar's cover and a second launch reach us even across integrity levels
    ChangeWindowMessageFilterEx(app->host, app->tbMsg, MSGFLT_ALLOW, nullptr);
    AddTray();
    AddAppbar();
    if (!app->trayOn || !app->appbar) Log(L"Shell not ready at start, retrying");
    SetCoalescableTimer(app->host, kSecTimer, 1000, nullptr, 250); // 1s tick drives model
    if (app->cfg.hotKey && !RegisterHotKey(app->host, kHotId, app->cfg.hotMods | MOD_NOREPEAT, app->cfg.hotKey)) // windows tells us when it's pressed, nothing polls
        Log(L"The shortcut is taken by another app; set another with hotkey = in astraeus.ini");
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

wchar_t crashLog[MAX_PATH] = {}; // filled at start: the crash handler mustn't build strings

LONG WINAPI Crashed(EXCEPTION_POINTERS* ep) { // the reason goes to astraeus.log, so a start that died at sign-in can be told apart from one that never ran
    HANDLE f = crashLog[0] ? CreateFileW(crashLog, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr) : INVALID_HANDLE_VALUE;
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    void* at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &mod);
    wchar_t name[MAX_PATH] = L"?";
    if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
    char line[MAX_PATH * 3 + 96];
    int n = _snprintf_s(line, _TRUNCATE, "Crashed: code %08lX at %ls+0x%llX\r\n", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0, name,
                        (unsigned long long)((const char*)at - (const char*)mod));
    DWORD written;
    if (n > 0) WriteFile(f, line, (DWORD)n, &written, nullptr);
    CloseHandle(f);
    return EXCEPTION_CONTINUE_SEARCH; // windows' own error reporting still runs
}

template <class F> bool Boot(const wchar_t* what, F&& step) { // a start step retried with a pause instead of quitting: at sign-in it can fail only because it's early
    for (int i = 1; i <= kBootTries; ++i) {
        if (step()) {
            if (i > 1) Log(std::wstring(what) + L" worked on try " + std::to_wstring(i));
            return true;
        }
        if (i == 1) Log(std::wstring(what) + L" failed, retrying");
        Sleep(kBootWaitMs);
    }
    Log(std::wstring(what) + L" failed, giving up");
    return false;
}

void Run() {
    MSG msg;
    for (;;) {
        Drawer& d = *app->drawer;
        if (d.NeedsFrame() && !d.FrameWait()) d.Frame(); // no waitable yet, draw once to create it
        HANDLE waits[2];
        DWORD n = 0;
        HANDLE frame = d.NeedsFrame() ? d.FrameWait() : nullptr; // only wait on vsync while animating
        if (frame) waits[n++] = frame;
        if (app->iniWatch != INVALID_HANDLE_VALUE) waits[n++] = app->iniWatch; // a write to astraeus.ini wakes us, nothing polls it
        DWORD r = MsgWaitForMultipleObjectsEx(n, n ? waits : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE); // sleep until vsync or input, 0 cpu idle
        if (r < WAIT_OBJECT_0 + n && waits[r - WAIT_OBJECT_0] == app->iniWatch) { // the folder changed: settle, then look at the ini
            FindNextChangeNotification(app->iniWatch);
            SetTimer(app->host, kIniTimer, kIniSettleMs, nullptr);
        }
        if (frame && r == WAIT_OBJECT_0) d.Frame(); // then fall through: glass commits its blur outline from queued messages, it must not wait a frame
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
    if (!mutex || WaitForSingleObject(mutex, 3000) == WAIT_TIMEOUT) { // wait for old instance on restart, else bail
        if (HWND running = FindWindowW(L"AstraeusHost", nullptr)) PostMessageW(running, WM_COMMAND, tray::kWake, 0); // opened by hand while it runs: that one shows itself
        return 0;
    }
    if (FAILED(OleInitialize(nullptr))) return 1; // an sta for wic and the shell, plus ole for the shelf's drag and drop
    SetUnhandledExceptionFilter(Crashed);

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
    wcscpy_s(crashLog, (state.dir + L"\\astraeus.log").c_str());
    Log(L"Start " APP_VER_W L", " + std::to_wstring(GetTickCount64() / 1000) + L" s after boot, " + state.exePath); // one line a launch: a start at sign-in that never showed still leaves this

    if (!Boot(L"Graphics initialisation", [&] { return gfx::Init(state.dir + L"\\fonts"); })) return 1; // shared d2d/dwrite factories + bundled fonts
    state.model = std::make_unique<Model>(state.cfg);
    bool made = Boot(L"Window creation", [&] {
        if (CreateHost(instance)) {
            state.drawer = std::make_unique<Drawer>(state.cfg, *state.model);
            if (state.drawer->Create(instance)) return true;
        }
        state.drawer.reset(); // half made: down again, the next try starts clean
        DestroyHost();
        app->host = nullptr;
        return false;
    });
    if (!made) return 1;
    state.model->Start(state.drawer->Hwnd()); // background pollers post to drawer
    UndoRename(); // before the startup entry is read
    if (!StartupOn()) tray::Ask(app->host, state.drawer->Shell()); // asks every launch while off
    else SyncBoot(), MarkBoot();
    if (wcsstr(GetCommandLineW(), kBootArg)) state.drawer->Greet(); // started with windows: the home page says hello until the mouse moves
    StartShortcut(); // after the boot copy is settled, so the shortcut points at it
    if (sys::FinishUpdate()) { // first run after a swap
        if (!state.model->quiet) state.drawer->Peek(L"Astraeus updated", L"Now on version " APP_VER_W, glyph::Sparkle); // do not disturb: the tray still says so
        tray::SetUpdate(tray::Upd::Updated); // the tray says so too until the next check
    }
    gfx::GiveBack(); // trim startup memory
    state.iniTime = IniTime();
    state.iniWatch = FindFirstChangeNotificationW(state.dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE); // edits to astraeus.ini apply live

    Run();

    if (state.iniWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(state.iniWatch);
    if (state.model->awake) sys::SetAwake(false); // restore normal sleep
    state.drawer.reset(); // drawer before model, it holds a ref
    state.model.reset();
    DestroyHost();
    gfx::Shutdown();
    OleUninitialize();
    if (state.mutex) {
        ReleaseMutex(state.mutex);
        CloseHandle(state.mutex);
    }
    return 0;
}
