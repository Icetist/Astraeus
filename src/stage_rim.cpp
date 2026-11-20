// The fullscreen player's edge glow: the shared one (rim.h), round the whole screen, in this room's colors.
#include <algorithm>

#include "stage.h"

namespace {
constexpr float kSize = 0.05f; // the glow at rest reaches this share of the screen's short side inward
}

bool Stage::Rim(bool shaped) {
    bool lit = rim.Step(wave, shaped, (left + right) / 2, model.hasTrack && model.track.playing, model.VizGain(), frameDt, model.reduceMotion != 0);
    if (!lit) return false;
    Seed base = artSeeded ? artSeed : theme::FromCol(look.accent);
    base.look = mode, base.bg = model.CustomBg();
    bool light = LightRoom();
    Color cols[rim::kHues];
    rim::Colors(base, light, cols);
    rim.Draw(ctx.Get(), wave, {0, 0, W, H}, 0, (std::min)(W, H) * kSize, cols, light, model.RimGlow(), frameDt);
    return true;
}
