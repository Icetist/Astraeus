#include <cwchar>
#include <d2d1_1helper.h>
#include <d2d1effects.h>

#include "drawer.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr float kRailW = 72; // left nav rail width in dips
constexpr int kPages = 5;
const int kRailIcons[kPages] = {glyph::Overview, glyph::Music, glyph::System, glyph::Network, glyph::Tools};
constexpr float kThemeSec = 0.8f; // palette crossfade duration

}

bool Drawer::EnsureDev() {
    if (ctx) return true; // already built, lazy create on first open
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED | // bgra needed for d2d interop
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS; // fewer driver threads, less idle memory
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr); // software fallback
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<IDXGISurface> surface;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || // d2d device on top of d3d
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx)) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { // factory that owns this adapter
        FreeDev();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = (UINT)((Side() ? Depth() + overshoot : W) * scale + 0.5f);
    d.Height = (UINT)((Side() ? Span() : H + overshoot) * scale + 0.5f); // max size, window clips the rest
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // flip-model swapchain for composition
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // per-pixel transparency
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; // lets main loop sleep until vsync
    if (lookNow == 2 && GlassDev(d)) return true; // glass: windows composition with a live backdrop blur, frames go straight into it
    if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc)) || FAILED(sc.As(&swap))) {
        FreeDev();
        return false;
    }
    swap->SetMaximumFrameLatency(1); // lowest input lag
    frameWait = swap->GetFrameLatencyWaitableObject();

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, // backbuffer is target only
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) || FAILED(ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &backBuf))) {
        FreeDev();
        return false;
    }
    ctx->SetTarget(backBuf.Get());
    ctx->SetDpi(96 * scale, 96 * scale); // draw in dips
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // cleartype breaks on transparent bg

    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) ||
        FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)) || // topmost target on our hwnd
        FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) { // bind swapchain to window
        FreeDev();
        return false;
    }
    canvas = std::make_unique<Canvas>(ctx.Get());
    return true;
}

bool Drawer::GlassDev(const DXGI_SWAP_CHAIN_DESC1& d) {
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, // drawn into, then copied to the composition surface
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(ctx->CreateBitmap({d.Width, d.Height}, nullptr, 0, bp, &backBuf))) return false;
    frameWait = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameWait || !OpenGlass(d)) { // manual reset: stays signaled once due, so a later redraw never waits on a spent timer
        if (frameWait) CloseHandle(frameWait);
        frameWait = nullptr;
        backBuf.Reset();
        return false;
    }
    LARGE_INTEGER now = {};
    SetWaitableTimer(frameWait, &now, 0, nullptr, nullptr, FALSE); // due at once: the first frame draws right away
    ctx->SetTarget(backBuf.Get());
    ctx->SetDpi(96 * scale, 96 * scale);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    canvas = std::make_unique<Canvas>(ctx.Get());
    return true;
}

void Drawer::FreeDev() {
    canvas.reset();
    cover.Reset();
    coverBlur.Reset();
    if (frameWait) {
        CloseHandle(frameWait);
        frameWait = nullptr; // main loop falls back to plain msg wait
    }
    FreeGlass();
    visual.Reset(); // release in reverse order of creation
    target.Reset();
    dcomp.Reset();
    backBuf.Reset();
    pillLayer.Reset(); // belongs to this device
    swap.Reset();
    ctx.Reset();
    d3d.Reset();
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0); // hand freed pages back to os
}

ComPtr<ID2D1Bitmap> Drawer::Blur(ID2D1Bitmap1* source) { return gfx::BakeBlur(ctx.Get(), source, 26.0f); } // heavy blur for backdrop

void Drawer::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id != coverId) {
        coverId = id;
        coverSrc.Reset();
        cover.Reset();
        coverBlur.Reset();
        const auto& bytes = model.track.cover;
        if (id != 0 && bytes && !bytes->empty()) coverSrc = gfx::Decode(bytes->data(), bytes->size(), 480); // decode capped at 480px
        if (coverSrc && cfg.albumCol) SetMedia(Make(mediaSeed = theme::FromBmp(coverSrc.Get()))); // tint media page from art
        else SetMedia(shellTo); // no art, follow shell theme
    }
    if (coverSrc && !cover && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(coverSrc.Get(), nullptr, &cover))) // gpu upload, redone after device loss
        coverBlur = Blur(cover.Get());
}

