#include "drawer.h"

#include "glyphs.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "shcore.lib")

namespace {

constexpr int kPageCount = 5; // home, media, system, network, tools: the keys walk them
constexpr UINT_PTR kHoverTimer = 1, kCloseTimer = 2, kReleaseTimer = 3, kPeekTimer = 4, kDevTimer = 5, kFollowTimer = 6;
constexpr UINT kFollowMs = 120; // every screen mode, open: how often to look which screen the cursor is on
constexpr int kDevTries = 30; // windows lets go of an old composition target a moment later: ~3 seconds of retries, 100 ms apart

bool PillStays(const Model& m) { return m.IslandAlways() && m.hasTrack && m.track.playing; } // always means: while something is playing
constexpr float kStrip = 3; // idle bezel height in dips, just enough to catch the mouse
constexpr float kFling = 800; // dips/s, fast flick decides open/close
constexpr float kOpenAt = 0.4f; // slow drag past 40% height opens
constexpr float kTap = 4; // moved less than this = click, not drag

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

}

Drawer::Drawer(const Config& c, Model& m) : cfg(c), model(m) {
    W = cfg.panW + 2 * cfg.fillet; // room for curved shoulders on both sides
    H = cfg.panH;
    accentModeNow = model.accentMode;
    accentHexNow = model.accentHex;
    shellSeed = mediaSeed = accentModeNow ? theme::FromCol(Color::Rgb((uint32_t)accentHexNow)) : Seed{}; // wallpaper theme fills in later
    lookNow = model.look;
    bgNow = model.customBg;
    edgeNow = model.edge;
    screensNow = model.screens;
    screenNow = model.screen;
    shellFrom = shellTo = shellNow = mediaFrom = mediaTo = mediaNow = Make(shellSeed);
}

Drawer::~Drawer() {
    FreeStrips();
    FreeDev();
    if (hwnd) DestroyWindow(hwnd);
}

bool Drawer::Create(HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AstraeusDrawer";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, // no focus steal, no gdi surface (dcomp only)
                           wc.lpszClassName, L"Astraeus", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this); // this -> WM_NCCREATE
    if (!hwnd) return false;
    BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off)); // no dwm fade, we animate ourselves
    if (!screensNow) mon = sys::FindScreen(screenNow); // the picked screen, primary if none or unplugged
    Place();
    BuildStrips();
    SyncWall();
    shellNow = shellTo; // start on final colors, no fade at boot
    mediaNow = mediaTo;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Log(L"started");
    return true;
}

void Drawer::Place() {
    MONITORINFO mi = {sizeof(mi)};
    if (!mon || !GetMonitorInfoW(mon, &mi)) { // primary, or the screen we were sent to is gone
        mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
    }
    UINT dpiX = 96, dpiY = 96;
    scale = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) ? dpiX / 96.0f : GetDpiForWindow(hwnd) / 96.0f; // the target screen's dpi
    monitor = mi.rcMonitor; // full rect, not work area, so it sits on the top edge
    DWM_TIMING_INFO ti = {sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator > 0) { // 144hz+ screens would otherwise run ambient at 72-120 fps
        constexpr double kAmbientFps = 30;
        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
        ambientSync = (UINT)(std::clamp)((int)std::lround(hz / kAmbientFps), 1, 4); // dxgi caps sync interval at 4
    }
    sideH = (std::min)(kSideMaxH, (mi.rcMonitor.bottom - mi.rcMonitor.top) / scale - 2 * cfg.fillet - 60); // sidebar fits the screen
    fastSync = 1; // a new screen, maybe a new refresh rate: judge it afresh
    framesSeen = framesLate = 0;
    Resize(winH > 0 ? winH : kStrip, false);
}

bool Drawer::MaximizedBelow() const {
    POINT p;
    GetCursorPos(&p);
    RECT self = {};
    GetWindowRect(hwnd, &self);
    LONG in = (LONG)(24 * scale); // sample just inside the screen past our own window, never ourselves
    POINT probe = edgeNow == 0 ? POINT{p.x, (std::max)(monitor.top + in, self.bottom + 2)}
                : edgeNow == 1 ? POINT{(std::max)(monitor.left + in, self.right + 2), p.y}
                               : POINT{(std::min)(monitor.right - in, self.left - 2), p.y};
    HWND w = WindowFromPoint(probe); // what is just under the hot zone
    HWND root = w ? GetAncestor(w, GA_ROOT) : nullptr;
    if (!root || root == hwnd) return false;
    wchar_t cls[32] = {};
    GetClassNameW(root, cls, 32);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false; // the desktop
    if (IsZoomed(root)) return true;
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(root, &r) || !GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONULL), &mi)) return false;
    bool spans = r.left <= mi.rcWork.left && r.right >= mi.rcWork.right && r.top <= mi.rcWork.top; // fills the screen from the top: maximized in all but name
    if (edgeNow == 0 ? spans : spans && r.bottom >= mi.rcWork.bottom) return true;
    return TiledEdge(mi.rcWork); // snapped or tiled windows that together cover the edge: the same as one maximized app
}

