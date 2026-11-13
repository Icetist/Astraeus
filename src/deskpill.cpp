// The desktop pill at rest: its window, its device, its body and what it shows.
#include "deskpill.h"

#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "pages.h"
#include "sys.h"

namespace {

constexpr wchar_t kClass[] = L"AstraeusDeskPill";
constexpr float kPad = 18; // room around it in its window: the bounce, the shoulders
constexpr float kEdgeRoom = 12; // never closer than this to the end of its edge
constexpr float kFillet = 10; // the shoulders where a stuck pill meets the edge
constexpr float kItemGap = 12;
constexpr float kSongLeast = 80; // a song's title is never squeezed below this, and never asks for more of the pill's own length
constexpr float kShapeRate = 12, kShapeFlat = 8; // changing over between its two shapes
constexpr float kBlurSigma = 22;
constexpr float kInkToBlack = 0.52f, kInkToWhite = 0.42f; // apart, so a mid gray wallpaper doesn't flip it back and forth
constexpr float kTextBox = 16; // text box height at the base size
constexpr float kBaseThick = 34; // the thickness its type is designed at
constexpr UINT kGlassMaxPx = 1024; // blurred anyway: a small decode
constexpr float kArtInset = 8; // the cover: the pill's thickness less this
constexpr UINT kCoverPx = 128; // a small square: no more is ever drawn
constexpr float kBarW = 3.5f, kBarStep = 6, kBarLow = 3, kBarRise = 11; // the visualizer, as the small pill's
constexpr int kBarCount = 4;
constexpr UINT kBarsSync = 4; // vsyncs per frame while only they move: about 15 a second at 60 Hz, as the small pill's
constexpr UINT_PTR kLyricTimer = 1;
constexpr double kLyricSoon = 1.2; // a line due sooner than the next second's look is waited for on its own
constexpr UINT kLyricLateMs = 20; // just past the line's start, so it's there when looked
constexpr double kMidFade = 0.62; // seconds: a line hands over to the next, or to what the middle holds
constexpr float kMidRise = 9; // the words drift up through the change, as a lyric sheet scrolls
constexpr float kMidOut = 0.55f, kMidIn = 0.3f; // of the fade: the old is gone by this much; the new starts at this much, so they barely cross

bool Stretchy(int kind) { return kind == Model::kDMusic || kind == Model::kDLyrics; } // a long title or line gives way

bool In(const std::vector<int>& items, int kind) { return std::find(items.begin(), items.end(), kind) != items.end(); }


double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / freq.QuadPart;
}

float TypeScale(float thick) { return (std::clamp)(thick / kBaseThick, 0.8f, 1.35f); }

std::wstring SongStem(std::wstring s) { // the title without what's in brackets at its end: "Song (Remastered 2011) [Live]" is "Song"
    auto trim = [&] {
        while (!s.empty() && (s.back() == L' ' || s.back() == L'-')) s.pop_back();
    };
    for (trim(); !s.empty(); trim()) {
        wchar_t close = s.back(), open = close == L')' ? L'(' : close == L']' ? L'[' : close == L'}' ? L'{' : 0;
        size_t at = open ? s.rfind(open) : std::wstring::npos;
        if (at == std::wstring::npos || at == 0) break; // nothing but brackets: that is its name
        s.resize(at);
    }
    return s;
}

bool WallInfo(const RECT& mon, std::wstring& path, DWORD& pos, Color& solid) { // this screen's picture, as windows places it
    ComPtr<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) {
        path = theme::WallPath();
        pos = DWPOS_FILL;
        return !path.empty();
    }
    COLORREF bg = 0;
    if (SUCCEEDED(dw->GetBackgroundColor(&bg))) solid = Color{GetRValue(bg) / 255.0f, GetGValue(bg) / 255.0f, GetBValue(bg) / 255.0f, 1};
    DESKTOP_WALLPAPER_POSITION p = DWPOS_FILL;
    if (SUCCEEDED(dw->GetPosition(&p))) pos = (DWORD)p;
    UINT n = 0;
    dw->GetMonitorDevicePathCount(&n);
    for (UINT i = 0; i < n; ++i) {
        LPWSTR id = nullptr;
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        RECT r{};
        bool here = SUCCEEDED(dw->GetMonitorRECT(id, &r)) && EqualRect(&r, &mon);
        LPWSTR file = nullptr;
        if (here && SUCCEEDED(dw->GetWallpaper(id, &file)) && file) path = file;
        if (file) CoTaskMemFree(file);
        CoTaskMemFree(id);
        if (here) break;
    }
    if (path.empty()) path = theme::WallPath();
    return !path.empty();
}

}

DeskPill::DeskPill(Model& m) : model(m) {}

DeskPill::~DeskPill() {
    FreeDev();
    if (hwnd) DestroyWindow(hwnd);
}

bool DeskPill::Create() {
    static bool registered = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return false;
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Astraeus desktop pill", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                    GetModuleHandleW(nullptr), this); // Proc keeps the handle
    if (!hwnd) return false;
    BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    return true;
}

