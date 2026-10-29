#include <cwchar>
#include <d2d1_1helper.h>
#include <d2d1effects.h>

#include "drawer.h"
#include "entry.h"
#include "glyphs.h"
#include "pages.h"
#include "starfall.h"

namespace {

constexpr float kThemeSec = 0.8f; // palette crossfade duration
constexpr float kFlipRate = 3.4f; // the cover's turn, ~0.7 s
constexpr float kRailEase = 14; // the rail's scroll glide rate
constexpr float kRailPeek = 22; // dips past the five shown where the next page's icon peeks
constexpr float kRailFadeLen = 30, kRailFadeLead = 12; // icons fade over this near a clipped end, starting a little past it
constexpr float kWidgetW = 52; // a status widget on either side of the pill's clock
constexpr float kArrowPad = 18; // a rail chevron keeps at least this from the panel's edge
constexpr float kArrowRoom = 22; // the top rail gives up this much at each end for its chevrons when it scrolls
constexpr float kArrowHalf = 11, kArrowLen = 3; // its hit box half size; its arms' reach: small
constexpr float kArrowRate = 14; // chevrons fade in and out at this rate

// a chevron pointing (dx, dy) toward pages out of view; shown only while there are some. true when clicked
bool RailArrow(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F at, float dx, float dy, bool more) {
    float v = ui.Smooth(id, more ? 1.0f : 0, kArrowRate);
    if (v < 0.01f) return false; // gone: nothing to click either
    Ui::Hit h = more ? ui.Button(id + 1, {at.x - kArrowHalf, at.y - kArrowHalf, at.x + kArrowHalf, at.y + kArrowHalf}) : Ui::Hit{};
    float hover = ui.Smooth(id + 2, h.hover ? 1.0f : 0, 16);
    Canvas& c = ui.C();
    if (hover > 0.01f) c.Dot(at, kArrowHalf - 1, t.text.Alpha(0.08f * hover * v));
    Color ink = t.textDim.Mix(t.text, hover).Alpha((0.45f + 0.55f * hover) * v); // quiet until the mouse comes near
    float px = -dy, py = dx; // across the arrow
    D2D1_POINT_2F tip = {at.x + dx * kArrowLen / 2, at.y + dy * kArrowLen / 2}, back = {at.x - dx * kArrowLen / 2, at.y - dy * kArrowLen / 2};
    c.Line({back.x + px * kArrowLen, back.y + py * kArrowLen}, tip, ink, 1.4f);
    c.Line({back.x - px * kArrowLen, back.y - py * kArrowLen}, tip, ink, 1.4f);
    return h.clicked;
}

bool MakeDev(ComPtr<ID3D11Device>& d3d, ComPtr<ID2D1DeviceContext>& ctx) { // a d3d device and a d2d context on it
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED | // bgra needed for d2d interop
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS; // fewer driver threads, less idle memory
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr); // software fallback
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device))) return false; // d2d device on top of d3d
    device->SetMaximumTextureMemory(24ull << 20); // d2d's own cache (glyph atlases, scratch layers) stays small however long a session runs
    return SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx));
}

DXGI_SWAP_CHAIN_DESC1 SwapDesc(UINT w, UINT h) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // flip-model swapchain for composition
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // per-pixel transparency
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; // lets main loop sleep until vsync
    return d;
}

bool MakeSwap(ID3D11Device* d3d, ID2D1DeviceContext* ctx, const DXGI_SWAP_CHAIN_DESC1& d, float dpi, ComPtr<IDXGISwapChain2>& swap,
              ComPtr<ID2D1Bitmap1>& buf, HANDLE& wait) { // a composition swapchain on d3d, its waitable and its backbuffer as a d2d target
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<IDXGISurface> surface;
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || // factory that owns this adapter
        FAILED(factory->CreateSwapChainForComposition(d3d, &d, nullptr, &sc)) || FAILED(sc.As(&swap)))
        return false;
    swap->SetMaximumFrameLatency(1); // lowest input lag
    wait = swap->GetFrameLatencyWaitableObject();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, // backbuffer is target only
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    return SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) && SUCCEEDED(ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &buf));
}

}