bool Drawer::TiledEdge(const RECT& work) const { // samples along the docked edge: every point an app window that reaches the edge
    LONG in = (LONG)(6 * scale), slack = (LONG)(10 * scale);
    constexpr int kSamples = 9;
    for (int i = 0; i < kSamples; ++i) {
        float f = (i + 0.5f) / kSamples;
        POINT q = edgeNow == 0 ? POINT{work.left + (LONG)((work.right - work.left) * f), work.top + in}
                : edgeNow == 1 ? POINT{work.left + in, work.top + (LONG)((work.bottom - work.top) * f)}
                               : POINT{work.right - in, work.top + (LONG)((work.bottom - work.top) * f)};
        HWND w = WindowFromPoint(q);
        HWND root = w ? GetAncestor(w, GA_ROOT) : nullptr;
        if (!root || root == hwnd || IsIconic(root)) return false;
        wchar_t cls[32] = {};
        GetClassNameW(root, cls, 32);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false; // desktop or taskbar shows through
        RECT r;
        if (!GetWindowRect(root, &r)) return false;
        bool touches = edgeNow == 0 ? r.top <= work.top + slack : edgeNow == 1 ? r.left <= work.left + slack : r.right >= work.right - slack;
        if (!touches) return false; // a floating window with a gap above it: the bezel still has room
    }
    return true;
}

void Drawer::ShowIsland() {
    if (model.look == 2) SampleBehind();
    if (hidden || opened || peeking || !EnsureDev()) return;
    KillTimer(hwnd, kReleaseTimer); // keep gpu alive
    KillTimer(hwnd, kCloseTimer);
    island = islandLook = true;
    model.meter.Refresh(); // bind the level meter to the current output for the mini visualizer
    Resize(kIslandH + overshoot, true); // room for the bounce, trimmed once it settles
    spring.target = kIslandH;
    spring.damp = 0.6f; // a little bounce, island style
    Redraw();
}

void Drawer::SampleBehind() {
    int a = (int)(Span() * scale + 0.5f), d = (int)(Depth() * scale + 0.5f); // the panel's full open footprint
    RECT r;
    if (edgeNow == 0) {
        LONG x = monitor.left + (monitor.right - monitor.left - a) / 2;
        r = {x, monitor.top, x + a, monitor.top + d};
    } else {
        LONG y = monitor.top + (monitor.bottom - monitor.top - a) / 2;
        r = edgeNow == 1 ? RECT{monitor.left, y, monitor.left + d, y + a} : RECT{monitor.right - d, y, monitor.right, y + a};
    }
    glassShade = anim::Clamp01((sys::Brightness(r) - 0.42f) / 0.38f); // dark and colorful stay clear, bright pages get a deeper tint
}

float Drawer::DepthOf(D2D1_POINT_2F p) const {
    return edgeNow == 0 ? p.y : edgeNow == 1 ? p.x : winH - p.x; // right edge: measured from the window's right side
}

D2D1_RECT_F Drawer::Visible() const { // the window's part of the surface, in dips: the top left corner, as big as the window
    if (pillRgn) { // the island: only the pill's slice of the full-width window
        float a = IslandSpan(), o = (winW - a) / 2;
        return Side() ? D2D1_RECT_F{0, o, winH, o + a} : D2D1_RECT_F{o, 0, o + a, winH};
    }
    return Side() ? D2D1_RECT_F{0, 0, winH, winW} : D2D1_RECT_F{0, 0, winW, winH};
}

D2D1_MATRIX_3X2_F Drawer::EdgeXform() const {
    float off = -(Span() - winW) / 2; // the window may be shorter than the panel (strip, island), the shape stays centered in it
    if (edgeNow == 1) return D2D1::Matrix3x2F(0, 1, 1, 0, 0, off); // left: depth runs along x
    if (edgeNow == 2) return D2D1::Matrix3x2F(0, 1, -1, 0, winH, off); // right: depth runs leftwards from the window's right side
    return D2D1::Matrix3x2F::Translation(off, 0);
}

void Drawer::ApplyPlace() {
    edgeNow = model.edge;
    screensNow = model.screens;
    screenNow = model.screen;
    if (!screensNow) mon = sys::FindScreen(screenNow); // one screen: the picked one, primary if none
    FreeDev(); // swapchain is sized for the old edge
    Place();
    BuildStrips();
    needsFrame = false;
    SyncHide(); // the new screen may or may not have a game on it
    Log(L"placed from settings");
}

constexpr UINT kAdoptMsg = WM_APP + 60; // a hover strip on another screen asks the panel to come over

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

void Drawer::ReleaseLater() {
    if (!opened && !peeking && spring.value <= 0 && cfg.gpuIdleSec > 0)
        SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000, nullptr); // timer handler re-checks state
}

