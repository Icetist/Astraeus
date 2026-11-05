// The fullscreen dashboard's window: over everything on its screen, never the keyboard's, drawn only when something on it changed.
#include "board.h"

#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "anim.h"
#include "glyphs.h"
#include "starfall.h"

namespace {

constexpr wchar_t kClass[] = L"AstraeusBoard";
constexpr int kMinRows = 4, kMaxRows = 12;
constexpr double kButtonsSec = 3; // its two buttons show this long after the mouse last moved
constexpr double kAmbientFps = 12; // music moving on it: enough to look alive, little enough to cost nothing
constexpr double kMouseGap = 1 / 30.0, kDragGap = 1 / 60.0; // a tile under the mouse follows it this often; a drag closer
constexpr float kStillDt = 10; // a step so long every ease lands at once: numbers change, they don't glide
constexpr int kLiveEvery = 2; // seconds between repaints of the live numbers
constexpr int kDiskEvery = 60, kWeatherEvery = 60; // seconds between looks at the drive and the forecast
constexpr int kTrimEvery = 30; // seconds of rest between handing the driver's scratch memory back

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / freq.QuadPart;
}

bool Touch(const D2D1_RECT_F& a, const D2D1_RECT_F& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

bool Live(int kind) { return kind == Model::kBdCpu || kind == Model::kBdGpu || kind == Model::kBdRam || kind == Model::kBdNet || kind == Model::kBdProcs; }

bool Plays(int kind) { return kind == Model::kBdMedia || kind == Model::kBdLyrics; }

uint64_t ColorsKey(std::initializer_list<Color> cs) { // a theme worth repainting for
    uint64_t h = 14695981039346656037ull;
    for (const Color& c : cs)
        for (float v : {c.r, c.g, c.b, c.a}) h = (h ^ (uint64_t)(v * 1024)) * 1099511628211ull;
    return h;
}

uint64_t ShellKey(const Theme& t) { return ColorsKey({t.bg, t.card, t.text, t.textDim, t.accent, t.accentHi}) + t.stars; }


bool FullOn(HMONITOR m) { // a game or a video fills that screen in front: it gets the screen, the dashboard waits
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != m || (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId())) return false;
    wchar_t cls[24] = {};
    GetClassNameW(fg, cls, 24);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcsncmp(cls, L"Shell_", 6)) return false;
    LONG style = GetWindowLongW(fg, GWL_STYLE), ex = GetWindowLongW(fg, GWL_EXSTYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (IsZoomed(fg) && (style & WS_THICKFRAME))) return false; // maximized, not fullscreen
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false; // an overlay, a dock
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(m, &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

}

Board::Board(Model& m, HMONITOR mon, const std::wstring& screenName) : model(m), name(screenName), screen(mon) {
    static bool registered = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) != 0;
    }();
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!registered || !screen || !GetMonitorInfoW(screen, &mi)) return;
    UINT dx = 96, dy = 96;
    scale = SUCCEEDED(GetDpiForMonitor(screen, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    const RECT& r = mi.rcMonitor;
    UINT pw = (UINT)(r.right - r.left), ph = (UINT)(r.bottom - r.top);
    W = pw / scale;
    H = ph / scale;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Astraeus Dashboard", WS_POPUP, r.left, r.top,
                           (int)pw, (int)ph, nullptr, nullptr, GetModuleHandleW(nullptr), this); // the whole screen, no taskbar button, never the keyboard's
    if (!hwnd) return;
    access.Attach(hwnd, L"Astraeus dashboard");
    BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    if (!MakeDev(pw, ph)) {
        ctx.Reset();
        return;
    }
    Fit();
    frameWait = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameWait) frameWait = CreateWaitableTimerW(nullptr, TRUE, nullptr); // older windows: a coarser one
    if (!frameWait) {
        ctx.Reset();
        return;
    }
    Arm(0);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

Board::~Board() {
    canvas.reset();
    cover.Reset();
    coverBlur.Reset();
    lyrics = LyricView{};
    visual.Reset();
    target.Reset();
    surface.Reset();
    dcomp.Reset();
    if (starfall::Store().rt == ctx.Get()) starfall::Forget(); // its brushes would pin this device
    ctx.Reset();
    gfx::Settle(d3d.Get());
    if (frameWait) CloseHandle(frameWait);
    if (hwnd) DestroyWindow(hwnd);
}