void Drawer::SyncWall() {
    if (model.accentMode != 0) return; // custom accent: the wallpaper doesn't decide
    std::wstring path = theme::WallPath();
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    std::wstring key = path + L"|" + std::to_wstring(info.ftLastWriteTime.dwLowDateTime); // path + mtime detects same-name replace
    if (key == wallKey) return;
    wallKey = key;
    ComPtr<IWICBitmapSource> src = gfx::DecodeFile(path.c_str(), 256); // small decode is enough for palette
    if (!src) return;
    SetShell(Make(shellSeed = theme::FromBmp(src.Get())));
}

Theme Drawer::Make(Seed s) const {
    s.look = lookNow;
    s.bg = bgNow;
    return Theme::From(s);
}

void Drawer::SyncLook() {
    if (model.look == lookNow && model.customBg == bgNow) return;
    bool rebuild = (model.look == 2) != (lookNow == 2) && ctx; // glass draws through a different pipeline
    lookNow = model.look;
    bgNow = model.customBg;
    if (rebuild) FreeDev(); // the caller rebuilds it right away for the new appearance
    SetShell(Make(shellSeed)); // media follows unless album tinted
    if (coverSrc && cfg.albumCol) SetMedia(Make(mediaSeed));
}

void Drawer::SyncAccent() {
    if (model.accentMode == accentModeNow && model.accentHex == accentHexNow) return;
    accentModeNow = model.accentMode;
    accentHexNow = model.accentHex;
    if (accentModeNow) {
        SetShell(Make(shellSeed = theme::FromCol(Color::Rgb((uint32_t)accentHexNow)))); // hue and saturation kept, brightness tuned by the theme
    } else {
        wallKey.clear(); // re-read the wallpaper even if it didn't change
        SyncWall();
    }
}

void Drawer::SyncTyping() {
    if (model.hexFocus == typing) return;
    typing = model.hexFocus;
    if (typing) { // the panel never takes focus normally, the hex field is the one exception
        typingFrom = GetForegroundWindow();
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    } else if (GetForegroundWindow() == hwnd && typingFrom && IsWindow(typingFrom)) {
        SetForegroundWindow(typingFrom); // give the keyboard back to where it was
    }
}

void Drawer::SetShell(const Theme& t) {
    shellFrom = shellNow;
    shellTo = t;
    shellT = 0;
    if (!coverSrc || !cfg.albumCol) SetMedia(t); // media follows shell unless album tinted
}

void Drawer::SetMedia(const Theme& t) {
    mediaFrom = mediaNow;
    mediaTo = t;
    mediaT = 0;
}

bool Drawer::StepTheme(float dt) {
    bool moving = false;
    if (shellT < 1) {
        shellT = (std::min)(1.0f, shellT + dt / kThemeSec);
        shellNow = shellFrom.Lerp(shellTo, anim::EaseOut(shellT));
        moving = true;
    }
    if (mediaT < 1) {
        mediaT = (std::min)(1.0f, mediaT + dt / kThemeSec);
        mediaNow = mediaFrom.Lerp(mediaTo, anim::EaseOut(mediaT));
        moving = true;
    }
    return moving;
}

ComPtr<ID2D1Geometry> Drawer::Shape(float h, float w) {
    if (shapeC && std::fabs(h - shapeH) < 0.01f && std::fabs(w - shapeW) < 0.01f) return shapeC; // reuse while size is steady
    shapeC = BuildShape(h, w);
    shapeH = h;
    shapeW = w;
    return shapeC;
}

