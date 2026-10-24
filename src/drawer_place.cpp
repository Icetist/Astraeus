// Where the drawer lives: its screen and edge, the hover strips on the other screens, and stepping aside for fullscreen apps.
#include "drawer.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr UINT kFullCheckMs = 250; // the shell's fullscreen notice is checked against the window in front this often
constexpr int kFullChecks = 4; // ~1 s: a real game has the screen by then, an overlay's blink is gone
constexpr UINT kDeskSettleMs = 300; // show desktop and minimize animate for about this long: one look once the windows have landed
constexpr DWORD kDeskFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS; // posted to our queue, nothing injected into other apps

Drawer* deskSelf = nullptr; // the hooks' callback has no context of its own; there's one drawer

bool IsClass(HWND w, std::initializer_list<const wchar_t*> names) {
    wchar_t cls[40] = {};
    if (!w || !GetClassNameW(w, cls, 40)) return false;
    for (const wchar_t* n : names)
        if (!wcscmp(cls, n)) return true;
    return false;
}

bool DeskFront(HWND w) { return IsClass(w, {L"Progman", L"WorkerW"}); } // show desktop, or a click on the wallpaper
bool ShellFront(HWND w) { return DeskFront(w) || IsClass(w, {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}); } // nothing that maximizes or snaps

}

void Drawer::Place() {
    MONITORINFO mi = {sizeof(mi)};
    if (!mon || !GetMonitorInfoW(mon, &mi)) { // primary, or the screen we were sent to is gone
        mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
    }
    UINT dpiX = 96, dpiY = 96;
    scale = (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) ? dpiX / 96.0f : GetDpiForWindow(hwnd) / 96.0f) * Zoom(); // the target screen's dpi, times the size setting
    monitor = mi.rcMonitor; // full rect, not work area, so it sits on the top edge
    DWM_TIMING_INFO ti = {sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator > 0) { // 144hz+ screens would otherwise run ambient at 72-120 fps
        constexpr double kAmbientFps = 30, kPillFps = 15; // the pill's bars are 14 dips tall: lively at 15, half the cost of 30
        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
        ambientSync = (UINT)(std::clamp)((int)std::lround(hz / kAmbientFps), 1, 4); // dxgi caps sync interval at 4
        pillSync = (UINT)(std::max)(1L, std::lround(hz / kPillFps)); // glass paces with a timer, no cap; the swapchain clamps it below
    }
    sideH = (std::min)(kSideMaxH, (mi.rcMonitor.bottom - mi.rcMonitor.top) / scale - 2 * cfg.fillet - 60); // sidebar fits the screen
    fastSync = 1; // a new screen, maybe a new refresh rate: judge it afresh
    framesSeen = framesLate = 0;
    Resize(winH > 0 ? winH : kStrip, false);
    if (deskOn) SetTimer(hwnd, kDeskTimer, kDeskSettleMs, nullptr); // another screen or edge: another patch of it to look under
}

RECT Drawer::Footprint() const { // the screen area the open panel drops into, in pixels
    int a = (int)(Span() * scale + 0.5f), d = (int)(Depth() * scale + 0.5f);
    if (edgeNow == 0) {
        LONG x = monitor.left + (monitor.right - monitor.left - a) / 2;
        return {x, monitor.top, x + a, monitor.top + d};
    }
    LONG y = monitor.top + (monitor.bottom - monitor.top - a) / 2;
    return edgeNow == 1 ? RECT{monitor.left, y, monitor.left + d, y + a} : RECT{monitor.right - d, y, monitor.right, y + a};
}

bool Drawer::AppBelow() const { // any app window under where the panel would drop: the pill comes first, the panel only on a tap
    struct Ctx {
        RECT area;
        DWORD self;
        bool found;
    } c{Footprint(), GetCurrentProcessId(), false};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL { // top level windows only, front to back
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
        LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
        if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT))) return TRUE; // tooltips, overlays, docks: not an app being worked in
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == c->self) return TRUE; // our own stage, prompts and strips
        wchar_t cls[40] = {};
        GetClassNameW(w, cls, 40);
        for (const wchar_t* skip : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
            if (!wcscmp(cls, skip)) return TRUE; // the desktop and taskbars
        if (!wcsncmp(cls, L"HaloBar", 7) || !wcsncmp(cls, L"Astraeus", 8)) return TRUE;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE; // another desktop, a suspended app
        RECT r{}, hit{};
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(w, &r)) return TRUE; // the visible frame, not the invisible resize border
        if (!IntersectRect(&hit, &r, &c->area)) return TRUE;
        c->found = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