void Drawer::Resize(float hDip, bool topmost) {
    winH = hDip;
    bool idle = hDip <= kStrip + 0.01f;
    if (idle != eco) sys::SetEco(eco = idle); // throttle only while fully closed
    float along = idle ? (std::min)(Span(), cfg.hoverW) : Span(); // closed: small hot zone; the island keeps the full width, a region trims it to the pill
    winW = along;
    int a = (int)(along * scale + 0.5f);
    int d = (std::max)(2, (int)(hDip * scale + 0.5f)); // min 2px so strip stays hoverable
    int x, y, w, h;
    if (edgeNow == 0) { // top: along = width, depth = height
        w = a, h = d;
        x = monitor.left + (monitor.right - monitor.left - w) / 2;
        y = monitor.top;
    } else { // left or right: along = height, depth = width, centered vertically
        w = d, h = a;
        x = edgeNow == 1 ? monitor.left : monitor.right - w;
        y = monitor.top + (monitor.bottom - monitor.top - h) / 2;
    }
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : nullptr, x, y, w, h,
                 SWP_NOACTIVATE | (topmost ? 0 : SWP_NOZORDER)); // only re-raise z-order when opening
    // a narrower window for the pill would move what's on screen when it grows into the panel: the pill would jump aside for a frame.
    // so the window keeps its width and a region makes everything beside the pill click-through; opening just lifts the region
    bool pill = islandLook && !opened && !idle;
    if (pill) {
        int pa = (int)(IslandSpan() * scale + 0.5f);
        RECT want = edgeNow == 0 ? RECT{(w - pa) / 2, 0, (w + pa) / 2, h} : RECT{0, (h - pa) / 2, w, (h + pa) / 2};
        if (!pillRgn || !EqualRect(&want, &pillRect)) { // only when it changed: resizes come often
            HRGN rg = CreateRectRgnIndirect(&want);
            if (rg && SetWindowRgn(hwnd, rg, FALSE)) pillRgn = true, pillRect = want; // the system owns rg now
            else if (rg) DeleteObject(rg);
        }
    } else if (pillRgn) {
        SetWindowRgn(hwnd, nullptr, FALSE);
        pillRgn = false;
    }
}

D2D1_POINT_2F Drawer::ToDip(LPARAM lp) const {
    return {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
}

void Drawer::TrackLeave() {
    if (tracking) return;
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
    tracking = TrackMouseEvent(&t) != FALSE;
}

bool Drawer::MouseIn() const {
    POINT p;
    RECT r;
    GetCursorPos(&p);
    GetWindowRect(hwnd, &r);
    if (!PtInRect(&r, p)) return false; // real cursor check, leave msgs can lie
    if (!pillRgn) return true;
    HRGN rg = CreateRectRgn(0, 0, 0, 0); // the island: inside the pill, not just the window
    bool in = rg && GetWindowRgn(hwnd, rg) != ERROR && PtInRegion(rg, p.x - r.left, p.y - r.top);
    if (rg) DeleteObject(rg);
    return in;
}

void Drawer::Open() {
    if (hidden) return;
    if (model.look == 2 && spring.value < 1) SampleBehind(); // read the background before the panel covers it
    grow = grow || islandLook; // expanding out of the island: width morphs as it drops
    island = islandLook = false;
    if (!EnsureDev()) return; // gpu may have been freed while idle
    KillTimer(hwnd, kReleaseTimer); // keep gpu alive
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekTimer);
    peeking = false; // peek upgrades to full open
    if (!opened) {
        opened = true;
        SyncWall(); // wallpaper may have changed while closed
        model.OnOpen();
        model.SetLive(true, page == 2, page == 3); // fast polling only while visible, the process list only on the system page (2), latency only on the network page (3)
    }
    Resize(Depth() + overshoot, true); // grow first so bounce isn't clipped
    spring.target = Depth();
    spring.damp = model.reduceMotion ? 1.0f : 0.62f; // underdamped, drips open with a bounce; calm: it just arrives
    if (screensNow == 1 && !strips.empty()) SetTimer(hwnd, kFollowTimer, kFollowMs, nullptr); // every screen: follow the cursor to another one
    Redraw();
}

void Drawer::KeyOpen() {
    if (opened) {
        Close();
        return;
    }
    if (hidden) return;
    if (screensNow == 1) { // every screen: open on the one with the mouse
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (m != mon && !dragging && spring.value <= 0.5f) {
            mon = m;
            FreeDev();
            Place();
            BuildStrips();
            SyncHide();
        }
    }
    Open();
    keys = true;
    keysFrom = GetForegroundWindow();
    SetForegroundWindow(hwnd); // the shortcut lets us take the keyboard: arrows, numbers and esc work at once
    SetFocus(hwnd);
}

void Drawer::Close() {
    if (keys) { // the keyboard goes back where it was
        keys = false;
        if (GetForegroundWindow() == hwnd && keysFrom && IsWindow(keysFrom)) SetForegroundWindow(keysFrom);
    }
    KillTimer(hwnd, kFollowTimer);
    if (cfg.gpuIdleSec > 0) SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000 + 1000, nullptr); // freed, or trimmed if the pill stays
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekTimer);
    peeking = false;
    opened = false;
    island = false; // the island retracts with the same spring, its shape stays until it's gone
    model.hexFocus = false;
    model.CancelPass();
    SyncTyping(); // hand the keyboard back right away
    model.CloseBt(); // menus die with the panel
    model.CloseWifi();
    model.CloseSettings();
    model.CloseCal();
    model.SetLive(false, false);
    spring.target = 0;
    spring.damp = 0.92f; // near critical, retract without bounce
    Redraw();
}

