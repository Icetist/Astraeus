#include "stage.h"

#include <d2d1_1helper.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr wchar_t kClass[] = L"AstraeusStage";
constexpr float kPi = 3.14159265f;
constexpr double kLyrWaitSec = 12; // how long the lyrics layout waits for a new song's lines before giving way
constexpr float kLeaveSec = 0.26f; // easing back toward the bezel as it fades
constexpr double kIdleSec = 2.5; // controls and cursor fade after the mouse rests this long
constexpr float kPairShade = 0.38f; // both halves of a two screen pair sit a shade darker than the single player: a dimmed room, the words and the cover the light in it
constexpr float kWordsAt = 0.42f; // the lyrics half: the sung line rests a little above the middle
constexpr float kWordsW = 0.64f; // and the words take this much of the screen's width
constexpr float kSwapSec = 0.45f; // the pair trading halves dips through dark this long
constexpr double kHintSec = 1.6; // a key's word stays this long, then fades

constexpr double kFastFps = 60, kAmbientFps = 30, kQuietFps = 20; // ui motion and glides, music-driven light only, paused drift
constexpr UINT kMaxSync = 4; // dxgi's longest present interval
constexpr double kGameCheckSec = 1; // how often the foreground is looked at for a fullscreen game
int liveStages = 0; // the pair counts as two: throttling lifts when the last goes

UINT SyncFor(double hz, double fps) { return (UINT)std::clamp<long>(std::lround(hz / fps), 1, kMaxSync); } // vsyncs per frame for a target rate

double RefreshOf(HMONITOR mon) { // the screen's refresh rate, cached per monitor
    static HMONITOR seen[4] = {};
    static double hz[4] = {};
    for (int i = 0; i < 4; ++i)
        if (seen[i] == mon) return hz[i];
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    double r = GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60.0;
    static int next = 0;
    seen[next] = mon, hz[next] = r;
    next = (next + 1) % 4;
    return r;
}

bool GameInFront() { // a fullscreen window of another app in front, on any screen: someone is playing
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId())) return false;
    LONG style = GetWindowLongW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (IsZoomed(fg) && (style & WS_THICKFRAME))) return false; // maximized, not fullscreen
    wchar_t cls[40] = {};
    GetClassNameW(fg, cls, 40);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
                                 L"ForegroundStaging", L"Windows.UI.Core.CoreWindow"})
        if (!wcsncmp(cls, shell, wcslen(shell))) return false; // desktop and shell surfaces cover screens too
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

bool gameMode = false;
void GameMode(bool on) { // a game in front: efficiency cores and a lower priority, so the player never competes with it
    if (on == gameMode) return;
    gameMode = on;
    PROCESS_POWER_THROTTLING_STATE pt = {PROCESS_POWER_THROTTLING_CURRENT_VERSION};
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    SetThreadPriority(GetCurrentThread(), on ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL); // back to the startup priority after
}

bool GameNow(double now) { // checked about once a second, shared by both halves of a pair
    static double at = -10;
    if (now - at >= kGameCheckSec) {
        at = now;
        GameMode(GameInFront());
    }
    return gameMode;
}

double Seconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / freq.QuadPart;
}

}

Stage::Stage(Model& m, HMONITOR mon, bool focus) : model(m), mode(m.Look()) {
    static bool registered = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.style = CS_DBLCLKS; // double-click leaves, the way it came in
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) != 0;
    }();
    ++liveStages; // paired with the destructor, even when building fails
    if (!registered) return;
    MONITORINFO mi = {sizeof(mi)};
    if (!mon) mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    screen = mon;
    GetMonitorInfoW(mon, &mi);
    if (mode == 2) shade = anim::Clamp01((sys::Brightness(mi.rcMonitor) - 0.42f) / 0.38f); // read the desktop before covering it
    UINT dx = 96, dy = 96;
    scale = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    const RECT& r = mi.rcMonitor;
    UINT pw = (UINT)(r.right - r.left), ph = (UINT)(r.bottom - r.top);
    W = pw / scale;
    H = ph / scale;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Astraeus", WS_POPUP, r.left, r.top, (int)pw, (int)ph,
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd || !MakeDev(pw, ph)) return;
    ShowWindow(hwnd, focus ? SW_SHOW : SW_SHOWNA); // the second of a pair shows without taking the keyboard
    if (focus) SetForegroundWindow(hwnd); // keys work right away: esc, space, arrows
    lastMove = openedAt = Seconds();
    model.meter.Refresh(); // levels from the current output, even if the panel's visualizer is off
}

Stage::~Stage() {
    canvas.reset();
    FreeGlass();
    visual.Reset(); // let go of everything on the device first, then let the driver drop it all
    target.Reset();
    dcomp.Reset();
    backBuf.Reset();
    swap.Reset();
    ctx.Reset();
    gfx::Settle(d3d.Get());
    if (frameWait) CloseHandle(frameWait);
    if (leaveTick) CloseHandle(leaveTick);
    if (hwnd) DestroyWindow(hwnd);
    if (--liveStages <= 0) GameMode(false); // the last of the pair: full speed again
}