bool Board::MakeDev(UINT pxW, UINT pxH) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
        return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || (device->SetMaximumTextureMemory(8ull << 20), false) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        return false;
    dxgi->SetGPUThreadPriority(-7); // a game or an editor on the other screen always comes first
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)))
        return false;
    if (FAILED(dcomp->CreateSurface(pxW, pxH, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &surface)) && // every pixel painted: the compositor needn't blend a whole screen
        FAILED(dcomp->CreateSurface(pxW, pxH, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)))
        return false;
    if (FAILED(visual->SetContent(surface.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) return false;
    dc->SetDpi(96 * scale, 96 * scale);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ctx = dc;
    canvas = std::make_unique<Canvas>(ctx.Get());
    return true;
}

void Board::Fit() {
    rows = (std::clamp)((int)std::lround(Model::kBoardCols * H / W), kMinRows, kMaxRows); // cells close to square on any screen
    cellW = (W - 2 * kMargin - (Model::kBoardCols - 1) * kGap) / Model::kBoardCols;
    cellH = (H - 2 * kMargin - (rows - 1) * kGap) / rows;
    const Model::BoardSpot* spot = model.Spot(name);
    tiles.clear();
    for (Model::BoardTile tl : spot && spot->edited ? spot->tiles : model.BoardPreset(rows)) { // never arranged: the preset, made for this screen's shape
        if (tl.y >= rows || !model.BoardOffered(tl.kind)) continue; // made on a taller screen, or this pc can't show it: it stays saved, it just isn't drawn
        tl.h = (std::min)(tl.h, rows - tl.y);
        tiles.push_back(tl);
    }
}

RECT Board::Pixels(const D2D1_RECT_F& r) const {
    RECT px = {(LONG)std::floor(r.left * scale) - 1, (LONG)std::floor(r.top * scale) - 1, (LONG)std::ceil(r.right * scale) + 1, (LONG)std::ceil(r.bottom * scale) + 1}; // a pixel over: the card's hairline
    px.left = (std::max)(px.left, 0L), px.top = (std::max)(px.top, 0L);
    px.right = (std::min)(px.right, (LONG)std::lround(W * scale)), px.bottom = (std::min)(px.bottom, (LONG)std::lround(H * scale));
    return px;
}

void Board::TakeKeys() {
    if (!hwnd) return;
    keysFrom = GetForegroundWindow();
    keyMode = true;
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    repaint = true;
    Poke();
}

void Board::Arm(double seconds) {
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((std::max)(seconds, 0.0) * 1e7) - 1; // negative: from now
    if (frameWait) SetWaitableTimer(frameWait, &due, 0, nullptr, nullptr, FALSE);
    armed = true;
}

void Board::Poke() {
    dirty = true;
    if (!armed) Arm(lastFrame + (input.down ? kDragGap : kMouseGap) - Seconds());
}

D2D1_RECT_F Board::CellRect(int x, int y, int w, int h) const {
    float l = kMargin + x * (cellW + kGap), tp = kMargin + y * (cellH + kGap);
    return {l, tp, l + w * cellW + (w - 1) * kGap, tp + h * cellH + (h - 1) * kGap};
}

D2D1_POINT_2F Board::CellAt(D2D1_POINT_2F p) const {
    return {(p.x - kMargin + kGap / 2) / (cellW + kGap), (p.y - kMargin + kGap / 2) / (cellH + kGap)};
}

LRESULT CALLBACK Board::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = reinterpret_cast<Board*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && !self->hwnd) self->hwnd = h;
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT Board::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // a click never takes the keyboard from the app you're in
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x != lastPt.x || pt.y != lastPt.y) movedAt = Seconds(); // windows resends moves on its own
        lastPt = pt;
        input.mouse = {pt.x / scale, pt.y / scale};
        TRACKMOUSEEVENT tm = {sizeof(tm), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tm);
        Poke();
        return 0;
    }
    case WM_MOUSELEAVE:
        input.mouse = {-1e6f, -1e6f};
        Poke();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        input.mouse = input.pressPos = {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
        input.pressed = input.down = true;
        Poke();
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        input.released = true;
        input.down = false;
        Poke();
        return 0;
    case WM_CAPTURECHANGED:
        if (input.down) input.down = false, input.released = true, Poke();
        return 0;
    case WM_RBUTTONUP:
        input.rclick = true;
        if (!editing) Edit(); // right click: arrange the tiles
        Poke();
        return 0;
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        Poke();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, grab >= 0 ? (sizing ? IDC_SIZENWSE : IDC_SIZEALL) : hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        return 0; // its switch in settings, its button or the shortcut take it away
    case WM_GETOBJECT: {
        LRESULT out = 0;
        if (access.OnGetObject(wp, lp, out)) return out;
        break;
    }
    case Access::kMsg:
        access.Apply(ui, wp, lp);
        repaint = true;
        Poke();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && keyMode) {
            keyMode = false;
            ui.Unfocus();
            repaint = true;
            Poke();
        }
        break;
    case WM_KEYUP:
        if (wp == VK_RETURN || wp == VK_SPACE) ui.KeyHold(false), Poke();
        return 0;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, first = !(lp & (1 << 30)); // not a held key's repeat
        if (wp == VK_TAB) ui.Tab(shift ? -1 : 1);
        else if ((wp == VK_RETURN || wp == VK_SPACE) && first) ui.FireFocus(), ui.KeyHold(true);
        else if ((wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) && ui.Focused()) {
            if (ui.SliderFocused()) ui.StepFocus(wp == VK_RIGHT || wp == VK_UP ? 1 : -1);
            else ui.Tab(wp == VK_RIGHT || wp == VK_DOWN ? 1 : -1);
        } else if (wp == VK_ESCAPE) { // one step back at a time: the control, the arranging, then the keyboard goes home
            if (ui.Focused()) ui.Unfocus();
            else if (!editing && keysFrom && IsWindow(keysFrom)) SetForegroundWindow(keysFrom);
        } else if (wp == 'E' && !editing) Edit();
        repaint = true;
        Poke();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