bool Drawer::EnsureDev() {
    if (ctx) return true; // already built, lazy create on first open
    if (!MakeDev(d3d, ctx)) {
        FreeDev();
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 d = SwapDesc((UINT)((Side() ? Depth() + overshoot : W) * scale + 0.5f),
                                       (UINT)((Side() ? Span() : H + overshoot) * scale + 0.5f)); // max size, window clips the rest
    surfW = d.Width;
    surfH = d.Height;
    if (lookNow == 2 && GlassDev(d)) return true; // glass: windows composition with a live backdrop blur, frames go straight into it
    if (!MakeSwap(d3d.Get(), ctx.Get(), d, 96 * scale, swap, backBuf, frameWait)) {
        FreeDev();
        return false;
    }
    ctx->SetTarget(backBuf.Get());
    ctx->SetDpi(96 * scale, 96 * scale); // draw in dips
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // cleartype breaks on transparent bg

    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp))) || // no rendering device: it only ever shows a swapchain
        FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)) || // topmost target on our hwnd
        FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) { // bind swapchain to window
        FreeDev();
        return false;
    }
    canvas = std::make_unique<Canvas>(ctx.Get());
    return true;
}

bool Drawer::RenewDev() {
    if (!ctx || !canvas || stage || (!glass && !(swap && visual && dcomp))) return false;
    // the old device goes first: the driver hands its pools back only once no device is left. nothing is committed in between, so the pill never blinks
    if (glass) GlassRenew(nullptr); // its surface off the content visual, the blur and outline stay
    else visual->SetContent(nullptr);
    ctx->SetTarget(nullptr);
    canvas.reset(); // everything made on the old device goes with it
    cover.Reset();
    coverBlur.Reset();
    coverPrev.Reset();
    ForgetShelfArt();
    pillLayer.Reset();
    pillKey = 0;
    starfall::Forget(); // its brushes too
    backBuf.Reset();
    if (!glass && frameWait) CloseHandle(frameWait), frameWait = nullptr;
    swap.Reset();
    ctx.Reset();
    gfx::Settle(d3d.Get());
    d3d.Reset();
    bool ok = MakeDev(d3d, ctx);
    if (ok && glass) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                             96 * scale, 96 * scale);
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        ok = SUCCEEDED(ctx->CreateBitmap({surfW, surfH}, nullptr, 0, bp, &backBuf)) && GlassRenew(dev.Get());
    } else if (ok) {
        ok = MakeSwap(d3d.Get(), ctx.Get(), SwapDesc(surfW, surfH), 96 * scale, swap, backBuf, frameWait);
    }
    if (!ok) { // no new device: the whole thing is built again the usual way on the next frame
        FreeDev();
        needsFrame = true;
        return true;
    }
    ctx->SetTarget(backBuf.Get());
    ctx->SetDpi(96 * scale, 96 * scale);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    canvas = std::make_unique<Canvas>(ctx.Get());
    Frame(); // the pill drawn on the new device before anything is shown
    if (!glass && swap && visual && dcomp && SUCCEEDED(visual->SetContent(swap.Get()))) dcomp->Commit();
    devWide = false;
    gfx::GiveBack(); // what the old device held goes back to windows now
    Log(L"pill moved to a fresh device");
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

void Drawer::FitSurfaces() {
    // the island alone: its pill and the room its spring bounces into. anything more (opening, a peek, the pill growing) gets the whole panel.
    // only the depth changes and the surfaces stay anchored to the same corner, so nothing on screen moves when they grow back
    bool alone = !opened && !peeking && !grow && !dragging && spring.target <= kIslandH + 1 && spring.value <= kIslandH + overshoot;
    float depth = alone ? kIslandH + overshoot : Depth() + overshoot;
    if (!alone && ctx) devWide = true, trimKey = 0; // the next resting pill tidies up, even if it looks just like the last one
    UINT w = (UINT)((Side() ? depth : W) * scale + 0.5f), h = (UINT)((Side() ? Span() : depth) * scale + 0.5f);
    if (!ctx || (w == surfW && h == surfH) || !w || !h) return;
    pillLayer.Reset(); // sized for the old surfaces
    pillKey = 0;
    ctx->SetTarget(nullptr);
    backBuf.Reset();
    bool ok = false;
    if (glass) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                             96 * scale, 96 * scale);
        ok = SUCCEEDED(ctx->CreateBitmap({w, h}, nullptr, 0, bp, &backBuf)) && GlassResize(w, h);
    } else if (swap) {
        ComPtr<IDXGISurface> surface;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
        ok = SUCCEEDED(swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) &&
             SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) && SUCCEEDED(ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &backBuf));
    }
    if (!ok) { // can't resize in place: start over at the right size next frame
        FreeDev();
        needsFrame = true;
        return;
    }
    ctx->SetTarget(backBuf.Get());
    surfW = w;
    surfH = h;
}

void Drawer::TrimDev() { // closed but the pill still needs the device: drop what d2d cached while the panel was open
    if (!ctx) return;
    ComPtr<ID2D1Device> dev;
    ctx->GetDevice(&dev);
    if (dev) dev->ClearResources(0); // glyph atlases, scratch textures, cached geometry; what the pill needs comes right back
    gfx::Settle(d3d.Get());
    HEAP_OPTIMIZE_RESOURCES_INFORMATION info{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof(info)); // freed heap back to windows; the working set stays, the pill is still animating
}

