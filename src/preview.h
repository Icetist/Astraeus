#pragma once
// a switch in settings, hovered a moment: a small see-through pill by the mouse shows what it does
#include "gfx.h"
#include "theme.h"
#include "ui.h"

namespace preview {

enum Kind {
    kNone,
    kCompact, kPillMusic, kAlbumArt, kArtBg, kVisualizer, kLyrics, // the media card's chips
    kPillAlways, kTimerBubble, kPinned, kFloating, kPillDesk, kAppsTheme, kKeyOnly, kPillFull, kPillFullOpen,
    kPowerPills, kHud, kShelfCatch, kShelfKeep, kShelfWipe, kStageLyrBtn, kStageEditBtn, kTourNext,
    kA11yFollow, kA11yKeys, kA11yReader,
};

void Want(Ui& ui, int kind, bool hover); // a control says whether it's under the mouse this frame
int ForId(const char* id); // a settings switch's id to what it previews, kNone when it has no picture
void Draw(Ui& ui, const Theme& t, const D2D1_RECT_F& bounds); // once, last, over the whole sheet

}
