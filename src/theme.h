#pragma once
#include "gfx.h"

struct Seed {
    float hue = 255; // degrees, default periwinkle blue
    float chroma = 0.45f; // 0..1, scales saturation of every role
    int look = 0; // appearance: 0 dark (default), 1 light, 2 glass
};

struct Theme {
    Color accent, accentHi, onAccent; // onAccent = ink drawn on accent fills
    Color text, textDim, textLow;
    Color card, cardHi, track;
    Color hairTop, hairBot, glow;
    Color bg; // panel body, black in dark mode so it melts into the screen bezel
    Color line, sheen; // divider lines, top-edge highlight on cards
    Color veil; // laid over art backdrops (alpha set by caller) so text stays legible
    bool glass = false; // surfaces are translucent over a live blur

    static Theme From(Seed seed);
    Theme Lerp(const Theme& to, float t) const; // for animated theme crossfade
};

namespace theme {
Color Hsl(float h, float s, float l, float a = 1);
Seed FromCol(Color c);
Seed FromBmp(IWICBitmapSource* source);
std::wstring WallPath();
}
