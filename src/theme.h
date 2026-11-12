#pragma once
#include "gfx.h"

struct Seed {
    float hue = 255; // degrees, default periwinkle blue
    float chroma = 0.45f; // 0..1, scales saturation of every role
    int look = 0; // appearance: 0 dark (default), 1 light, 2 glass, 3 custom, 5 starfall, 7 chrome
    int rgb = -1; // the exact color this came from, if any: the custom look keeps it as the accent
    int bg = 0x121826; // custom look: the panel's background, 0xRRGGBB
    int tint = -1; // glass: its tint, 0xRRGGBB; -1 the plain dark one
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
    bool stars = false; // starfall: a night sky behind everything
    bool chrome = false; // chrome: polished metal behind everything, its lights drifting while the panel is open

    static Theme From(Seed seed);
    Theme Lerp(const Theme& to, float t) const; // for animated theme crossfade
};

namespace theme {
constexpr int kStarfall = 5; // the look's number; 4 is taken by following halobar
constexpr int kClear = 6; // as halobar numbers it: the resting pill is only ink, the panel is glass
constexpr int kChrome = 7; // the same number in halobar, so either can follow the other into it
bool TintIsPale(int tint); // that tint wants black ink
Theme ClearInk(const Theme& t, float dark); // nothing behind it: all one ink, 0 white .. 1 black
float BmpLuma(IWICBitmapSource* source); // its average brightness, 0..1
void SetContrast(bool on); // every look comes out in windows' high contrast colors, or strong plain ones
bool Contrast();
Color Hsl(float h, float s, float l, float a = 1);
Seed FromCol(Color c);
float Luma(Color c); // relative luminance, 0 black .. 1 white
Theme Lift(const Theme& t); // accent leaned toward the ink: controls that read over a glow of their own color
Seed FromBmp(IWICBitmapSource* source);
std::wstring WallPath();
}