LRESULT CALLBACK DeskPill::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = reinterpret_cast<DeskPill*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && !self->hwnd) self->hwnd = h;
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT DeskPill::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp != kLyricTimer) break;
        KillTimer(hwnd, kLyricTimer);
        if (wantUp && !Editing()) Watch();
        return 0;
    case WM_MOUSEACTIVATE:
        return studio ? MA_ACTIVATE : MA_NOACTIVATE; // never the keyboard's while it's the pill: esc comes by hotkey. the widgets editor has a search to type in
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        Poke();
        return 0;
    case WM_CHAR:
        if (studio && model.faceSearch) {
            wchar_t ch = (wchar_t)wp;
            if (ch == VK_BACK) {
                if (!model.faceQuery.empty()) model.faceQuery.pop_back();
            } else if (ch >= 32 && model.faceQuery.size() < Model::kFaceQueryMax) {
                model.faceQuery += ch;
            }
            Poke();
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    case WM_MOUSEMOVE: {
        input.mouse = {GET_X_LPARAM(lp) / scale + view.x, GET_Y_LPARAM(lp) / scale + view.y};
        if (!tracking) {
            TRACKMOUSEEVENT tm = {sizeof(tm), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&tm) != FALSE;
        }
        if (editing || !hot) hot = true, Poke();
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = hot = false;
        input.mouse = {-1e6f, -1e6f};
        Poke();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        input.mouse = input.pressPos = {GET_X_LPARAM(lp) / scale + view.x, GET_Y_LPARAM(lp) / scale + view.y};
        input.pressed = input.down = true;
        Poke();
        return 0;
    case WM_LBUTTONUP: {
        if (GetCapture() == hwnd) ReleaseCapture();
        bool was = input.down;
        input.mouse = {GET_X_LPARAM(lp) / scale + view.x, GET_Y_LPARAM(lp) / scale + view.y};
        input.down = false;
        input.released = true;
        Poke();
        if (!editing && !leaving && was && onOpen) onOpen();
        return 0;
    }
    case WM_RBUTTONUP:
        if (studio) input.rclick = true, Poke();
        if (!editing && !leaving && onEdit) onEdit(); // press and hold on a phone; here a right click
        return 0;
    case WM_HOTKEY:
        if ((int)wp != kEscKey || !editing) return 0;
        if (studio && model.faceSearch) { // esc steps back: what's typed, then the search
            if (model.faceQuery.empty()) model.faceSearch = false;
            else model.faceQuery.clear();
            Poke();
            return 0;
        }
        Finish();
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        OnDisplayChange(screen);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) {
            OnDisplayChange(screen);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DeskPill::Measure(HMONITOR on) {
    screen = on ? on : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(screen, &mi)) {
        screen = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(screen, &mi)) return;
    }
    RECT oldMon = monitor, oldWork = work;
    float oldScale = scale;
    monitor = mi.rcMonitor;
    work = mi.rcWork;
    UINT dx = 96, dy = 96;
    scale = SUCCEEDED(GetDpiForMonitor(screen, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : (hwnd ? GetDpiForWindow(hwnd) / 96.0f : 1.0f);
    MW = (monitor.right - monitor.left) / scale;
    MH = (monitor.bottom - monitor.top) / scale;
    if (!editing) { // the saved values; while editing the working copy is what counts
        spec.edge = model.deskEdge, spec.pos = (float)model.deskPos;
        spec.shape[0] = {(float)model.deskLen, (float)model.deskGap, model.deskFloat != 0, (float)model.deskThick};
        spec.shape[1] = {(float)model.deskLenApps, (float)model.deskGapApps, model.deskFloatApps != 0, (float)model.deskThickApps};
        spec.apps = model.deskApps != 0, spec.desk = model.deskPill != 0, spec.fill = model.deskFill, spec.fillApps = model.deskFillApps, spec.items = model.deskItems;
        spec.itemsOther = model.deskItemsApps, spec.split = model.deskSplit != 0, itemsCtx = 0;
        spec.mid = model.deskMid, spec.midOther = model.deskSplit ? model.deskMidApps : model.deskMid;
        DropAppItems();
    }
    bool hadMonitor = oldMon.right > oldMon.left && oldMon.bottom > oldMon.top;
    bool changed = hadMonitor && ((oldScale > 0 && oldScale != scale) || !EqualRect(&oldMon, &monitor) || !EqualRect(&oldWork, &work));
    if (changed && wantUp && !editing) {
        FreeDev();
        rest = Goal();
        FitWindow();
        SampleInk();
        inkDark = inkGoal;
        Poke();
    }
}

void DeskPill::OnDisplayChange(HMONITOR on) {
    HMONITOR tgt = on ? on : screen;
    if (Editing()) {
        Measure(tgt);
        FreeDev();
        FitWindow();
        Poke();
        return;
    }
    shapeShift = false;
    Measure(tgt);
    FreeDev();
    rest = Goal();
    if (wantUp) {
        FitWindow();
        SampleInk();
        inkDark = inkGoal;
        Poke();
    }
}

void DeskPill::SyncItems() {
    int want = spec.split && spec.apps ? Ctx() : 0;
    if (want == itemsCtx) return;
    std::swap(spec.items, spec.itemsOther);
    std::swap(spec.mid, spec.midOther); // the middle goes with its set
    itemsCtx = want;
    Poke();
}

void DeskPill::DropAppItems() {
    if (!spec.apps) spec.items.erase(std::remove(spec.items.begin(), spec.items.end(), (int)Model::kWTitle), spec.items.end()); // desktop only: there's never a window to name
}

void DeskPill::Span(int edge, float& lo, float& hi) const {
    if (edge == 0) lo = 0, hi = MW;
    else lo = (work.top - monitor.top) / scale, hi = (work.bottom - monitor.top) / scale; // a side keeps clear of the taskbars
}

DeskPill::Pose DeskPill::Goal(int which) const {
    const Spec::Shape& s = spec.shape[which && spec.apps ? 1 : 0];
    float lo, hi;
    Span(spec.edge, lo, hi);
    Pose p;
    p.thick = s.thick;
    p.len = (std::min)(s.len, hi - lo - 2 * kEdgeRoom);
    float room = p.len / 2 + kEdgeRoom, along = (std::clamp)(lo + (hi - lo) * spec.pos / 1000, lo + room, hi - room), gap = s.floats ? s.gap : 0;
    p.flat = s.floats ? 0.0f : 1.0f;
    if (spec.edge == 0) p.ax = along, p.ay = gap, p.angle = 0;
    else if (spec.edge == 1) p.ax = gap, p.ay = along, p.angle = -90;
    else p.ax = MW - gap, p.ay = along, p.angle = 90;
    return p;
}

D2D1_RECT_F DeskPill::Reach(float pad) const {
    D2D1_RECT_F a = Box(Goal(0), pad);
    if (!spec.apps) return a;
    D2D1_RECT_F b = Box(Goal(1), pad);
    return {(std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
}

bool DeskPill::StepRest(float dt) {
    Pose g = Goal();
    bool calm = model.reduceMotion != 0, moving = false;
    float rate = calm ? 200.0f : kShapeRate;
    moving = anim::Approach(rest.ax, g.ax, rate, dt, 0.02f) || moving;
    moving = anim::Approach(rest.ay, g.ay, rate, dt, 0.02f) || moving;
    moving = anim::Approach(rest.len, g.len, rate, dt, 0.02f) || moving;
    moving = anim::Approach(rest.flat, g.flat, calm ? 200.0f : kShapeFlat, dt, 0.004f) || moving;
    moving = anim::Approach(rest.thick, g.thick, rate, dt, 0.02f) || moving; // each place has its own: it swells or slims as it goes over
    rest.angle = g.angle;
    if (!moving && shapeShift) shapeShift = false, FitWindow(); // settled: an empty window would only be in the way of clicks
    return moving;
}

D2D1_MATRIX_3X2_F DeskPill::Local(const Pose& p) const {
    return D2D1::Matrix3x2F::Rotation(p.angle) * D2D1::Matrix3x2F::Translation(p.ax - view.x, p.ay - view.y); // the same drawing on any edge: type reads along it, its foot away from the edge
}

D2D1_POINT_2F DeskPill::ToLocal(const Pose& p, D2D1_POINT_2F s) const {
    float a = p.angle * 3.14159265f / 180, cs = std::cos(a), sn = std::sin(a), dx = s.x - p.ax, dy = s.y - p.ay;
    return {dx * cs + dy * sn, -dx * sn + dy * cs};
}

D2D1_RECT_F DeskPill::Box(const Pose& p, float pad) const {
    float x0 = -p.len / 2 - pad, x1 = p.len / 2 + pad, y0 = -pad, y1 = p.thick + pad;
    D2D1_RECT_F r = {1e9f, 1e9f, -1e9f, -1e9f};
    float a = p.angle * 3.14159265f / 180, cs = std::cos(a), sn = std::sin(a);
    for (D2D1_POINT_2F q : {D2D1_POINT_2F{x0, y0}, D2D1_POINT_2F{x1, y0}, D2D1_POINT_2F{x0, y1}, D2D1_POINT_2F{x1, y1}}) {
        float sx = p.ax + q.x * cs - q.y * sn, sy = p.ay + q.x * sn + q.y * cs;
        r.left = (std::min)(r.left, sx), r.right = (std::max)(r.right, sx), r.top = (std::min)(r.top, sy), r.bottom = (std::max)(r.bottom, sy);
    }
    r.left = (std::max)(r.left, 0.0f), r.top = (std::max)(r.top, 0.0f), r.right = (std::min)(r.right, MW), r.bottom = (std::min)(r.bottom, MH);
    return r;
}

bool DeskPill::MouseOn() const {
    POINT p;
    RECT r;
    return hwnd && IsWindowVisible(hwnd) && GetCursorPos(&p) && GetWindowRect(hwnd, &r) && PtInRect(&r, p);
}

RECT DeskPill::Spot(HMONITOR on) {
    if (!Editing()) Measure(on);
    D2D1_RECT_F b = Reach(0); // both shapes: looking under only the one it has now, it could flip between them for ever
    return {monitor.left + (LONG)std::floor(b.left * scale), monitor.top + (LONG)std::floor(b.top * scale), monitor.left + (LONG)std::ceil(b.right * scale), monitor.top + (LONG)std::ceil(b.bottom * scale)};
}

void DeskPill::FitWindow() {
    D2D1_RECT_F b = editing ? D2D1_RECT_F{0, 0, MW, MH} : shapeShift ? Reach(kPad) : Box(Goal(), kPad);
    view = {std::floor(b.left * scale) / scale, std::floor(b.top * scale) / scale};
    int w = (int)std::ceil((b.right - view.x) * scale), h = (int)std::ceil((b.bottom - view.y) * scale);
    SetWindowPos(hwnd, HWND_TOPMOST, monitor.left + (int)std::lround(view.x * scale), monitor.top + (int)std::lround(view.y * scale), w, h, SWP_NOACTIVATE);
    if (ctx && !SizeBuffers((UINT)(std::max)(w, 1), (UINT)(std::max)(h, 1))) FreeDev(); // the next frame builds it again at this size
}

bool DeskPill::SizeBuffers(UINT w, UINT h) {
    if (w == bufW && h == bufH && buf) return true;
    ctx->SetTarget(nullptr);
    buf.Reset();
    ComPtr<IDXGISurface> surface;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) || FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) ||
        FAILED(ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &buf)))
        return false;
    bufW = w, bufH = h;
    ctx->SetTarget(buf.Get());
    ctx->SetDpi(96 * scale, 96 * scale);
    return true;
}