bool Stage::MakeDev(UINT pxW, UINT pxH) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
        return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || (device->SetMaximumTextureMemory(32ull << 20), false) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = pxW;
    d.Height = pxH;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // glass lets the blurred desktop through, dark and light paint every pixel
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc)) || FAILED(sc.As(&swap))) return false;
    swap->SetMaximumFrameLatency(1);
    frameWait = swap->GetFrameLatencyWaitableObject();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) || FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &backBuf))) return false;
    if (mode == 2 && !OpenGlass(pxW, pxH)) mode = 0; // no composition: a dark room instead
    if (mode != 2 && (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) ||
                      FAILED(dcomp->CreateVisual(&visual)) || FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) ||
                      FAILED(dcomp->Commit())))
        return false;
    dc->SetTarget(backBuf.Get());
    dc->SetDpi(96 * scale, 96 * scale);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ctx = dc;
    canvas = std::make_unique<Canvas>(ctx.Get());
    return true;
}

LRESULT CALLBACK Stage::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = reinterpret_cast<Stage*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && !self->hwnd) self->hwnd = h; // messages sent during CreateWindowEx, before it returns
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT Stage::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (pt.x != lastPt.x || pt.y != lastPt.y) lastMove = Seconds(); // windows resends moves on its own, only real ones wake the controls
        lastPt = pt;
        input.mouse = {pt.x / scale, pt.y / scale};
        return 0;
    }
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; // scrolls the lyrics
        lastMove = Seconds();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        lastMove = Seconds();
        input.pressPos = input.mouse;
        input.pressed = input.down = true;
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        input.released = true;
        input.down = false;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (!ui.Hovered()) Leave(); // on empty space; double-clicking play shouldn't also leave
        else {
            input.pressPos = input.mouse;
            input.pressed = input.down = true;
        }
        return 0;
    case WM_RBUTTONUP:
        Leave(); // right click leaves, like it closes sheets in the panel
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(Seconds() - lastMove < kIdleSec ? LoadCursorW(nullptr, ui.Hand() ? IDC_HAND : IDC_ARROW) : nullptr); // cursor rests out of sight
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN: // alt held: only the two screen keys can use it
        Keys((UINT)wp);
        return 0;
    case WM_KEYDOWN:
        Keys((UINT)wp);
        if (wp == VK_ESCAPE || wp == 'F' || wp == VK_F11) Leave();
        else if (wp == VK_SPACE) model.media.TogglePlay();
        else if (wp == VK_RIGHT) model.media.Next();
        else if (wp == VK_LEFT) model.media.Previous();
        else if (wp == VK_OEM_4 || wp == VK_OEM_6) { // [ and ]: lyrics a touch earlier or later, remembered
            model.lyricOffsetMs = (std::clamp)(model.lyricOffsetMs + (wp == VK_OEM_6 ? 100 : -100), -3000, 3000);
            if (!lyrKey.empty()) model.lyrics.SetOffset(lyrKey, model.lyricOffsetMs); // this song's own, kept for next time
            nudgeAt = Seconds();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) lastMove = -10; // alt-tabbed away: hide the controls
        return 0;
    case WM_CLOSE:
        Leave();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Stage::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id == coverId) return;
    coverId = id;
    artSeeded = false;
    cover.Reset();
    coverBlur.Reset();
    const auto& bytes = model.track.cover;
    if (!id || !bytes || bytes->empty()) return;
    ComPtr<IWICBitmapSource> src = gfx::Decode(bytes->data(), bytes->size(), 1024); // sharper than the panel's copy, the art is big here
    if (src && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &cover))) coverBlur = gfx::BakeBlur(ctx.Get(), cover.Get(), 40.0f);
    artSeeded = src && model.cfg.albumCol; // same rule as the media page: album colors unless turned off
    if (artSeeded) artSeed = theme::FromBmp(src.Get());
}

void Stage::SetRole(Role r) {
    if (r == role) return;
    if (lookSet) roleAt = Seconds(); // the first frame just starts as it is
    role = r;
}

void Stage::Keys(UINT vk) { // the two screen keys, as picked in settings; the player's own keys never reach here as a match
    StagePrefs& p = StagePrefs::Get();
    UINT combo = StagePrefs::Combo(vk);
    if (combo != p.keySplit && combo != p.keySwap) return;
    hintAt = Seconds();
    if (GetSystemMetrics(SM_CMONITORS) < 2) { // one screen: nothing to split
        hint = L"Needs a second screen";
        return;
    }
    if (combo == p.keySplit) {
        p.Flip(p.split, L"StageSplit");
        hint = p.split ? L"Two screens on" : L"Two screens off";
    } else if (p.split) {
        p.Flip(p.swap, L"StageSwap");
        hint = L"Screens swapped";
    } else {
        hint = L"Turn on two screens first";
    }
}

