#include "drawer.h"

#include <dwmapi.h>
#include <windowsx.h>

namespace {

constexpr UINT_PTR kHoverTimer = 1, kCloseTimer = 2, kReleaseTimer = 3, kPeekTimer = 4;
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
    Seed seed = cfg.wallTheme ? Seed{} : theme::FromCol(cfg.themeCol); // wallpaper theme fills in later
    shellFrom = shellTo = shellNow = mediaFrom = mediaTo = mediaNow = Theme::From(seed);
}

Drawer::~Drawer() {
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
    Place();
    SyncWall();
    shellNow = shellTo; // start on final colors, no fade at boot
    mediaNow = mediaTo;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void Drawer::Place() {
    scale = GetDpiForWindow(hwnd) / 96.0f;
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi); // primary monitor
    monitor = mi.rcMonitor; // full rect, not work area, so it sits on the top edge
    Resize(winH > 0 ? winH : kStrip, false);
}

void Drawer::Resize(float hDip, bool topmost) {
    winH = hDip;
    bool idle = hDip <= kStrip + 0.01f;
    if (idle != eco) sys::SetEco(eco = idle); // throttle only while fully closed
    float wDip = hDip <= kStrip + 0.01f ? (std::min)(W, cfg.hoverW) : W; // closed: only a small centered hot zone
    int w = (int)(wDip * scale + 0.5f);
    int h = (std::max)(2, (int)(hDip * scale + 0.5f)); // min 2px so strip stays hoverable
    int x = monitor.left + (monitor.right - monitor.left - w) / 2;
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : nullptr, x, monitor.top, w, h,
                 SWP_NOACTIVATE | (topmost ? 0 : SWP_NOZORDER)); // only re-raise z-order when opening
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
    return PtInRect(&r, p) != FALSE; // real cursor check, leave msgs can lie
}

void Drawer::Open() {
    if (hidden) return;
    if (!EnsureDev()) return; // gpu may have been freed while idle
    KillTimer(hwnd, kReleaseTimer); // keep gpu alive
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekTimer);
    peeking = false; // peek upgrades to full open
    if (!opened) {
        opened = true;
        SyncWall(); // wallpaper may have changed while closed
        model.OnOpen();
        model.SetLive(true, page == 2); // fast polling only while visible, page 2 = media
    }
    Resize(H + overshoot, true); // grow first so bounce isn't clipped
    spring.target = H;
    spring.damp = 0.62f; // underdamped, drips open with a bounce
    Redraw();
}

void Drawer::Close() {
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekTimer);
    peeking = false;
    opened = false;
    model.CloseBt(); // menu dies with the panel
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
    fullApp = on;
    SyncHide();
}

