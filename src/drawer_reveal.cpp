// A new look arrives as one quiet, deliberate reveal: it opens from the pointer with a soft feathered edge, a faint band of the new accent
// riding along it. Each look adds only a touch of its own: glass frosts the old look over as it goes, chrome sends a single glint
// round the edge, starfall lets a few stars catch along it.
// The old look's last frame is kept in memory once, so it outlives a new device (into or out of glass), and is drawn over the new look as it goes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "anim.h"
#include "drawer.h"
#include "gfx.h"

namespace {

constexpr float kRevealSec = 0.65f; // unhurried, never waited on
enum Style { kPlain, kGlass, kChrome, kStars };
constexpr float kFeather = 60; // dips of soft edge between the old look and the new
constexpr float kReach = 1.04f; // past the farthest corner, edge and all
constexpr float kBlurSigma = 14; // glass: how far the old look softens as it goes
constexpr float kGlintSweep = 38, kGlintTurns = 0.55f; // chrome: the glint's length round the edge in degrees, and how far it travels
constexpr int kGlints = 10; // starfall: stars on the edge
constexpr float kPi = 3.14159265f;

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }

float Hash(uint32_t i) { // a steady scatter: the same stars every frame, only their twinkle moves
    uint32_t x = i * 2654435761u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (float)(x & 0xFFFF) / 65535.0f;
}

}

void Drawer::RevealGrab() {
    if (revealStage != 1) return;
    revealStage = 0; // a copy that fails just lets the look crossfade, as it always did
    if (!ctx || !backBuf) return;
    D2D1_SIZE_U px = backBuf->GetPixelSize();
    ComPtr<ID2D1Bitmap1> read; // the frame back from the gpu, once: it has to outlive this device
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, backBuf->GetPixelFormat(), 96 * scale, 96 * scale);
    D2D1_MAPPED_RECT m{};
    if (FAILED(ctx->CreateBitmap(px, nullptr, 0, bp, &read)) || FAILED(read->CopyFromBitmap(nullptr, backBuf.Get(), nullptr)) || FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &m))) return;
    size_t row = (size_t)px.width * 4;
    revealPix.resize(row * px.height);
    for (UINT y = 0; y < px.height; ++y) std::memcpy(revealPix.data() + y * row, m.bits + (size_t)y * m.pitch, row);
    read->Unmap();
    revealPxW = px.width, revealPxH = px.height;
    revealSnap.Reset();
    revealBlur.Reset();
    revealStage = 2;
    needsFrame = true; // the next frame brings the new look in
}

bool Drawer::RevealDraw(float dt) {
    if (revealT >= 1) return false;
    if (!revealSnap && !revealPix.empty() && ctx) { // on whichever device draws now: the old one, or the one made for glass
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
        ctx->CreateBitmap({revealPxW, revealPxH}, revealPix.data(), revealPxW * 4, bp, &revealSnap);
        revealPix.clear();
        revealPix.shrink_to_fit(); // a few megabytes, not kept a moment longer
        if (revealSnap && revealStyle == kGlass) revealBlur = gfx::BakeBlur(ctx.Get(), revealSnap.Get(), kBlurSigma); // once, not every frame
    }
    if (!revealSnap) {
        revealT = 1;
        return false;
    }
    revealT = (std::min)(1.0f, revealT + dt / kRevealSec);
    float e = EaseInOut(revealT), shine = std::sin(kPi * revealT);
    D2D1_SIZE_F sz = revealSnap->GetSize();
    D2D1_RECT_F all = {0, 0, sz.width, sz.height};
    float farthest = 0;
    for (D2D1_POINT_2F p : {D2D1_POINT_2F{0, 0}, D2D1_POINT_2F{sz.width, 0}, D2D1_POINT_2F{0, sz.height}, D2D1_POINT_2F{sz.width, sz.height}})
        farthest = (std::max)(farthest, std::hypot(p.x - revealAt.x, p.y - revealAt.y));
    float r = (farthest + kFeather) * kReach * e, outer = r + kFeather; // inside r the new look, past outer the old, between them a soft blend

    // the old look, faded out by a radial mask: clear inside the circle, whole beyond its feathered edge
    D2D1_GRADIENT_STOP stops[2] = {{r / outer, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, 1)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> mask;
    if (SUCCEEDED(ctx->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(ctx->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(revealAt, {0, 0}, outer, outer), collection.Get(), &mask))) {
        ctx->PushLayer(D2D1::LayerParameters1(all, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1, mask.Get()), nullptr);
        if (revealBlur) { // glass: the old look frosts over as it fades, the softened frame rising as the sharp one gives way
            float frost = anim::Clamp01(e * 1.6f);
            ctx->DrawBitmap(revealBlur.Get(), all, frost);
            ctx->DrawBitmap(revealSnap.Get(), all, 1 - frost);
        } else {
            ctx->DrawBitmap(revealSnap.Get(), all);
        }
        ctx->PopLayer();
    } else { // no mask: a plain fade still lands softly
        ctx->DrawBitmap(revealSnap.Get(), all, 1 - e);
    }

    // the edge: a faint band of the new accent riding the feather, brightest midway
    Canvas& c = *canvas;
    float band = r + kFeather * 0.5f;
    c.Ring(revealAt, band, shellNow.accent.Alpha(0.10f * shine), kFeather * 0.7f);
    c.Ring(revealAt, band, shellNow.accentHi.Alpha(0.22f * shine), 1.5f);
    switch (revealStyle) {
    case kChrome: { // one glint of polished light, travelling round the edge
        constexpr float kRad = kPi / 180; // the canvas' arcs are in radians
        float from = (-90 + 360 * kGlintTurns * e) * kRad, sweep = kGlintSweep * kRad;
        c.Arc(revealAt, band, from, sweep, Color{1, 1, 1, 0.20f * shine}, 6);
        c.Arc(revealAt, band, from + sweep * 0.25f, sweep * 0.5f, Color{1, 1, 1, 0.75f * shine}, 1.5f);
        break;
    }
    case kStars: // a few stars catching on the edge, each with its own twinkle
        for (uint32_t i = 0; i < (uint32_t)kGlints; ++i) {
            float a = 2 * kPi * Hash(i), tw = 0.5f + 0.5f * std::sin(revealT * 14 + (float)i * 2.1f);
            D2D1_POINT_2F p = {revealAt.x + std::cos(a) * band, revealAt.y + std::sin(a) * band};
            c.Dot(p, 4.5f, shellNow.accentHi.Alpha(0.16f * shine * tw));
            c.Dot(p, 1.4f, Color{1, 1, 1, 0.85f * shine * tw});
        }
        break;
    default:
        break;
    }
    if (revealT >= 1) revealSnap.Reset(), revealBlur.Reset();
    return true;
}