void Drawer::Toggle() {
    if (opened) Close();
    else Open();
}

void Drawer::SetFull(bool on) {
    if (on && stage) return; // that's our own fullscreen player, not a game: the panel comes back when it leaves
    fullApp = on;
    SyncHide();
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

void Drawer::OnSecond() {
    SyncHide(); // a fullscreen app on the panel's screen, or gone from it
    model.IdleSpectrum();
    if (!opened) {
        bool always = PillStays(model);
        if (always && !island && !peeking && !dragging && !hidden && !stage && spring.value <= 0.5f) ShowIsland(); // the pill comes back by itself
        else if (!always && stayWasAlways && island && !MouseIn()) Close(); // always was just turned off
        stayWasAlways = always;
        if (island) Redraw(); // its clock keeps time
        return;
    }
    model.ReadAudio(); // volume only matters when visible
    Redraw();
}

void Drawer::SetPage(int p) {
    if (p == page) return;
    model.CloseBt();
    model.CloseWifi();
    model.CloseSettings();
    model.CloseCal();
    prevPage = page;
    page = p;
    pageT = 0;
    model.SetLive(opened, page == 2, page == 3);
    Redraw();
}

void Drawer::Peek(const std::wstring& title, const std::wstring& detail, int icon) {
    peekIcon = icon;
    peekTitle = title;
    peekDetail = detail;
    if (opened) {
        SetPage(4); // already open, jump to notice page
        return;
    }
    if (hidden || !EnsureDev()) return;
    island = islandLook = grow = false; // a notice replaces the island
    peeking = true;
    float reach = Side() ? kPeekSide : kPeekH; // on a side edge the banner needs room for its text
    Resize(reach + overshoot, true);
    spring.target = reach; // drop only a short banner
    spring.damp = 0.55f; // extra bouncy to catch the eye
    SetTimer(hwnd, kPeekTimer, model.notifyWatch.More() ? 3500 : 6000, nullptr); // auto retract after 6s, quicker when more are waiting
    Redraw();
}

void Drawer::StartDrag(float y) {
    if (!EnsureDev()) return;
    dragging = true;
    pressTo = spring.target; // remember if it was closed, for tap-to-open
    dragOff = (std::max)(spring.value, 0.0f) - y; // grab point offset so panel doesn't jump
    dragY0 = dragY = y;
    dragT = Seconds();
    dragVel = 0;
    spring.vel = 0; // finger takes over from spring
    KillTimer(hwnd, kCloseTimer);
    Resize(Depth() + overshoot, true); // full size so drag isn't clipped
    Redraw();
}

void Drawer::MoveDrag(float y) {
    double now = Seconds();
    double dt = now - dragT;
    if (dt > 0) dragVel = dragVel * 0.3f + (float)((y - dragY) / dt) * 0.7f; // smoothed velocity, favors latest sample
    dragY = y;
    dragT = now;
    spring.value = Rubber(dragOff + y); // panel follows cursor directly
    Redraw();
}

void Drawer::EndDrag() {
    if (!dragging) return;
    dragging = false;
    if (std::fabs(dragY - dragY0) < kTap) { // barely moved, treat as click
        if (pressTo <= 0) Open(); // click on strip opens
        Redraw();
        return;
    }
    if (Seconds() - dragT > 0.12) dragVel = 0; // paused before release, no fling
    bool open = dragVel > kFling || (dragVel > -kFling && spring.value > Depth() * kOpenAt); // fling wins, else by position
    spring.vel = dragVel; // hand off momentum to spring
    if (open) Open();
    else Close();
}

namespace {
void PerfWrite(const wchar_t* line) { // perf.log beside the exe
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\perf.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fputws(line, f);
        fclose(f);
    }
}
}

void Drawer::Log(const wchar_t* what) const {
    if (!cfg.perfLog) return;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT w = {};
    GetWindowRect(hwnd, &w);
    wchar_t line[512];
    _snwprintf_s(line, _TRUNCATE, L"[screen] %s: on %s (%ld,%ld %ldx%ld) scale %.2f, mode %s '%s', window %ld,%ld %ldx%ld, %s%s%s, %zu strips\n", what,
               mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
               scale, screensNow ? L"every" : L"one", screenNow.c_str(), w.left, w.top, w.right - w.left, w.bottom - w.top,
               IsWindowVisible(hwnd) ? L"visible" : L"HIDDEN", fullApp ? L", fullscreen notice" : L"", hidden ? L", hidden for fullscreen" : L"",
               strips.size());
    OutputDebugStringW(line);
    PerfWrite(line);
}