void Drawer::WatchDesk() { // only window events wake us, and only while the option is on: nothing polls
    UnwatchDesk();
    deskOn = model.pillDesk != 0;
    if (!deskOn) {
        if (!opened) SyncStay(); // a pill held up by the desktop alone goes
        return;
    }
    deskSelf = this;
    deskHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, DeskEvent, 0, 0, kDeskFlags); // win+d, a click on the wallpaper, the last window closing
    deskHooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, DeskEvent, 0, 0, kDeskFlags); // dragged under or away
    deskHooks[2] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, DeskEvent, 0, 0, kDeskFlags); // minimize all, restore
    OnDeskTimer(); // the first look right away
}

void Drawer::UnwatchDesk() {
    for (HWINEVENTHOOK& h : deskHooks)
        if (h) UnhookWinEvent(h), h = nullptr;
    if (frontHook) UnhookWinEvent(frontHook), frontHook = nullptr;
    frontThread = 0;
    deskOn = deskView = false;
    if (hwnd) KillTimer(hwnd, kDeskTimer);
}

void Drawer::HookFront(HWND front) {
    DWORD tid = front && !ShellFront(front) ? GetWindowThreadProcessId(front, nullptr) : 0; // the desktop and taskbars never come under the island
    if (tid == frontThread) return;
    if (frontHook) UnhookWinEvent(frontHook), frontHook = nullptr;
    frontThread = tid;
    if (!tid) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    frontHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, DeskEvent, pid, tid, kDeskFlags); // one thread's windows, not every cursor move on the system
}

void CALLBACK Drawer::DeskEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG child, DWORD, DWORD) {
    if (!deskSelf || !deskSelf->deskOn || !w || obj != OBJID_WINDOW || child != CHILDID_SELF) return; // carets, scrollbars, controls: not a window
    if (ev == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(w, GA_ROOT) != w) return;
    SetTimer(deskSelf->hwnd, kDeskTimer, kDeskSettleMs, nullptr); // a burst of events, a drag: one look once they stop
}

void Drawer::OnDeskTimer() {
    if (!deskOn) return;
    HWND front = GetForegroundWindow();
    HookFront(front);
    bool view = DeskFront(front) || !AppBelow(); // the desktop has the focus, or nothing of an app's lies under the island
    if (view == deskView) return;
    deskView = view;
    if (!opened) SyncStay(); // shows or goes now, not on the next second
}

void Drawer::ApplyPlace() {
    edgeNow = model.Edge();
    screensNow = model.screens;
    screenNow = model.screen;
    if (!screensNow) mon = sys::FindScreen(screenNow); // one screen: the picked one, primary if none
    island = islandLook = false; // a resting pill goes with the old edge; the next second brings it back on the new one
    spring = anim::Spring{};
    winH = kStrip; // Place keeps the window this deep: the plain strip, never an empty pill-deep band
    FreeDev(); // swapchain is sized for the old edge
    Place();
    BuildStrips();
    needsFrame = false;
    SyncHide(); // the new screen may or may not have a game on it
    Log(L"placed from settings");
}

LRESULT CALLBACK Drawer::StripProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE) { // hand the hover to the panel: it moves to this screen and takes it from there
        HWND owner = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (owner) PostMessageW(owner, kAdoptMsg, (WPARAM)MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), 0);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

void Drawer::FreeStrips() {
    for (HWND s : strips) DestroyWindow(s);
    strips.clear();
}

void Drawer::BuildStrips() {
    FreeStrips();
    if (screensNow != 1) return; // primary only: nothing to add
    static bool registered = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = StripProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AstraeusStrip";
        registered = RegisterClassExW(&wc) != 0;
    }
    struct Ctx { Drawer* self; HINSTANCE inst; };
    Ctx c{this, inst};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT r, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        Drawer* d = c->self;
        if (m == d->mon) return TRUE; // the panel itself lives here
        UINT dx = 96, dy = 96;
        float s = SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
        int along = (int)(d->cfg.hoverW * s + 0.5f), deep = (std::max)(2, (int)(kStrip * s + 0.5f));
        int x, y, w, h;
        if (d->edgeNow == 0) { w = along, h = deep; x = r->left + (r->right - r->left - w) / 2; y = r->top; }
        else { w = deep, h = along; x = d->edgeNow == 1 ? r->left : r->right - w; y = r->top + (r->bottom - r->top - h) / 2; }
        HWND sw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"AstraeusStrip", L"",
                                  WS_POPUP, x, y, w, h, nullptr, nullptr, c->inst, nullptr); // no surface, nothing drawn, just catches the mouse
        if (sw) {
            SetWindowLongPtrW(sw, GWLP_USERDATA, (LONG_PTR)d->hwnd);
            ShowWindow(sw, SW_SHOWNOACTIVATE);
            d->strips.push_back(sw);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
}