bool DeskPill::EnsureDev() {
    if (ctx) return true;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
        return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    RECT wr{};
    GetClientRect(hwnd, &wr);
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = (UINT)(std::max)(wr.right, 1L);
    d.Height = (UINT)(std::max)(wr.bottom, 1L);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx)) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc)) || FAILED(sc.As(&swap))) {
        FreeDev();
        return false;
    }
    swap->SetMaximumFrameLatency(1);
    frameWait = swap->GetFrameLatencyWaitableObject();
    bufW = bufH = 0;
    if (!SizeBuffers(d.Width, d.Height)) {
        FreeDev();
        return false;
    }
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)) ||
        FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) {
        FreeDev();
        return false;
    }
    canvas = std::make_unique<Canvas>(ctx.Get());
    body.Reset();
    bodyLen = -1;
    return true;
}

void DeskPill::FreeDev() {
    if (ctx) ctx->SetTarget(nullptr);
    ui.Forget();
    canvas.reset();
    body.Reset();
    cover.Reset();
    coverBlur.Reset();
    coverId = ~0ull;
    wall.Reset();
    wallBlur.Reset();
    wallFor = 0;
    buf.Reset();
    if (visual) visual->SetContent(nullptr);
    if (dcomp) dcomp->Commit();
    visual.Reset();
    target.Reset();
    dcomp.Reset();
    swap.Reset();
    if (frameWait) CloseHandle(frameWait), frameWait = nullptr;
    ctx.Reset();
    if (d3d) gfx::Settle(d3d.Get());
    d3d.Reset();
    bufW = bufH = 0;
}

