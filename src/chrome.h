#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gfx.h"
#include "liquid.h"
#include "starfall.h"
#include "theme.h"

// the chrome look's metal: liquid chrome, as on the site. its body is the flowing metal darkened so words read on it; a ring of it at full
// brightness runs inside the edge; a still band of light lies along the top. drawn in passes like starfall's sky: the still parts bake into the
// caller's layer, the metal flows only while the panel is open (the caller keeps ~20 fps for it), and everything else holds it still at t 0
namespace chrome {

constexpr float kHue = 222; // a cool steel, faintly blue like real chrome under daylight
constexpr float kSat = 0.07f;
constexpr float kRimW = 5, kGlowW = 18, kGlowA = 0.30f; // the bright ring inside the edge (half of each width shows), and its softer spill inward
constexpr float kRimOfH = 0.08f, kGlowOfH = 0.22f; // on a thin pill or dock the ring keeps to this share of its height: words stay on the darker body
constexpr float kBandH = 34, kBandA = 0.14f; // the still specular band along the top: its reach and strength
constexpr float kRimLowA = 0.10f; // a faint outline over everything, so the edge never vanishes into a dark band
constexpr float kSeamA = 0.22f; // dividers
constexpr float kPillMinH = 60; // shorter than this it's a pill
constexpr size_t kMaxGraphs = 8; // one per size being drawn: the panel, the pill, swatches; more and the oldest go
const Color kWhite{1, 1, 1, 1};

inline Color Steel(float l, float a = 1) { return theme::Hsl(kHue, kSat, l, a); }

// effect graphs per device, one per size drawn, so drawing a swatch never re-sizes the panel's metal in the same frame
struct Cache {
    ID2D1DeviceContext* dc = nullptr;
    std::vector<std::pair<uint64_t, liquid::Graph>> graphs;
};

inline Cache& Store() {
    static Cache c;
    return c;
}

inline void Forget() { Store() = Cache{}; } // with the device: the effects would pin it

inline ID2D1DeviceContext* Dc(Canvas& c) { // the canvas' target as a device context: only those run effects
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(c.Target()->QueryInterface(IID_PPV_ARGS(&dc)))) return nullptr;
    return dc.Get(); // the canvas holds the target: borrowed, not kept
}

// the liquid for a w x h area at t, ready to draw; null without a device context or effects
inline liquid::Graph* Liquid(Canvas& c, float w, float h, float t) {
    ID2D1DeviceContext* dc = Dc(c);
    if (!dc || w < 1 || h < 1) return nullptr;
    Cache& cache = Store();
    if (cache.dc != dc) cache = Cache{}, cache.dc = dc; // another target: nothing made for the old one is any use
    uint64_t key = (uint64_t)std::lround(w * 4) << 32 | (uint64_t)std::lround(h * 4);
    auto& list = cache.graphs; // least recently used first
    auto it = std::find_if(list.begin(), list.end(), [key](const auto& e) { return e.first == key; });
    if (it != list.end()) {
        std::rotate(it, it + 1, list.end()); // used now: to the back
    } else if (list.size() >= kMaxGraphs) { // a size not seen, as a panel springs open: the oldest graph takes it, a resize is only a property
        std::rotate(list.begin(), list.begin() + 1, list.end());
        list.back().first = key;
    } else {
        liquid::Graph made;
        if (!liquid::Build(made, dc)) return nullptr;
        list.emplace_back(key, std::move(made));
    }
    liquid::Graph* g = &list.back().second;
    liquid::Set(*g, w, h, t);
    return g;
}

// the body: the flowing metal, darkened so what's written on it reads
inline void MetalUnder(Canvas& c, const D2D1_RECT_F& r, float radius, float t, float alpha, ID2D1Geometry* geo = nullptr) {
    float w = r.right - r.left, h = r.bottom - r.top;
    liquid::Graph* g = Liquid(c, w, h, t);
    if (!g) { // no effects here: plain steel, still a body
        if (geo) c.FillGeoV(geo, r, Steel(0.20f, alpha), Steel(0.07f, alpha), true);
        else c.FillV(r, radius, Steel(0.20f, alpha), Steel(0.07f, alpha));
        return;
    }
    ComPtr<ID2D1ImageBrush> b = liquid::Brush(g->dc, liquid::Dim(*g), r, alpha);
    if (!b) return;
    if (geo) c.FillGeo(geo, b.Get(), true); // the panel's shape is kept frame to frame: its tessellation is cached
    else c.Target()->FillRoundedRectangle({r, radius, radius}, b.Get());
}

