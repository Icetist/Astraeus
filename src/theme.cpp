#include "theme.h"

#include <cmath>
#include <vector>

namespace {

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float Lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

int Rgb(Color c) {
    auto ch = [](float v) { return (int)(Clamp(v, 0, 1) * 255 + 0.5f); };
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

void ToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = (std::max)(r, (std::max)(g, b)), mn = (std::min)(r, (std::min)(g, b));
    float d = mx - mn;
    l = (mx + mn) / 2;
    s = d < 1e-5f ? 0 : d / (1 - std::fabs(2 * l - 1)); // grey: no saturation, avoid div by 0
    if (d < 1e-5f) h = 0;
    else if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0f); // hue sector from dominant channel
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}

}

namespace theme {

Color Hsl(float h, float s, float l, float a) {
    s = Clamp(s, 0, 1);
    float c = (1 - std::fabs(2 * l - 1)) * s; // chroma from lightness and sat
    float hp = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60; // wrap hue, split into 6 sectors
    float x = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } // standard hsl -> rgb sector table
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    float m = l - c / 2; // lift all channels to match lightness
    return {Clamp(r + m, 0, 1), Clamp(g + m, 0, 1), Clamp(b + m, 0, 1), a};
}

Seed FromCol(Color c) {
    float h, s, l;
    ToHsl(c.r, c.g, c.b, h, s, l);
    Seed seed{h, s};
    seed.rgb = Rgb(c);
    return seed;
}

float Luma(Color c) { return 0.2126f * Lin(c.r) + 0.7152f * Lin(c.g) + 0.0722f * Lin(c.b); }

Theme Lift(const Theme& t) {
    Theme v = t;
    v.accent = t.accent.Mix(t.text, 0.45f); // pale and tinted on dark looks, deep on light ones: away from the glow either way
    v.accentHi = t.accentHi.Mix(t.text, 0.55f);
    v.onAccent = Luma(v.accent) > 0.30f ? Color{0.07f, 0.07f, 0.08f, 1} : Color{0.98f, 0.98f, 0.98f, 1};
    v.glow = v.accent.Alpha(0.16f);
    return v;
}

Seed FromBmp(IWICBitmapSource* source) {
    const UINT side = 48; // 48x48 sample is plenty for dominant hue
    const int bins = 24; // 15 degree hue bins
    Seed fallback;
    if (!source) return fallback;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(gfx::Wic()->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(source, side, side, WICBitmapInterpolationModeLinear))) // ignores aspect, fine for stats
        return fallback;
    std::vector<BYTE> px(side * side * 4);
    if (FAILED(scaler->CopyPixels(nullptr, side * 4, (UINT)px.size(), px.data()))) return fallback;

    float weight[bins] = {}, sn[bins] = {}, cs[bins] = {}, sat[bins] = {}; // hue histogram + circular mean parts
    float total = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 3] < 128) continue;
        float a = px[i + 3] / 255.0f;
        float h, s, l;
        ToHsl(px[i + 2] / 255.0f / a, px[i + 1] / 255.0f / a, px[i] / 255.0f / a, h, s, l); // unpremultiply, bgr order
        if (l < 0.08f || l > 0.94f) continue; // near black/white carry no hue
        float w = s * (1 - std::fabs(2 * l - 1)); // weight by visible chroma
        if (w < 0.05f) continue;
        int b = (int)(h / (360.0f / bins)) % bins;
        float rad = h * 3.14159265f / 180;
        weight[b] += w;
        sn[b] += std::sin(rad) * w; // sum vectors so hue avg wraps correctly
        cs[b] += std::cos(rad) * w;
        sat[b] += s * w;
        total += w;
    }
    if (total / (side * side) < 0.02f) return {fallback.hue, 0.05f}; // near greyscale wallpaper: default hue, almost no chroma

    int best = 0;
    float bestS = -1;
    for (int b = 0; b < bins; ++b) {
        float score = weight[b] + 0.5f * (weight[(b + bins - 1) % bins] + weight[(b + 1) % bins]); // neighbors count half, smooths bin edges
        if (score > bestS) { bestS = score; best = b; } // pick strongest bin
    }
    float hue = std::atan2(sn[best], cs[best]) * 180 / 3.14159265f; // circular mean within winning bin
    if (hue < 0) hue += 360;
    return {hue, sat[best] / weight[best]}; // weighted avg saturation
}

std::wstring WallPath() {
    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0)) return L""; // empty when solid color bg
    return path;
}

}