void DeskPill::LoadWall(bool sharp) {
    int want = sharp ? 2 : 1;
    if (wallFor >= want || !ctx) return;
    wallFor = want;
    std::wstring path;
    wall.Reset();
    wallBlur.Reset();
    wallPos = DWPOS_FILL;
    if (!WallInfo(monitor, path, wallPos, wallColor)) return; // a plain color: the backdrop and the glass use that
    UINT px = sharp ? (UINT)(std::max)(monitor.right - monitor.left, monitor.bottom - monitor.top) : kGlassMaxPx;
    ComPtr<IWICBitmapSource> src = gfx::DecodeFile(path.c_str(), px);
    if (!src || FAILED(ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &wall))) return;
    D2D1_SIZE_U s = wall->GetPixelSize();
    wallSize = {(float)s.width, (float)s.height};
    wallBlur = gfx::BakeBlur(ctx.Get(), wall.Get(), kBlurSigma);
    if (!sharp) wall.Reset(); // only its blur is needed from here on
}

D2D1_RECT_F DeskPill::WallRect() const { // in screen dips
    if (wallSize.width <= 0 || wallPos == DWPOS_STRETCH) return {0, 0, MW, MH};
    float sx = MW / wallSize.width, sy = MH / wallSize.height, k = wallPos == DWPOS_FIT ? (std::min)(sx, sy) : (std::max)(sx, sy); // fit shows it all; fill, span and the rest cover the screen
    float w = wallSize.width * k, h = wallSize.height * k;
    return Rect((MW - w) / 2, (MH - h) / 2, w, h);
}

void DeskPill::SampleInk() {
    if (FillNow() != kFillClear) return;
    D2D1_RECT_F b = Box(Goal(), 0), r;
    constexpr float kOff = 3, kBand = 10;
    if (spec.edge == 0) r = {b.left, b.bottom + kOff, b.right, b.bottom + kOff + kBand}; // a thin band along its whole length, on its open side: as close as it gets to what's under it without reading its own ink
    else if (spec.edge == 1) r = {b.right + kOff, b.top, b.right + kOff + kBand, b.bottom};
    else r = {b.left - kOff - kBand, b.top, b.left - kOff, b.bottom};
    float lum = sys::Brightness({monitor.left + (LONG)(r.left * scale), monitor.top + (LONG)(r.top * scale), monitor.left + (LONG)(r.right * scale), monitor.top + (LONG)(r.bottom * scale)});
    float was = inkGoal;
    inkGoal = was > 0.5f ? (lum < kInkToWhite ? 0.0f : 1.0f) : (lum > kInkToBlack ? 1.0f : 0.0f);
    if (inkGoal != was) Poke();
}

void DeskPill::Text(int kind, std::wstring& text, int& icon, bool& idle) const {
    icon = 0;
    text = model.DeskItemText(kind, icon);
    idle = text.empty() && icon == 0;
    if (kind == Model::kDMusic && !idle) text = SongStem(model.track.title); // from the whole title: the model's cap can cut a closing bracket off
}

float DeskPill::ItemW(int kind, const std::wstring& text, int icon, bool idle, float thick) {
    float k = TypeScale(thick);
    if (!idle && kind == Model::kDCover) return thick - kArtInset;
    if (!idle && kind == Model::kDBars) return (kBarStep * (kBarCount - 1) + kBarW) * k;
    return (icon ? 15 * k + (text.empty() ? 0 : 5 * k) : 0) + (text.empty() ? 0 : canvas->Measure(text, Weight::Medium, 11.5f * k));
}

bool DeskPill::Has(int kind) const { return std::find(spec.items.begin(), spec.items.end(), kind) != spec.items.end(); }

