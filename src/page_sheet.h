#pragma once
#include <string>

#include "pages.h"

namespace sheet { // shared pieces of the slide-up device sheets (bluetooth, wi-fi)

constexpr float kPi = 3.14159265f;
constexpr float kHero = 236; // left column width
constexpr float kRowH = 54, kRowGap = 6, kHeadH = 30, kNoteH = 40; // list item heights
inline const Color kDanger = Color::Rgb(0xFF8A80); // soft red for destructive actions

struct Frame { // what Begin pushed, End pops, plus where the header and list go
    ComPtr<ID2D1DeviceContext> dc;
    D2D1_MATRIX_3X2_F base;
    bool layered = false;
    bool tall = false; // sidebar: header row on top instead of a hero column
    D2D1_POINT_2F badge{}, sw{}; // hero badge center, power switch center
    D2D1_RECT_F title{}, sub{}, pill{}, list{};
    Align align = Align::Center;
    float titleSize = 21;
};
Frame Begin(PageCtx& p, float k, bool split = true); // rise-in + fade (k = open anim 0..1), dims the page, draws the sheet body; split: the hero column and its divider
void End(PageCtx& p, const Frame& f);

int Key(const std::wstring& s); // stable per-row ui id, survives re-sorting
void Spinner(Canvas& c, D2D1_POINT_2F o, float r, Color col, double now);
void Radar(Ui& ui, const Theme& t, const char* id, D2D1_POINT_2F o, bool on, bool scanning, int glyphOn, int glyphOff,
           int glyphScan); // hero badge, rings while scanning
bool ScanBtn(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, bool scanning, bool enabled, int icon, const wchar_t* label,
             const wchar_t* stopLabel); // primary pill that turns into a ghost pill while scanning
bool Act(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool busy, bool live); // row action pill
void Head(Ui& ui, const Theme& t, const D2D1_RECT_F& r, const wchar_t* label, bool spin); // list section header
void Empty(Canvas& c, const Theme& t, const D2D1_RECT_F& v, int glyph, const wchar_t* title, const wchar_t* sub);
void Bars(Canvas& c, const Theme& t, float x, float bottom, int quality, Color lit); // 4-step signal meter
float Scroll(Ui& ui, const char* id, float& target, const D2D1_RECT_F& view, float content); // wheel -> eased offset
bool Knob(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value, float& v, bool live,
          bool& released); // label, slider, value text; true while dragging, released on let go
void Fades(Canvas& c, const Theme& t, const D2D1_RECT_F& view, float scroll, float content); // hint more above/below

}
