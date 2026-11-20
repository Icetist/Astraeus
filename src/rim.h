#pragma once
// The edge glow: light round an edge in two colors, alive with the music. The fullscreen player wears it round the screen,
// the media page round its card. The light swells thicker where the music pushes it and those swells flow along the edge,
// thrown on by each beat, so the tempo sets the pace. One of these per surface: it keeps that surface's flow and its baked falloff.
#include <d2d1_1.h>

#include "gfx.h"
#include "pages.h"
#include "theme.h"

namespace rim {
constexpr int kMediaGlow = 0, kMediaBars = 1, kMediaRim = 2; // the media page's one visualizer: the soft glow, the waveform, or the edge glow
Color Second(const Seed& base, bool light); // the second of the two colors: the cover's next one, or one a little round from the first
void ForgetMedia(); // the panel's device is going: the media card's glow lets go of what it baked on it
}

struct EdgeGlow {
    static constexpr int kSwells = 3; // trains of swells: the low end, the middle, the top
    float run[kSwells] = {}, turn = 0; // how far each has flowed round, and the two colors with it, in turns
    float loud = 0, slow = 0, beat = 0; // the music's level eased, its running average, and the beat's pulse
    double clock = 0, lastBeat = -1;
    Color two{}; // the second color, eased to a new song's
    bool twoSet = false;
    ComPtr<ID2D1Bitmap1> fade; // how the light falls off inward, baked once on the surface's device
    // advances the flow with the music: level is the music's loudness now, shaped whether any of the waveform stands. false while it's dark
    bool Step(const Waveform& w, bool shaped, float level, bool playing, float gain, float dt, bool calm);
    // round box, its corners rounded by radius; size is how far in the light reaches at rest. twoWant eases in as the second color
    void Draw(ID2D1DeviceContext* ctx, const Waveform& w, const D2D1_RECT_F& box, float radius, float size, Color one, Color twoWant, bool light, float dt);
    void Forget() { fade.Reset(); } // its device is going
};