void DeskPill::SyncCover() {
    uint64_t id = Has(Model::kDCover) && model.hasTrack ? model.track.coverId : 0;
    if (id == coverId) return;
    coverId = id;
    cover.Reset();
    coverBlur.Reset();
    const auto& bytes = model.track.cover;
    if (!id || !bytes || bytes->empty()) return;
    ComPtr<IWICBitmapSource> src = gfx::Decode(bytes->data(), bytes->size(), kCoverPx);
    if (src && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &cover))) coverBlur = gfx::BakeBlur(ctx.Get(), cover.Get(), 6.0f);
}

bool DeskPill::StepBars(float dt, double now) {
    if (!Has(Model::kDBars) || !model.vizOn) return false;
    bool playing = model.hasTrack && model.track.playing;
    float l = 0, r = 0;
    if (playing) model.Levels(l, r);
    float level = std::sqrt((l + r) / 2); // lifts quiet passages so they stay alive
    bool moving = false;
    for (int i = 0; i < kBarCount; ++i) {
        float wobble = 0.55f + 0.45f * std::sin((float)now * (5.0f + i * 1.7f) + i * 1.3f); // each on its own rhythm
        float want = playing ? anim::Clamp01(level * wobble * 1.4f) : 0;
        bars[i] += (want - bars[i]) * (std::min)(1.0f, dt * 14);
        moving = moving || std::fabs(want - bars[i]) > 0.01f || want > 0.01f;
    }
    return moving;
}

void DeskPill::FitSong(std::vector<Placed>& all, size_t from, size_t to, float room, float gap, float thick) {
    float sum = 0;
    size_t song = to;
    for (size_t i = from; i < to; ++i) {
        sum += all[i].w + (i > from ? gap : 0);
        if (Stretchy(all[i].kind) && !all[i].idle) song = i;
    }
    if (sum <= room || song == to) return;
    Placed& p = all[song];
    float others = sum - p.w;
    p.w = (std::min)(p.w, (std::max)(room - others, kSongLeast * TypeScale(thick))); // it trims, ending in "..."
}

int DeskPill::Anchor(const std::vector<int>& items) const { // the chosen item holds the middle; unchosen, the clock does, as it always has
    int mid = spec.mid;
    if (mid == Model::kMidNone) return -1;
    if (mid >= 0 && mid != Model::kDLyrics && In(items, mid)) return mid; // the lyrics only ever borrow the middle
    return In(items, Model::kDClock) ? (int)Model::kDClock : -1;
}

std::vector<DeskPill::Placed> DeskPill::LayOut(const std::vector<int>& items, float len, float thick) {
    float k = TypeScale(thick), gap = kItemGap * k, inner = len - thick; // the round ends keep their own room
    std::vector<Placed> all;
    int clockAt = -1;
    int anchor = Anchor(items);
    for (int kind : items) {
        if (kind == Model::kDLyrics) continue; // no place of its own: it borrows the middle's
        Placed p{kind, 0, 0, L"", 0, false};
        Text(kind, p.text, p.icon, p.idle);
        if (p.idle) {
            if (!editing) continue; // nothing to show: it takes no room
            p.text = Model::DeskItemName(kind);
        }
        if (kind == anchor) clockAt = (int)all.size();
        p.w = ItemW(kind, p.text, p.icon, p.idle, thick);
        all.push_back(std::move(p));
    }
    Placed sung{Model::kDLyrics, 0, 0, L"", 0, false};
    if (!editing && In(items, Model::kDLyrics)) sung.text = model.DeskItemText(Model::kDLyrics, sung.icon); // the editor shows the middle as it really is
    if (!sung.text.empty()) { // a line is sung: it takes the middle's place until the next gap
        int mid = clockAt >= 0 ? clockAt : (int)all.size() / 2;
        sung.w = ItemW(sung.kind, sung.text, sung.icon, false, thick);
        if (mid < (int)all.size()) all[mid] = std::move(sung);
        else all.push_back(std::move(sung));
        clockAt = mid;
    }
    if (clockAt >= 0 && Stretchy(all[clockAt].kind)) { // the sung line in the middle gives way to what's beside it
        float before = 0, after = 0;
        for (int i = 0; i < (int)all.size(); ++i) {
            if (i == clockAt) continue;
            float w = Stretchy(all[i].kind) ? (std::min)(all[i].w, kSongLeast * k) : all[i].w;
            (i < clockAt ? before : after) += w + gap;
        }
        all[clockAt].w = (std::min)(all[clockAt].w, (std::max)(inner - 2 * (std::max)(before, after), kSongLeast * k));
    }
    std::vector<Placed> out;
    midAt = -1;
    if (clockAt < 0) { // no clock to anchor on: one row, centered
        FitSong(all, 0, all.size(), inner, gap, thick);
        float sum = 0;
        size_t n = 0;
        for (; n < all.size(); ++n) { // what doesn't fit waits at the far end
            float next = sum + (n ? gap : 0) + all[n].w;
            if (next > inner) break;
            sum = next;
        }
        float x = -sum / 2;
        for (size_t i = 0; i < n; ++i) {
            all[i].x = x;
            x += all[i].w + gap;
            out.push_back(all[i]);
        }
        midAt = all.size() / 2 < n ? (int)(all.size() / 2) : -1; // where a sung line would land
        return out;
    }
    all[clockAt].x = -all[clockAt].w / 2;
    float half = inner / 2 - all[clockAt].w / 2 - gap;
    FitSong(all, 0, (size_t)clockAt, half, gap, thick);
    FitSong(all, (size_t)clockAt + 1, all.size(), half, gap, thick);
    float used = 0, x = -inner / 2;
    for (int i = 0; i < clockAt; ++i) { // before the clock, from the pill's start
        used += all[i].w + (i ? gap : 0);
        if (used > half) break;
        all[i].x = x;
        x += all[i].w + gap;
        out.push_back(all[i]);
    }
    midAt = (int)out.size();
    out.push_back(all[clockAt]);
    used = 0;
    x = inner / 2;
    std::vector<Placed> right;
    for (int i = (int)all.size() - 1; i > clockAt; --i) { // after it, from the pill's end
        used += all[i].w + (i == (int)all.size() - 1 ? 0 : gap);
        if (used > half) break;
        x -= all[i].w;
        all[i].x = x;
        x -= gap;
        right.push_back(all[i]);
    }
    out.insert(out.end(), right.rbegin(), right.rend());
    return out;
}

