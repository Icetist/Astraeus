// Glass appearance: the panel sits on a live blur of whatever is behind it, cut to the exact panel shape.
// The fullscreen stage uses the same pieces, its blur simply covers the whole screen.
// Only built while the glass appearance is active and the panel is shown; dark and light never load any of this.
// windows headers first: they declare their own Color, which must not bind to ours from gfx.h
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <windows.graphics.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>

#include "drawer.h"


namespace WUC = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;

namespace {

constexpr DWORD kUseHostBackdrop = 17; // DWMWA_USE_HOSTBACKDROPBRUSH, older sdks don't name it

bool EnsureQueue() { // composition needs a dispatcher queue on the ui thread, made once on first use
    static const bool ok = [] {
        if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return true;
        using MakeQueue = HRESULT(WINAPI*)(DispatcherQueueOptions, ABI::Windows::System::IDispatcherQueueController**);
        HMODULE lib = LoadLibraryExW(L"CoreMessaging.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // loaded only once glass is used
        auto make = lib ? reinterpret_cast<MakeQueue>(GetProcAddress(lib, "CreateDispatcherQueueController")) : nullptr;
        if (!make) return false;
        DispatcherQueueOptions o = {sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE}; // com is already set up on this thread
        ABI::Windows::System::IDispatcherQueueController* q = nullptr;
        return SUCCEEDED(make(o, &q)); // kept on purpose: it lives as long as the ui thread
    }();
    return ok;
}

struct ShapeSource // hands our d2d outline to composition as a clip path
    : winrt::implements<ShapeSource, winrt::Windows::Graphics::IGeometrySource2D, ABI::Windows::Graphics::IGeometrySource2DInterop> {
    ComPtr<ID2D1Geometry> geo;
    explicit ShapeSource(ID2D1Geometry* g) : geo(g) {}
    HRESULT __stdcall GetGeometry(ID2D1Geometry** value) override { return geo.CopyTo(value); }
    HRESULT __stdcall TryGetGeometryUsingFactory(ID2D1Factory*, ID2D1Geometry** value) override {
        *value = nullptr;
        return E_NOTIMPL;
    }
};

WUC::CompositionSurfaceBrush Pinned(const WUC::Compositor& comp, const WUC::ICompositionSurface& surface) { // 1:1 pixels, top-left
    auto b = comp.CreateSurfaceBrush(surface);
    b.Stretch(WUC::CompositionStretch::None);
    b.HorizontalAlignmentRatio(0);
    b.VerticalAlignmentRatio(0);
    return b;
}

}

struct Drawer::Glass {
    WUC::Compositor comp{nullptr};
    WUC::Desktop::DesktopWindowTarget target{nullptr};
    WUC::SpriteVisual frost{nullptr}; // the blur layer, its opacity is the blur setting
    WUC::CompositionPathGeometry outline{nullptr}; // clips the blur to the panel shape
    WUC::CompositionGraphicsDevice gfx{nullptr};
    WUC::CompositionDrawingSurface surface{nullptr}; // our frame, updated in the same composition batch as the outline
    WUC::SpriteVisual content{nullptr}; // shows the surface
    WUC::ContainerVisual root{nullptr};
    bool filled = false; // a new surface must be drawn whole once before it accepts partial updates
    float blurShown = -1;
};

bool Drawer::OpenGlass(const DXGI_SWAP_CHAIN_DESC1& desc) {
    try {
        if (!EnsureQueue()) return false;
        auto g = std::make_unique<Glass>();

        BOOL hostBackdrop = TRUE; // windows 11: without this the backdrop brush stays transparent, no blur
        DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &hostBackdrop, sizeof(hostBackdrop));
        g->comp = WUC::Compositor();
        auto desk = g->comp.as<abi::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desk->CreateDesktopWindowTarget(hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(g->target))));
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        winrt::check_hresult(g->comp.as<abi::ICompositorInterop>()->CreateGraphicsDevice(dev.Get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(g->gfx))));
        g->surface = g->gfx.CreateDrawingSurface({(float)desc.Width, (float)desc.Height}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

        winrt::Windows::Foundation::Numerics::float2 size{(float)desc.Width, (float)desc.Height};
        // what's behind the window, blurred by windows itself. a backdrop can't sit inside a mask brush, so the shape clips the visual instead
        auto frost = g->comp.CreateSpriteVisual();
        frost.Size(size);
        frost.Brush(g->comp.CreateHostBackdropBrush());
        g->outline = g->comp.CreatePathGeometry();
        frost.Clip(g->comp.CreateGeometricClip(g->outline));
        frost.IsVisible(false); // until the first frame hands us a shape
        auto content = g->comp.CreateSpriteVisual(); // everything we draw, on top of the blur
        content.Size(size);
        content.Brush(Pinned(g->comp, g->surface));
        auto root = g->comp.CreateContainerVisual();
        root.Size(size);
        g->frost = frost;
        g->content = content;
        g->root = root;
        root.Children().InsertAtBottom(frost);
        root.Children().InsertAtTop(content);
        g->target.Root(root);
        glass = g.release();
        return true;
    } catch (...) {
        return false; // no composition: the caller falls back to the plain path
    }
}

