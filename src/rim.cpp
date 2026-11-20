// The edge glow, shared by the fullscreen player and the media page. See rim.h.
// Cheap by design: the inward falloff is baked once; each edge is one gradient along it, drawn in thin slices of their own depth.
#include "rim.h"

#include <d2d1_1helper.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kFadePx = 128; // the baked falloff's length; it's stretched to each slice's depth
constexpr int kStops = 40; // colors along each edge: enough that the light flows smooth
constexpr float kSlice = 10; // dips: the glow's depth is set this often along the edge
constexpr float kRest = 0.55f, kLoud = 0.3f; // its depth at rest, and what the song's loudness adds, in sizes
constexpr int kLow = 12, kMid = 30; // the waveform's bars: the low end below this, the middle below the next, the top above
constexpr float kCycles[EdgeGlow::kSwells] = {2, 3, 5}; // swells round the whole edge: few and broad for the low end, more for the top
constexpr float kSwell[EdgeGlow::kSwells] = {0.9f, 0.6f, 0.45f}; // how far each can push the light in, in sizes
constexpr float kFlow[EdgeGlow::kSwells] = {0.045f, 0.065f, 0.085f}; // turns a second each drifts with the music barely there
constexpr float kRush = 1.5f; // how much quicker its part of the music drives it, times the strength slider. kept small: a slow song is just as busy for its loudness
constexpr float kKick = 16; // and each beat throws it on, fading as the beat does: the low end goes about an eighth of a lap a beat, so the tempo sets the pace. a dense beat races, a slow one drifts
constexpr float kTurn = 0.06f; // turns a second the two colors flow round, before the music hurries them
constexpr float kPeak = 3; // sharpens each swell, so they read as swells and not a ripple
constexpr float kNextHue = 50; // a cover with one color: its second is this many degrees round
constexpr float kTwoRate = 3.75f; // a new song's second color eases in over about a second, as the room's own colors do
constexpr float kRise = 34, kFall = 7; // the level follows the music up quickly and down more slowly
constexpr float kAverage = 2.5f; // the running average a beat stands out from
constexpr float kBeatOver = 1.2f, kBeatFloor = 0.035f, kBeatGap = 0.2f; // a beat: this far over the average, and not sooner than this after the last
constexpr float kBeatDecay = 6; // the beat's pulse fades this fast

float Clamp01(float v) { return (std::min)(1.0f, (std::max)(0.0f, v)); }

float Band(const Waveform& w, int from, int to) {
    float sum = 0;
    for (int i = from; i < to; ++i) sum += w.level[i];
    return sum / (to - from);
}

}

Color rim::Second(const Seed& base, bool light) {
    Seed two = base;
    two.hue = base.hue2 >= 0 ? base.hue2 : std::fmod(base.hue + kNextHue, 360.0f);
    Theme t = Theme::From(two);
    return light ? t.accent : t.accentHi; // a light surface keeps the deeper color, or the light washes out
}

bool EdgeGlow::Step(const Waveform& w, bool shaped, float level, bool playing, float gain, float dt, bool calm) {
    clock += dt;
    if (!playing) level = 0;
    loud += (level - loud) * (std::min)(1.0f, dt * (level > loud ? kRise : kFall));
    slow += (loud - slow) * (std::min)(1.0f, dt * kAverage);
    if (playing && loud > slow * kBeatOver + kBeatFloor && clock - lastBeat > kBeatGap) beat = 1, lastBeat = clock;
    beat *= std::exp(-dt * kBeatDecay);
    if (!shaped && loud < 0.01f) return false; // silent: no light, no work
    if (calm) return true; // reduced motion: the light still swells, it just doesn't travel
    float energy[kSwells] = {Band(w, 0, kLow), Band(w, kLow, kMid), Band(w, kMid, Waveform::kBars)};
    for (int j = 0; j < kSwells; ++j) run[j] = std::fmod(run[j] + dt * kFlow[j] * (1 + gain * (kRush * energy[j] + kKick * beat)), 1.0f);
    turn = std::fmod(turn + dt * kTurn * (1 + gain * (loud + 4 * beat)), 1.0f);
    return true;
}