float DeskPill::NeedAll(const std::vector<int>& items) {
    float need = NeedLen(items, spec.shape[0].thick);
    return spec.apps ? (std::max)(need, NeedLen(items, spec.shape[1].thick)) : need;
}

void DeskPill::Grow(const std::vector<int>& items, float least) {
    for (Spec::Shape& s : spec.shape) s.len = (std::max)({s.len, least, NeedLen(items, s.thick)});
}

float DeskPill::NeedLen(const std::vector<int>& items, float thick) {
    float gap = kItemGap * TypeScale(thick), left = 0, right = 0, mid = 0;
    bool past = false;
    int anchor = Anchor(items);
    for (int kind : items) {
        if (kind == Model::kDLyrics) continue; // it borrows the middle: no room of its own
        std::wstring text;
        int ic = 0;
        bool idle = false;
        Text(kind, text, ic, idle);
        if (idle) text = Model::DeskItemName(kind);
        float w = ItemW(kind, text, ic, idle, thick);
        if (Stretchy(kind)) w = (std::min)(w, kSongLeast * TypeScale(thick)); // how long a song's title or a line is never decides how long the pill must be
        if (kind == anchor) mid = w, past = true;
        else (past ? right : left) += w + gap;
    }
    if (anchor < 0) return left - (left > 0 ? gap : 0) + thick + 2;
    return 2 * (std::max)(left, right) + mid + thick + 2;
}