void Stage::SyncTheme(const Theme& media, float dt) {
    Seed s = artSeeded ? artSeed : theme::FromCol(media.accent); // no art: whatever the panel's media palette is
    s.look = mode;
    s.bg = model.CustomBg();
    Theme want = Theme::From(s);
    if (!lookSet) { // first frame: start on the right colors
        look = lookFrom = lookTo = want;
        lookSet = true;
        return;
    }
    const Color& a = want.accent, & b = lookTo.accent;
    if (std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) > 0.01f) { // a new song's colors: crossfade from where we are
        lookFrom = look;
        lookTo = want;
        lookT = 0;
    }
    lookT = (std::min)(1.0f, lookT + dt / 0.8f);
    look = lookFrom.Lerp(lookTo, anim::EaseOut(lookT));
}

void Stage::Backdrop(double now) { // the media page's glow, scaled to the room: four pools of light that chase the music
    Canvas& c = *canvas;
    D2D1_RECT_F full = {0, 0, W, H};
    bool light = LightRoom(), glassy = mode == 2;
    if (glassy) c.Fill(full, 0, look.bg); // a tint over the blurred desktop
    if (glassy && shade > 0.01f) c.Fill(full, 0, Color{0, 0, 0, 0.42f * shade}); // a bright desktop gets neutral smoked glass so white type reads
    else c.Fill(full, 0, light || mode == 3 ? look.bg.Alpha(1) : Color{0.02f, 0.02f, 0.03f, 1}); // custom: the room in the user's color
    float zoom = 1.10f + 0.03f * (float)std::sin(now * 0.07) + 0.03f * beat; // slow breathing, nudged by the beat
    if (coverBlur) {
        float gw = W * (zoom - 1) / 2, gh = H * (zoom - 1) / 2;
        c.Image(coverBlur.Get(), {-gw, -gh, W + gw, H + gh}, 0, glassy ? 0.30f : light ? 0.55f : 0.7f); // glass keeps the desktop showing
    }
    c.Fill(full, 0, light ? Color{1, 1, 1, 0.38f} : Color{0, 0, 0, glassy ? 0.12f : 0.30f}); // wash so the type reads
    StepSwell(swell, model, frameDt, model.hasTrack && model.track.playing, kBlobs);
    if (beat > 0.01f) c.Glow({W / 2, H}, W * (0.45f + 0.15f * beat), H * 0.35f * beat, look.accent.Alpha(0.18f * beat)); // the kick blooms from the floor
    DrawSwell(c, swell, kBlobs, {0, H * 0.25f, W, H}, look, now); // soft light swelling up from the floor
    Color floor = light ? Color{1, 1, 1, 1} : Color{0, 0, 0, 1};
    c.FillV(full, 0, floor.Alpha(light ? 0.05f : 0.10f), floor.Alpha(light ? 0.35f : glassy ? 0.30f : 0.45f)); // settles the bottom so the room doesn't feel lopsided
}

void Stage::Clock(float y, float k) { // just the time, faint, top center
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t time[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, 32);
    canvas->Text(time, Rect(0, y, W, 34 * k), Weight::Light, 22 * k, look.text.Alpha(0.45f), Align::Center);
}

void Stage::Seek(const D2D1_RECT_F& r, float show) { // the media page's wave, scaled up for the room
    const sys::Track& tr = model.track;
    if (!model.hasTrack) return;
    bool known = tr.duration > 0; // unknown length: the whole line ripples, no playhead or times
    uint32_t id = Ui::Id("st.seek");
    float v = !known ? 1.0f : preview >= 0 ? preview : (float)tr.Progress();
    if (known && tr.canSeek && show > 0.5f && ui.Slider(id, r, v, false)) preview = v;
    if (ui.IsActive(id) && ui.In().released) {
        model.media.Seek(v);
        preview = -1;
    }
    if (!ui.IsActive(id)) preview = -1;
    Canvas& c = *canvas;
    float k = std::clamp(H / 900.0f, 0.8f, 1.6f);
    float cy = (r.top + r.bottom) / 2, x = r.left + Width(r) * anim::Clamp01(v), gap = 9 * k;
    float amp = ui.Smooth(id + 1, tr.playing && !ui.IsActive(id) ? 2.8f * k : 0, 5); // flattens when paused or dragging
    WaveLine(c, r.left + 2, known ? x - gap : r.right - 2, cy, amp, ui.Now(), look.accent, 4.5f * k, 34 * k);
    if (!known) return;
    if (x + gap < r.right - 2) c.Line({x + gap, cy}, {r.right - 2, cy}, look.accent.Alpha(0.3f), 4.5f * k); // unplayed part stays flat, a faint wash of the album colour
    float grow = ui.Smooth(id + 2, ui.IsActive(id) || (show > 0.5f && Inside(r, ui.In().mouse)) ? 1.0f : 0, 16);
    float hh = (12 + 3 * grow) * k;
    c.Fill({x - 2.5f, cy - hh, x + 2.5f, cy + hh}, 2.5f, look.accentHi); // vertical bar playhead
    if (show < 0.01f) return;
    double total = tr.duration / 1e7, elapsed = preview >= 0 ? total * preview : tr.Elapsed() / 1e7;
    float ty = cy + 14 * k;
    c.Text(fmt::Clock(elapsed), Rect(r.left, ty, 100, 20), Weight::Regular, 13 * k, look.textDim.Alpha(show));
    c.Text(fmt::Clock(total), Rect(r.right - 100, ty, 100, 20), Weight::Regular, 13 * k, look.textDim.Alpha(show), Align::Right);
}

