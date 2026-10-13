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
    d.Width = (UINT)(W * scale + 0.5f);
    d.Height = (UINT)((H + overshoot) * scale + 0.5f); // max size, window clips the rest
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // flip-model swapchain for composition
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // per-pixel transparency
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; // lets main loop sleep until vsync
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

void Drawer::FreeDev() {
    canvas.reset();
    cover.Reset();
    coverBlur.Reset();
    if (frameWait) {
        CloseHandle(frameWait);
        frameWait = nullptr; // main loop falls back to plain msg wait
    }
    visual.Reset(); // release in reverse order of creation
    target.Reset();
    dcomp.Reset();
    backBuf.Reset();
    swap.Reset();
    ctx.Reset();
    d3d.Reset();
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0); // hand freed pages back to os
}

ComPtr<ID2D1Bitmap> Drawer::Blur(ID2D1Bitmap1* source) {
    ComPtr<ID2D1Effect> blur;
    ComPtr<ID2D1Bitmap1> out;
    D2D1_SIZE_U size = source->GetPixelSize();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(ctx->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) || FAILED(ctx->CreateBitmap(size, nullptr, 0, bp, &out))) return nullptr;
    blur->SetInput(0, source);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 26.0f); // heavy blur for backdrop
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD); // no dark fade at edges
    float dx, dy;
    ctx->GetDpi(&dx, &dy);
    ctx->SetDpi(96, 96);
    ctx->SetTarget(out.Get()); // bake once, not every frame
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ctx->DrawImage(blur.Get());
    ctx->EndDraw();
    ctx->SetTarget(backBuf.Get());
    ctx->SetDpi(dx, dy);
    return out;
}

void Drawer::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id != coverId) {
        coverId = id;
        coverSrc.Reset();
        cover.Reset();
        coverBlur.Reset();
        const auto& bytes = model.track.cover;
        if (id != 0 && bytes && !bytes->empty()) coverSrc = gfx::Decode(bytes->data(), bytes->size(), 480); // decode capped at 480px
        if (coverSrc && cfg.albumCol) SetMedia(Theme::From(theme::FromBmp(coverSrc.Get()))); // tint media page from art
        else SetMedia(shellTo); // no art, follow shell theme
    }
    if (coverSrc && !cover && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(coverSrc.Get(), nullptr, &cover))) // gpu upload, redone after device loss
        coverBlur = Blur(cover.Get());
}

void Drawer::SyncWall() {
    if (!cfg.wallTheme) return;
    std::wstring path = theme::WallPath();
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    std::wstring key = path + L"|" + std::to_wstring(info.ftLastWriteTime.dwLowDateTime); // path + mtime detects same-name replace
    if (key == wallKey) return;
    wallKey = key;
    ComPtr<IWICBitmapSource> src = gfx::DecodeFile(path.c_str(), 256); // small decode is enough for palette
    if (!src) return;
    SetShell(Theme::From(theme::FromBmp(src.Get())));
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

ComPtr<ID2D1Geometry> Drawer::Shape(float h) {
    if (shapeC && std::fabs(h - shapeH) < 0.01f) return shapeC; // reuse while height is steady
    shapeC = BuildShape(h);
    shapeH = h;
    return shapeC;
}

ComPtr<ID2D1Geometry> Drawer::BuildShape(float h) const {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> s;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&s))) return nullptr;
    float f = (std::min)(cfg.fillet, h / 2), r = (std::min)(cfg.corner, h / 2); // shrink radii when short so arcs don't overlap
    float xr = W - cfg.fillet, xl = cfg.fillet; // panel edges inside the fillet margins
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
    float panelW = W - 2 * cfg.fillet;
    float dy = h - H; // content slides down with the shape, not stretched
    D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(cfg.fillet, dy);
    ctx->SetTransform(base);
    canvas->PushClip({0, H - h, panelW, H});
    float reveal = anim::Clamp01((h - H * 0.25f) / (H * 0.55f)); // fade content in from 25% to 80% height
    bool layered = reveal < 0.999f; // skip costly layer once fully shown
    if (layered) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                       D2D1::IdentityMatrix(), reveal), nullptr);

    Input local = input;
    local.mouse = {input.mouse.x - cfg.fillet, input.mouse.y - dy}; // mouse into panel space
    local.pressPos = {input.pressPos.x - cfg.fillet, input.pressPos.y - dy};
    ui.Begin(*canvas, local, dt, now);

    DrawRail(shellNow);
    canvas->Line({kRailW, 34}, {kRailW, H - 34}, Color{1, 1, 1, 0.06f}, 1, false);

    if (pageT < 1) pageT = (std::min)(1.0f, pageT + dt / 0.38f); // 380ms page switch
    float e = anim::EaseOut(pageT);
    float slide = (1 - e) * 26 * (page > prevPage ? 1 : -1); // slide 26 dips in the nav direction
    bool fading = e < 0.999f;
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, slide) * base);
    if (fading) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), e), nullptr); // opacity follows slide
    float fade = ui.SmoothFrom(Ui::Id("cover.fade", (int)coverId), 1, 0, 6); // id per cover restarts fade on track change
    PageCtx pc{ui, model, shellNow, mediaNow, cover.Get(), coverBlur.Get(), fade,
                   D2D1_RECT_F{kRailW + 14, 18 - slide, panelW - 20, H - 20 - slide}}; // undo slide so hit rects stay put
    float sheet = ui.Smooth(Ui::Id("bt.sheet"), model.btOpen ? 1.0f : 0, 14); // bt menu open anim
    ui.Block(sheet > 0.01f); // page under the menu takes no input
    switch (page) {
    case 0: DrawHome(pc); break;
    case 1: DrawMedia(pc); break;
    case 2: DrawSys(pc); break;
    case 3: DrawNet(pc); break;
    default: DrawTools(pc); break;
    }
    if (fading) ctx->PopLayer();
    ctx->SetTransform(base);
    ui.Block(false);
    if (sheet > 0.001f) {
        PageCtx bc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, D2D1_RECT_F{kRailW + 14, 18, panelW - 20, H - 20}};
        DrawBt(bc, sheet);
        pc.wantClose = pc.wantClose || bc.wantClose;
    }

    ui.End();
    hand = ui.Hand(); // picked up by WM_SETCURSOR
    if (layered) ctx->PopLayer();
    canvas->PopClip();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    if (pc.wantClose) Close(); // page asked to close, after drawing finishes
}

void Drawer::DrawPeek(float h) {
    const Theme& t = shellNow;
    float a = anim::Clamp01((h - 26) / 30); // fade text in as banner drops
    if (a <= 0) return;
    float cx = W / 2, cy = h / 2 + 2;
    canvas->Dot({cx - 112, cy}, 17, t.accent.Alpha(0.18f * a));
    canvas->Icon(peekIcon ? peekIcon : glyph::Timer, {cx - 112, cy}, 21, t.accentHi.Alpha(a), true);
    canvas->Text(peekTitle, Rect(cx - 84, cy - 20, 240, 22), Weight::Medium, 15, t.text.Alpha(a));
    canvas->Text(peekDetail, Rect(cx - 84, cy + 1, 240, 18), Weight::Light, 12.5f, t.textDim.Alpha(a));
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
        float hover = ui.Smooth(Ui::Id("rail.h", i), h.hover && i != page ? 1.0f : 0, 16); // no hover tint on active
        bool active = i == page;
        Color ink = active ? t.accentHi : t.textDim.Mix(t.text, hover);
        c.Icon(kRailIcons[i], {kRailW / 2, y}, 23, ink, active);
    }
}