bool Drawer::GlassResize(UINT w, UINT h) {
    if (!glass) return false;
    try {
        glass->surface.Resize({(int32_t)w, (int32_t)h}); // keeps its place; only how far it reaches changes
        winrt::Windows::Foundation::Numerics::float2 size{(float)w, (float)h};
        glass->frost.Size(size);
        glass->content.Size(size);
        glass->root.Size(size);
        glass->filled = false; // a resized surface wants one whole draw first
        return true;
    } catch (...) {
        return false;
    }
}

void Drawer::GlassMask(ID2D1Geometry* shape) {
    if (!glass) return;
    ComPtr<ID2D1Factory> f;
    ComPtr<ID2D1TransformedGeometry> placed; // same placement as the panel itself, in pixels like the visual
    ctx->GetFactory(&f);
    bool ok = shape && SUCCEEDED(f->CreateTransformedGeometry(shape, shapeXform * D2D1::Matrix3x2F::Scale(scale, scale), &placed));
    D2D1_RECT_F box{};
    ok = ok && SUCCEEDED(placed->GetBounds(nullptr, &box)) && box.right - box.left >= 4 && box.bottom - box.top >= 4; // a sliver of an outline makes composition fail fast, hide the blur instead
    try {
        if (ok) glass->outline.Path(WUC::CompositionPath(winrt::make<ShapeSource>(placed.Get())));
        glass->frost.IsVisible(ok);
    } catch (...) {
        glass->frost.IsVisible(false); // no clip, no blur: never frost the whole window
    }
}

void Drawer::GlassPresent() {
    if (!glass || !glass->surface) return;
    auto interop = glass->surface.as<abi::ICompositionDrawingSurfaceInterop>();
    ComPtr<IDXGISurface> dst;
    POINT off = {};
    D2D1_RECT_F vis = Visible();
    D2D1_SIZE_U full = backBuf->GetPixelSize();
    RECT update = {0, 0, (LONG)(std::min)((UINT32)std::ceil(vis.right * scale), full.width), (LONG)(std::min)((UINT32)std::ceil(vis.bottom * scale), full.height)};
    bool part = glass->filled && update.right > 0 && update.bottom > 0; // only what the window shows, once the surface has been filled whole
    if (!part || FAILED(interop->BeginDraw(&update, IID_PPV_ARGS(&dst), &off))) {
        part = false;
        if (FAILED(interop->BeginDraw(nullptr, IID_PPV_ARGS(&dst), &off))) return; // surface lost: the next frame tries again
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> into;
    if (SUCCEEDED(ctx->CreateBitmapFromDxgiSurface(dst.Get(), &bp, &into))) {
        D2D1_POINT_2U at = {(UINT32)off.x, (UINT32)off.y}; // the surface may live inside a shared atlas
        D2D1_RECT_U src = {0, 0, part ? (UINT32)update.right : full.width, part ? (UINT32)update.bottom : full.height};
        into->CopyFromBitmap(&at, backBuf.Get(), &src); // one gpu copy, no drawing
        glass->filled = true;
    }
    interop->EndDraw();
}

void Drawer::GlassBlur(float amount) {
    if (!glass || !glass->frost || amount == glass->blurShown) return; // only touch composition when it changes
    glass->frost.Opacity(amount); // 0 = clear glass, 1 = full backdrop blur
    glass->blurShown = amount;
}

void Drawer::FreeGlass() {
    if (!glass) return;
    BOOL hostBackdrop = FALSE; // dark and light don't need dwm sampling what's behind us
    DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &hostBackdrop, sizeof(hostBackdrop));
    try { // closed outright: only dropping them leaves windows to clean up later, and meanwhile the empty glass layer covers what's drawn next
        glass->target.Root(nullptr);
        glass->target.Close();
        glass->surface.Close();
        glass->gfx.Close();
        glass->comp.Close();
    } catch (...) {
    }
    delete glass;
    glass = nullptr;
}