ComPtr<ID2D1Geometry> Drawer::BuildShape(float h, float w) const {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> s;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&s))) return nullptr;
    float f = (std::min)(cfg.fillet, h / 2), r = (std::min)(cfg.corner, h / 2); // shrink radii when short so arcs don't overlap
    float left = (Span() - w) / 2;
    float xr = left + w - cfg.fillet, xl = left + cfg.fillet; // panel edges inside the fillet margins
    s->BeginFigure({xl - f, 0}, D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine({xr + f, 0});
    s->AddArc({{xr, f}, {f, f}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL}); // concave shoulder, looks like dripping from bezel
    s->AddLine({xr, h - r});
    s->AddArc({{xr - r, h}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL}); // convex bottom corner
    s->AddLine({xl + r, h});
    s->AddArc({{xl, h - r}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    s->AddLine({xl, f});
    s->AddArc({{xl - f, 0}, {f, f}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    return path;
}

void Drawer::DrawBody(float h, float dt, double now) {
    float panelW = CW(), panelH = CH();
    D2D1_POINT_2F org = edgeNow == 0 ? D2D1_POINT_2F{cfg.fillet, h - H} // content slides with the shape, never stretched
                      : edgeNow == 1 ? D2D1_POINT_2F{h - panelW, cfg.fillet}
                                     : D2D1_POINT_2F{winH - h, cfg.fillet};
    D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(org.x, org.y);
    ctx->SetTransform(base);
    canvas->PushClip(edgeNow == 0 ? D2D1_RECT_F{0, H - h, panelW, H} : edgeNow == 1 ? D2D1_RECT_F{panelW - h, 0, panelW, panelH} : D2D1_RECT_F{0, 0, h, panelH});
    float reach = Depth();
    float reveal = anim::Clamp01((h - reach * 0.25f) / (reach * 0.55f)); // fade content in from 25% to 80% of the way out
    bool layered = reveal < 0.999f; // skip costly layer once fully shown
    if (layered) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                       D2D1::IdentityMatrix(), reveal), nullptr);

    Input local = input;
    local.mouse = {input.mouse.x - org.x, input.mouse.y - org.y}; // mouse into panel space
    local.pressPos = {input.pressPos.x - org.x, input.pressPos.y - org.y};
    ui.Begin(*canvas, local, dt, now);

    if (Side()) { // sidebar: icons along the top
        DrawRailTop(shellNow);
        canvas->Line({20, kTopRail}, {panelW - 20, kTopRail}, shellNow.line, 1, false);
    } else {
        DrawRail(shellNow);
        canvas->Line({kRailW, 34}, {kRailW, H - 34}, shellNow.line, 1, false);
    }
    D2D1_RECT_F area = Side() ? D2D1_RECT_F{14, kTopRail + 10, panelW - 14, panelH - 14} : D2D1_RECT_F{kRailW + 14, 18, panelW - 20, H - 20};

    if (pageT < 1) pageT = (std::min)(1.0f, pageT + dt / 0.38f); // 380ms page switch
    float e = anim::EaseOut(pageT);
    float slide = (1 - e) * 26 * (page > prevPage ? 1 : -1); // slide 26 dips in the nav direction
    float sheet = ui.Smooth(Ui::Id("bt.sheet"), model.btOpen ? 1.0f : 0, 14); // bt menu open anim
    float wsheet = ui.Smooth(Ui::Id("wifi.sheet"), model.wifiOpen ? 1.0f : 0, 14); // wi-fi picker open anim
    float ssheet = ui.Smooth(Ui::Id("set.sheet"), model.settingsOpen ? 1.0f : 0, 14); // settings open anim
    float csheet = ui.Smooth(Ui::Id("cal.sheet"), model.calOpen ? 1.0f : 0, 14); // calendar open anim
    float under = 1 - anim::Clamp01((std::max)((std::max)(sheet, wsheet), (std::max)(ssheet, csheet)) * 1.6f); // the page fades out as a sheet fades in, a crossfade, not a stack
    float pageAlpha = e * under;
    bool fading = pageAlpha < 0.999f;
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, slide) * base);
    if (fading) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), pageAlpha), nullptr); // opacity follows slide and sheets
    float fade = ui.SmoothFrom(Ui::Id("cover.fade", (int)coverId), 1, 0, 6); // id per cover restarts fade on track change
    PageCtx pc{ui, model, shellNow, mediaNow, cover.Get(), coverBlur.Get(), fade,
                   D2D1_RECT_F{area.left, area.top - slide, area.right, area.bottom - slide}}; // undo slide so hit rects stay put
    ui.Block(sheet > 0.01f || wsheet > 0.01f || ssheet > 0.01f || csheet > 0.01f); // page under a sheet takes no input
    if (under > 0.001f) { // a sheet that has taken over hides the page, skip drawing it
        switch (page) {
        case 0: DrawHome(pc); break;
        case 1: DrawMedia(pc); break;
        case 2: DrawSys(pc); break;
        case 3: DrawNet(pc); break;
        default: DrawTools(pc); break;
        }
    }
    if (fading) ctx->PopLayer();
    ctx->SetTransform(base);
    ui.Block(false);
    if (sheet > 0.001f) {
        PageCtx bc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, area};
        DrawBt(bc, sheet);
        pc.wantClose = pc.wantClose || bc.wantClose;
    }
    if (wsheet > 0.001f) {
        PageCtx wc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, area};
        DrawWifi(wc, wsheet);
        pc.wantClose = pc.wantClose || wc.wantClose;
    } else {
        model.DropWifiList(); // faded out, free it
    }
    if (ssheet > 0.001f) {
        PageCtx sc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, area};
        DrawSettings(sc, ssheet);
        pc.wantClose = pc.wantClose || sc.wantClose;
    }
    if (csheet > 0.001f) {
        PageCtx cc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, area};
        DrawCalendar(cc, csheet);
    }
    DrawRailTip(shellNow); // over the page and its sheets

    ui.End();
    hand = ui.Hand(); // picked up by WM_SETCURSOR
    if (layered) ctx->PopLayer();
    canvas->PopClip();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    hintText = pc.hint;
    hintFade = pc.hint ? pc.hintFade : 0;
    hintRect = {pc.hintRect.left + org.x, pc.hintRect.top + org.y, pc.hintRect.right + org.x, pc.hintRect.bottom + org.y};
    DrawHint(h);
    if (pc.wantClose) Close(); // page asked to close, after drawing finishes
    if (pc.wantStage) OpenStage();
    if (pc.wantPage >= 0) SetPage(pc.wantPage);
}