float Stage::LyricSize() const {
    float pick = model.lyricScale / 100.0f; // the settings' lyric size; a new size re-lays the lines, the blur bakes again
    if (role == kLyrics) return std::clamp((std::min)(H * 0.084f, W * 0.052f), 30.0f, 160.0f) * pick; // a screen of its own: big enough to read across the room
    return std::clamp(H * 0.06f, 26.0f, 78.0f) * pick; // bold: the sung line is the room's headline
}

D2D1_RECT_F Stage::LyricRect() const {
    if (role == kLyrics) { // the whole screen to the words: a wide column, top to bottom, fading at both ends
        float cw = W * kWordsW;
        return {(W - cw) / 2, 0, (W + cw) / 2, H};
    }
    float m = W * 0.065f, lw = W * 0.34f, lx = m + lw + W * 0.05f;
    return {lx, H * 0.07f, W - W * 0.05f, H * 0.93f};
}

void Stage::SyncLyrics(double now) { // asks for the song's lyrics once its length is known, picks them up when they land
    const sys::Track& tr = model.track;
    std::wstring key = model.fullLyrics && role != kCover && model.hasTrack && tr.music && !tr.title.empty() ? sys::LyricSrc::Key(tr.title, tr.artist) : L""; // off in settings: the classic layout
    if (key != lyrKey) {
        lyrKey = key;
        lyrDoc.reset();
        model.lyricOffsetMs = 0; // a new song starts in sync until its own nudge arrives
        lyrAsked = -1;
        lyrKeyAt = now;
    }
    if (key.empty()) return;
    if ((lyrAsked < 0 && (tr.duration > 0 || now - lyrKeyAt > 1.5)) || (!lyrDoc && lyrAsked >= 0 && now - lyrAsked > 16)) { // the length picks the right cut; a failed lookup is asked again
        model.lyrics.Want(tr.title, tr.artist, tr.album, tr.duration / 1e7);
        lyrAsked = now;
    }
    if ((!lyrDoc || lyrDoc->interim) && lyrAsked >= 0 && now - lyrPoll > 0.25) { // an early version keeps listening for the word-timed one
        lyrPoll = now;
        auto got = model.lyrics.Get(key);
        if (got && got != lyrDoc) model.lyricOffsetMs = got->offsetMs; // the song's remembered nudge
        lyrDoc = got;
    }
}

