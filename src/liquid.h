#pragma once
// liquid chrome, the way the site's buttons and star look: bright and dark bands of polished metal that bend and flow, a thin colour at each band's edge.
// made only of direct2d's own effects (noise, a warp, a tone table), so it runs on the gpu with no shader of ours to ship.
// shared with halobar as a copy: port changes by hand
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace liquid {

using Microsoft::WRL::ComPtr;

constexpr float kRes = 0.5f; // the noise is made at half size and scaled up: metal has no fine detail to lose
constexpr float kFreqX = 0.0026f, kFreqY = 0.0095f; // per dip at that size: long bands lying across, like metal poured sideways
constexpr float kWarpFreq = 0.0040f, kWarp = 80; // the slower noise that bends the first, and how far: what makes it liquid, not a pattern sliding by
constexpr float kMargin = 44; // noise made past every edge, at that size: the warp reaches kWarp / 2 in, and must never pull in emptiness
constexpr int kTable = 128; // tone table steps: enough that a band's edge stays smooth
constexpr float kLo = 0.27f, kHi = 0.70f; // where the noise lives, as measured through the premultiplied matrix below
constexpr float kBands = 3.0f; // light-dark bands across that range: a few broad reflections, as the site's metal has
constexpr float kShift = 0.03f; // red runs ahead and blue behind by this much of a band: the thin colour at its edges
constexpr float kDim = 0.36f, kDimLift = 0.03f; // the body under the cards: the same metal, darker, so white words read on it
constexpr float kFlow[2] = {9, 2.5f}; // dips/s the bands drift, before the sways
constexpr float kSwayHz[3] = {0.053f, 0.031f, 0.077f}; // the drifts' slow sways: unrelated, so the motion never repeats visibly

// chrome's reflection along one band: a bright sky, a soft fall, a dark horizon, a ground that lifts back to the sky
inline float Profile(float u) {
    static const float kAt[] = {0.0f, 0.30f, 0.50f, 0.60f, 0.66f, 0.74f, 0.88f, 1.0f};
    static const float kLum[] = {0.98f, 0.90f, 0.70f, 0.14f, 0.06f, 0.20f, 0.66f, 0.98f}; // mostly bright silver; the dark is a thin line, as on polished chrome
    u -= std::floor(u);
    int i = 0;
    while (i < 6 && u > kAt[i + 1]) ++i;
    float k = (u - kAt[i]) / (kAt[i + 1] - kAt[i]);
    k = k * k * (3 - 2 * k); // eased between the marks: no visible kinks
    return kLum[i] + (kLum[i + 1] - kLum[i]) * k;
}

struct Graph {
    ID2D1DeviceContext* dc = nullptr; // what they were made on: another context needs its own
    ComPtr<ID2D1Effect> noise, bend, noiseBack, bendBack, warp, gray, tone, grow, home, cut, dim; // *Back: a noise's offset moves its window, not its pattern: moved home again
    float w = 0, h = 0; // the area the last Set made it for
};

inline bool Build(Graph& g, ID2D1DeviceContext* dc) {
    g = Graph{};
    if (!dc || FAILED(dc->CreateEffect(CLSID_D2D1Turbulence, &g.noise)) || FAILED(dc->CreateEffect(CLSID_D2D1Turbulence, &g.bend)) ||
        FAILED(dc->CreateEffect(CLSID_D2D1DisplacementMap, &g.warp)) || FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, &g.gray)) ||
        FAILED(dc->CreateEffect(CLSID_D2D1TableTransfer, &g.tone)) || FAILED(dc->CreateEffect(CLSID_D2D1Scale, &g.grow)) ||
        FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, &g.dim)) || FAILED(dc->CreateEffect(CLSID_D2D12DAffineTransform, &g.noiseBack)) ||
        FAILED(dc->CreateEffect(CLSID_D2D12DAffineTransform, &g.bendBack)) || FAILED(dc->CreateEffect(CLSID_D2D12DAffineTransform, &g.home)) ||
        FAILED(dc->CreateEffect(CLSID_D2D1Crop, &g.cut)))
        return false;
    g.dc = dc;
    for (ID2D1Effect* e : {g.noise.Get(), g.bend.Get(), g.noiseBack.Get(), g.bendBack.Get(), g.warp.Get(), g.gray.Get(), g.tone.Get()})
        e->SetValue(D2D1_PROPERTY_PRECISION, D2D1_BUFFER_PRECISION_16BPC_FLOAT); // the tone curve is steep: 8-bit noise steps into stairs along every band
    g.noise->SetValue(D2D1_TURBULENCE_PROP_BASE_FREQUENCY, D2D1::Vector2F(kFreqX, kFreqY));
    g.noise->SetValue(D2D1_TURBULENCE_PROP_NUM_OCTAVES, 2u);
    g.noise->SetValue(D2D1_TURBULENCE_PROP_SEED, 7u);
    g.bend->SetValue(D2D1_TURBULENCE_PROP_BASE_FREQUENCY, D2D1::Vector2F(kWarpFreq, kWarpFreq));
    g.bend->SetValue(D2D1_TURBULENCE_PROP_NUM_OCTAVES, 1u);
    g.bend->SetValue(D2D1_TURBULENCE_PROP_SEED, 31u);
    g.noiseBack->SetInputEffect(0, g.noise.Get());
    g.bendBack->SetInputEffect(0, g.bend.Get());
    g.warp->SetInputEffect(0, g.noiseBack.Get());
    g.warp->SetInputEffect(1, g.bendBack.Get());
    g.warp->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, kWarp);
    g.warp->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R);
    g.warp->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G);
    // one noise for every channel, fully opaque: the red channel copied into all three
    D2D1_MATRIX_5X4_F toGray = D2D1::Matrix5x4F(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    g.gray->SetInputEffect(0, g.warp.Get());
    g.gray->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, toGray);
    g.gray->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED); // said outright: kLo and kHi are measured in it
    float r[kTable], gr[kTable], b[kTable];
    for (int i = 0; i < kTable; ++i) {
        float x = (float)i / (kTable - 1);
        float u = ((std::clamp)(x, kLo, kHi) - kLo) / (kHi - kLo) * kBands;
        r[i] = (std::min)(1.0f, Profile(u + kShift) * 0.97f);
        gr[i] = Profile(u) * 0.985f;
        b[i] = (std::min)(1.0f, Profile(u - kShift) * 1.0f + 0.02f); // a cool steel, faintly blue in its lights
    }
    g.tone->SetInputEffect(0, g.gray.Get());
    g.tone->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE, (const BYTE*)r, sizeof(r));
    g.tone->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE, (const BYTE*)gr, sizeof(gr));
    g.tone->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE, (const BYTE*)b, sizeof(b));
    g.tone->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
    g.grow->SetInputEffect(0, g.tone.Get());
    g.grow->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1 / kRes, 1 / kRes));
    g.grow->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_CUBIC); // smooth band edges from the half-size noise
    // the margin cut off inside the chain, so the metal starts at 0,0: a brush's source rect off the origin left d2d computing the wrong part of it
    float m = kMargin / kRes;
    g.home->SetInputEffect(0, g.grow.Get());
    g.home->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Translation(-m, -m));
    g.cut->SetInputEffect(0, g.home.Get());
    D2D1_MATRIX_5X4_F darker = D2D1::Matrix5x4F(kDim, 0, 0, 0, 0, kDim, 0, 0, 0, 0, kDim * 1.04f, 0, 0, 0, 0, 1, kDimLift, kDimLift, kDimLift * 1.3f, 0);
    g.dim->SetInputEffect(0, g.cut.Get());
    g.dim->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, darker);
    return true;
}

