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
constexpr double kIdleSec = 2.5; // controls and cursor fade after the mouse rests this long

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

Stage::Stage(Model& m, HMONITOR mon) : model(m), mode(m.look) {
    static bool registered = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.style = CS_DBLCLKS; // double-click leaves, the way it came in
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return;
    MONITORINFO mi = {sizeof(mi)};
    if (!mon) mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    scale = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    const RECT& r = mi.rcMonitor;
    UINT pw = (UINT)(r.right - r.left), ph = (UINT)(r.bottom - r.top);
    W = pw / scale;
    H = ph / scale;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Astraeus", WS_POPUP, r.left, r.top, (int)pw, (int)ph,
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd || !MakeDev(pw, ph)) return;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd); // keys work right away: esc, space, arrows
    lastMove = openedAt = Seconds();
    model.meter.Refresh(); // levels from the current output, even if the panel's visualizer is off
}

Stage::~Stage() {
    canvas.reset();
    FreeGlass();
    if (frameWait) CloseHandle(frameWait);
    if (hwnd) DestroyWindow(hwnd);
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
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
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
        if (!ui.Hovered()) done = true; // on empty space; double-clicking play shouldn't also leave
        else {
            input.pressPos = input.mouse;
            input.pressed = input.down = true;
        }
        return 0;
    case WM_RBUTTONUP:
        done = true; // right click leaves, like it closes sheets in the panel
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(Seconds() - lastMove < kIdleSec ? LoadCursorW(nullptr, ui.Hand() ? IDC_HAND : IDC_ARROW) : nullptr); // cursor rests out of sight
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == 'F' || wp == VK_F11) done = true;
        else if (wp == VK_SPACE) model.media.TogglePlay();
        else if (wp == VK_RIGHT) model.media.Next();
        else if (wp == VK_LEFT) model.media.Previous();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) lastMove = -10; // alt-tabbed away: hide the controls
        return 0;
    case WM_CLOSE:
        done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Stage::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id == coverId) return;
    coverId = id;
    cover.Reset();
    coverBlur.Reset();
    const auto& bytes = model.track.cover;
    if (!id || !bytes || bytes->empty()) return;
    ComPtr<IWICBitmapSource> src = gfx::Decode(bytes->data(), bytes->size(), 1024); // sharper than the panel's copy, the art is big here
    if (src && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &cover))) coverBlur = gfx::BakeBlur(ctx.Get(), cover.Get(), 40.0f);
}

void Stage::SyncTheme(const Theme& media) {
    const Color& a = media.accent;
    if (a.r == lookKey.r && a.g == lookKey.g && a.b == lookKey.b) return;
    lookKey = a;
    Seed s = theme::FromCol(a);
    look = Theme::From(Seed{s.hue, s.chroma, mode}); // the panel's appearance, in the media's hue
}

void Stage::Backdrop(double now) { // the media page's glow, scaled to the room: four pools of light that chase the music
    Canvas& c = *canvas;
    D2D1_RECT_F full = {0, 0, W, H};
    bool light = mode == 1, glassy = mode == 2;
    if (glassy) c.Fill(full, 0, look.bg); // a tint over the blurred desktop
    else c.Fill(full, 0, light ? look.bg.Alpha(1) : Color{0.02f, 0.02f, 0.03f, 1});
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
    if (x + gap < r.right - 2) c.Line({x + gap, cy}, {r.right - 2, cy}, look.track, 4.5f * k); // unplayed part stays flat
    float grow = ui.Smooth(id + 2, ui.IsActive(id) || (show > 0.5f && Inside(r, ui.In().mouse)) ? 1.0f : 0, 16);
    float hh = (12 + 3 * grow) * k;
    c.Fill({x - 2.5f, cy - hh, x + 2.5f, cy + hh}, 2.5f, look.accentHi); // vertical bar playhead
    if (show < 0.01f) return;
    double total = tr.duration / 1e7, elapsed = preview >= 0 ? total * preview : tr.Elapsed() / 1e7;
    float ty = cy + 14 * k;
    c.Text(fmt::Clock(elapsed), Rect(r.left, ty, 100, 20), Weight::Regular, 13 * k, look.textDim.Alpha(show));
    c.Text(fmt::Clock(total), Rect(r.right - 100, ty, 100, 20), Weight::Regular, 13 * k, look.textDim.Alpha(show), Align::Right);
}