void Drawer::OpenStage() {
    Close(); // the panel steps aside, the stage covers this screen
    stage = std::make_unique<Stage>(model, MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
    if (!stage->Ok()) stage.reset();
}

void Drawer::DrawHint(float h) {
    if (!hintText || hintFade < 0.01f) return;
    const Theme& t = shellNow;
    const float pw = 104, ph = 28, gapOut = 9;
    float cy = (hintRect.top + hintRect.bottom) / 2, cx = (hintRect.left + hintRect.right) / 2;
    D2D1_RECT_F pill = edgeNow == 0 ? Rect(cx - pw / 2, h + gapOut - 4 * (1 - hintFade), pw, ph) // under the panel, below the cover
                     : edgeNow == 1 ? Rect(h + gapOut - 4 * (1 - hintFade), cy - ph / 2, pw, ph) // beside the sidebar
                                    : Rect(winH - h - gapOut - pw + 4 * (1 - hintFade), cy - ph / 2, pw, ph);
    Color fill = t.bg;
    fill.a = 0.94f; // solid even in glass, it sits off the blurred panel
    canvas->Fill(pill, ph / 2, fill.Alpha(hintFade));
    canvas->StrokeV(pill, ph / 2, t.hairTop.Alpha(hintFade), t.hairBot.Alpha(hintFade), 1);
    canvas->Text(hintText, pill, Weight::Medium, 12, t.text.Alpha(hintFade), Align::Center);
}

void Drawer::DrawPeek(float h) {
    const Theme& t = shellNow;
    float a = anim::Clamp01((h - 26) / 30); // fade text in as banner drops
    if (a <= 0) return;
    D2D1_RECT_F area = edgeNow == 0 ? Rect(0, 0, W, h) : edgeNow == 1 ? Rect(0, cfg.fillet, h, CH()) : Rect(winH - h, cfg.fillet, h, CH()); // visible part
    float cx = (area.left + area.right) / 2 + (Side() ? 60 : 0), cy = Side() ? area.top + 70 : (area.top + area.bottom) / 2 + 2; // sidebar: near the top
    canvas->Dot({cx - 112, cy}, 17, t.accent.Alpha(0.18f * a));
    canvas->Icon(peekIcon ? peekIcon : glyph::Timer, {cx - 112, cy}, 21, t.accentHi.Alpha(a), true);
    canvas->Text(peekTitle, Rect(cx - 84, cy - 20, 240, 22), Weight::Medium, 15, t.text.Alpha(a));
    canvas->Text(peekDetail, Rect(cx - 84, cy + 1, 240, 18), Weight::Light, 12.5f, t.textDim.Alpha(a));
}

bool Drawer::StepIslandBars(float dt, double now) {
    bool playing = model.hasTrack && model.track.playing;
    float l = 0, r = 0;
    if (playing) model.meter.Read(l, r);
    float level = std::sqrt((l + r) / 2); // sqrt lifts quiet passages so the bars stay alive
    bool moving = false;
    for (int i = 0; i < 4; ++i) {
        float wobble = 0.55f + 0.45f * std::sin((float)now * (5.0f + i * 1.7f) + i * 1.3f); // each bar on its own rhythm
        float want = playing ? anim::Clamp01(level * wobble * 1.4f) : 0;
        float& v = islandBars[i];
        v += (want - v) * (std::min)(1.0f, dt * 14); // quick attack, smooth fall
        moving = moving || std::fabs(want - v) > 0.01f || want > 0.01f;
    }
    return moving;
}

bool Drawer::DrawIsland(float h, float dt, double now) {
    const Theme& t = shellNow;
    const Theme& mt = mediaNow; // the wave in the album's colors
    float a = anim::Clamp01((h - 14) / 18); // fade in as the pill drops
    hand = true; // the whole pill is a button
    if (a <= 0) return false;
    bool moving = pillPass != 1 && model.hasTrack && !model.track.title.empty() && StepIslandBars(dt, now); // the layer pass leaves the bars alone
    if (Side()) return DrawIslandSide(h, a) && moving;
    bool still = pillPass != 2, bars = pillPass != 1; // which parts this pass draws
    float inner = kIslandW - 2 * cfg.fillet - 20; // usable width inside the shoulders
    float x0 = W / 2 - inner / 2, cy = h / 2 - 1; // content sits a little high, the handle takes the bottom
    float cx = W / 2;

    float hw = islandHot ? 26.0f : 16.0f; // handle bar: the "pull me down" hint, wider and brighter on hover
    if (still) canvas->Line({cx - hw / 2, h - 3.5f}, {cx + hw / 2, h - 3.5f}, t.text.Alpha((islandHot ? 0.75f : 0.30f) * a), 2.5f);

    if (!model.hasTrack || model.track.title.empty()) { // nothing playing: time and date
        if (!still) return false;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t clock[32] = {}, day[48] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, clock, 32);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", day, 48, nullptr);
        canvas->Text(std::wstring(clock) + L"  ·  " + day, Rect(x0, cy - 8, inner, 16), Weight::Medium, 11.5f, t.text.Alpha(a), Align::Center);
        return false;
    }

    D2D1_RECT_F art = Rect(x0, cy - 10, 20, 20); // cover thumbnail, rounded like an app icon
    const float barsW = 22; // mini visualizer on the right, or the expand chevron on hover
    float tx = art.right + 7, tw = inner - (tx - x0) - barsW - 6;
    if (still) {
        if (cover) {
            canvas->Art(cover.Get(), coverBlur.Get(), art, 5, a);
        } else {
            canvas->Fill(art, 5, mt.cardHi.Alpha(a));
            canvas->Icon(glyph::Music, {art.left + 10, art.top + 10}, 12, mt.accentHi.Alpha(a), true);
        }
        canvas->Text(model.track.title, Rect(tx, cy - 12, tw, 14), Weight::Medium, 10.5f, t.text.Alpha(a));
        if (!model.track.artist.empty()) canvas->Text(model.track.artist, Rect(tx, cy + 1, tw, 12), Weight::Light, 9.5f, t.textDim.Alpha(a));
    }

    float bx = x0 + inner - barsW;
    if (islandHot) { // chevron: tap opens the full panel
        if (!still) return false;
        float ccx = bx + barsW / 2;
        Color ink = mt.accentHi.Alpha(a);
        canvas->Line({ccx - 6, cy - 3}, {ccx, cy + 3}, ink, 2);
        canvas->Line({ccx, cy + 3}, {ccx + 6, cy - 3}, ink, 2);
        return false;
    }
    for (int i = 0; bars && i < 4; ++i) {
        float v = islandBars[i], bh = 3 + 11 * v;
        canvas->Fill(Rect(bx + i * 6, cy - bh / 2, 3.5f, bh), 1.75f, mt.accent.Mix(mt.accentHi, v).Alpha(a));
    }
    return moving;
}