// the metal for a w x h dip area at t seconds; t 0 holds it still
inline void Set(Graph& g, float w, float h, float t) {
    D2D1_VECTOR_2F size = D2D1::Vector2F((std::max)(8.0f, w * kRes + 2) + 2 * kMargin, (std::max)(8.0f, h * kRes + 2) + 2 * kMargin);
    float sx = 40 * std::sin(t * kSwayHz[0]) + 18 * std::sin(t * kSwayHz[2] + 1.3f), sy = 24 * std::sin(t * kSwayHz[1] + 0.6f);
    D2D1_VECTOR_2F a = D2D1::Vector2F(t * kFlow[0] + sx, t * kFlow[1] + sy);
    D2D1_VECTOR_2F b = D2D1::Vector2F(-t * kFlow[0] * 0.6f + sy * 1.5f, t * kFlow[1] * 1.7f + sx * 0.8f); // the bend drifts its own way: the bands change shape as they go
    g.noise->SetValue(D2D1_TURBULENCE_PROP_SIZE, size);
    g.noise->SetValue(D2D1_TURBULENCE_PROP_OFFSET, a);
    g.noiseBack->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Translation(-a.x, -a.y));
    g.bend->SetValue(D2D1_TURBULENCE_PROP_SIZE, size);
    g.bend->SetValue(D2D1_TURBULENCE_PROP_OFFSET, b);
    g.bendBack->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Translation(-b.x, -b.y));
    if (w != g.w || h != g.h) g.w = w, g.h = h, g.cut->SetValue(D2D1_CROP_PROP_RECT, D2D1::Vector4F(0, 0, w, h));
}

inline ID2D1Image* Bright(Graph& g) { // full chrome: rims and small things
    ID2D1Image* out = nullptr;
    g.cut->GetOutput(&out);
    if (out) out->Release(); // the effect keeps it alive
    return out;
}

inline ID2D1Image* Dim(Graph& g) { // a panel's body: darker, so what's written on it reads
    ID2D1Image* out = nullptr;
    g.dim->GetOutput(&out);
    if (out) out->Release();
    return out;
}

// a brush of that metal laid over r (the area Set was given); null if it can't be made
inline ComPtr<ID2D1ImageBrush> Brush(ID2D1DeviceContext* dc, ID2D1Image* img, const D2D1_RECT_F& r, float opacity) {
    ComPtr<ID2D1ImageBrush> b;
    if (!img) return b;
    D2D1_RECT_F src = {0, 0, r.right - r.left, r.bottom - r.top};
    dc->CreateImageBrush(img, D2D1::ImageBrushProperties(src), D2D1::BrushProperties(opacity, D2D1::Matrix3x2F::Translation(r.left, r.top)), &b);
    return b;
}

}