uint64_t Board::Look(int kind) const { // what the tile would show, coarsely: the same as when it was painted means no paint
    auto pct = [](double v) { return std::to_wstring((int)std::lround(v * 100)); };
    const sys::Stats& x = model.stats;
    std::wstring s;
    switch (kind) {
    case Model::kBdClock:
    case Model::kBdCalendar: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        s = std::to_wstring(st.wDay) + L':' + std::to_wstring(kind == Model::kBdClock ? st.wHour * 60 + st.wMinute : calMonth);
        break;
    }
    case Model::kBdCpu: s = pct(x.cpu) + L"," + std::to_wstring(model.cpuHist.Version()); break;
    case Model::kBdRam: s = pct(x.mem) + L"," + std::to_wstring(model.ramHist.Version()); break;
    case Model::kBdGpu: s = pct(x.gpu); break;
    case Model::kBdNet: s = fmt::Rate(model.net.upBps) + fmt::Rate(model.net.downBps) + model.net.ssid + std::to_wstring(model.downHist.Version()); break;
    case Model::kBdDisk: s = pct(diskUsed); break;
    case Model::kBdWeather: s = std::to_wstring(model.wx.valid) + std::to_wstring((int)model.wx.tempC) + L"," + std::to_wstring(model.wx.code); break;
    case Model::kBdMedia:
    case Model::kBdLyrics: {
        const sys::Track& tr = model.track;
        s = std::to_wstring(model.hasTrack) + tr.title + tr.artist + std::to_wstring(tr.playing) + std::to_wstring(tr.coverId);
        if (kind == Model::kBdMedia && tr.duration > 0) s += L"," + std::to_wstring(tr.Elapsed() / 10000000); // its clock and progress line
        break;
    }
    case Model::kBdProcs:
        for (const auto& p : model.procs) s += p.name + pct(p.cpu);
        break;
    case Model::kBdVolume: s = pct(model.vol) + std::to_wstring(model.muted); break;
    case Model::kBdTodo:
        for (const auto& td : model.todos) s += td.text + (td.done ? L"1" : L"0");
        break;
    case Model::kBdWork: s = std::to_wstring(model.wsNow.current) + L"," + std::to_wstring(model.wsNow.list.size()); break;
    case Model::kBdShelf: s = std::to_wstring(model.shelf.Version()); break;
    case Model::kBdTimer: s = std::to_wstring(model.Activity()) + model.ActivityText(); break;
    case Model::kBdServers:
        for (const auto& h : model.hostNow) s += h.host + std::to_wstring(h.up) + std::to_wstring(h.ms);
        break;
    case Model::kBdBattery: s = std::to_wstring(model.batPct) + std::to_wstring(model.charging); break;
    }
    return std::hash<std::wstring>{}(s);
}