uint64_t Drawer::PillKey(float h) const {
    uint64_t k = 1469598103934665603ull; // fnv-1a over everything the still parts are drawn from
    auto mix = [&k](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) k = (k ^ ((const unsigned char*)p)[i]) * 1099511628211ull;
    };
    auto text = [&mix](const std::wstring& s) { mix(s.data(), s.size() * sizeof(wchar_t)); };
    bool track = model.hasTrack && !model.track.title.empty();
    if (track) {
        text(model.track.title);
        text(model.track.artist);
    } else { // the clock and date show instead: the minute is part of it
        SYSTEMTIME st;
        GetLocalTime(&st);
        mix(&st.wMinute, sizeof(st.wMinute));
        mix(&st.wHour, sizeof(st.wHour));
        mix(&st.wDay, sizeof(st.wDay));
    }
    void* art = cover.Get();
    mix(&art, sizeof(art));
    float f[] = {h, W, winW, winH, glassShade, (float)edgeNow, islandHot ? 1.0f : 0.0f, track ? 1.0f : 0.0f};
    mix(f, sizeof(f));
    for (const Color* c : {&shellNow.bg, &shellNow.text, &shellNow.textDim, &shellNow.sheen, &mediaNow.cardHi, &mediaNow.accentHi}) mix(c, sizeof(Color));
    return k;
}