void EdgeGlow::Draw(ID2D1DeviceContext* ctx, const Waveform& w, const D2D1_RECT_F& box, float radius, float size, Color one, Color twoWant, bool light, float dt) {
    if (!fade) { // white, fading inward from the edge: tinted by whatever brush it's drawn with
        std::vector<uint32_t> px(4 * kFadePx);
        for (int y = 0; y < kFadePx; ++y) {
            float t = y / (float)(kFadePx - 1), a = std::pow(1 - t, 1.5f) * (0.62f + 0.38f * std::exp(-6 * t)); // brightest at the edge, but most of the light is the body falling softly inward
            uint32_t v = (uint32_t)std::lround(a * 255);
            for (int x = 0; x < 4; ++x) px[y * 4 + x] = v << 24 | v << 16 | v << 8 | v; // premultiplied white
        }
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ctx->CreateBitmap({4, (UINT32)kFadePx}, px.data(), 16, bp, &fade);
        if (!fade) return;
    }
    if (!twoSet) two = twoWant, twoSet = true;
    two = two.Mix(twoWant, (std::min)(1.0f, dt * kTwoRate));
    float energy[kSwells] = {Band(w, 0, kLow), Band(w, kLow, kMid), Band(w, kMid, Waveform::kBars)};
    float W = box.right - box.left, H = box.bottom - box.top, P = 2 * (W + H);
    auto thick = [&](float s) { // how far in the light reaches at s, in sizes
        float f = s / P, v = kRest + kLoud * loud;
        for (int j = 0; j < kSwells; ++j) {
            float c = 0.5f + 0.5f * std::cos(2 * kPi * kCycles[j] * (f - run[j]));
            v += kSwell[j] * energy[j] * std::pow(c, kPeak);
        }
        return v;
    };
    auto color = [&](float s, float v) { // thicker is brighter too: the swell is more light, not just more reach
        float m = 0.5f + 0.5f * std::cos(2 * kPi * (s / P - turn));
        return one.Mix(two, m).Alpha(Clamp01(0.5f + 0.45f * v));
    };

    float l = box.left, t = box.top, r = box.right, b = box.bottom;
    struct Edge {
        float len, from; // its length, and where it starts round the edge, from the bottom left corner
        D2D1::Matrix3x2F m; // from along-and-inward to the surface
    } edges[4] = {
        {W, 0, D2D1::Matrix3x2F(1, 0, 0, -1, l, b)}, // the bottom, left to right
        {H, W, D2D1::Matrix3x2F(0, -1, -1, 0, r, b)}, // the right, bottom to top
        {W, W + H, D2D1::Matrix3x2F(-1, 0, 0, 1, r, t)}, // the top, right to left
        {H, 2 * W + H, D2D1::Matrix3x2F(0, 1, 1, 0, l, t)}, // the left, top to bottom
    };
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RoundedRectangleGeometry> round;
    if (radius > 0) { // the light stays inside the rounded corners
        ctx->GetFactory(&factory);
        if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, radius, radius), &round);
        if (round) ctx->PushLayer(D2D1::LayerParameters(box, round.Get()), nullptr);
    }
    D2D1::Matrix3x2F was;
    ctx->GetTransform(&was);
    D2D1_ANTIALIAS_MODE aa = ctx->GetAntialiasMode();
    D2D1_PRIMITIVE_BLEND blend = ctx->GetPrimitiveBlend();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // the mask needs it; every slice is square to the edge anyway
    if (!light) ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD); // light adds to what's under it, so it glows instead of laying paint. a light surface has nothing to add to
    D2D1_RECT_F src = {0, 0, 4, (float)kFadePx};
    for (const Edge& e : edges) {
        D2D1_GRADIENT_STOP stops[kStops];
        for (int j = 0; j < kStops; ++j) {
            float s = e.from + e.len * j / (kStops - 1);
            stops[j] = {j / (float)(kStops - 1), color(s, thick(s)).D2D()};
        }
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> brush;
        if (FAILED(ctx->CreateGradientStopCollection(stops, kStops, &coll)) ||
            FAILED(ctx->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {e.len, 0}), coll.Get(), &brush)))
            continue;
        ctx->SetTransform(e.m * was);
        int n = (std::max)(1, (int)std::ceil(e.len / kSlice));
        for (int i = 0; i < n; ++i) {
            float u0 = e.len * i / n, u1 = e.len * (i + 1) / n;
            D2D1_RECT_F slice = {u0, 0, u1, size * thick(e.from + (u0 + u1) / 2)};
            ctx->FillOpacityMask(fade.Get(), brush.Get(), &slice, &src);
        }
    }
    ctx->SetTransform(was);
    ctx->SetAntialiasMode(aa);
    ctx->SetPrimitiveBlend(blend);
    if (round) ctx->PopLayer();
}