bool Drawer::FullOn(HMONITOR m) const { // the window in front fills that whole screen: a game, a video, a presentation
    HWND fg = GetForegroundWindow();
    if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != m) return false;
    if ((GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false; // a window with a title bar is maximized, not fullscreen, even under an auto-hidden taskbar
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false; // our own fullscreen player
    wchar_t cls[40] = {};
    GetClassNameW(fg, cls, 40);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
                                 L"ForegroundStaging", L"Windows.UI.Core.CoreWindow"})
        if (!wcscmp(cls, shell)) return false; // the desktop, taskbars, alt-tab, task view and the start menu cover screens too
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(m, &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

void Drawer::Govern(bool fast, double gap) {
    bool steady = fast && lastFast && gap > 0 && gap < 3.5 * fastSync / hz; // unbroken animation only; one long stall (a disk hiccup, a drag) isn't the machine's pace
    lastFast = fast;
    if (!steady || hz < 50) return;
    ++framesSeen;
    if (gap > 1.5 * fastSync / hz) ++framesLate;
    if (framesSeen < 90) return; // judge a window of frames, not one
    double now = Seconds();
    if (fastSync == 1 && framesLate * 2 > framesSeen) { // most frames late: a steady lower rate looks smoother than a stuttering high one
        fastSync = hz > 100 ? (UINT)std::lround(hz / 60) : 2;
        syncRetry = now + 30;
        Log(L"animation stepped down, frames were landing late");
    } else if (fastSync > 1 && now > syncRetry) { // a while on: maybe it was only busy, try full rate again
        fastSync = 1;
    }
    framesSeen = framesLate = 0;
}

void Drawer::Perf(double drawSec) {
    double now = Seconds();
    ++perfN;
    perfDraw += drawSec;
    if (perfT0 == 0) perfT0 = now;
    if (now - perfT0 < 5) return;
    wchar_t line[256];
    swprintf_s(line, L"%.0f frames/s, %.2f ms avg draw, %.1f%% of a core | why: spring %d drag %d ui %d theme %d ambient %d input %d\n",
               perfN / (now - perfT0), perfDraw * 1000 / perfN, perfDraw * 100 / (now - perfT0),
               perfWhy[0], perfWhy[1], perfWhy[2], perfWhy[3], perfWhy[4], perfWhy[5]);
    for (int& n : perfWhy) n = 0;
    OutputDebugStringW(line);
    PerfWrite(line);
    perfT0 = now;
    perfN = 0;
    perfDraw = 0;
}

float Drawer::Rubber(float raw) const {
    if (raw <= 0) return 0;
    float full = Depth();
    if (raw <= full) return raw;
    float over = raw - full;
    return full + overshoot * 0.8f * (1 - std::exp(-over / (overshoot * 2))); // rubber band past full height, caps at 80% of overshoot
}

LRESULT CALLBACK Drawer::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) { // stash this ptr from CreateWindowEx
        auto* self = (Drawer*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = hwnd; // set early, msgs arrive before Create returns
    }
    auto* self = (Drawer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Drawer::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // clicks never steal focus from user's app
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = ToDip(lp);
        TrackLeave();
        if (dragging) {
            MoveDrag(DepthOf(p));
            return 0;
        }
        if (peeking) { // a notification pill: a click opens the panel, hovering only lights it
            TrackLeave();
            return 0;
        }
        if (!opened && !islandLook && spring.value < 1) { // on the closed strip; the island handles its own hover
            if (cfg.hoverOpen && !armed) {
                EnsureDev(); // warm up gpu during hover delay
                islandNext = MaximizedBelow(); // one check per hover, nothing polls
                SetTimer(hwnd, kHoverTimer, islandNext ? kIslandMs : (UINT)cfg.hoverMs, nullptr); // open only if mouse lingers
                armed = true;
            }
            return 0;
        }
        KillTimer(hwnd, kCloseTimer); // back inside, cancel pending close
        if (islandLook && !islandHot) { // entering the island: light up its expand hint
            islandHot = true;
            Redraw();
        }
        // while a pill shows, the panel only ever opens on a click: hovering the pill never drops it
        input.mouse = p;
        D2D1_POINT_2F local = {p.x - cfg.fillet, p.y - (spring.value - H)}; // to panel space, panel slides down from -H
        if (ui.Captured() || ui.HitAt(local) != ui.Hovered()) Redraw(); // only redraw when hover target changes
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        KillTimer(hwnd, kHoverTimer);
        armed = false;
        islandHot = false;
        input.mouse = {-1e6f, -1e6f}; // park offscreen to clear hover
        if ((opened || island) && !dragging && GetCapture() != hwnd) {
            bool pill = island && !opened;
            if (!(pill && PillStays(model))) // always: the pill stays put
                SetTimer(hwnd, kCloseTimer, pill ? model.IslandLingerMs((UINT)cfg.closeMs) : (UINT)cfg.closeMs, nullptr); // close after grace period, the pill can linger
        }
        ReleaseLater(); // brushed the strip without opening: the hover warm-up made a device
        Redraw();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp); // all timers are one-shot
        if (wp == kFollowTimer) { // open, every screen mode: the cursor crossed to another screen, the panel goes with it
            POINT pt;
            GetCursorPos(&pt);
            HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (opened && !dragging && m && m != mon) {
                followTo = m;
                Close(); // up here, then down there once it's out of sight
            } else if (opened) {
                SetTimer(hwnd, kFollowTimer, kFollowMs, nullptr);
            }
            return 0;
        }
        if (wp == kDevTimer) { // try the device again: the frame picks up where it left off
            needsFrame = true;
            return 0;
        }
        if (wp == kHoverTimer) {
            armed = false;
            if (!MouseIn()) ReleaseLater();
            else if (islandNext) ShowIsland(); // a maximized app below: the pill first, a tap opens the panel
            else Open();
        } else if (wp == kCloseTimer) {
            if (!dragging && GetCapture() != hwnd && !MouseIn() && !(island && !opened && PillStays(model))) Close();
        } else if (wp == kReleaseTimer) {
            if (!opened && !peeking && spring.value <= 0) FreeDev(); // free gpu after idle
            else if (!opened && !peeking && !dragging) TrimDev(); // the pill keeps it: just drop what the panel left cached
        } else if (wp == kPeekTimer) {
            if (peeking) Close();
            if (model.notifyWatch.More()) PostMessageW(hwnd, WM_APP_NOTIFY, 0, 0); // next in line
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if ((island || peeking) && !opened) { // tap a pill: the full panel grows out of it
            Open();
            return 0;
        }
        D2D1_POINT_2F p = ToDip(lp);
        SetCapture(hwnd); // keep getting moves outside window while held
        if (!opened && spring.value < 1) {
            StartDrag(DepthOf(p)); // pull out from the strip
            return 0;
        }
        input.mouse = p;
        input.pressPos = p;
        input.pressed = true;
        input.down = true;
        Frame(); // run ui now to see what was hit
        if (ui.FreePress()) StartDrag(DepthOf(p)); // press on empty area drags panel
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging) {
            ReleaseCapture();
            EndDrag();
            return 0;
        }
        input.mouse = ToDip(lp);
        input.released = true;
        input.down = false;
        ReleaseCapture();
        Redraw();
        if (!MouseIn() && opened) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr); // released outside, leave was suppressed by capture
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging) EndDrag(); // capture stolen, finish drag cleanly
        if (input.down) { // synthesize release so widgets don't stick
            input.down = false;
            input.released = true;
            Redraw();
        }
        return 0;
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; // in notches, accumulates till next frame
        Redraw();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW)); // hand set by ui over clickables
        return TRUE;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        FreeDev(); // swapchain size/dpi stale, rebuild lazily
        if (!screensNow) mon = sys::FindScreen(screenNow); // the picked screen may have just been plugged back in
        model.screenList.clear(); // settings re-reads the monitors next frame
        Place();
        BuildStrips(); // screens may have come or gone
        Log(msg == WM_DPICHANGED ? L"dpi changed" : L"displays changed");
        return 0;
    case WM_APP_MEDIA:
        model.OnMedia();
        Redraw();
        return 0;
    case WM_APP_BRIGHT:
        model.OnBright(wp);
        Redraw();
        return 0;
    case WM_APP_WEATHER:
        model.OnWx();
        Redraw();
        return 0;
    case WM_APP_STATS:
        model.OnStats();
        Redraw();
        return 0;
    case WM_APP_BT:
        model.OnBt();
        Redraw();
        return 0;
    case WM_APP_WIFI:
        model.OnWifi();
        Redraw();
        return 0;
    case kAdoptMsg: { // hovered the strip on another screen
        HMONITOR m = (HMONITOR)wp;
        if (!opened && !dragging && !peeking && spring.value <= 0.5f && m != mon) {
            mon = m;
            FreeDev(); // other screen, maybe other dpi
            Place();
            BuildStrips();
            SyncHide();
            Log(L"moved to the hovered screen");
        }
        return 0;
    }
    case WM_APP_PICKED: // the color picker finished
        if (lp) {
            wchar_t hex[8];
            swprintf_s(hex, L"#%06X", (unsigned)wp);
            Peek(std::wstring(L"Copied ") + hex, L"Picked from the screen", glyph::Copy);
        }
        return 0;
    case WM_APP_NOTIFY: { // a windows notification arrived and pills are on
        sys::Toast toast;
        if (peeking) return 0; // one is showing: the rest wait their turn
        if (model.notifyWatch.Pop(toast) && !opened) // open panel: don't interrupt
            Peek(toast.title.empty() ? toast.app : toast.title, toast.body.empty() ? toast.app : toast.body, glyph::Sparkle);
        return 0;
    }
    case WM_APP_SHARE:
        model.OnShare();
        Redraw();
        return 0;
    case WM_CHAR: // only reaches us while a field holds the keyboard
        if (model.passFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v, first line only, appended in place
                std::wstring clip = sys::ClipText();
                for (wchar_t c : clip) {
                    if (c == L'\r' || c == L'\n') break;
                    if (c >= 32 && c < 127 && model.passEdit.size() < 63) model.passEdit += c;
                }
                SecureZeroMemory(clip.data(), clip.size() * sizeof(wchar_t));
            } else if (ch == 8) {
                if (!model.passEdit.empty()) model.passEdit.back() = 0, model.passEdit.pop_back();
            } else if (ch >= 32 && ch < 127 && model.passEdit.size() < 63) { // wpa passphrases are printable ascii, 63 at most
                model.passEdit += ch;
            }
            model.passWrong = false;
            Redraw();
            return 0;
        }
        if (model.hexFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v
                int rgb = 0;
                if (Model::ParseHex(sys::ClipText(), rgb)) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"%06X", rgb);
                    model.hexEdit = buf;
                }
            } else if (ch == 8) {
                if (!model.hexEdit.empty()) model.hexEdit.pop_back();
            } else if (iswxdigit(ch) && model.hexEdit.size() < 6) {
                model.hexEdit += (wchar_t)towupper(ch);
            }
            Redraw();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && keys && opened && !model.Typing()) { // clicked away from a panel opened by the keyboard: it goes too
            keys = false;
            Close();
        }
        return 0;
    case WM_KEYDOWN:
        if (keys && !model.Typing()) { // a panel opened from the shortcut: move with the keys
            if (wp == VK_ESCAPE) Close();
            else if (wp == VK_RIGHT || wp == VK_DOWN) SetPage((std::min)(page + 1, kPageCount - 1));
            else if (wp == VK_LEFT || wp == VK_UP) SetPage((std::max)(page - 1, 0));
            else if (wp >= '1' && wp < '1' + (WPARAM)kPageCount) SetPage((int)(wp - '1'));
            Redraw();
            return 0;
        }
        if (model.passFocus) {
            if (wp == VK_RETURN) model.SubmitPass();
            else if (wp == VK_ESCAPE) model.CancelPass();
            Redraw();
            return 0;
        }
        if (model.hexFocus) {
            if (wp == VK_RETURN) model.CommitHex();
            else if (wp == VK_ESCAPE) model.hexFocus = false;
            Redraw();
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP:
        if (opened) { // right click only means something inside the open panel
            input.mouse = ToDip(lp);
            input.rclick = true;
            Redraw();
        }
        return 0;
    case WM_APP_RADIOS:
    case WM_APP_WANIP:
        Redraw();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Drawer::PaintShape(ID2D1Geometry* shape, float h) {
    if (!shape) return;
    canvas->FillGeometry(shape, shellNow.bg); // dark mode: black, blends with screen bezel
    if (shellNow.glass && glassShade > 0.01f) // over a white page: neutral smoked glass under the highlights, the blur still shows through
        canvas->FillGeometry(shape, Color{0, 0, 0, 0.42f * glassShade});
    if (shellNow.glass) { // liquid glass: glossy light from above and a bright specular rim
        canvas->FillGeoV(shape, {0, 0, W, (std::min)(h, 90.0f)}, shellNow.sheen, shellNow.sheen.Alpha(0));
        canvas->FillGeoV(shape, {0, h - (std::min)(h * 0.5f, 34.0f), W, h}, Color{1, 1, 1, 0}, Color{1, 1, 1, 0.09f}); // light caught in the lower lip, like a drop of glass
        canvas->StrokeGeo(shape, Color{1, 1, 1, 0.26f}, 1.2f);
    }
}

void Drawer::Frame() {
    if (stage) { // stage paces the loop while it's up, the panel only draws if it's still retracting
        stage->Frame(mediaNow);
        if (stage->Leaving() && !stageBack) { // fullscreen fades out over the panel dropping open on the media page, where it came from
            stageBack = true;
            if (page != 1) SetPage(1); // the media page
            Open();
        }
        if (stage->Done()) { // its window, device and audio capture all go with it
            stage.reset();
            stageBack = false;
        }
        if (!needsFrame) return;
    }
    double now = Seconds();
    double gap = lastFrame > 0 ? now - lastFrame : 0; // the real interval, for the governor
    float dt = (float)(std::min)(now - lastFrame, 0.05); // clamp so a stall doesn't jump the spring
    if (lastFrame == 0) dt = 1 / 60.0f; // first frame, assume 60hz
    lastFrame = now;

    bool moving = !dragging && spring.Step(dt); // drag drives value directly
    if (!moving && !dragging) { // spring settled
        grow = false; // expansion finished, or retracted
        if (spring.target <= 0 && winH > kStrip) {
            islandLook = hand = false; // fully retracted, back to the plain strip and arrow cursor
            Resize(kStrip, false); // shrink window so it can't block clicks
            ui.Forget();
            if (model.pickPending) { // the color picker waited for the panel to be gone
                model.pickPending = false;
                model.StartPicker();
            }
            if (followTo) { // retracted on the old screen: move over and drop open on the new one
                mon = followTo;
                followTo = nullptr;
                FreeDev(); // other screen, maybe other dpi
                Place();
                BuildStrips();
                SyncHide();
                Log(L"followed the mouse");
                Open();
                return;
            }
            if (cfg.gpuIdleSec > 0) SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000, nullptr); // schedule gpu release
        } else if (spring.target > 0) {
            float room = spring.target + (hintFade > 0.01f ? kHintRoom : 0); // a tip outside the panel needs a little window
            if (std::fabs(winH - room) > 1) Resize(room, false); // otherwise drop the overshoot margin once at rest
        }
    }
    if (PlaceStale() && opened) Close(); // re-dock once it has retracted
    SyncLook(); // before the device: switching to or from glass rebuilds it
    SyncAccent();
    SyncTyping();
    if (!EnsureDev()) {
        needsFrame = false; // no device, stop loop instead of spinning
        if (devTries++ < kDevTries) SetTimer(hwnd, kDevTimer, 100, nullptr); // often just the old glass target still being released: try again shortly
        return;
    }
    devTries = 0;
    bool themes = StepTheme(dt);
    SyncCover();

    float h = (std::max)(0.0f, (std::min)(spring.value, Depth() + overshoot)); // clamp to window room
    double drawT0 = Seconds();
    FitSurfaces(); // before drawing: a pill alone keeps a small strip, the panel gets its full size back
    if (!ctx || !canvas) return; // resizing failed and dropped the device: the next frame rebuilds it
    canvas->NewFrame();
    bool pillCached = false;
    if (islandLook && !opened && !peeking && !Side() && !moving && !dragging && !themes && h > 0.5f && std::fabs(h - spring.target) < 0.01f) { // resting pill
        shapeXform = EdgeXform();
        pillCached = PillLayer(h, IslandSpan());
    }
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F vis = Visible();
    ctx->PushAxisAlignedClip(vis, D2D1_ANTIALIAS_MODE_ALIASED); // the surface is panel-sized, the window shows only this corner: work only there
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent, desktop shows through via dcomp
    bool uiBusy = false, ambient = false;
    ComPtr<ID2D1Geometry> outline; // this frame's shape, the glass blur is cut to it
    if (h > 0.5f) {
        float shapeWide = Span();
        if (islandLook) shapeWide = IslandSpan();
        auto wideAt = [&](float at) { return IslandSpan() + (Span() - IslandSpan()) * anim::Clamp01((at - kIslandH) / (Depth() * 0.3f - kIslandH)); };
        if (!islandLook && grow) shapeWide = wideAt(h); // pill widens into the panel
        shapeXform = EdgeXform();
        ctx->SetTransform(shapeXform); // turned to the docked edge, centered in the window
        ComPtr<ID2D1Geometry> shape = Shape(h, shapeWide);
        if (pillCached) { // a resting pill: its still parts come from the layer, only the bars are drawn fresh
            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            ctx->DrawImage(pillLayer.Get());
            ctx->SetTransform(shapeXform);
        } else {
            PaintShape(shape.Get(), h);
        }
        outline = shape;
        if (glass && std::fabs(spring.vel) > 1 && !dragging) { // composition applies the blur's outline a few frames late: cut it where the glass will be, both ways
            float frames = spring.vel < 0 ? 3.0f : 1.5f; // dropping: a shorter lead, any more and the blur runs ahead of the glass
            float lead = (std::clamp)(h + spring.vel * dt * frames, 0.0f, Depth() + overshoot);
            outline = lead > 0.5f ? BuildShape(lead, !islandLook && grow ? wideAt(lead) : shapeWide) : nullptr;
        }
        ctx->SetTransform(Side() ? D2D1::Matrix3x2F::Identity() : shapeXform);
        if (peeking) DrawPeek(h);
        else if (islandLook) { // bars bounce only while it shows and music plays
            pillPass = pillCached ? 2 : 0;
            ambient = DrawIsland(h, dt, now);
            pillPass = 0;
        }
        else if (h > 24) { // too short for content, just shape
            DrawBody(h, dt, now);
            uiBusy = ui.Busy();
            ambient = ui.Ambient();
        }
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->PopAxisAlignedClip();
    HRESULT hr = ctx->EndDraw();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    if (hr == D2DERR_RECREATE_TARGET) { // device lost, rebuild next frame
        FreeDev();
        needsFrame = true;
        return;
    }
    if (glass) {
        GlassBlur(model.glassBlur / 100.0f);
        GlassMask(outline.Get());
    }
    bool fast = moving || dragging || uiBusy || themes || pageT < 1; // anything actively animating
    if (cfg.perfLog) {
        int why = moving ? 0 : dragging ? 1 : uiBusy ? 2 : (themes || pageT < 1) ? 3 : ambient ? 4 : 5;
        ++perfWhy[why];
        Perf(Seconds() - drawT0);
    }
    Govern(fast, gap);
    UINT calm = ambientSync; // ambient motion, the pill's bars included, at ~30 fps: any slower and they stop looking alive
    if (glass) { // no swapchain: the frame lands with the blur outline, a timer paces the next one
        GlassPresent();
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((fast ? fastSync : calm) * 1e7 / hz); // relative, 100 ns units
        SetWaitableTimer(frameWait, &due, 0, nullptr, nullptr, FALSE);
        hr = S_OK;
    } else {
        hr = swap->Present(fast ? fastSync : calm, 0); // ambient-only motion runs at ~30 fps; a whole present: with dirty rects, a frame shown while the window is a sliver is never retired and the loop stalls
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { // driver reset / gpu switch
        FreeDev();
        needsFrame = true;
        return;
    }
    needsFrame = fast || (ambient && h > 0.5f); // otherwise go idle until next input
    if (PlaceStale() && !opened && !dragging && spring.value <= 0.5f) ApplyPlace();
}
