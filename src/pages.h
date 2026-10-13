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
};

void VizGlow(PageCtx& p, const D2D1_RECT_F& r, float scale); // audio-reactive glow behind art

void DrawHome(PageCtx& p);
void DrawMedia(PageCtx& p);
void DrawSys(PageCtx& p);
void DrawNet(PageCtx& p);
void DrawTools(PageCtx& p);
void DrawBt(PageCtx& p, float t); // bluetooth menu overlay, t = open anim 0..1

namespace fmt {
std::wstring Rate(double bps); // auto-scaled bytes/s units
std::wstring Clock(double seconds);
std::wstring Upper(std::wstring s);
std::wstring Temp(double c, bool fahr);
std::wstring Gb(double gb);
int WxIcon(int code, bool day);
const wchar_t* WxText(int code);
}