void Board::Second(const Theme& shell, const Theme& media) {
    if (!hwnd) return;
    access.Enable(model.a11yReader != 0);
    bool full = FullOn(screen);
    if (full != covered) { // a game or video took the screen: out of its way, and no frames; back when it's gone
        covered = full;
        ShowWindow(hwnd, full ? SW_HIDE : SW_SHOWNOACTIVATE); // its surface keeps the picture
        if (!full) Poke();
    }
    if (covered) return;
    if (GetWindow(hwnd, GW_HWNDPREV)) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // a dock or a taskbar that came over it: under it again
    auto has = [&](int k) { return std::any_of(tiles.begin(), tiles.end(), [k](const Model::BoardTile& tl) { return tl.kind == k; }); };
    if (has(Model::kBdGpu)) gpuHist.Push((float)(std::max)(0.0, model.stats.gpu));
    if (has(Model::kBdDisk) && (diskUsed < 0 || ++diskTick >= kDiskEvery)) {
        diskTick = 0;
        if (!sys::extras::SystemDrive(diskUsed, diskFree)) diskUsed = 0;
    }
    if (has(Model::kBdWeather) && wxTick++ % kWeatherEvery == 0) model.Prefetch(); // a no-op while the forecast is fresh
    bool liveTick = ++tick % kLiveEvery == 0;
    if (has(Model::kBdVolume) && liveTick) model.ReadAudio();

    bool change = false;
    uint64_t sk = ShellKey(shell), mk = ColorsKey({media.bg, media.text, media.accent, media.accentHi});
    if (sk != shellKey) shellKey = sk, repaint = change = true; // the whole look changed
    bool songLook = mk != mediaKey;
    mediaKey = mk;
    stale.resize(tiles.size(), 1);
    seen.resize(tiles.size(), 0);
    for (size_t i = 0; i < tiles.size(); ++i) {
        int kind = tiles[i].kind;
        if (Live(kind) && !liveTick) continue; // the gauges move every couple of seconds, as a monitor's do
        if (Look(kind) != seen[i] || (songLook && Plays(kind))) stale[i] = 1, change = true; // only what changed is worth a paint
    }
    if ((Seconds() - movedAt < kButtonsSec) != buttons) change = true;
    if (change && !editing) Poke();
    if (!busy && ++rest >= kTrimEvery) { // resting: the driver's scratch memory goes back
        rest = 0;
        ComPtr<IDXGIDevice3> dx;
        if (d3d && SUCCEEDED(d3d.As(&dx))) dx->Trim();
    }
}

void Board::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id == coverId) return;
    coverId = id;
    cover.Reset();
    coverBlur.Reset();
    const auto& bytes = model.track.cover;
    if (!id || !bytes || bytes->empty()) return;
    ComPtr<IWICBitmapSource> src = gfx::Decode(bytes->data(), bytes->size(), 512);
    if (src && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &cover))) coverBlur = gfx::BakeBlur(ctx.Get(), cover.Get(), 30.0f);
}

void Board::SyncLyrics(double now) { // as the fullscreen player does: asked for once the length is known, picked up when they land
    const sys::Track& tr = model.track;
    bool want = std::any_of(tiles.begin(), tiles.end(), [](const Model::BoardTile& tl) { return tl.kind == Model::kBdLyrics; }) && model.fullLyrics &&
                model.hasTrack && tr.music && !tr.title.empty();
    std::wstring key = want ? sys::LyricSrc::Key(tr.title, tr.artist) : L"";
    if (key != lyrKey) {
        lyrKey = key;
        lyrDoc.reset();
        lyrAsked = -1;
        lyrKeyAt = now;
    }
    if (key.empty()) return;
    if ((lyrAsked < 0 && (tr.duration > 0 || now - lyrKeyAt > 1.5)) || (!lyrDoc && lyrAsked >= 0 && now - lyrAsked > 16)) {
        model.lyrics.Want(tr.title, tr.artist, tr.album, tr.duration / 1e7);
        lyrAsked = now;
    }
    if ((!lyrDoc || lyrDoc->interim) && lyrAsked >= 0 && now - lyrPoll > 0.25) {
        lyrPoll = now;
        lyrDoc = model.lyrics.Get(key);
    }
}