ComPtr<ID2D1Geometry> DeskPill::BuildBody(float len, float thick, float flat) const {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> s;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&s))) return nullptr;
    float xl = -len / 2, xr = len / 2, r = thick / 2;
    if (flat >= 0.5f) { // stuck: square to the edge, shoulders flowing into it, as the island
        float f = (std::min)(kFillet, r) * (flat * 2 - 1);
        s->BeginFigure({xl - f, 0}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({xr + f, 0});
        if (f > 0.01f) s->AddArc({{xr, f}, {f, f}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xr, thick - r});
        s->AddArc({{xr - r, thick}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xl + r, thick});
        s->AddArc({{xl, thick - r}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xl, f});
        if (f > 0.01f) s->AddArc({{xl - f, 0}, {f, f}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    } else { // floating: round all over; the top corners round out as it leaves the edge
        float tr = (std::max)(r * (1 - flat * 2), 0.01f);
        s->BeginFigure({xl + tr, 0}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({xr - tr, 0});
        s->AddArc({{xr, tr}, {tr, tr}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xr, thick - r});
        s->AddArc({{xr - r, thick}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xl + r, thick});
        s->AddArc({{xl, thick - r}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({xl, tr});
        s->AddArc({{xl + tr, 0}, {tr, tr}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    }
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return path;
}

void DeskPill::DrawPill(const Theme& t, const Pose& p, float alpha) {
    Canvas& c = *canvas;
    D2D1_MATRIX_3X2_F local = Local(p);
    if (std::fabs(p.len - bodyLen) > 0.01f || std::fabs(p.thick - bodyThick) > 0.01f || std::fabs(p.flat - bodyFlat) > 0.001f) {
        body = BuildBody(p.len, p.thick, p.flat);
        bodyLen = p.len, bodyThick = p.thick, bodyFlat = p.flat;
    }
    if (!body) return;
    Color white{1, 1, 1, 1}, black{0.03f, 0.03f, 0.04f, 1};
    Color ink = t.text, dim = t.textDim, lit = t.accentHi;
    int fill = FillNow();
    if (fill == kFillAccent) ink = dim = lit = theme::Luma(t.accent) > 0.30f ? black : white;
    else if (fill == kFillGlass) ink = dim = lit = theme::TintIsPale(model.GlassTint()) ? black : white;
    else if (fill == kFillClear) ink = dim = lit = white.Mix(black, inkDark);
    ctx->SetTransform(local);
    if (fill != kFillClear && p.flat < 0.5f) c.Fill(Rect(-p.len / 2 - 1, 1.5f, p.len + 2, p.thick + 1.5f), p.thick / 2 + 1, Color{0, 0, 0, 0.16f * alpha}); // floating: a soft lift off the desktop
    switch (fill) {
    case kFillTheme: {
        Color b = t.bg;
        b.a *= model.BgOpacity() / 100.0f;
        c.FillGeometry(body.Get(), b.Alpha(alpha));
        c.StrokeGeo(body.Get(), t.hairTop.Alpha(alpha), 1);
        break;
    }
    case kFillAccent:
        c.FillGeometry(body.Get(), t.accent.Alpha(alpha));
        break;
    case kFillGlass: { // only ever on the desktop, so what's behind it is the wallpaper: its blur, cut to the pill. nothing live to pay for
        if (!wallBlur) {
            c.FillGeometry(body.Get(), wallColor.Alpha(alpha));
        } else {
            ctx->SetTransform(D2D1::Matrix3x2F::Translation(-view.x, -view.y));
            ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), body.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Rotation(p.angle) * D2D1::Matrix3x2F::Translation(p.ax, p.ay)), nullptr);
            ctx->DrawBitmap(wallBlur.Get(), WallRect(), alpha, D2D1_INTERPOLATION_MODE_LINEAR);
            ctx->PopLayer();
            ctx->SetTransform(local);
        }
        int tint = model.GlassTint();
        c.FillGeometry(body.Get(), tint >= 0 ? Color::Rgb((uint32_t)tint).Alpha((theme::TintIsPale(tint) ? 0.36f : 0.42f) * alpha) : Color{0, 0, 0, 0.30f * alpha}); // the look's tint, as a panel wears it
        c.StrokeGeo(body.Get(), Color{1, 1, 1, 0.26f * alpha}, 1.1f);
        break;
    }
    default:
        break; // clear: the ink, straight on the desktop
    }

    float k = TypeScale(p.thick), cy = p.thick / 2, size = 11.5f * k, box = kTextBox * k;
    bool holding = editing && drag != Drag::None;
    SyncItems(); // on the desktop or over an app: its set for there
    if (!editing && input.wheel != 0) { // the wheel over the score: the next game, or the one before; before the layout, so this frame shows it
        D2D1_POINT_2F m = ToLocal(p, input.mouse);
        for (const Placed& it : placed)
            if (it.kind == Model::kDScore && m.x >= it.x - 4 && m.x <= it.x + it.w + 4) {
                model.StepScore(input.wheel > 0 ? -1 : 1);
                input.wheel = 0;
                break;
            }
    }
    placed = LayOut(spec.items, p.len, p.thick);
    float content = anim::Clamp01((alpha - 0.35f) / 0.65f);
    auto draw = [&](const Placed& it, float a, float dy) {
        float x = it.x, y = cy + dy;
        if (!it.idle && it.kind == Model::kDCover) { // the song's art, rounded like an app icon
            float art = p.thick - kArtInset;
            D2D1_RECT_F r = Rect(x, y - art / 2, art, art);
            if (cover) c.Art(cover.Get(), coverBlur.Get(), r, art * 0.24f, a);
            else c.Fill(r, art * 0.24f, ink.Alpha(0.14f * a)), c.Icon(glyph::Music, {x + art / 2, y}, art * 0.6f, lit.Alpha(a), true);
            return;
        }
        if (!it.idle && it.kind == Model::kDBars) {
            for (int i = 0; i < kBarCount; ++i) {
                float bh = (kBarLow + kBarRise * bars[i]) * k, bw = kBarW * k;
                c.Fill(Rect(x + i * kBarStep * k, y - bh / 2, bw, bh), bw / 2, lit.Alpha(a));
            }
            return;
        }
        float tx = x;
        if (it.icon) {
            float is = 15 * k;
            c.Icon(it.icon, {x + is / 2, y}, is, lit.Alpha(a), true);
            tx += is + 5 * k;
        }
        if (!it.text.empty()) c.Text(it.text, Rect(tx, y - box / 2, x + it.w - tx + 2, box), Weight::Medium, size, (it.kind == Model::kDClock || it.kind == Model::kDScore || it.kind == Model::kDLyrics ? ink : dim).Alpha(a));
    };
    std::wstring key; // the middle: a line coming, changing or going crossfades instead of snapping
    if (midAt >= 0) key = placed[midAt].kind == Model::kDLyrics ? L"L" + placed[midAt].text : L"K" + std::to_wstring(placed[midAt].kind);
    double now = Seconds();
    if (key != midKey) {
        bool sung = (!key.empty() && key[0] == L'L') || (!midKey.empty() && midKey[0] == L'L');
        midFading = sung && !editing;
        if (midFading) midSince = now, midWas = midLast, midHad = midLastOn;
        midKey = key;
    }
    float u = 1; // how far through the fade
    if (midFading) {
        u = (float)((now - midSince) / kMidFade);
        if (u >= 1) midFading = false, u = 1;
    }
    auto soft = [](float v) { v = anim::Clamp01(v); return v * v * (3 - 2 * v); };
    float glide = 1 - (1 - u) * (1 - u) * (1 - u); // fast off the mark, settling slowly
    float e = soft((u - kMidIn) / (1 - kMidIn)), gone = soft(u / kMidOut); // the new one's in, the old one's out
    float rise = model.reduceMotion ? 0 : kMidRise * k;
    for (int i = 0; i < (int)placed.size(); ++i) {
        Placed& it = placed[i];
        bool mid = i == midAt && midFading;
        float eased = ui.Smooth(Ui::Id("dk.x", it.kind), it.x, holding ? 22.0f : 16.0f); // neighbours slide aside, they don't jump
        if (!mid) it.x = eased; // a new line rises in where it belongs, not sideways from where the last one sat
        if (drag == Drag::Item && it.kind == dragKind && moved) continue; // its place stays open under the mouse
        float a = content * (it.idle ? 0.45f : 1.0f);
        draw(it, mid ? a * e : a, mid ? rise * (1 - glide) : 0);
    }
    if (midFading && midHad) draw(midWas, content * (1 - gone), -rise * glide); // the one before, on its way out
    midLastOn = midAt >= 0;
    if (midLastOn) midLast = placed[midAt];
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-view.x, -view.y));
}

void DeskPill::Want(bool up, HMONITOR on) {
    if (Editing()) return;
    if (up == wantUp && (!up || on == screen)) return;
    wantUp = up;
    if (!hwnd) return;
    if (up) {
        Measure(on);
        shapeShift = false;
        rest = Goal();
        FitWindow();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        model.WantDeskAudio();
        SampleInk();
        inkDark = inkGoal;
    }
    appear.target = up ? 100.0f : 0.0f;
    appear.damp = model.reduceMotion ? 1.0f : up ? 0.62f : 0.92f;
    cursor = IDC_HAND;
    Poke();
}

void DeskPill::Second() {
    if (studio && editing) Poke(); // the clocks in it tick
    if (!wantUp || Editing()) return;
    if (std::find(spec.items.begin(), spec.items.end(), (int)Model::kWWeather) != spec.items.end()) model.Prefetch(); // a no-op while the forecast is fresh
    if (std::find(spec.items.begin(), spec.items.end(), (int)Model::kDVolume) != spec.items.end()) model.ReadAudio();
    static int inkTick = 0;
    constexpr int kInkEvery = 2; // seconds: a page scrolls, a slideshow moves on; a thin band is a cheap read
    if (FillNow() == kFillClear && ++inkTick % kInkEvery == 0) SampleInk();
    Watch();
}

void DeskPill::Watch() {
    std::wstring now = std::to_wstring(model.hasTrack ? model.track.coverId : 0) + (model.hasTrack && model.track.playing ? L"p|" : L"|"); // a new song's art; the bars start and stop
    for (int kind : spec.items) {
        std::wstring text;
        int ic = 0;
        bool idle = false;
        Text(kind, text, ic, idle); // as it's drawn, the sung line too
        now += text + L"|" + std::to_wstring(ic) + L"|";
    }
    if (now != sig) sig = std::move(now), Poke();
    double wait = Has(Model::kDLyrics) ? model.LyricWait() : -1;
    if (wait >= 0 && wait < kLyricSoon) SetTimer(hwnd, kLyricTimer, (UINT)(wait * 1000) + kLyricLateMs, nullptr); // a new line lands between the seconds
}

void DeskPill::Frame(const Theme& shell) {
    double now = Seconds();
    float dt = lastFrame > 0 ? (float)(std::min)(now - lastFrame, 0.05) : 1 / 60.0f;
    lastFrame = now;
    if (!editing && !leaving && appear.target <= 0 && appear.value <= 0.5f) { // gone: the window and its memory too
        appear = anim::Spring{};
        ShowWindow(hwnd, SW_HIDE);
        FreeDev();
        gfx::GiveBack();
        dirty = false;
        lastFrame = 0;
        return;
    }
    if (!EnsureDev()) {
        dirty = false;
        return;
    }
    bool moving = (editing || leaving) && StepEdit(dt);
    if (leaving && intro <= 0.01f) { // before the frame opens, Leave can drop the buffers; then on to a real frame: the wait that brought us here is only paid back by a present
        Leave();
        moving = true;
        if (!EnsureDev()) {
            dirty = false;
            return;
        }
    }
    bool arranging = editing || leaving;
    if (!arranging) {
        moving = appear.Step(dt) || moving;
        moving = StepRest(dt) || moving;
        if (!EnsureDev()) { // settling can drop the buffers
            dirty = false;
            return;
        }
    }
    if (FillNow() == kFillGlass || editing) LoadWall(editing);
    moving = anim::Approach(inkDark, inkGoal, 8, dt, 0.004f) || moving;
    SyncCover();
    bool barsMoving = StepBars(dt, now);
    canvas->NewFrame();
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ui.Begin(*canvas, input, dt, now);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-view.x, -view.y));
    if (arranging) {
        DrawBackdrop();
        if (studio) {
            DrawStudio(shell);
        } else {
            bool shown = TabLong() && !askLong; // the regular pill picked here, or still asking: no long one to arrange
            DrawGuides(shell);
            if (shown) DrawPill(shell, pose, 1);
            if (editing && askLong) {
                DrawAsk(shell);
            } else if (editing) {
                if (shown) DrawGrips(shell);
                DrawBar(shell);
                if (editing && input.pressed && !pressedOnBar) {
                    if (shown) PressPill();
                    else bareDown = true; // nothing to hold: a click away finishes
                }
                if (editing && drag != Drag::None) DragNow();
                if (editing) DrawFloater(shell);
                if (editing && input.released) Land();
            }
        }
    } else {
        float s = appear.value / 100, a = anim::Clamp01(s); // grows from a short bead, sliding out from its edge; a touch past full on the bounce
        Pose p = rest;
        p.len = p.len * (0.45f + 0.55f * s);
        float back = (1 - s) * (p.thick + (Sh().floats ? Sh().gap : 0) + 6);
        p.ax += spec.edge == 1 ? -back : spec.edge == 2 ? back : 0;
        p.ay += spec.edge == 0 ? -back : 0;
        DrawPill(shell, p, a);
    }
    if (editing && drag == Drag::None && ui.Hand()) cursor = IDC_HAND;
    ui.End();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = ctx->EndDraw();
    bool busy = ui.Busy();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    if (hr == D2DERR_RECREATE_TARGET) {
        FreeDev();
        dirty = true;
        return;
    }
    bool calm = !moving && !busy && drag == Drag::None; // only the little bars are moving: a fraction of the frames
    hr = swap->Present(calm && barsMoving ? kBarsSync : 1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        FreeDev();
        dirty = true;
        return;
    }
    if (inkDue > 0 && now >= inkDue) inkDue = 0, SampleInk();
    dirty = moving || busy || barsMoving || midFading || drag != Drag::None || inkDue > 0 || leaving || (!editing && appear.target <= 0); // going away: on to the frame that lets go of everything
}