void Drawer::SyncHide() {
    bool hide = fullApp;
    if (hide == hidden) return;
    hidden = hide;
    if (hide) {
        opened = false;
        spring = anim::Spring{}; // reset instantly, no close anim
        Resize(kStrip, false);
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

void Drawer::OnSecond() {
    if (!opened) return;
    model.ReadAudio(); // volume only matters when visible
    Redraw();
}

void Drawer::SetPage(int p) {
    if (p == page) return;
    model.CloseBt();
    prevPage = page;
    page = p;
    pageT = 0;
    model.SetLive(opened, page == 2);
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
    peeking = true;
    Resize(kPeekH + overshoot, true);
    spring.target = kPeekH; // drop only a short banner
    spring.damp = 0.55f; // extra bouncy to catch the eye
    SetTimer(hwnd, kPeekTimer, 6000, nullptr); // auto retract after 6s
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
    Resize(H + overshoot, true); // full size so drag isn't clipped
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
    bool open = dragVel > kFling || (dragVel > -kFling && spring.value > H * kOpenAt); // fling wins, else by position
    spring.vel = dragVel; // hand off momentum to spring
    if (open) Open();
    else Close();
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
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\perf.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fputws(line, f);
        fclose(f);
    }
    perfT0 = now;
    perfN = 0;
    perfDraw = 0;
}

float Drawer::Rubber(float raw) const {
    if (raw <= 0) return 0;
    if (raw <= H) return raw;
    float over = raw - H;
    return H + overshoot * 0.8f * (1 - std::exp(-over / (overshoot * 2))); // rubber band past full height, caps at 80% of overshoot
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
            MoveDrag(p.y);
            return 0;
        }
        if (peeking) {
            Open(); // hovering a peek expands it
            return 0;
        }
        if (!opened && spring.value < 1) { // on the closed strip
            if (cfg.hoverOpen && !armed) {
                EnsureDev(); // warm up gpu during hover delay
                SetTimer(hwnd, kHoverTimer, (UINT)cfg.hoverMs, nullptr); // open only if mouse lingers
                armed = true;
            }
            return 0;
        }
        KillTimer(hwnd, kCloseTimer); // back inside, cancel pending close
        input.mouse = p;
        D2D1_POINT_2F local = {p.x - cfg.fillet, p.y - (spring.value - H)}; // to panel space, panel slides down from -H
        if (ui.Captured() || ui.HitAt(local) != ui.Hovered()) Redraw(); // only redraw when hover target changes
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        KillTimer(hwnd, kHoverTimer);
        armed = false;
        input.mouse = {-1e6f, -1e6f}; // park offscreen to clear hover
        if (opened && !dragging && GetCapture() != hwnd) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr); // close after grace period
        Redraw();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp); // all timers are one-shot
        if (wp == kHoverTimer) {
            armed = false;
            if (MouseIn()) Open();
        } else if (wp == kCloseTimer) {
            if (!dragging && GetCapture() != hwnd && !MouseIn()) Close();
        } else if (wp == kReleaseTimer) {
            if (!opened && !peeking && spring.value <= 0) FreeDev(); // free gpu after idle
        } else if (wp == kPeekTimer) {
            if (peeking) Close();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = ToDip(lp);
        SetCapture(hwnd); // keep getting moves outside window while held
        if (!opened && spring.value < 1) {
            StartDrag(p.y); // pull down from strip
            return 0;
        }
        input.mouse = p;
        input.pressPos = p;
        input.pressed = true;
        input.down = true;
        Frame(); // run ui now to see what was hit
        if (ui.FreePress()) StartDrag(p.y); // press on empty area drags panel
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
        Place();
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

void Drawer::Frame() {
    double now = Seconds();
    float dt = (float)(std::min)(now - lastFrame, 0.05); // clamp so a stall doesn't jump the spring
    if (lastFrame == 0) dt = 1 / 60.0f; // first frame, assume 60hz
    lastFrame = now;

    bool moving = !dragging && spring.Step(dt); // drag drives value directly
    if (!moving && !dragging) { // spring settled
        if (spring.target <= 0 && winH > kStrip) {
            Resize(kStrip, false); // shrink window so it can't block clicks
            ui.Forget();
            if (cfg.gpuIdleSec > 0) SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000, nullptr); // schedule gpu release
        } else if (spring.target > 0 && winH > spring.target + 1) {
            Resize(spring.target, false); // drop overshoot margin once at rest
        }
    }
    if (!EnsureDev()) {
        needsFrame = false; // no device, stop loop instead of spinning
        return;
    }
    bool themes = StepTheme(dt);
    SyncCover();

    float h = (std::max)(0.0f, (std::min)(spring.value, H + overshoot)); // clamp to window room
    double drawT0 = Seconds();
    canvas->NewFrame();
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent, desktop shows through via dcomp
    bool uiBusy = false, ambient = false;
    if (h > 0.5f) {
        ComPtr<ID2D1Geometry> shape = Shape(h);
        if (shape) canvas->FillGeometry(shape.Get(), Color{0, 0, 0, 1}); // black blends with screen bezel
        if (peeking) DrawPeek(h);
        else if (h > 24) { // too short for content, just shape
            DrawBody(h, dt, now);
            uiBusy = ui.Busy();
            ambient = ui.Ambient();
        }
    }
    HRESULT hr = ctx->EndDraw();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    if (hr == D2DERR_RECREATE_TARGET) { // device lost, rebuild next frame
        FreeDev();
        needsFrame = true;
        return;
    }
    bool fast = moving || dragging || uiBusy || themes || pageT < 1; // anything actively animating
    if (cfg.perfLog) {
        int why = moving ? 0 : dragging ? 1 : uiBusy ? 2 : (themes || pageT < 1) ? 3 : ambient ? 4 : 5;
        ++perfWhy[why];
        Perf(Seconds() - drawT0);
    }
    hr = swap->Present(fast ? 1 : 2, 0); // half refresh rate for ambient-only motion
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { // driver reset / gpu switch
        FreeDev();
        needsFrame = true;
        return;
    }
    needsFrame = fast || (ambient && h > 0.5f); // otherwise go idle until next input
}