void Board::Backdrop() { // the look behind the tiles, drawn straight: no screen-sized bitmap kept for it
    Canvas& c = *canvas;
    D2D1_RECT_F full = {0, 0, W, H};
    Color base = t.bg;
    base.a = 1;
    c.Fill(full, 0, base);
    if (t.stars) {
        starfall::Sky(c, full, 0, t, 0, 1);
    } else { // two pools of the accent's light, far apart and faint: the panel's glow, spread over a room
        c.Glow({W * 0.12f, H * 0.05f}, W * 0.55f, H * 0.6f, t.accent.Alpha(0.10f));
        c.Glow({W * 0.92f, H * 1.0f}, W * 0.5f, H * 0.55f, t.accent.Alpha(0.07f));
    }
}

D2D1_RECT_F Board::ButtonsBox() const {
    float x = W - kMargin - 18, y = kMargin + 18;
    return {x - 42 - 18, y - 18, x + 18, y + 18};
}

void Board::Buttons() { // arrange, and back to the desktop: only while the mouse is about
    D2D1_POINT_2F at = {W - kMargin - 18, kMargin + 18}, away = {at.x - 42, at.y};
    canvas->Dot(at, 17, t.card.Alpha(0.92f));
    canvas->Dot(away, 17, t.card.Alpha(0.92f));
    if (w::IconBtn(ui, t, Ui::Id("bd.edit"), at, 34, glyph::Edit, 17)) Edit();
    if (w::IconBtn(ui, t, Ui::Id("bd.away"), away, 34, glyph::Close, 18)) leave = true; // ctrl+alt+d does the same, and brings it back
}

bool Board::Paint(const RECT& px, double now, float dt) {
    ComPtr<IDXGISurface> dxgi;
    ComPtr<ID2D1Bitmap1> bmp;
    POINT off = {};
    HRESULT hr = surface->BeginDraw(&px, IID_PPV_ARGS(&dxgi), &off);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ctx.Reset(); // the drawer builds a new one next second
        return false;
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(ctx->CreateBitmapFromDxgiSurface(dxgi.Get(), &bp, &bmp))) {
        surface->EndDraw();
        return false;
    }
    ctx->SetTarget(bmp.Get());
    ctx->SetDpi(96 * scale, 96 * scale);
    ctx->BeginDraw();
    paintShift = {(off.x - px.left) / scale, (off.y - px.top) / scale};
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(paintShift.x, paintShift.y)); // the patch sits anywhere in windows' atlas
    D2D1_RECT_F patch = {px.left / scale, px.top / scale, px.right / scale, px.bottom / scale};
    ctx->PushAxisAlignedClip(patch, D2D1_ANTIALIAS_MODE_ALIASED); // nothing past it may be touched
    ui.Begin(*canvas, input, kStillDt, now); // the backdrop's own widgets: none, but the base transform is read here
    Backdrop();
    if (editing) {
        ui.Begin(*canvas, input, dt, now); // arranging keeps its easing
        EditFrame();
        busy = busy || ui.Busy() || grab >= 0;
    } else {
        for (int i = 0; i < (int)tiles.size(); ++i) {
            D2D1_RECT_F r = CellRect(tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h);
            if (!Touch(r, patch)) continue;
            ui.Begin(*canvas, input, Plays(tiles[i].kind) ? dt : kStillDt, now);
            DrawTile(tiles[i], r, i);
            seen[i] = Look(tiles[i].kind);
            stale[i] = 0;
            moving[i] = ui.Busy() || ui.Ambient();
            hand = hand || ui.Hand();
        }
        if (buttons && Touch(ButtonsBox(), patch)) {
            ui.Begin(*canvas, input, kStillDt, now);
            Buttons();
            hand = hand || ui.Hand();
        }
    }
    ui.DrawFocus(t.accentHi, t.onAccent.Alpha(1));
    ctx->PopAxisAlignedClip();
    hr = ctx->EndDraw();
    ctx->SetTarget(nullptr);
    surface->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) ctx.Reset(); // the drawer builds a new one next second
    return SUCCEEDED(hr);
}