bool Drawer::PillLayer(float h, float shapeWide) { // before the frame: rebuilt only when something it shows changed
    D2D1_SIZE_U px = ctx->GetPixelSize();
    if (!px.width || !px.height) return false;
    bool fits = pillLayer && pillLayer->GetPixelSize().width == px.width && pillLayer->GetPixelSize().height == px.height;
    uint64_t key = PillKey(h);
    if (fits && key == pillKey) return true;
    if (!fits) {
        pillLayer.Reset();
        float dx = 96, dy = 96;
        ctx->GetDpi(&dx, &dy);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dx, dy);
        if (FAILED(ctx->CreateBitmap(px, nullptr, 0, bp, &pillLayer))) return false;
    }
    ComPtr<ID2D1Image> was;
    ctx->GetTarget(&was);
    ctx->SetTarget(pillLayer.Get());
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ctx->SetTransform(shapeXform);
    PaintShape(Shape(h, shapeWide).Get(), h);
    pillPass = 1;
    DrawIsland(h, 0, 0);
    pillPass = 0;
    bool ok = SUCCEEDED(ctx->EndDraw());
    ctx->SetTarget(was.Get());
    pillKey = ok ? key : 0;
    if (!ok) pillLayer.Reset();
    return ok;
}

bool Drawer::DrawIslandSide(float h, float a) {
    const Theme& t = shellNow;
    const Theme& mt = mediaNow;
    bool left = edgeNow == 1;
    auto X = [&](float d) { return left ? d : winH - d; }; // d = distance from the docked edge, y runs along it
    float mid = winW / 2, dc = h / 2 - 1; // the handle takes the inner side
    float hw = islandHot ? 26.0f : 16.0f;
    canvas->Line({X(h - 3.5f), mid - hw / 2}, {X(h - 3.5f), mid + hw / 2}, t.text.Alpha((islandHot ? 0.75f : 0.30f) * a), 2.5f);

    if (!model.hasTrack || model.track.title.empty()) { // nothing playing: the time, hours over minutes
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t hh[4], mm[4];
        swprintf_s(hh, L"%02d", st.wHour);
        swprintf_s(mm, L"%02d", st.wMinute);
        canvas->Text(hh, {X(dc) - 14, mid - 17, X(dc) + 14, mid - 1}, Weight::Medium, 12, t.text.Alpha(a), Align::Center);
        canvas->Text(mm, {X(dc) - 14, mid + 1, X(dc) + 14, mid + 17}, Weight::Medium, 12, mt.accentHi.Alpha(a), Align::Center);
        return false;
    }
    // reads like a book spine: bottom to top on the left edge, top to bottom on the right, cover first, bars last
    float inner = kIslandSide - 2 * cfg.fillet - 20, dir = left ? -1.0f : 1.0f;
    float start = mid - dir * inner / 2, end = mid + dir * inner / 2;
    float artC = start + dir * 10, barsC = end - dir * 11;
    D2D1_RECT_F art = {X(dc) - 10, artC - 10, X(dc) + 10, artC + 10};
    if (cover) {
        canvas->Art(cover.Get(), coverBlur.Get(), art, 5, a);
    } else {
        canvas->Fill(art, 5, mt.cardHi.Alpha(a));
        canvas->Icon(glyph::Music, {X(dc), artC}, 12, mt.accentHi.Alpha(a), true);
    }
    float t0 = artC + dir * 17, t1 = barsC - dir * 17, len = std::fabs(t1 - t0), tc = (t0 + t1) / 2;
    D2D1_MATRIX_3X2_F was;
    ctx->GetTransform(&was);
    ctx->SetTransform(D2D1::Matrix3x2F::Rotation(left ? -90.0f : 90.0f) * D2D1::Matrix3x2F::Translation(X(dc), tc) * was); // turned text, centered in its stretch
    canvas->Text(model.track.title, Rect(-len / 2, -12, len, 14), Weight::Medium, 10.5f, t.text.Alpha(a));
    if (!model.track.artist.empty()) canvas->Text(model.track.artist, Rect(-len / 2, 1, len, 12), Weight::Light, 9.5f, t.textDim.Alpha(a));
    ctx->SetTransform(was);
    if (islandHot) { // chevron pointing into the screen: tap opens the full panel
        Color ink = mt.accentHi.Alpha(a);
        float s = left ? 1.0f : -1.0f;
        canvas->Line({X(dc) - 3 * s, barsC - 6}, {X(dc) + 3 * s, barsC}, ink, 2);
        canvas->Line({X(dc) + 3 * s, barsC}, {X(dc) - 3 * s, barsC + 6}, ink, 2);
        return false;
    }
    for (int i = 0; i < 4; ++i) { // bars grow across the pill
        float v = islandBars[i], bl = 3 + 11 * v, y = barsC - 11 + i * 6;
        canvas->Fill({X(dc) - bl / 2, y, X(dc) + bl / 2, y + 3.5f}, 1.75f, mt.accent.Mix(mt.accentHi, v).Alpha(a));
    }
    return true;
}