void Stage::Player(float show, float lyr) { // the cover big on the left and the song on its right; with lyrics, the song moves under the cover
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    bool has = model.hasTrack;
    float k = std::clamp(H / 900.0f, 0.8f, 1.6f); // type and spacing grow with the screen
    float e = lyr * lyr * (3 - 2 * lyr);
    auto mix = [e](float a, float b) { return a + (b - a) * e; };
    float z = 1 - show, zz = z * z * (3 - 2 * z); // the mouse resting: the controls go and the cover grows into their room

    // the classic layout
    float side = (std::min)(H * 0.54f, W * 0.36f) * (1 + 0.14f * zz), gap = side * 0.16f, colW = (std::min)(W * 0.40f, 760.0f * k);
    float x0 = (W - side - gap - colW) / 2, cy = H * 0.5f;
    float tx = x0 + side + gap, lineH = 66 * k, colH = 22 * k + lineH + 66 * k + 76 * k + 42 * k; // header, title, artist to the wave, wave to the controls, the play button
    float ty = cy - colH / 2, waveY = ty + 22 * k + lineH + 66 * k;
    // the lyrics layout: a column on the left, cover over title over the wave over the controls
    bool alone = role == kCover; // the song's half of a pair: the stacked layout, centred on its own screen
    float lw = alone ? (std::min)(W * 0.5f, H * 0.95f) : W * 0.34f, lcx = alone ? W / 2 : W * 0.065f + lw / 2;
    float side2 = (std::min)(H * 0.44f + 92 * k * zz, lw * (0.80f + 0.12f * zz));
    float blockH = side2 + 20 * k + 44 * k + 28 * k + 40 * k + (66 * k + 34 * k) * (1 - zz);
    float ly = (std::max)(H * 0.09f, H * 0.53f - blockH / 2), lty = ly + side2 + 20 * k, lWaveY = lty + 44 * k + 28 * k + 40 * k;

    float sd = mix(side, side2), pulse = 1 + 0.018f * beat; // the cover thumps with the kick
    D2D1_POINT_2F mid = {mix(x0 + side / 2, lcx), mix(cy, ly + side2 / 2)};
    D2D1_RECT_F art = Rect(mid.x - sd * pulse / 2, mid.y - sd * pulse / 2, sd * pulse, sd * pulse);
    float rad = sd * 0.06f;
    c.Glow({mid.x, mid.y + sd * 0.08f}, sd * 1.05f, sd * 0.95f, look.accent.Alpha(0.30f + 0.30f * beat)); // light spilling from the cover
    c.Glow({mid.x, art.bottom}, sd * 0.70f, sd * 0.20f, Color{0, 0, 0, LightRoom() ? 0.18f : 0.45f}); // and a soft shadow under it
    Ui::Hit artHit = ui.Button(Ui::Id("st.art"), art);
    if (artHit.clicked) Leave(); // the cover brought you here, the cover takes you back
    if (cover) c.Art(cover.Get(), coverBlur.Get(), art, rad, 1);
    else c.FillV(art, rad, look.accentHi.Alpha(0.85f), look.accent.Alpha(0.55f)); // no art: a plain glowing tile
    c.Stroke(art, rad, Color{1, 1, 1, 0.10f}, 1);
    if (float tip = ui.Smooth(Ui::Id("st.art", 1), artHit.hover ? 1.0f : 0, 14); tip > 0.01f) { // hovering the cover: say what a click does, like "Fullscreen?" in the panel
        float pw = 150 * k, ph = 32 * k;
        D2D1_RECT_F pill = Rect(mid.x - pw / 2, art.bottom - ph - 14 * k + 4 * (1 - tip), pw, ph);
        Color fill = look.bg;
        fill.a = 0.90f;
        c.Fill(pill, ph / 2, fill.Alpha(tip));
        c.StrokeV(pill, ph / 2, look.hairTop.Alpha(tip), look.hairBot.Alpha(tip), 1);
        c.Text(L"Close fullscreen?", pill, Weight::Medium, 13 * k, look.text.Alpha(tip), Align::Center);
    }

    std::wstring title = has && !tr.title.empty() ? tr.title : L"Nothing playing";
    std::wstring sub = !has ? L"Start music or a video in any app" : tr.album.empty() || tr.album == tr.artist ? tr.artist : tr.artist + L"  \u00B7  " + tr.album;
    if (e < 0.999f) { // the classic column slides out to the left as the lyrics come in
        float a = (1 - e) * (1 - e), dx = -40 * k * e, y = ty;
        c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(tx + dx, y, colW, 20 * k), Weight::Medium, 13 * k, look.accentHi.Alpha(a), 3.0f);
        y += 22 * k;
        c.Text(title, Rect(tx + dx, y, colW, lineH), Weight::Medium, 52 * k, look.text.Alpha(a));
        y += lineH;
        c.Text(sub, Rect(tx + dx, y, colW, 34 * k), Weight::Light, 24 * k, look.textDim.Alpha(a));
    }
    if (e > 0.001f) { // under the cover, centered: just the song and who sings it
        float dy = 24 * k * (1 - e);
        c.Text(title, Rect(lcx - lw / 2, lty + dy, lw, 44 * k), Weight::Medium, 34 * k, look.text.Alpha(e), Align::Center);
        c.Text(tr.artist, Rect(lcx - lw / 2, lty + 44 * k + dy, lw, 28 * k), Weight::Light, 19 * k, look.textDim.Alpha(e), Align::Center);
    }

    float ww = mix(colW, lw * 0.84f), wx = mix(tx, lcx - lw * 0.42f), wy = mix(waveY, lWaveY);
    Seek(Rect(wx, wy - 16 * k, ww, 32 * k), show);

    float alpha = show; // the controls leave with the cursor and come back with the mouse
    float ck = k * mix(1.0f, 0.8f), ccx = mix(tx + colW / 2, lcx), y = mix(waveY + 76 * k, lWaveY + 66 * k);
    D2D1_RECT_F row = {ccx - 270 * ck, y - 50 * ck, ccx + 270 * ck, y + 50 * ck};
    const Theme ctl = theme::Lift(look); // the play button leans toward the ink, so it stands clear of the light behind it
    if (alpha > 0.01f) { // hidden: not drawn at all
        bool layer = alpha < 0.999f;
        if (layer) ctx->PushLayer(D2D1::LayerParameters1(row, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), nullptr);
        if (w::IconBtn(ui, ctl, Ui::Id("st.prev"), {ccx - 100 * ck, y}, 60 * ck, glyph::Prev, 38 * ck, has && tr.canPrev)) model.media.Previous();
        if (w::PlayBtn(ui, ctl, Ui::Id("st.play"), {ccx, y}, 84 * ck, has && tr.playing, has)) model.media.TogglePlay();
        if (w::IconBtn(ui, ctl, Ui::Id("st.next"), {ccx + 100 * ck, y}, 60 * ck, glyph::Next, 38 * ck, has && tr.canNext)) model.media.Next();
        D2D1_POINT_2F shuf = {ccx - 196 * ck, y}, rep = {ccx + 196 * ck, y};
        if (w::IconBtn(ui, ctl, Ui::Id("st.shuffle"), shuf, 48 * ck, glyph::Shuffle, 26 * ck, has && tr.canShuf, has && tr.shuffle)) model.media.ToggleShuffle();
        if (w::IconBtn(ui, ctl, Ui::Id("st.repeat"), rep, 48 * ck, has && tr.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 26 * ck, has && tr.canRep,
                       has && tr.repeat != 0))
            model.media.CycleRepeat();
        float sOn = ui.Smooth(Ui::Id("st.shuffle", 7), has && tr.shuffle ? 1.0f : 0, 14), rOn = ui.Smooth(Ui::Id("st.repeat", 7), has && tr.repeat != 0 ? 1.0f : 0, 14);
        if (sOn > 0.01f) c.Dot({shuf.x, shuf.y + 22 * ck}, 2.8f * ck * sOn, ctl.accent.Alpha(sOn)); // on: a dot under it, not just a color change
        if (rOn > 0.01f) c.Dot({rep.x, rep.y + 22 * ck}, 2.8f * ck * rOn, ctl.accent.Alpha(rOn));
        if (layer) ctx->PopLayer();
    }

    if (e > 0.001f && !alone) { // the lyrics drift in from the right
        D2D1_RECT_F lr = LyricRect();
        float dx = 60 * k * (1 - e);
        lr.left += dx;
        lr.right += dx;
        double at = tr.Elapsed() / 1e7 - model.lyricOffsetMs / 1000.0; // [ and ] fine-tune it
        if (!lyrics.Has()) lyrics.Waiting(c, lr, e, look, ui.Now());
        double jump = lyrics.Draw(ctx.Get(), c, ui, lr, at, e, look, has && tr.playing);
        if (jump >= 0 && tr.canSeek && tr.duration > 0) model.media.Seek(jump * 1e7 / tr.duration);
    }
}