void Board::Frame(const Theme& shell, const Theme& media) {
    if (!ctx || covered) return;
    armed = false;
    double now = Seconds();
    float dt = lastFrame == 0 ? 1 / 60.0f : (float)(std::min)(now - lastFrame, 0.1);
    lastFrame = now;
    t = shell;
    mt = media;
    SyncCover();
    SyncLyrics(now);
    float pl = 0, pr = 0;
    bool hasSong = std::any_of(tiles.begin(), tiles.end(), [](const Model::BoardTile& tl) { return Plays(tl.kind); });
    bool playing = hasSong && !editing && model.hasTrack && model.track.playing;
    if (playing) model.Levels(pl, pr);
    level += ((pl + pr) / 2 - level) * (std::min)(1.0f, dt * 10);

    for (const auto& tl : tiles) { // the lyrics tile, laid out before the frame starts
        if (tl.kind != Model::kBdLyrics || editing) continue;
        D2D1_RECT_F r = CellRect(tl.x, tl.y, tl.w, tl.h);
        lyrWidth = Width(r) - 40;
        lyrSize = std::clamp(Height(r) * 0.075f, 15.0f, 44.0f) * model.lyricScale / 100.0f;
        if (lyrDoc && !lyrDoc->lines.empty()) lyrics.Sync(ctx.Get(), lyrDoc, lyrWidth, lyrSize);
        else lyrics.Sync(ctx.Get(), nullptr, 0, 0);
        lyrics.Prepare(ctx.Get());
    }

    stale.resize(tiles.size(), 1);
    moving.resize(tiles.size(), 0);
    seen.resize(tiles.size(), 0);
    bool startEdit = editing;
    busy = false;
    bool showBtns = now - movedAt < kButtonsSec, press = input.pressed || input.released || input.rclick || input.wheel != 0;
    D2D1_RECT_F box = ButtonsBox();
    auto under = [&](const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    bool onBox = showBtns && (under(box, input.mouse) || (input.down && under(box, input.pressPos)) || (input.released && under(box, input.pressPos)));
    bool hear = access.Listening(); // a screen reader is here: this frame says what it draws, all of it
    bool whole = repaint || editing || editing != wasEditing || showBtns != buttons || onBox || wasOnBox || keyMode || hear || ui.Keyed(); // those buttons float over tiles: repainted with them
    wasOnBox = onBox;
    wasEditing = editing;
    buttons = showBtns;
    repaint = false;
    canvas->NewFrame();
    canvas->Listen(hear);
    ui.Listen(hear);
    ui.Keyboard(keyMode);
    hand = false;
    bool painted = false, ok = true;
    if (whole) {
        ok = Paint(Pixels({0, 0, W, H}), now, dt);
        painted = true;
    } else {
        int hoverNow = -1;
        for (int i = 0; i < (int)tiles.size() && ok; ++i) {
            D2D1_RECT_F r = CellRect(tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h);
            bool here = under(r, input.mouse), held = (input.down || input.released) && under(r, input.pressPos);
            if (here) hoverNow = i;
            bool music = playing && tiles[i].kind == Model::kBdMedia ? model.vizOn : tiles[i].kind == Model::kBdLyrics && playing && lyrics.Busy();
            if (!stale[i] && !moving[i] && !music && !here && !held && i != hoverTile) continue;
            ok = Paint(Pixels(r), now, dt);
            painted = true;
            busy = busy || music || moving[i];
        }
        hoverTile = hoverNow;
    }
    if (ok && painted) ui.End();
    if (ctx && painted) dcomp->Commit();
    if (hear && ok && painted) access.Publish(ui, *canvas, scale, paintShift);
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    bool again = press || editing != startEdit; // what a click changed shows on the next frame
    if (whole && !editing) { // a whole paint sets what moves; it keeps moving on its own patches
        for (size_t i = 0; i < tiles.size(); ++i) {
            bool music = playing && (tiles[i].kind == Model::kBdMedia ? model.vizOn : tiles[i].kind == Model::kBdLyrics && lyrics.Busy());
            busy = busy || music || moving[i];
        }
    }
    dirty = again;
    if (again || input.down || grab >= 0 || (editing && busy)) Arm(kDragGap);
    else if (busy) Arm(1 / kAmbientFps);
    if (busy) rest = 0;
}