void Drawer::DrawRailTop(const Theme& t) {
    Canvas& c = *canvas;
    float step = (CW() - 28) / kPages, cy = kTopRail / 2;
    float x = ui.Smooth(Ui::Id("railx"), 14 + step * (page + 0.5f), 18); // highlight glides to the active page
    c.Fill({x - 26, cy - 19, x + 26, cy + 19}, 19, t.accent.Alpha(0.13f));
    c.Fill({x - 11, kTopRail - 6, x + 11, kTopRail - 3}, 1.5f, t.accent); // accent bar under the active icon
    for (int i = 0; i < kPages; ++i) {
        float ix = 14 + step * (i + 0.5f);
        Ui::Hit h = ui.Button(Ui::Id("railt", i), {ix - 28, 4, ix + 28, kTopRail - 4});
        if (h.clicked) SetPage(i);
        if (h.hover) railTip = i, railTipAt = {ix, kTopRail + 4};
        float hover = ui.Smooth(Ui::Id("railt.h", i), h.hover && i != page ? 1.0f : 0, 16);
        bool active = i == page;
        c.Icon(kRailIcons[i], {ix, cy}, 22, active ? t.accentHi : t.textDim.Mix(t.text, hover), active);
    }
}

void Drawer::DrawRailTip(const Theme& t) { // a word beside the rail icon under the mouse
    static const wchar_t* const kNames[kPages] = {L"Home", L"Media", L"System", L"Network", L"Tools"};
    if (railTip >= 0) railTipShown = railTip;
    float a = ui.Smooth(Ui::Id("rail.tip"), railTip >= 0 ? 1.0f : 0, 18);
    railTip = -1; // the rail sets it again next frame while the mouse stays
    if (a < 0.01f || railTipShown < 0) return;
    Canvas& c = *canvas;
    const wchar_t* name = kNames[railTipShown];
    float w = std::wcslen(name) * 7.8f + 30, h = 30, o = (1 - a) * 8;
    D2D1_RECT_F r = Side() ? Rect(railTipAt.x - w / 2, railTipAt.y - o, w, h) : Rect(railTipAt.x + o, railTipAt.y - h / 2, w, h);
    c.Glow({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, w * 0.8f, h * 1.2f, t.accent.Alpha(0.30f * a));
    c.FillH(r, h / 2, t.accent.Alpha(a), t.accentHi.Alpha(a)); // a bold accent pill
    c.Text(name, r, Weight::Semi, 12.5f, t.onAccent.Alpha(a), Align::Center);
}

void Drawer::DrawRail(const Theme& t) {
    Canvas& c = *canvas;
    const float spacing = 54;
    float first = (H - spacing * (kPages - 1)) / 2;
    float targetTop = first + page * spacing - 20, targetBottom = targetTop + 40; // 40 dip pill around active icon
    if (railBot <= railTop) {
        railTop = targetTop;
        railBot = targetBottom;
    }
    bool down = targetTop > railTop;
    bool a = anim::Approach(railTop, targetTop, down ? 9.0f : 24.0f, ui.Dt(), 0.05f); // leading edge fast, trailing slow
    bool b = anim::Approach(railBot, targetBottom, down ? 24.0f : 9.0f, ui.Dt(), 0.05f); // so pill stretches like goo
    if (a || b) ui.Wake(); // keep frames coming while pill moves

    c.Fill({13, railTop, kRailW - 13, railBot}, 20, t.accent.Alpha(0.13f));
    c.Fill({5, railTop + 10, 8, railBot - 10}, 1.5f, t.accent); // thin accent bar on left edge

    for (int i = 0; i < kPages; ++i) {
        float y = first + i * spacing;
        D2D1_RECT_F hit = {10, y - 22, kRailW - 10, y + 22};
        Ui::Hit h = ui.Button(Ui::Id("rail", i), hit);
        if (h.clicked) SetPage(i);
        if (h.hover) railTip = i, railTipAt = {kRailW - 6, y};
        float hover = ui.Smooth(Ui::Id("rail.h", i), h.hover && i != page ? 1.0f : 0, 16); // no hover tint on active
        bool active = i == page;
        Color ink = active ? t.accentHi : t.textDim.Mix(t.text, hover);
        c.Icon(kRailIcons[i], {kRailW / 2, y}, 23, ink, active);
    }
}