void Drawer::SetFull(bool on) {
    if (on && stage) return; // that's our own fullscreen player, not a game: the panel comes back when it leaves
    if (on && !fullApp) { // the shell can be fooled by a screen sized overlay: believed only once a real fullscreen app is seen in front
        fullChecks = kFullChecks;
        SetTimer(hwnd, kFullTimer, kFullCheckMs, nullptr);
        return;
    }
    fullChecks = 0;
    KillTimer(hwnd, kFullTimer);
    fullApp = on;
    SyncHide();
}

void Drawer::OnFullTimer() { // the shell said fullscreen: is there really one in front?
    if (FullOn(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY))) {
        fullChecks = 0;
        fullApp = true;
        SyncHide();
    } else if (--fullChecks > 0) {
        SetTimer(hwnd, kFullTimer, kFullCheckMs, nullptr);
    } else {
        Log(L"fullscreen notice ignored, nothing fullscreen in front");
    }
}

void Drawer::Wake() {
    fullApp = false; // started by hand while running: nothing may keep it out of sight
    fullChecks = 0;
    KillTimer(hwnd, kFullTimer);
    if (!screensNow) mon = sys::FindScreen(screenNow);
    Place(); // the screens may have settled differently since sign-in
    BuildStrips();
    hidden = false;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // back over whatever topmost came up after us
    for (HWND s : strips) SetWindowPos(s, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SyncHide(); // a real game in front still hides it
    Log(L"woken by a second launch");
}

void Drawer::SyncHide() {
    // windows sends its fullscreen notice for the main screen only: it hides the panel there, any other screen is checked directly
    bool onMain = mon == MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    bool hide = cfg.hideFull && ((fullApp && onMain) || FullOn(mon));
    if (hide == hidden) return;
    hidden = hide;
    Log(hide ? L"hide, fullscreen app" : L"show again");
    if (hide) {
        model.CancelPass(); // a password being typed never outlives the panel
        KillTimer(hwnd, kHoverTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kPeekTimer);
        armed = peeking = dragging = island = islandLook = grow = false;
        if (GetCapture() == hwnd) ReleaseCapture(); // mid-drag when the game took over
        if (opened) { // same teardown as Close, minus the animation
            opened = false;
            model.CloseBt();
            model.CloseWifi();
            model.CloseSettings();
            model.CloseCal();
            model.CloseMixer();
            model.SetLive(false, false);
        }
        spring = anim::Spring{}; // reset instantly, no close anim
        Resize(kStrip, false);
        ui.Forget();
        ShowWindow(hwnd, SW_HIDE);
        needsFrame = false;
        FreeDev(); // a game is fullscreen, give the gpu back now
    } else {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

bool Drawer::Overlay(HWND w) {
    if (!IsWindowVisible(w) || IsIconic(w)) return true; // nothing on screen to be fullscreen with
    wchar_t cls[40] = {};
    GetClassNameW(w, cls, 40);
    if (!wcsncmp(cls, L"HaloBar", 7) || !wcsncmp(cls, L"Astraeus", 8)) return true; // halobar's dock and menus, or one of ours
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return true; // game bars, recorders, chat overlays: they float over apps, they aren't one
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return true; // a click-through layer the size of the screen (geforce, discord, steam)
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked; // there but never drawn: a suspended app, another desktop
}

bool Drawer::FullOn(HMONITOR m) const { // the window in front fills that whole screen: a game, a video, a presentation
    HWND fg = GetForegroundWindow();
    if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != m || Overlay(fg)) return false;
    LONG style = GetWindowLongW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false; // a window with a title bar is maximized, not fullscreen, even under an auto-hidden taskbar
    if (IsZoomed(fg) && (style & WS_THICKFRAME)) return false; // maximized and drawing its own title bar (a terminal, a browser): fullscreen drops the sizing frame
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false; // our own fullscreen player
    wchar_t cls[40] = {};
    GetClassNameW(fg, cls, 40);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
                                 L"ForegroundStaging", L"Windows.UI.Core.CoreWindow"})
        if (!wcsncmp(cls, shell, wcslen(shell))) return false; // the desktop, taskbars, alt-tab, task view and the start menu cover screens too; "_WASDK" and the like are the same shell
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(m, &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