void Drawer::FreeDev() {
    if (ctx) ctx->SetTarget(nullptr); // nothing left pointing at the backbuffer
    canvas.reset();
    cover.Reset();
    coverBlur.Reset();
    coverPrev.Reset(); // the flip's old cover too: it would hold the device
    ForgetShelfArt(); // its previews belong to this device
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
    starfall::Forget(); // the sky's brushes, likewise
    devWide = false;
    swap.Reset();
    ctx.Reset();
    gfx::Settle(d3d.Get());
    d3d.Reset();
    if (!opened && !peeking) gfx::GiveBack(); // closed: hand it all back; mid-use (a dpi or look change) would only stutter
}

ComPtr<ID2D1Bitmap> Drawer::Blur(ID2D1Bitmap1* source) { return gfx::BakeBlur(ctx.Get(), source, 26.0f); } // heavy blur for backdrop

void Drawer::SyncCover() {
    uint64_t id = model.hasTrack ? model.track.coverId : 0;
    if (id != coverId) {
        coverId = id;
        bool turn = opened && !model.reduceMotion; // turns over only where someone sees it; calm motion just crossfades
        coverPrev = turn ? cover : nullptr;
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
    if (model.AccentMode() != 0) return; // custom accent: the wallpaper doesn't decide
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
    int look = model.Look(), bg = model.CustomBg(); // halobar's while following it
    if (look == lookNow && bg == bgNow) return;
    bool rebuild = (look == 2) != (lookNow == 2) && ctx; // glass draws through a different pipeline
    lookNow = look;
    bgNow = bg;
    if (rebuild) FreeDev(); // the caller rebuilds it right away for the new appearance
    SetShell(Make(shellSeed)); // media follows unless album tinted
    if (coverSrc && cfg.albumCol) SetMedia(Make(mediaSeed));
}

void Drawer::SyncAccent() {
    if (model.AccentMode() == accentModeNow && model.AccentHex() == accentHexNow) return;
    accentModeNow = model.AccentMode();
    accentHexNow = model.AccentHex();
    if (accentModeNow) {
        SetShell(Make(shellSeed = theme::FromCol(Color::Rgb((uint32_t)accentHexNow)))); // hue and saturation kept, brightness tuned by the theme
    } else {
        wallKey.clear(); // re-read the wallpaper even if it didn't change
        SyncWall();
    }
}

void Drawer::SyncTyping() {
    bool want = model.Typing() || StagePrefs::Get().capture || Entry::Get().focus; // a text field, or settings waiting for a fullscreen key
    if (want == typing) return;
    typing = want;
    if (typing) { // the panel never takes focus normally, its text fields are the exception
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

    if (!model.HasPage(page)) { // the page showing was just taken off the rail in settings: the first one stands in, no slide
        page = prevPage = model.pages.front();
        pageT = 1;
        model.SetLive(opened, page == Model::kPgSystem, page == Model::kPgNetwork);
    }
    auto divider = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) { // starfall: a seam of starlight instead of a plain rule
        if (shellNow.stars) starfall::Seam(*canvas, a, b, shellNow);
        else canvas->Line(a, b, shellNow.line, 1, false);
    };
    if (Side()) { // sidebar: icons along the top, under them the divider
        divider({20, kTopRail}, {panelW - 20, kTopRail});
        DrawRailTop(shellNow);
    } else {
        divider({kRailW, 34}, {kRailW, H - 34});
        DrawRail(shellNow);
    }
    bool fieldFocus = model.Typing() || Entry::Get().focus || StagePrefs::Get().capture || ui.Captured(); // a text field, a key being picked or a slider held keeps its arrows
    railHot = opened && !fieldFocus && Inside(Side() ? D2D1_RECT_F{0, 0, panelW, kTopRail} : D2D1_RECT_F{0, 0, kRailW, H}, local.mouse);
    D2D1_RECT_F area = Side() ? D2D1_RECT_F{14, kTopRail + 10, panelW - 14, panelH - 14} : D2D1_RECT_F{kRailW + 14, 18, panelW - 20, H - 20};

    if (pageT < 1) pageT = (std::min)(1.0f, pageT + dt / 0.38f); // 380ms page switch
    float e = anim::EaseOut(pageT);
    float slide = (1 - e) * (model.reduceMotion ? 0 : 26) * (model.SlotOf(page) > model.SlotOf(prevPage) ? 1 : -1); // slide 26 dips in the nav direction down the rail; calm: a crossfade
    float sheet = ui.Smooth(Ui::Id("bt.sheet"), model.btOpen ? 1.0f : 0, 14); // bt menu open anim
    float wsheet = ui.Smooth(Ui::Id("wifi.sheet"), model.wifiOpen ? 1.0f : 0, 14); // wi-fi picker open anim
    float ssheet = ui.Smooth(Ui::Id("set.sheet"), model.settingsOpen ? 1.0f : 0, 14); // settings open anim
    float csheet = ui.Smooth(Ui::Id("cal.sheet"), model.calOpen ? 1.0f : 0, 14); // calendar open anim
    float msheet = ui.Smooth(Ui::Id("mix.sheet"), model.mixerOpen ? 1.0f : 0, 14); // volume mixer open anim
    float under = 1 - anim::Clamp01((std::max)((std::max)((std::max)(sheet, wsheet), (std::max)(ssheet, csheet)), msheet) * 1.6f); // the page fades out as a sheet fades in, a crossfade, not a stack
    float pageAlpha = e * under;
    bool fading = pageAlpha < 0.999f;
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, slide) * base);
    if (fading) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), pageAlpha), nullptr); // opacity follows slide and sheets
    float fade = ui.SmoothFrom(Ui::Id("cover.fade", (int)coverId), 1, 0, 6); // id per cover restarts fade on track change
    bool art = model.showArt != 0; // album art off: placeholders and plain cards
    PageCtx pc{ui, model, shellNow, mediaNow, art ? cover.Get() : nullptr, art ? coverBlur.Get() : nullptr, fade,
                   D2D1_RECT_F{area.left, area.top - slide, area.right, area.bottom - slide}}; // undo slide so hit rects stay put
    pc.flip = coverPrev ? ui.SmoothFrom(Ui::Id("cover.flip", (int)coverId), 1, 0, kFlipRate) : 1;
    pc.coverPrev = art && pc.flip < 0.5f ? coverPrev.Get() : nullptr;
    if (pc.flip > 0.999f) coverPrev.Reset(); // turned: the old one can go
    ui.Block(sheet > 0.01f || wsheet > 0.01f || ssheet > 0.01f || csheet > 0.01f || msheet > 0.01f); // page under a sheet takes no input
    if (under > 0.001f) { // a sheet that has taken over hides the page, skip drawing it
        switch (page) {
        case Model::kPgHome: DrawHome(pc); break;
        case Model::kPgMedia: DrawMedia(pc); break;
        case Model::kPgSystem: DrawSys(pc); break;
        case Model::kPgNetwork: DrawNet(pc); break;
        case Model::kPgTools: DrawTools(pc); break;
        default: DrawShelf(pc); break;
        }
    }
    if (fading) ctx->PopLayer();
    ctx->SetTransform(base);
    ui.Block(false); // the sheets themselves take input
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
    if (msheet > 0.001f) {
        PageCtx mc{ui, model, shellNow, mediaNow, nullptr, nullptr, 1, area};
        DrawMixer(mc, msheet);
        pc.wantClose = pc.wantClose || mc.wantClose;
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

void Drawer::PlayFull() {
    if (stage || hidden || !model.hasTrack) return; // already up, a game has the screen, or nothing to show
    stageAway = true;
    OpenStage();
    stageHalo = true; // after OpenStage: the way back is halobar's, even with this panel open
}

void Drawer::OpenStage() {
    if (opened) stageAway = false; // asked from the panel: it comes back when the stage leaves; a resting pill or a notice isn't the panel
    Close(); // the panel steps aside, the stage covers this screen
    stageHome = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    stage = std::make_unique<Stage>(model, stageHome);
    if (!stage->Ok()) stage.reset();
    twinSplit = -1; // the first frame decides whether a second screen joins
    twinFailed = false;
}

HMONITOR Drawer::TwinScreen() const { // the nearest other screen
    if (GetSystemMetrics(SM_CMONITORS) < 2) return nullptr; // one screen: the split does nothing
    struct Near {
        HMONITOR home, best;
        RECT at;
        double d;
    } n{stageHome, nullptr, {}, 1e300};
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(stageHome, &mi)) n.at = mi.rcMonitor;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT r, LPARAM lp) -> BOOL { // the closest by centre: the one beside it, not across the room
        auto* n = reinterpret_cast<Near*>(lp);
        if (m == n->home) return TRUE;
        double dx = (r->left + r->right - n->at.left - n->at.right) / 2.0, dy = (r->top + r->bottom - n->at.top - n->at.bottom) / 2.0;
        if (dx * dx + dy * dy < n->d) n->d = dx * dx + dy * dy, n->best = m;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&n));
    return n.best;
}

