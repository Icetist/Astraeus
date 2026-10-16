#pragma once
#include <string>

#include "model.h"
#include "theme.h"
#include "ui.h"

struct PageCtx {
    Ui& ui;
    Model& m;
    const Theme& shell;
    const Theme& media;
    ID2D1Bitmap* cover;
    ID2D1Bitmap* coverBlur; // blurred art for page backdrop
    float artFade; // 0..1 crossfade on cover change
    D2D1_RECT_F area; // page content rect below tabs
    bool wantClose = false; // page asks drawer to retract
    bool wantStage = false; // media page asks for the fullscreen stage
    const wchar_t* hint = nullptr; // a short tip the drawer shows just outside the panel, next to hintRect
    D2D1_RECT_F hintRect{};
    float hintFade = 0;
};

void VizGlow(PageCtx& p, const D2D1_RECT_F& r, float scale, const Theme& t); // audio-reactive glow behind art, in t's accent
struct Swell { // soft blobs of light that rise from the bottom and swell with the music
    static constexpr int kMax = 8;
    float level[kMax] = {}, gain[kMax] = {}, heat = 0; // eased blob sizes, each blob's recent loudest, overall warmth
};
bool StepSwell(Swell& s, Model& m, float dt, bool playing, int blobs); // true while anything still glows
void DrawSwell(Canvas& c, const Swell& s, int blobs, const D2D1_RECT_F& r, const Theme& t, double now); // glows up from r's bottom
void WaveLine(Canvas& c, float a0, float a1, float cy, float amp, double now, Color col, float width, float wavelen); // the played part of the seek line, rippling

void DrawHome(PageCtx& p);
void DrawMedia(PageCtx& p);
void DrawSys(PageCtx& p);
void DrawNet(PageCtx& p);
void DrawTools(PageCtx& p);
void DrawBt(PageCtx& p, float t); // bluetooth menu overlay, t = open anim 0..1
void DrawWifi(PageCtx& p, float t); // wi-fi network picker overlay, same sheet style
void DrawShare(PageCtx& p, const D2D1_RECT_F& view); // shared-audio tab inside the bluetooth sheet
void DrawSettings(PageCtx& p, float t); // astraeus settings sheet

namespace fmt {
std::wstring Rate(double bps); // auto-scaled bytes/s units
std::wstring Clock(double seconds);
std::wstring Upper(std::wstring s);
std::wstring Temp(double c, bool fahr);
std::wstring Gb(double gb);
int WxIcon(int code, bool day);
const wchar_t* WxText(int code);
}