void Stage::Words() { // the lyrics half of a pair: nothing but the words, the whole screen theirs
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    bool has = model.hasTrack;
    float e = lyrMix * lyrMix * (3 - 2 * lyrMix);
    D2D1_RECT_F lr = LyricRect();
    if (e > 0.001f) {
        double at = tr.Elapsed() / 1e7 - model.lyricOffsetMs / 1000.0; // [ and ] fine-tune it
        if (!lyrics.Has()) lyrics.Waiting(c, lr, e, look, ui.Now());
        double jump = lyrics.Draw(ctx.Get(), c, ui, lr, at, e, look, has && tr.playing);
        if (jump >= 0 && tr.canSeek && tr.duration > 0) model.media.Seek(jump * 1e7 / tr.duration);
    }
    if (e > 0.999f) return;
    float a = (1 - e) * (1 - e), k = K(); // no words for this song: one quiet line where they would be
    const wchar_t* why = !has ? L"Nothing playing" : !model.fullLyrics ? L"Lyrics are off" : tr.music ? L"No synced lyrics for this song" : L"Lyrics show for songs from music apps";
    c.Text(why, Rect(lr.left, H * kWordsAt - 24 * k, Width(lr), 48 * k), Weight::Medium, 30 * k, look.textDim.Alpha(0.8f * a));
}

