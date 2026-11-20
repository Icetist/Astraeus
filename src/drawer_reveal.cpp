// A new look pours in from the panel's own edge, the way the panel itself drops: one sheet of liquid glass, its front leading with a soft bulge
// where the pointer was, then levelling out and settling with a last small wobble as it lands. Its lip reads like glass: a fine highlight,
// the new accent glowing just inside, a faint shadow cast on the old look below. Each look adds only a touch: glass frosts the old look as the
// front passes, chrome runs a polished highlight along the lip, starfall lets a few stars glint on it.
// The old look's last frame is kept in memory once, so it outlives a new device (into or out of glass), and is drawn below the front.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "anim.h"
#include "drawer.h"
#include "gfx.h"

namespace {

constexpr float kRevealSec = 0.8f; // the panel's own drop takes about this long
enum Style { kPlain, kGlass, kChrome, kStars };
constexpr float kBulge = 0.32f; // how far the front leads where the pointer was, a share of the panel's depth
constexpr float kBulgeWidth = 0.30f; // and how wide that lead is, a share of its length
constexpr float kWobble = 10, kWobbleHz = 2.2f; // the last small settle as it lands, dips and times a second
constexpr float kStep = 8; // dips between points along the front: smooth at any size
constexpr float kBlurSigma = 12; // glass: how far the old look frosts as the front passes
constexpr int kGlints = 7; // starfall: stars along the lip
constexpr float kPi = 3.14159265f;

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }

float Hash(uint32_t i) { // a steady scatter: the same stars every frame, only their twinkle moves
    uint32_t x = i * 2654435761u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (float)(x & 0xFFFF) / 65535.0f;
}

ComPtr<ID2D1PathGeometry> Path(const std::vector<D2D1_POINT_2F>& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (pts.size() < 2 || FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, (UINT32)(pts.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return path;
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
    float t = revealT, e = EaseInOut(t), secs = t * kRevealSec, shine = std::sin(kPi * t);
    D2D1_SIZE_F sz = revealSnap->GetSize();
    D2D1_RECT_F all = {0, 0, sz.width, sz.height};

    // the panel's own axes: "along" its docked edge and "deep" away from it; the front pours from the docked edge
    bool side = edgeNow != 0;
    float along = side ? sz.height : sz.width, deep = side ? sz.width : sz.height;
    auto at = [&](float a, float d) { // a point given along and deep, on the surface
        if (!side) return D2D1_POINT_2F{a, d};
        return edgeNow == 1 ? D2D1_POINT_2F{d, a} : D2D1_POINT_2F{sz.width - d, a};
    };
    float hit = side ? revealAt.y : revealAt.x; // where the pointer was, along the edge: the front leads there
    float lead = kBulge * deep * (1 - e), wide = kBulgeWidth * along;
    float settle = kWobble * std::sin(2 * kPi * kWobbleHz * secs) * std::pow(1 - t, 2.0f); // a last small wobble, gone as it lands
    float base = -lead + (deep + lead + kWobble * 2) * e;
    std::vector<D2D1_POINT_2F> front;
    for (float a = 0;; a += kStep) {
        float x = (std::min)(a, along), g = (x - hit) / wide;
        front.push_back(at(x, base + lead * std::exp(-g * g) + settle * std::cos(x / along * kPi * 2)));
        if (x >= along) break;
    }

    // the old look, past the front
    std::vector<D2D1_POINT_2F> rest = front;
    rest.push_back(at(along, deep + kWobble * 4));
    rest.push_back(at(0, deep + kWobble * 4));
    if (ComPtr<ID2D1PathGeometry> keep = Path(rest, true)) {
        ctx->PushLayer(D2D1::LayerParameters1(all, keep.Get()), nullptr);
        if (revealBlur) { // glass: the old look frosts as the front comes for it
            float frost = anim::Clamp01(e * 1.4f);
            ctx->DrawBitmap(revealBlur.Get(), all, frost);
            ctx->DrawBitmap(revealSnap.Get(), all, 1 - frost);
        } else {
            ctx->DrawBitmap(revealSnap.Get(), all);
        }
        ctx->PopLayer();
    }

    // the lip, like the edge of a sheet of glass: a shadow cast ahead of it, the accent glowing behind it, a fine highlight on top
    Canvas& c = *canvas;
    auto lip = [&](float off, Color col, float width) {
        std::vector<D2D1_POINT_2F> p = front;
        D2D1_POINT_2F shift = at(0, off), zero = at(0, 0);
        for (auto& q : p) q.x += shift.x - zero.x, q.y += shift.y - zero.y; // along "deep", whichever way that is
        if (ComPtr<ID2D1PathGeometry> g = Path(p, false)) c.StrokeGeo(g.Get(), col, width);
    };
    lip(7, Color{0, 0, 0, 0.16f * shine}, 12); // cast on the old look
    lip(-6, shellNow.accent.Alpha(0.20f * shine), 12); // the new accent, lit from within
    lip(-1, Color{1, 1, 1, (revealStyle == kChrome ? 0.75f : 0.5f) * shine}, 1.4f); // the highlight on the lip
    if (revealStyle == kChrome) { // a polished highlight running along the lip
        float run = std::fmod(t * 1.6f, 1.0f) * along, half = along * 0.08f;
        std::vector<D2D1_POINT_2F> seg;
        for (const auto& q : front) {
            float a = side ? q.y : q.x;
            if (std::fabs(a - run) < half) seg.push_back(q);
        }
        if (ComPtr<ID2D1PathGeometry> g = Path(seg, false)) c.StrokeGeo(g.Get(), Color{1, 1, 1, 0.9f * shine}, 2.5f);
    } else if (revealStyle == kStars) { // a few stars riding the lip
        for (uint32_t i = 0; i < (uint32_t)kGlints && !front.empty(); ++i) {
            const D2D1_POINT_2F& q = front[(size_t)(Hash(i) * (front.size() - 1))];
            float tw = 0.5f + 0.5f * std::sin(t * 16 + (float)i * 2.3f);
            c.Dot(q, 4.5f, shellNow.accentHi.Alpha(0.18f * shine * tw));
            c.Dot(q, 1.4f, Color{1, 1, 1, 0.9f * shine * tw});
        }
    }
    if (revealT >= 1) revealSnap.Reset(), revealBlur.Reset();
    return true;
}
