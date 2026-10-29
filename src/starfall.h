#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gfx.h"
#include "theme.h"

// the starfall look's sky, kept quiet: near-black indigo, one soft violet bloom, sparse star dust, a faint light along one edge.
// shared by the island and the bar. the field never moves; the few slow breaths ride frames that are drawn anyway, never ask for one
namespace starfall {

constexpr float kStarArea = 6000; // dips² per mote of dust: sparse
constexpr int kMaxStars = 60;
constexpr int kTwinklers = 11; // every n-th mote breathes, barely
constexpr float kTwinkleHz = 0.2f;
constexpr float kHeroMinH = 60; // a pill is too thin for the one brighter star
constexpr float kBloomX = 0.68f, kBloomA = 0.34f; // the bloom's place across the sky, its strength
constexpr float kEdgeGlow = 40, kEdgeA = 0.10f; // the lit edge's reach and strength
constexpr float kRimA = 0.30f, kSeamA = 0.16f; // hairlines: the outline, dividers
constexpr float kHaloA = 0.16f; // the violet reach around an active page's glow
constexpr float kDriftX[2] = {0.071f, 0.173f}, kDriftY[2] = {0.059f, 0.137f}; // rad/s of the orb's two sways per axis: unrelated, so the path never repeats visibly
constexpr size_t kMaxBrushes = 24; // a crossfade walks through colours: start over past this
constexpr float kPi = 3.14159265f;
const Color kDust{0.90f, 0.93f, 1, 1}; // a touch cool, like real starlight

inline float Hash(uint32_t n) { // 0..1, stable per index: the field never moves
    n = (n ^ 61) ^ (n >> 16);
    n *= 9;
    n ^= n >> 4;
    n *= 0x27d4eb2d;
    n ^= n >> 15;
    return (n & 0xFFFFFF) / 16777216.0f;
}

inline float HueMix(float a, float b, float t) { return a + (std::fmod(b - a + 540.0f, 360.0f) - 180) * t; } // the short way round the wheel

struct Palette {
    Color night; // the deep wash
    Color bloom; // the one soft cloud of colour, a muted violet
    Color edge; // the light on the edge and the hairlines, faintly violet
};

inline Palette Colors(const Theme& th) {
    Seed s = theme::FromCol(th.accent);
    float lean = (std::min)(1.0f, s.chroma * 2); // a grey accent has no hue worth leaning toward
    Palette p;
    p.night = theme::Hsl(HueMix(236, s.hue, 0.2f * lean), 0.50f, 0.035f);
    p.bloom = theme::Hsl(HueMix(268, s.hue, 0.3f * lean), 0.46f, 0.40f); // leans violet whatever the accent
    p.edge = theme::Hsl(HueMix(248, s.hue, 0.3f * lean), 0.40f, 0.80f);
    return p;
}

// gradient brushes, kept per device: the canvas only caches its own kinds
struct Cache {
    ID2D1RenderTarget* rt = nullptr;
    std::vector<std::pair<uint32_t, ComPtr<ID2D1RadialGradientBrush>>> blobs;
    std::vector<std::pair<uint32_t, ComPtr<ID2D1LinearGradientBrush>>> fades;
};

inline Cache& Store() {
    static Cache c;
    return c;
}

inline void Forget() { Store() = Cache{}; } // with the device: the brushes would pin it

inline uint32_t Key(Color c) {
    auto b = [](float v) { return (uint32_t)((std::clamp)(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return b(c.r) << 16 | b(c.g) << 8 | b(c.b);
}

inline Cache& For(ID2D1RenderTarget* rt) {
    Cache& c = Store();
    if (c.rt != rt) c = Cache{}, c.rt = rt; // another target: nothing made for the old one is any use
    return c;
}

template <class B, class Make>
B* Find(std::vector<std::pair<uint32_t, ComPtr<B>>>& list, uint32_t key, Make make) {
    for (auto& e : list)
        if (e.first == key) return e.second.Get();
    if (list.size() >= kMaxBrushes) list.clear();
    ComPtr<B> made = make();
    if (!made) return nullptr;
    list.emplace_back(key, made);
    return made.Get();
}

inline ID2D1GradientStopCollection* Stops(ID2D1RenderTarget* rt, Color c, const float (&at)[4], const float (&a)[4], ComPtr<ID2D1GradientStopCollection>& out) {
    D2D1_GRADIENT_STOP s[4];
    for (int i = 0; i < 4; ++i) s[i] = {at[i], Color{c.r, c.g, c.b, a[i]}.D2D()};
    return SUCCEEDED(rt->CreateGradientStopCollection(s, 4, &out)) ? out.Get() : nullptr;
}

inline ID2D1RadialGradientBrush* Blob(ID2D1RenderTarget* rt, Color c) { // soft falloff, clear at its rim so a fill past it shows nothing
    return Find(For(rt).blobs, Key(c), [&] {
        ComPtr<ID2D1GradientStopCollection> st;
        ComPtr<ID2D1RadialGradientBrush> b;
        if (Stops(rt, c, {0, 0.35f, 0.7f, 1}, {1, 0.55f, 0.15f, 0}, st)) rt->CreateRadialGradientBrush({{0, 0}, {0, 0}, 1, 1}, st.Get(), &b);
        return b;
    });
}

inline ID2D1LinearGradientBrush* Fade(ID2D1RenderTarget* rt, Color c) { // full at its start point, gone at its end
    return Find(For(rt).fades, Key(c), [&] {
        ComPtr<ID2D1GradientStopCollection> st;
        ComPtr<ID2D1LinearGradientBrush> b;
        if (Stops(rt, c, {0, 0.3f, 0.65f, 1}, {1, 0.45f, 0.12f, 0}, st)) rt->CreateLinearGradientBrush({{0, 0}, {0, 1}}, st.Get(), &b);
        return b;
    });
}

// a soft cloud of colour inside r (or geo), clipped to its own box so it costs only what it covers
inline void Cloud(Canvas& c, ID2D1Geometry* geo, const D2D1_RECT_F& r, float radius, D2D1_POINT_2F at, float rx, float ry, Color col) {
    if (col.a < 0.004f || rx < 1 || ry < 1) return;
    ID2D1RenderTarget* rt = c.Target();
    ID2D1RadialGradientBrush* b = Blob(rt, col);
    if (!b) return;
    D2D1_RECT_F box = {(std::max)(at.x - rx, r.left), (std::max)(at.y - ry, r.top), (std::min)(at.x + rx, r.right), (std::min)(at.y + ry, r.bottom)};
    if (box.right <= box.left || box.bottom <= box.top) return;
    b->SetCenter(at);
    b->SetRadiusX(rx);
    b->SetRadiusY(ry);
    b->SetOpacity(col.a);
    rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED); // the brush is clear at the box edge: a hard scissor never shows
    if (geo) rt->FillGeometry(geo, b);
    else rt->FillRoundedRectangle({r, radius, radius}, b);
    rt->PopAxisAlignedClip();
}

inline void Streak(Canvas& c, D2D1_POINT_2F from, D2D1_POINT_2F to, Color col, float width) { // bright at from, tapering out by to
    if (col.a < 0.004f) return;
    ID2D1RenderTarget* rt = c.Target();
    ID2D1LinearGradientBrush* b = Fade(rt, col);
    if (!b) return;
    b->SetStartPoint(from);
    b->SetEndPoint(to);
    b->SetOpacity(col.a);
    rt->DrawLine(from, to, b, width);
}

// the outline lit from the edge's side, fading out toward the other
inline void Rim(Canvas& c, ID2D1Geometry* geo, const D2D1_RECT_F& r, float radius, Color col, int edge, float width) {
    if (col.a < 0.004f) return;
    ID2D1RenderTarget* rt = c.Target();
    ID2D1LinearGradientBrush* b = Fade(rt, col);
    if (!b) return;
    b->SetStartPoint({r.left, edge ? r.top : r.bottom});
    b->SetEndPoint({r.left, edge ? r.bottom : r.top});
    b->SetOpacity(col.a);
    if (geo) rt->DrawGeometry(geo, b, width);
    else rt->DrawRoundedRectangle({Inset(r, width / 2, width / 2), (std::max)(0.0f, radius - width / 2), (std::max)(0.0f, radius - width / 2)}, b, width);
}

// where the drifting orb is at t, as fractions of the sky: two slow sways per axis, turning back softly near the edges (~15-30 dips/s on a panel)
inline D2D1_POINT_2F Drift(float t) {
    return {0.5f + 0.30f * std::sin(t * kDriftX[0]) + 0.12f * std::sin(t * kDriftX[1] + 1.3f), 0.5f + 0.28f * std::sin(t * kDriftY[0] + 0.7f) + 0.12f * std::sin(t * kDriftY[1] + 2.1f)};
}

// r: where the sky shows, rounded by radius or cut to geo; edge: 0 its glow along the bottom (a panel hanging down), 1 along the top;
// t: seconds, 0 holds everything still; drifting: the bloom wanders the sky with t, the caller keeps frames coming for it.
// restraint over spectacle: near-black indigo, one soft bloom, star dust, a whisper of light on the edge
inline void Sky(Canvas& c, const D2D1_RECT_F& r, float radius, const Theme& th, float t, float alpha, int edge = 0, ID2D1Geometry* geo = nullptr, float density = 1,
                bool drifting = false) {
    float w = r.right - r.left, h = r.bottom - r.top;
    if (w < 4 || h < 4 || alpha < 0.01f) return;
    Palette p = Colors(th);
    bool thin = h < kHeroMinH; // a pill or a bar: its ink sits over every part of it
    float away = edge ? r.bottom : r.top, dir = edge ? 1.0f : -1.0f; // dir: from the lit edge inward

    // depth: a near-black indigo wash, a shade deeper away from the lit edge
    Color top = p.night.Alpha((edge ? 0.45f : 0.60f) * alpha), bot = p.night.Alpha((edge ? 0.60f : 0.45f) * alpha);
    if (geo) c.FillGeoV(geo, r, top, bot);
    else c.FillV(r, radius, top, bot);

    // one bloom, soft, off to one side; or an orb of light drifting slowly round the whole sky
    D2D1_POINT_2F at = {r.left + w * kBloomX, thin ? (r.top + r.bottom) / 2 : away - dir * h * 0.3f};
    if (drifting && t > 0) {
        D2D1_POINT_2F f = Drift(t);
        at.x = r.left + w * f.x;
        if (!thin) at.y = r.top + h * f.y; // a pill or a bar is too short to wander up and down: the orb slides along it
    }
    Cloud(c, geo, r, radius, at, w * (thin ? 0.42f : 0.5f), thin ? h * 1.4f : h * 0.6f, p.bloom.Alpha(kBloomA * alpha));

    // the edge: barely there, a gradient of light fading inward
    float gh = (std::min)(h * (thin ? 0.5f : 0.3f), kEdgeGlow);
    D2D1_RECT_F band = edge ? D2D1_RECT_F{r.left, r.top, r.right, r.top + gh} : D2D1_RECT_F{r.left, r.bottom - gh, r.right, r.bottom};
    Color lit = p.edge.Alpha(kEdgeA * alpha), none = p.edge.Alpha(0);
    if (geo) c.FillGeoV(geo, band, edge ? lit : none, edge ? none : lit);
    else c.FillV(band, radius, edge ? lit : none, edge ? none : lit);

    // dust: sparse, small, dim; a slow breath on a few, only on frames drawn anyway
    float in = (std::max)(4.0f, radius * 0.7f); // kept off rounded corners
    int n = (std::min)(kMaxStars, (int)(w * h * density / kStarArea) + 2);
    for (int i = 0; i < n; ++i) {
        float x = r.left + in + Hash((uint32_t)i * 3 + 1) * (w - 2 * in), y = r.top + 4 + Hash((uint32_t)i * 3 + 2) * (h - 8), s = Hash((uint32_t)i * 3 + 3);
        float a = 0.09f + 0.32f * s * s * s; // nearly all of it barely there
        if (i % kTwinklers == 0 && t > 0) a *= 0.8f + 0.2f * std::sin(t * 2 * kPi * kTwinkleHz + s * 40);
        c.Dot({x, y}, 0.35f + 0.35f * s * s, kDust.Alpha(a * alpha));
    }
    if (!thin) { // a single brighter star: the one detail the eye lands on
        D2D1_POINT_2F s = {r.left + w * (0.18f + 0.2f * Hash(701)), away - dir * h * (0.16f + 0.12f * Hash(809))};
        Cloud(c, nullptr, {s.x - 6, s.y - 6, s.x + 6, s.y + 6}, 0, s, 5, 5, p.edge.Alpha(0.10f * alpha));
        c.Dot(s, 0.9f, kDust.Alpha(0.6f * alpha));
    }

    Rim(c, geo, r, radius, p.edge.Alpha(kRimA * alpha), edge, 1.0f); // a fine line of the same light along the outline
}

// a quiet divider: a hairline that fades out at both ends
inline void Seam(Canvas& c, D2D1_POINT_2F a, D2D1_POINT_2F b, const Theme& th, float alpha = 1) {
    D2D1_POINT_2F mid = {(a.x + b.x) / 2, (a.y + b.y) / 2};
    Color ink = Colors(th).edge.Alpha(kSeamA * alpha);
    Streak(c, mid, a, ink, 1);
    Streak(c, mid, b, ink, 1);
}

// the active page's glow: the theme's accent glow inside a faint violet reach
inline void Halo(Canvas& c, D2D1_POINT_2F at, float rx, float ry, const Theme& th, float alpha = 1) {
    c.Glow(at, rx * 1.25f, ry * 1.2f, Colors(th).bloom.Alpha(kHaloA * alpha));
    c.Glow(at, rx, ry, th.glow.Alpha(alpha));
}

}