void Stage::Leave() { // the compositor shrinks the last frame into the bezel; nothing is drawn while it plays
    if (leaving) return;
    leaving = true;
    leaveAt = Seconds();
    float pw = W * scale, ph = H * scale;
    float cx = model.Edge() == 1 ? 0 : model.Edge() == 2 ? pw : pw / 2, cy = model.Edge() == 0 ? 0 : ph / 2; // where the panel lives
    leaveTick = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); // auto-reset: one wake per tick
    LARGE_INTEGER first;
    first.QuadPart = -160000; // 16 ms
    if (!leaveTick || !SetWaitableTimer(leaveTick, &first, 16, nullptr, nullptr, FALSE)) {
        done = true;
        return;
    }
    if (glass) {
        if (!GlassLeave(kLeaveSec, cx, cy)) done = true;
        return;
    }
    ComPtr<IDCompositionAnimation> grow, fade;
    ComPtr<IDCompositionScaleTransform> shrink;
    ComPtr<IDCompositionEffectGroup> fx;
    if (!dcomp || !visual || FAILED(dcomp->CreateAnimation(&grow)) || FAILED(dcomp->CreateAnimation(&fade)) || FAILED(dcomp->CreateScaleTransform(&shrink)) ||
        FAILED(dcomp->CreateEffectGroup(&fx))) {
        done = true; // no compositor to animate with: just go
        return;
    }
    const float T = kLeaveSec, end = 0.93f; // a few percent back toward the bezel, quick then settling, while it fades smoothly away
    grow->AddCubic(0, 1, -2 * (1 - end) / T, (1 - end) / (T * T), 0); // 1 - (1 - end)(2u - u^2), u = t / T
    grow->End(T, end);
    fade->AddCubic(0, 1, 0, -3 / (T * T), 2 / (T * T * T)); // smoothstep from 1 to 0
    fade->End(T, 0);
    shrink->SetCenterX(cx);
    shrink->SetCenterY(cy);
    shrink->SetScaleX(grow.Get());
    shrink->SetScaleY(grow.Get());
    fx->SetOpacity(fade.Get());
    visual->SetTransform(shrink.Get());
    visual->SetEffect(fx.Get());
    dcomp->Commit();
}