// the bright ring of the same metal inside the edge: it flows with the body, so it's drawn with it, not baked
inline void MetalShine(Canvas& c, const D2D1_RECT_F& r, float radius, float t, float alpha, ID2D1Geometry* geo = nullptr) {
    float w = r.right - r.left, h = r.bottom - r.top;
    liquid::Graph* g = Liquid(c, w, h, t);
    if (!g) return;
    ID2D1Image* bright = liquid::Bright(*g);
    ComPtr<ID2D1ImageBrush> rim = liquid::Brush(g->dc, bright, r, alpha), glow = liquid::Brush(g->dc, bright, r, kGlowA * alpha);
    if (!rim || !glow) return;
    ID2D1DeviceContext* dc = g->dc;
    float side = (std::min)(w, h), rimW = (std::min)(kRimW, side * kRimOfH * 2), glowW = (std::min)(kGlowW, side * kGlowOfH * 2); // doubled: half of a stroke shows
    if (geo) { // stroked on the outline, clipped to the shape: only the inner half shows, never past the silhouette
        dc->PushLayer(D2D1::LayerParameters1(r, geo), nullptr);
        dc->DrawGeometry(geo, glow.Get(), glowW);
        dc->DrawGeometry(geo, rim.Get(), rimW);
        dc->PopLayer();
        return;
    }
    float in = (std::min)(rimW / 2, side / 4); // a rounded rect: inset by half the width instead, the same inner ring
    D2D1_RECT_F q = {r.left + in, r.top + in, r.right - in, r.bottom - in};
    float qr = (std::max)(0.0f, radius - in);
    dc->DrawRoundedRectangle({q, qr, qr}, rim.Get(), in * 2);
}

// the still parts over the metal: the light band along the top, and a faint outline
inline void MetalOver(Canvas& c, const D2D1_RECT_F& r, float radius, float alpha, ID2D1Geometry* geo = nullptr) {
    float h = r.bottom - r.top;
    float bh = (std::min)(h * (h < kPillMinH ? 0.5f : 0.3f), kBandH);
    D2D1_RECT_F band = {r.left, r.top, r.right, r.top + bh};
    Color lit = kWhite.Alpha(kBandA * alpha), none = kWhite.Alpha(0);
    if (geo) c.FillGeoV(geo, band, lit, none, true);
    else c.FillV(band, radius, lit, none);
    if (geo) c.StrokeGeo(geo, kWhite.Alpha(kRimLowA * alpha), 1.0f, true);
    else c.Stroke(r, radius, kWhite.Alpha(kRimLowA * alpha), 1.0f);
}

// r: where the metal shows, rounded by radius or cut to geo; t: seconds, 0 holds it still
inline void Metal(Canvas& c, const D2D1_RECT_F& r, float radius, float t, float alpha, ID2D1Geometry* geo = nullptr) {
    if (r.right - r.left < 4 || r.bottom - r.top < 4 || alpha < 0.01f) return;
    MetalUnder(c, r, radius, t, alpha, geo);
    MetalShine(c, r, radius, t, alpha, geo);
    MetalOver(c, r, radius, alpha, geo);
}

// a divider: a bright hairline that fades out at both ends, like an engraved line catching light
inline void Seam(Canvas& c, D2D1_POINT_2F a, D2D1_POINT_2F b, float alpha = 1) {
    D2D1_POINT_2F mid = {(a.x + b.x) / 2, (a.y + b.y) / 2};
    Color ink = kWhite.Alpha(kSeamA * alpha);
    starfall::Streak(c, mid, a, ink, 1);
    starfall::Streak(c, mid, b, ink, 1);
}

// the active page: the accent's glow inside a cool white sheen
inline void Halo(Canvas& c, D2D1_POINT_2F at, float rx, float ry, const Theme& th, float alpha = 1) {
    c.Glow(at, rx * 1.2f, ry * 1.15f, kWhite.Alpha(0.07f * alpha));
    c.Glow(at, rx, ry, th.glow.Alpha(alpha));
}

}