struct Stage::Glass {
    WUC::Compositor comp{nullptr};
    WUC::Desktop::DesktopWindowTarget target{nullptr};
    WUC::ContainerVisual root{nullptr}; // the blur and the frame together, so they leave together
};

bool Stage::OpenGlass(UINT pxW, UINT pxH) {
    try {
        if (!EnsureQueue()) return false;
        auto g = std::make_unique<Glass>();
        BOOL hostBackdrop = TRUE;
        DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &hostBackdrop, sizeof(hostBackdrop));
        g->comp = WUC::Compositor();
        auto desk = g->comp.as<abi::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desk->CreateDesktopWindowTarget(hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(g->target))));
        WUC::ICompositionSurface main{nullptr};
        winrt::check_hresult(g->comp.as<abi::ICompositorInterop>()->CreateCompositionSurfaceForSwapChain(
            swap.Get(), reinterpret_cast<abi::ICompositionSurface**>(winrt::put_abi(main))));
        winrt::Windows::Foundation::Numerics::float2 size{(float)pxW, (float)pxH};
        auto frost = g->comp.CreateSpriteVisual(); // the desktop behind, blurred by windows, edge to edge
        frost.Size(size);
        frost.Brush(g->comp.CreateHostBackdropBrush());
        auto content = g->comp.CreateSpriteVisual();
        content.Size(size);
        content.Brush(Pinned(g->comp, main));
        auto root = g->comp.CreateContainerVisual();
        root.Size(size);
        root.Children().InsertAtBottom(frost);
        root.Children().InsertAtTop(content);
        g->target.Root(root);
        g->root = root;
        glass = g.release();
        return true;
    } catch (...) {
        return false; // the caller falls back to the dark room
    }
}

bool Stage::GlassLeave(float seconds, float cx, float cy) {
    try {
        auto& c = glass->comp;
        auto out = c.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.2f, 1.0f}); // quick, then settling
        auto smooth = c.CreateCubicBezierEasingFunction({0.45f, 0.0f}, {0.55f, 1.0f});
        auto span = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::duration<float>(seconds));
        glass->root.CenterPoint({cx, cy, 0});
        auto shrink = c.CreateVector3KeyFrameAnimation(); // a few percent back toward the bezel as it fades
        shrink.InsertKeyFrame(1.0f, {0.93f, 0.93f, 1.0f}, out);
        shrink.Duration(span);
        auto fade = c.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 0.0f, smooth);
        fade.Duration(span);
        glass->root.StartAnimation(L"Scale", shrink);
        glass->root.StartAnimation(L"Opacity", fade);
        return true;
    } catch (...) {
        return false;
    }
}

void Stage::FreeGlass() {
    delete glass; // the window goes right after, no need to reset the dwm attribute
    glass = nullptr;
}