Theme Theme::From(Seed seed) {
    using theme::Hsl;
    float h = seed.hue;
    float k = seed.chroma < 0.08f ? 0.0f : Clamp(seed.chroma * 1.4f, 0.12f, 1.0f); // boost dull seeds, keep a hint of tint; colorless stays neutral grey
    Theme t;
    if (seed.look == 1) { // same roles, lightness ramps flipped: dark ink on pale tinted surfaces
        t.accent = Hsl(h, (std::max)(0.55f, 0.80f * k), 0.47f); // deep enough to read on pale cards and carry white ink
        t.accentHi = Hsl(h, (std::max)(0.65f, 0.90f * k), 0.38f); // "hi" = more emphasis, so darker here
        t.onAccent = Hsl(h, 0.30f * k, 0.98f);
        t.text = Hsl(h, 0.20f * k, 0.11f); // near black, faintly tinted
        t.textDim = Hsl(h, 0.12f * k, 0.36f);
        t.textLow = Hsl(h, 0.10f * k, 0.56f);
        t.card = Hsl(h, 0.30f * k, 0.975f); // cards sit a step above the body
        t.cardHi = Hsl(h, 0.28f * k, 0.935f);
        t.track = Hsl(h, 0.22f * k, 0.86f);
        t.hairTop = Color{0, 0, 0, 0.10f};
        t.hairBot = Color{0, 0, 0, 0.04f};
        t.bg = Hsl(h, 0.22f * k, 0.925f);
        t.line = Color{0, 0, 0, 0.08f};
        t.sheen = Color{1, 1, 1, 0.55f};
        t.veil = Hsl(h, 0.20f * k, 0.97f);
    } else {
        t.accent = Hsl(h, (std::max)(0.62f, 0.85f * k), 0.71f); // accents keep min sat so they always pop
        t.accentHi = Hsl(h, (std::max)(0.72f, 0.95f * k), 0.83f);
        t.onAccent = Hsl(h, 0.50f * k, 0.09f);
        t.text = Hsl(h, 0.12f * k, 0.94f); // near white, faintly tinted
        t.textDim = Hsl(h, 0.10f * k, 0.66f);
        t.textLow = Hsl(h, 0.08f * k, 0.40f);
        t.card = Hsl(h, 0.32f * k, 0.052f); // very dark surfaces, lightness ramps card < cardHi < track
        t.cardHi = Hsl(h, 0.26f * k, 0.095f);
        t.track = Hsl(h, 0.20f * k, 0.155f);
        t.hairTop = Color{1, 1, 1, 0.11f}; // neutral white hairlines, work on any hue
        t.hairBot = Color{1, 1, 1, 0.02f};
        t.bg = Color{0, 0, 0, 1}; // black blends with the screen bezel
        t.line = Color{1, 1, 1, 0.06f};
        t.sheen = Color{1, 1, 1, 0.035f};
        t.veil = Color{0, 0, 0, 1};
    }
    if (seed.look == 2) { // liquid glass: clear tinted surfaces over the blur, glossy light on every edge
        t.text = Hsl(h, 0.08f * k, 0.98f);
        t.textDim = Hsl(h, 0.10f * k, 0.84f); // brighter than dark mode, the background shows through
        t.textLow = Hsl(h, 0.08f * k, 0.78f); // labels stay readable over the blur and over smoked glass on bright pages
        t.card = Hsl(h, 0.25f * k, 0.92f).Alpha(0.10f); // clear glass pieces, not solid cards
        t.cardHi = Hsl(h, 0.25f * k, 0.92f).Alpha(0.17f);
        t.track = Color{1, 1, 1, 0.20f};
        t.hairTop = Color{1, 1, 1, 0.38f}; // specular edge, light catching the top of each piece
        t.hairBot = Color{1, 1, 1, 0.08f};
        t.bg = Hsl(h, 0.45f * k, 0.10f).Alpha(0.30f); // light tinted wash: the blur stays visible, white text stays readable
        t.line = Color{1, 1, 1, 0.14f};
        t.sheen = Color{1, 1, 1, 0.16f}; // glossy band at the top of every surface
        t.veil = Hsl(h, 0.30f * k, 0.06f);
        t.glass = true;
    }
    if (seed.look == theme::kStarfall) { // deep space: near-black indigo, quiet surfaces, one luminous accent
        auto toward = [&](float base, float f) { return base + (std::fmod(h - base + 540.0f, 360.0f) - 180) * f; }; // hue blend the short way round
        t.bg = Hsl(toward(236, 0.2f), 0.45f, 0.045f); // indigo so deep it reads as black until it sits beside black
        t.card = Hsl(toward(238, 0.3f), 0.26f, 0.095f).Alpha(0.74f); // smoked glass: the sky barely through
        t.cardHi = Hsl(toward(238, 0.3f), 0.24f, 0.14f).Alpha(0.76f);
        t.track = Hsl(toward(236, 0.3f), 0.20f, 0.21f).Alpha(0.88f);
        t.accent = Hsl(h, (std::max)(0.60f, 0.80f * k), 0.72f); // clear and luminous, never neon
        t.accentHi = Hsl(h, 0.70f, 0.86f);
        t.text = Hsl(h, 0.08f, 0.95f); // full contrast ink on a near black sky
        t.textDim = Hsl(h, 0.10f, 0.70f);
        t.textLow = Hsl(h, 0.08f, 0.50f);
        t.hairTop = Hsl(toward(215, 0.35f), 0.30f, 0.85f, 0.12f); // a faint cool edge, no colour show
        t.hairBot = Color{1, 1, 1, 0.025f};
        t.line = Hsl(toward(220, 0.3f), 0.25f, 0.80f, 0.08f);
        t.sheen = Color{1, 1, 1, 0.025f};
        t.veil = t.bg;
        t.stars = true;
    }
    if (seed.chroma < 0.08f) { // black and white art or wallpaper: a colored accent would invent a mood that isn't there
        bool lightLook = seed.look == 1;
        t.accent = Hsl(h, 0, lightLook ? 0.20f : 0.86f);
        t.accentHi = Hsl(h, 0, lightLook ? 0.08f : 0.97f);
        t.onAccent = Hsl(h, 0, lightLook ? 0.98f : 0.07f);
    }
    if (seed.look == 3) { // custom: the background the user picked, ink that reads on it, their accent kept legible against it
        Color bg = Color::Rgb((uint32_t)seed.bg);
        bool dark = theme::Luma(bg) < 0.18f; // where white and black ink read equally well
        float bh, bs, bl;
        ToHsl(bg.r, bg.g, bg.b, bh, bs, bl);
        auto step = [&](float d) { return Hsl(bh, bs, Clamp(bl + (dark ? d : -d), 0, 1)); }; // surfaces lift off the background
        t.bg = bg;
        t.card = step(0.05f);
        t.cardHi = step(0.09f);
        t.track = step(0.16f);
        t.text = dark ? Color{0.96f, 0.96f, 0.97f, 1} : Color{0.07f, 0.07f, 0.08f, 1}; // plain white or black ink, whichever reads on it
        t.textDim = t.text.Mix(bg, 0.30f);
        t.textLow = t.text.Mix(bg, 0.50f);
        t.hairTop = dark ? Color{1, 1, 1, 0.11f} : Color{0, 0, 0, 0.10f};
        t.hairBot = dark ? Color{1, 1, 1, 0.02f} : Color{0, 0, 0, 0.04f};
        t.line = dark ? Color{1, 1, 1, 0.06f} : Color{0, 0, 0, 0.08f};
        t.sheen = dark ? Color{1, 1, 1, 0.035f} : Color{1, 1, 1, 0.45f};
        t.veil = bg;
        t.glass = false;
        Color a = seed.rgb >= 0 ? Color::Rgb((uint32_t)seed.rgb)
                : seed.chroma < 0.08f ? Hsl(h, 0, dark ? 0.86f : 0.20f)
                                      : Hsl(h, (std::max)(0.62f, 0.85f * k), dark ? 0.68f : 0.45f);
        float ah, as, al;
        ToHsl(a.r, a.g, a.b, ah, as, al);
        t.accent = a; // exactly what was picked, no second-guessing
        t.accentHi = Hsl(ah, (std::min)(1.0f, as + 0.08f), Clamp(al + (dark ? 0.10f : -0.10f), 0.03f, 0.97f));
        t.onAccent = theme::Luma(a) > 0.30f ? Color{0.07f, 0.07f, 0.08f, 1} : Color{0.98f, 0.98f, 0.98f, 1}; // ink on the accent, same rule
    }
    t.glow = t.accent.Alpha(t.stars ? 0.26f : 0.20f); // starfall: active controls glow a little brighter
    return t;
}

Theme Theme::Lerp(const Theme& o, float t) const {
    Theme r;
    r.accent = accent.Mix(o.accent, t);
    r.accentHi = accentHi.Mix(o.accentHi, t);
    r.onAccent = onAccent.Mix(o.onAccent, t);
    r.text = text.Mix(o.text, t);
    r.textDim = textDim.Mix(o.textDim, t);
    r.textLow = textLow.Mix(o.textLow, t);
    r.card = card.Mix(o.card, t);
    r.cardHi = cardHi.Mix(o.cardHi, t);
    r.track = track.Mix(o.track, t);
    r.hairTop = hairTop.Mix(o.hairTop, t);
    r.hairBot = hairBot.Mix(o.hairBot, t);
    r.glow = glow.Mix(o.glow, t);
    r.bg = bg.Mix(o.bg, t);
    r.line = line.Mix(o.line, t);
    r.sheen = sheen.Mix(o.sheen, t);
    r.veil = veil.Mix(o.veil, t);
    r.glass = t < 0.5f ? glass : o.glass;
    r.stars = t < 0.5f ? stars : o.stars;
    return r;
}