void Drawer::SyncTwin() {
    if (stage->Leaving()) return;
    const StagePrefs& p = StagePrefs::Get();
    if (p.split != twinSplit) { // the setting moved, or the screens did: where does the other half go
        twinSplit = p.split;
        HMONITOR want = p.split ? TwinScreen() : nullptr;
        if (twin && twin->Screen() != want) twin.reset(); // off, or that screen went: its window goes
        if (want && !twin && !twinFailed) {
            twin = std::make_unique<Stage>(model, want, false); // the keyboard stays with the first
            if (!twin->Ok()) twin.reset(), twinFailed = true;
        }
    }
    Stage::Role mine = !twin ? Stage::kWhole : p.swap ? Stage::kLyrics : Stage::kCover;
    stage->SetRole(mine);
    if (twin) twin->SetRole(mine == Stage::kCover ? Stage::kLyrics : Stage::kCover);
}

void Drawer::StageFrame() {
    constexpr WPARAM kHaloPlayer = 120; // halobar opens its player panel on this command: the fullscreen player it asked for has closed
    SyncTwin();
    stage->Frame(mediaNow);
    if (twin && WaitForSingleObject(twin->Wait(), 0) == WAIT_OBJECT_0) twin->Frame(mediaNow); // its own screen's pace: drawn when that screen is ready, never waited on
    if (twin && twin->Leaving() != stage->Leaving()) stage->Leave(), twin->Leave(); // closing either half closes both
    if (stage->Leaving() && !stageBack) { // fullscreen fades out over the panel dropping open on the media page, where it came from
        stageBack = true; // once, for the pair
        if (stageHalo) {
            if (HWND halo = FindWindowW(L"HaloBar", nullptr)) PostMessageW(halo, WM_COMMAND, kHaloPlayer, 0); // where it came from: halobar's player panel
        } else if (!stageAway) {
            if (page != Model::kPgMedia) SetPage(Model::kPgMedia); // the media page
            pageSet = true; // even with a fixed opening page picked in settings
            Open();
        }
    }
    if (stage->Done() && (!twin || twin->Done())) { // their windows, devices and audio capture all go with them
        twin.reset();
        stage.reset();
        stageBack = stageAway = stageHalo = false;
    }
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
    if (peekLevel >= 0) { // the hud: what changed, a bar and the number
        float lv = peekLevel;
        canvas->Text(peekTitle, Rect(cx - 84, cy - 20, 150, 20), Weight::Medium, 14, t.text.Alpha(a));
        canvas->Text(w::Pct(peekLevel), Rect(cx + 76, cy - 20, 60, 20), Weight::Medium, 13, t.textDim.Alpha(a), Align::Right);
        D2D1_RECT_F bar = Rect(cx - 84, cy + 7, 220, 6);
        canvas->Fill(bar, 3, t.track.Alpha(a));
        if (lv > 0.01f) canvas->FillH({bar.left, bar.top, bar.left + Width(bar) * lv, bar.bottom}, 3, t.accent.Alpha(a), t.accentHi.Alpha(a));
        return;
    }
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
    bool music = model.pillMusic && model.hasTrack && !model.track.title.empty(); // music mode can be turned off: the clock stays
    bool moving = pillPass != 1 && music && model.vizOn && StepIslandBars(dt, now); // the layer pass leaves the bars alone
    if (pillPass != 2 && Bubble()) DrawBubble();
    if (Side()) return DrawIslandSide(h, a) && moving;
    bool still = pillPass != 2, bars = pillPass != 1; // which parts this pass draws
    float inner = kIslandW - 2 * cfg.fillet - 20; // usable width inside the shoulders
    float x0 = W / 2 - inner / 2, cy = h / 2 - 1; // content sits a little high, the handle takes the bottom
    float cx = W / 2;

    float hw = islandHot ? 26.0f : 16.0f; // handle bar: the "pull me down" hint, wider and brighter on hover
    if (still) canvas->Line({cx - hw / 2, h - 3.5f}, {cx + hw / 2, h - 3.5f}, t.text.Alpha((islandHot ? 0.75f : 0.30f) * a), 2.5f);

    if (!music) { // nothing playing: time and date, or the clock between two widgets
        if (!still) return false;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t clock[32] = {}, day[48] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, clock, 32);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", day, 48, nullptr);
        int gl = 0, gr = 0;
        std::wstring left = model.pillAlways ? model.PillWidget(model.pillLeft, gl) : L"", right = model.pillAlways ? model.PillWidget(model.pillRight, gr) : L"";
        if (left.empty() && right.empty()) {
            canvas->Text(std::wstring(clock) + L"  ·  " + day, Rect(x0, cy - 8, inner, 16), Weight::Medium, 11.5f, t.text.Alpha(a), Align::Center);
            return false;
        }
        canvas->Text(clock, Rect(x0 + kWidgetW, cy - 8, inner - 2 * kWidgetW, 16), Weight::Medium, 11.5f, t.text.Alpha(a), Align::Center);
        auto widget = [&](const std::wstring& text, int g, bool atRight) { // icon and value, hugging the pill's ends
            if (text.empty()) return;
            float ix = atRight ? x0 + inner - kWidgetW + 7 : x0 + 7;
            canvas->Icon(g, {ix, cy}, 13, mt.accentHi.Alpha(a), true);
            canvas->Text(text, Rect(ix + 9, cy - 7, kWidgetW - 14, 14), Weight::Medium, 10, t.textDim.Alpha(a));
        };
        widget(left, gl, false);
        widget(right, gr, true);
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
    for (int i = 0; bars && model.vizOn && i < 4; ++i) {
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
    bool track = model.pillMusic && model.hasTrack && !model.track.title.empty();
    text(PillSig()); // widgets, the bubble's time
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
    float f[] = {h, W, winW, winH, glassShadeNow, (float)edgeNow, islandHot ? 1.0f : 0.0f, track ? 1.0f : 0.0f, (float)model.BgOpacity(), Bubble() ? model.ActivityFrac() : -1};
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

    if (!model.pillMusic || !model.hasTrack || model.track.title.empty()) { // nothing playing: the time, hours over minutes
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

float Drawer::RailScroll(int n, int slot) {
    int most = (std::max)(0, n - kRailShown);
    if (most == 0) return railScroll = railScrollTo = 0, railSlotSeen = slot, 0.0f; // five or fewer: everything shows, as always
    if (slot != railSlotSeen) { // a new page: bring it into view, only once, so the wheel can look elsewhere
        if (slot < railScrollTo) railScrollTo = (float)slot;
        if (slot > railScrollTo + kRailShown - 1) railScrollTo = (float)(slot - kRailShown + 1);
        railSlotSeen = slot;
    }
    railScrollTo = (std::clamp)(railScrollTo, 0.0f, (float)most);
    if (model.reduceMotion) railScroll = railScrollTo;
    else if (anim::Approach(railScroll, railScrollTo, kRailEase, ui.Dt(), 0.002f)) ui.Wake();
    return railScroll;
}

void Drawer::SyncRailKeys(bool want) {
    if (want == railKeys) return;
    railKeys = want;
    if (!want) {
        UnregisterHotKey(hwnd, kRailKeyPrev);
        UnregisterHotKey(hwnd, kRailKeyNext);
        return;
    }
    bool prev = RegisterHotKey(hwnd, kRailKeyPrev, 0, Side() ? VK_LEFT : VK_UP) != FALSE; // the panel never takes focus: a hotkey reaches it anyway, only while hovered
    bool next = RegisterHotKey(hwnd, kRailKeyNext, 0, Side() ? VK_RIGHT : VK_DOWN) != FALSE;
    if (!prev && !next) railKeys = false; // another app holds them: hovering just doesn't add keys
}

float Drawer::RailFade(float pos, float lo, float hi, float n) const { // pos and the view's ends along the rail; n pages
    if (n <= kRailShown) return 1;
    return anim::Clamp01(((std::min)(pos - lo, hi - pos) + kRailFadeLead) / kRailFadeLen);
}

void Drawer::DrawRailTop(const Theme& t) {
    Canvas& c = *canvas;
    int n = (int)model.pages.size(), slot = (std::max)(0, model.SlotOf(page)), shown = (std::min)(n, kRailShown);
    float room = n > shown ? kArrowRoom : 0; // scrolling: each end keeps a little space for its chevron
    float step = (CW() - 28 - 2 * room) / shown, cy = kTopRail / 2;
    float off = step * RailScroll(n, slot); // the row slides left as it scrolls
    float lo = 14 + room, hi = lo + step * shown; // the view's ends
    bool clip = n > shown;
    if (clip) c.PushClip({lo - 6, 0, hi + 6, kTopRail});
    float x = ui.Smooth(Ui::Id("railx"), 14 + step * (slot + 0.5f), 18) - off; // highlight glides to the active page
    c.Fill({x - 26, cy - 19, x + 26, cy + 19}, 19, t.accent.Alpha(0.13f * RailFade(x, lo, hi, (float)n)));
    if (t.stars) starfall::Halo(c, {x, cy}, 30, 22, t, RailFade(x, lo, hi, (float)n)); // starfall: the active page glows, with a faint violet reach
    c.Fill({x - 11, kTopRail - 6, x + 11, kTopRail - 3}, 1.5f, t.accent.Alpha(RailFade(x, lo, hi, (float)n))); // accent bar under the active icon
    for (int i = 0; i < n; ++i) {
        int id = model.pages[(size_t)i];
        float ix = 14 + step * (i + 0.5f) - off, fade = RailFade(ix, lo, hi, (float)n);
        if (fade <= 0) continue; // scrolled out of view: neither drawn nor clickable
        Ui::Hit h = ui.Button(Ui::Id("railt", id), {(std::max)(lo, ix - 28), 4, (std::min)(hi, ix + 28), kTopRail - 4});
        if (h.clicked) SetPage(id);
        if (h.hover) railTip = id, railTipAt = {ix, kTopRail + 4};
        float hover = ui.Smooth(Ui::Id("railt.h", id), h.hover && id != page ? 1.0f : 0, 16);
        bool active = id == page;
        c.Icon(Model::PageGlyph(id), {ix, cy}, 22, (active ? t.accentHi : t.textDim.Mix(t.text, hover)).Alpha(fade), active);
    }
    if (clip) {
        c.PopClip();
        float most = (float)(n - shown);
        if (RailArrow(ui, t, Ui::Id("railt.prev"), {lo - room / 2, cy}, -1, 0, railScrollTo > 0.01f)) railScrollTo = std::round(railScrollTo) - 1, ui.Wake(); // one slot; RailScroll clamps
        if (RailArrow(ui, t, Ui::Id("railt.next"), {hi + room / 2, cy}, 1, 0, railScrollTo < most - 0.01f)) railScrollTo = std::round(railScrollTo) + 1, ui.Wake();
    }
    if (ui.RClick({0, 0, CW(), kTopRail})) model.OpenSettings(); // settings stay a click away even with the tools page taken off
}

void Drawer::DrawRailTip(const Theme& t) { // a word beside the rail icon under the mouse
    if (railTip >= 0) railTipShown = railTip;
    float a = ui.Smooth(Ui::Id("rail.tip"), railTip >= 0 ? 1.0f : 0, 18);
    railTip = -1; // the rail sets it again next frame while the mouse stays
    if (a < 0.01f || railTipShown < 0) return;
    Canvas& c = *canvas;
    const wchar_t* name = Model::PageName(railTipShown);
    static float widths[Model::kPageIds] = {}; // measured once per name
    float& tw = widths[railTipShown];
    if (tw <= 0) {
        tw = std::wcslen(name) * 6.6f;
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(name, (UINT32)std::wcslen(name), gfx::TextFmt(Weight::Medium, 12, Align::Left), 400, 30, &lay)) &&
            SUCCEEDED(lay->GetMetrics(&tm)))
            tw = tm.widthIncludingTrailingWhitespace;
    }
    float w = tw + 34, h = 26, o = (1 - a) * 6;
    D2D1_RECT_F r = Side() ? Rect(railTipAt.x - w / 2, railTipAt.y - o, w, h) : Rect(railTipAt.x + o, railTipAt.y - h / 2, w, h);
    c.Fill(Inset(r, -1, -2), h / 2 + 2, Color{0, 0, 0, 0.22f * a}); // a quiet tag: soft shadow, card body, hairline, a small accent tick
    c.Fill(r, h / 2, t.veil.Alpha(0.94f * a)); // solid: under glass whatever is behind would read through
    c.Fill(r, h / 2, t.cardHi.Alpha(a));
    c.StrokeV(r, h / 2, t.hairTop.Alpha(a), t.hairBot.Alpha(a), 1);
    c.Dot({r.left + 13, (r.top + r.bottom) / 2}, 2.5f, t.accentHi.Alpha(a));
    c.Text(name, {r.left + 22, r.top, r.right - 8, r.bottom}, Weight::Medium, 12, t.text.Alpha(a), Align::Center);
}

void Drawer::DrawRail(const Theme& t) {
    Canvas& c = *canvas;
    const float spacing = 54;
    int n = (int)model.pages.size(), slot = (std::max)(0, model.SlotOf(page)), shown = (std::min)(n, kRailShown);
    float first = (H - spacing * (shown - 1)) / 2; // however many pages show, they sit centered
    float off = spacing * RailScroll(n, slot); // more than five: the list slides up under a fixed window
    float lo = first - spacing / 2, hi = first + spacing * (shown - 0.5f); // the window's ends
    float targetTop = first + slot * spacing - 20, targetBottom = targetTop + 40; // 40 dip pill around active icon, in list space
    if (railBot <= railTop) {
        railTop = targetTop;
        railBot = targetBottom;
    }
    bool down = targetTop > railTop;
    bool a = anim::Approach(railTop, targetTop, down ? 9.0f : 24.0f, ui.Dt(), 0.05f); // leading edge fast, trailing slow
    bool b = anim::Approach(railBot, targetBottom, down ? 24.0f : 9.0f, ui.Dt(), 0.05f); // so pill stretches like goo
    if (a || b) ui.Wake(); // keep frames coming while pill moves

    bool clip = n > shown;
    if (clip) c.PushClip({0, lo - kRailPeek, kRailW, hi + kRailPeek}); // a sliver of the next one peeks, faded
    float pt = railTop - off, pb = railBot - off, pf = RailFade((pt + pb) / 2, lo, hi, (float)n);
    if (t.stars) starfall::Halo(c, {kRailW / 2, (pt + pb) / 2}, 34, 30, t, pf); // starfall: the active page glows, with a faint violet reach
    c.Fill({13, pt, kRailW - 13, pb}, 20, t.accent.Alpha(0.13f * pf));
    c.Fill({5, pt + 10, 8, pb - 10}, 1.5f, t.accent.Alpha(pf)); // thin accent bar on left edge

    for (int i = 0; i < n; ++i) {
        int id = model.pages[(size_t)i];
        float y = first + i * spacing - off, fade = RailFade(y, lo - kRailPeek, hi + kRailPeek, (float)n);
        if (fade <= 0) continue; // scrolled out of view: neither drawn nor clickable
        D2D1_RECT_F hit = {10, (std::max)(lo, y - 22), kRailW - 10, (std::min)(hi, y + 22)};
        Ui::Hit h = ui.Button(Ui::Id("rail", id), hit);
        if (h.clicked) SetPage(id);
        if (h.hover) railTip = id, railTipAt = {kRailW - 6, y};
        float hover = ui.Smooth(Ui::Id("rail.h", id), h.hover && id != page ? 1.0f : 0, 16); // no hover tint on active
        bool active = id == page;
        Color ink = active ? t.accentHi : t.textDim.Mix(t.text, hover);
        c.Icon(Model::PageGlyph(id), {kRailW / 2, y}, 23, ink.Alpha(fade), active);
    }
    if (clip) {
        c.PopClip();
        float most = (float)(n - shown), top = (std::max)(kArrowPad, (lo - kRailPeek) / 2), bot = (std::min)(H - kArrowPad, (H + hi + kRailPeek) / 2); // midway between the panel's edge and the peeking icon
        if (RailArrow(ui, t, Ui::Id("rail.prev"), {kRailW / 2, top}, 0, -1, railScrollTo > 0.01f)) railScrollTo = std::round(railScrollTo) - 1, ui.Wake(); // one slot; RailScroll clamps
        if (RailArrow(ui, t, Ui::Id("rail.next"), {kRailW / 2, bot}, 0, 1, railScrollTo < most - 0.01f)) railScrollTo = std::round(railScrollTo) + 1, ui.Wake();
    }
    if (ui.RClick({0, 0, kRailW, H})) model.OpenSettings(); // settings stay a click away even with the tools page taken off
}

void Drawer::DrawBubble() { // a running timer, stopwatch or focus session: a ring and its time, beside the pill
    const Theme& t = shellNow;
    D2D1_MATRIX_3X2_F was;
    ctx->GetTransform(&was);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity()); // window dips: the box is worked out there
    D2D1_RECT_F b = BubbleBox();
    D2D1_POINT_2F o = {(b.left + b.right) / 2, (b.top + b.bottom) / 2};
    float r = kBubble / 2;
    Color fill = t.glass ? t.veil.Alpha(0.92f) : t.bg; // no blur reaches out here: glass gets its dark twin
    if (!t.glass) fill.a *= model.BgOpacity() / 100.0f;
    canvas->Dot(o, r, fill);
    if (t.stars) starfall::Sky(*canvas, {o.x - r, o.y - r, o.x + r, o.y + r}, r, t, 0, fill.a, 0); // starfall: the bubble is a little moon of the same sky
    if (t.glass) canvas->Ring(o, r - 0.5f, Color{1, 1, 1, 0.26f}, 1);
    canvas->Ring(o, r - 3, t.track, 2);
    float f = anim::Clamp01(model.ActivityFrac());
    if (f > 0.005f) canvas->Arc(o, r - 3, -3.14159265f / 2, f * 2 * 3.14159265f, t.accent, 2);
    canvas->Text(model.ActivityText(), Rect(b.left - 2, o.y - 7, kBubble + 4, 14), Weight::Medium, 9.5f, t.text, Align::Center);
    ctx->SetTransform(was);
}