void Stage::Player(float show) { // the cover big on the left, everything about the song big on its right
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    bool has = model.hasTrack;
    float k = std::clamp(H / 900.0f, 0.8f, 1.6f); // type and spacing grow with the screen
    float side = (std::min)(H * 0.54f, W * 0.36f), gap = side * 0.16f, colW = (std::min)(W * 0.40f, 760.0f * k);
    float x0 = (W - side - gap - colW) / 2, cy = H * 0.5f;

    float pulse = 1 + 0.018f * beat; // the cover thumps with the kick
    D2D1_RECT_F art = Rect(x0 + side / 2 * (1 - pulse), cy - side * pulse / 2, side * pulse, side * pulse);
    float rad = side * 0.06f;
    D2D1_POINT_2F mid = {(art.left + art.right) / 2, cy};
    c.Glow({mid.x, mid.y + side * 0.08f}, side * 1.05f, side * 0.95f, look.accent.Alpha(0.30f + 0.30f * beat)); // light spilling from the cover
    c.Glow({mid.x, art.bottom}, side * 0.70f, side * 0.20f, Color{0, 0, 0, mode == 1 ? 0.18f : 0.45f}); // and a soft shadow under it
    if (ui.Button(Ui::Id("st.art"), art).clicked) done = true; // the cover brought you here, the cover takes you back
    if (cover) c.Art(cover.Get(), coverBlur.Get(), art, rad, 1);
    else c.FillV(art, rad, look.accentHi.Alpha(0.85f), look.accent.Alpha(0.55f)); // no art: a plain glowing tile
    c.Stroke(art, rad, Color{1, 1, 1, 0.10f}, 1);

    // the column, centered on the cover: app, title, artist, wave, controls
    float tx = x0 + side + gap, lineH = 66 * k, colH = 22 * k + lineH + 66 * k + 76 * k + 42 * k; // header, title, artist to the wave, wave to the controls, the play button
    float y = cy - colH / 2;
    c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(tx, y, colW, 20 * k), Weight::Medium, 13 * k, look.accentHi, 3.0f);
    y += 22 * k;
    c.Text(has && !tr.title.empty() ? tr.title : L"Nothing playing", Rect(tx, y, colW, lineH), Weight::Medium, 52 * k, look.text);
    y += lineH;
    std::wstring sub = !has ? L"Start music or a video in any app" : tr.album.empty() || tr.album == tr.artist ? tr.artist : tr.artist + L"  \u00B7  " + tr.album;
    c.Text(sub, Rect(tx, y, colW, 34 * k), Weight::Light, 24 * k, look.textDim);
    y += 40 * k + 26 * k;

    Seek(Rect(tx, y - 16 * k, colW, 32 * k), show);
    y += 76 * k; // controls just under the wave

    float alpha = 0.35f + 0.65f * show; // controls stay faintly visible, fully there when the mouse moves
    float ccx = tx + colW / 2; // controls centered under the wave
    D2D1_RECT_F row = {ccx - 270 * k, y - 50 * k, ccx + 270 * k, y + 50 * k};
    bool layer = alpha < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(row, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), nullptr);
    float px = ccx - 100 * k;
    if (w::IconBtn(ui, look, Ui::Id("st.prev"), {px, y}, 60 * k, glyph::Prev, 38 * k, has && tr.canPrev)) model.media.Previous();
    if (w::PlayBtn(ui, look, Ui::Id("st.play"), {px + 100 * k, y}, 84 * k, has && tr.playing, has)) model.media.TogglePlay();
    if (w::IconBtn(ui, look, Ui::Id("st.next"), {px + 200 * k, y}, 60 * k, glyph::Next, 38 * k, has && tr.canNext)) model.media.Next();
    D2D1_POINT_2F shuf = {px - 96 * k, y}, rep = {px + 296 * k, y};
    if (w::IconBtn(ui, look, Ui::Id("st.shuffle"), shuf, 48 * k, glyph::Shuffle, 26 * k, has && tr.canShuf, has && tr.shuffle)) model.media.ToggleShuffle();
    if (w::IconBtn(ui, look, Ui::Id("st.repeat"), rep, 48 * k, has && tr.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 26 * k, has && tr.canRep,
                   has && tr.repeat != 0))
        model.media.CycleRepeat();
    float sOn = ui.Smooth(Ui::Id("st.shuffle", 7), has && tr.shuffle ? 1.0f : 0, 14), rOn = ui.Smooth(Ui::Id("st.repeat", 7), has && tr.repeat != 0 ? 1.0f : 0, 14);
    if (sOn > 0.01f) c.Dot({shuf.x, shuf.y + 22 * k}, 2.8f * k * sOn, look.accent.Alpha(sOn)); // on: a dot under it, not just a color change
    if (rOn > 0.01f) c.Dot({rep.x, rep.y + 22 * k}, 2.8f * k * rOn, look.accent.Alpha(rOn));
    if (layer) ctx->PopLayer();
}

void Stage::Frame(const Theme& media) {
    if (!ctx) {
        done = true;
        return;
    }
    double now = Seconds();
    float dt = lastFrame == 0 ? 1 / 60.0f : (float)(std::min)(now - lastFrame, 0.05);
    lastFrame = now;
    SyncTheme(media);
    SyncCover();

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
    float show = ui.Smooth(Ui::Id("st.show"), now - lastMove < kIdleSec || paused || ui.Captured() ? 1.0f : 0, 6);

    canvas->NewFrame();
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ui.Begin(*canvas, input, dt, now);
    Backdrop(now);
    Clock(H * 0.035f, std::clamp(H / 900.0f, 0.8f, 1.6f));
    Player(show);
    float since = (float)(now - openedAt), tip = anim::Clamp01(since / 0.3f) * anim::Clamp01((2.6f - since) / 0.4f); // in, hold ~2 s, out
    if (tip > 0.001f) {
        float k = std::clamp(H / 900.0f, 0.8f, 1.6f), pw = 370 * k, ph = 38 * k;
        D2D1_RECT_F pill = Rect((W - pw) / 2, H * 0.085f + 8 * (1 - tip), pw, ph);
        Color fill = look.bg;
        fill.a = 0.85f;
        canvas->Fill(pill, ph / 2, fill.Alpha(tip));
        canvas->StrokeV(pill, ph / 2, look.hairTop.Alpha(tip), look.hairBot.Alpha(tip), 1);
        canvas->Text(L"Click the album cover or press Esc to leave", pill, Weight::Medium, 13 * k, look.text.Alpha(tip), Align::Center);
    }
    ui.End();
    HRESULT hr = ctx->EndDraw();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    bool lively = left + right > 0.03f || beat > 0.02f || ui.Busy() || ui.Captured() || (show > 0.01f && show < 0.99f) || tip > 0.001f;
    UINT sync = lively ? 1 : 2; // paused or silent: half rate, the slow drift doesn't need every refresh
    if (FAILED(hr) || FAILED(swap->Present(sync, 0))) done = true; // device lost: just leave, the panel is one hover away
}