void Stage::Frame(const Theme& media) {
    if (!ctx) {
        done = true;
        return;
    }
    double now = Seconds();
    if (leaving) { // the compositor plays the exit on its own; the last frame stays as it was
        if (now - leaveAt >= kLeaveSec) done = true;
        return;
    }
    float dt = lastFrame == 0 ? 1 / 60.0f : (float)(std::min)(now - lastFrame, 0.05);
    lastFrame = now;
    SyncCover(); // first: a new cover brings its colors
    SyncTheme(media, dt);

    float pl = 0, pr = 0;
    bool playing = model.hasTrack && model.track.playing;
    if (playing) model.meter.Read(pl, pr); // paused: levels just fall away
    auto follow = [dt](float& v, float to) { v += (to - v) * (std::min)(1.0f, dt * (to > v ? 34.0f : 7.0f)); }; // quicker than the panel's
    follow(left, std::sqrt(pl)); // sqrt lifts quiet passages
    follow(right, std::sqrt(pr));
    float level = (left + right) / 2;
    slow += (level - slow) * (std::min)(1.0f, dt * 2.5f);
    if (playing && level > slow * 1.20f + 0.035f && now - lastBeat > 0.20) {
        beat = 1;
        lastBeat = now;
    }
    beat *= std::exp(-dt * 6);
    frameDt = dt;

    bool paused = !model.hasTrack || !model.track.playing;
    float show = ui.Smooth(Ui::Id("st.show"), !model.stageHide || now - lastMove < kIdleSec || paused || ui.Captured() ? 1.0f : 0, 6); // the hide-at-rest switch off: they stay

    SyncLyrics(now);
    bool lyrOn = lyrDoc && !lyrDoc->lines.empty();
    bool waiting = !lyrDoc && !lyrKey.empty() && now - lyrKeyAt < kLyrWaitSec && (lyrMix > 0.5f || role == kLyrics); // next song's lyrics on their way: the layout holds still
    if (role != styled) { // the lyrics half sets its words its own way; the full player's are as they always were
        styled = role;
        LyricView::Look ls;
        if (role == kLyrics) ls = {kWordsAt, 0.2f, 0.82f, 120, 8, 0.95f, 0.36f, 0.42f, 1.0f, 0.05f}; // the lyrics half: the sung line big, the rest smaller and dimmer, a slow soft glide
        lyrics.Style(ls);
    }
    if (lyrOn) lyrics.Sync(ctx.Get(), lyrDoc, Width(LyricRect()), LyricSize()); // lays out a new song's lines before the frame starts
    else if (waiting) lyrics.Sync(ctx.Get(), nullptr, Width(LyricRect()), LyricSize()); // the old song's lines go, dots wait in their place
    else if (lyrMix < 0.001f) lyrics.Sync(ctx.Get(), nullptr, 0, 0);
    lyrics.Prepare(ctx.Get()); // bakes a few lines' blur, never inside the frame
    bool lyrMoving = anim::Approach(lyrMix, lyrOn || waiting ? 1.0f : 0, 3.2f, dt, 0.001f);
    bool idle = now - lastMove >= kIdleSec && !ui.Captured();
    if (idle && !cursorGone) { // resting: the cursor goes too, not only when it next moves
        POINT p;
        if (GetCursorPos(&p) && WindowFromPoint(p) == hwnd) SetCursor(nullptr);
        cursorGone = true;
    } else if (!idle) {
        cursorGone = false;
    }

    canvas->NewFrame();
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ui.Begin(*canvas, input, dt, now);
    Backdrop(now);
    if (role != kWhole) canvas->Fill({0, 0, W, H}, 0, Color{0, 0, 0, kPairShade}); // the same shade on both, so the two screens read as one room
    if (role != kLyrics) Clock(H * 0.035f, std::clamp(H / 900.0f, 0.8f, 1.6f)); // the words' half: only the words
    if (role == kLyrics) Words();
    else Player(show, role == kCover ? 1.0f : lyrMix); // the song's half of a pair: the stacked layout, on its own
    if (float nudge = anim::Clamp01((float)(1.6 - (now - nudgeAt)) / 0.3f); nudge > 0.001f && (lyrMix > 0.5f || role == kLyrics) && role != kCover) { // readout while nudging the lyrics
        float k = std::clamp(H / 900.0f, 0.8f, 1.6f), pw = 210 * k, ph = 34 * k;
        D2D1_RECT_F lr = LyricRect(), pill = Rect(lr.left, H * 0.04f, pw, ph);
        wchar_t txt[48];
        swprintf_s(txt, L"Lyrics %+.1f s", model.lyricOffsetMs / 1000.0);
        canvas->Fill(pill, ph / 2, look.bg.Alpha(0.85f * nudge));
        canvas->StrokeV(pill, ph / 2, look.hairTop.Alpha(nudge), look.hairBot.Alpha(nudge), 1);
        canvas->Text(model.lyricOffsetMs == 0 ? std::wstring(L"Lyrics in sync") : std::wstring(txt), pill, Weight::Medium, 13 * k, look.text.Alpha(nudge), Align::Center);
        ui.Wake();
    }
    float since = (float)(now - openedAt), tip = anim::Clamp01(since / 0.3f) * anim::Clamp01((2.6f - since) / 0.4f); // in, hold ~2 s, out
    if (float said = anim::Clamp01((float)(kHintSec - (now - hintAt)) / 0.3f) * anim::Clamp01((float)(now - hintAt) / 0.15f); said > 0.001f && hint) { // what a two screen key just did
        float k = K(), pw = (std::max)(220 * k, canvas->Measure(hint, Weight::Medium, 13 * k) + 40 * k), ph = 34 * k;
        D2D1_RECT_F pill = Rect((W - pw) / 2, H * 0.085f + 6 * (1 - said), pw, ph);
        Color fill = look.bg;
        fill.a = 0.85f;
        canvas->Fill(pill, ph / 2, fill.Alpha(said));
        canvas->StrokeV(pill, ph / 2, look.hairTop.Alpha(said), look.hairBot.Alpha(said), 1);
        canvas->Text(hint, pill, Weight::Medium, 13 * k, look.text.Alpha(said), Align::Center);
        ui.Wake();
        tip = 0; // the key's word takes the place of the opening tip
    }
    if (tip > 0.001f && role != kLyrics) { // the words' half has no cover to click
        float k = std::clamp(H / 900.0f, 0.8f, 1.6f), pw = 370 * k, ph = 38 * k;
        D2D1_RECT_F pill = Rect((W - pw) / 2, H * 0.085f + 8 * (1 - tip), pw, ph);
        Color fill = look.bg;
        fill.a = 0.85f;
        canvas->Fill(pill, ph / 2, fill.Alpha(tip));
        canvas->StrokeV(pill, ph / 2, look.hairTop.Alpha(tip), look.hairBot.Alpha(tip), 1);
        canvas->Text(L"Click the album cover or press Esc to leave", pill, Weight::Medium, 13 * k, look.text.Alpha(tip), Align::Center);
    }
    float swapDim = anim::Clamp01(1 - (float)(now - roleAt) / kSwapSec); // the halves traded: a breath of dark over the change
    if (swapDim > 0.001f) canvas->Fill({0, 0, W, H}, 0, Color{0, 0, 0, swapDim * swapDim});
    ui.End();
    HRESULT hr = ctx->EndDraw();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    bool fast = ui.Busy() || ui.Captured() || (show > 0.01f && show < 0.99f) || tip > 0.001f || lyrMoving || lyrics.Busy() || swapDim > 0.001f;
    bool ambient = left + right > 0.03f || beat > 0.02f; // only the music-driven light moves
    double hz = RefreshOf(screen);
    bool game = GameNow(now); // a game in front elsewhere: nothing needs more than the ambient rate
    UINT sync = SyncFor(hz, fast && !game ? kFastFps : fast || ambient ? kAmbientFps : kQuietFps); // capped at 60 on fast screens; the slow drift needs far less
    if (FAILED(hr) || FAILED(swap->Present(sync, 0))) done = true; // device lost: just leave, the panel is one hover away
}
