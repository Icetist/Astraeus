#pragma once
#include <algorithm>
#include <string>
#include <vector>

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
D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title); // a settings card, returns its inner area
int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur, bool live); // picked index, -1 none
constexpr float kPrefRow = 132, kPrefGap = 10; // the newer settings cards

struct Ids { // a few pages or tiles held inline: the editors rebuild their sets every frame, none of it on the heap
    static constexpr int kCap = 16; // more than there are pages or tiles
    int v[kCap] = {};
    int n = 0;
    Ids() = default;
    Ids(const std::vector<int>& from) { // implicit: a saved list goes straight into an editor
        for (int x : from) push_back(x);
    }
    int* begin() { return v; }
    int* end() { return v + n; }
    const int* begin() const { return v; }
    const int* end() const { return v + n; }
    size_t size() const { return (size_t)n; }
    bool empty() const { return n == 0; }
    int operator[](size_t i) const { return v[i]; }
    void push_back(int x) {
        if (n < kCap) v[n++] = x;
    }
    int* erase(int* at) { return erase(at, at + 1); }
    int* erase(int* first, int* last) {
        std::copy(last, end(), first);
        n -= (int)(last - first);
        return first;
    }
    void insert(int* at, int x) {
        if (n >= kCap) return;
        std::copy_backward(at, end(), end() + 1);
        *at = x;
        ++n;
    }
    bool operator==(const Ids& o) const { return std::equal(begin(), end(), o.begin(), o.end()); }
    std::vector<int> Vec() const { return {begin(), end()}; } // for saving
};

struct Arrange { // an editable set, the widgets way: what's shown in order, and what could come back
    Ids shown, hidden;
    int (*glyph)(int) = nullptr;
    const wchar_t* (*name)(int) = nullptr;
    const wchar_t* (*shortName)(int) = nullptr; // when the name doesn't fit its cell; may be null
    size_t keep = 0; // the fewest shown: at this count their x badges go
    int lit = -1; // a shown item drawn picked, e.g. the page the panel opens on
    float litMix = 1; // how strongly it shows, 0..1
    int tapped = -1; // out: a shown item clicked, not carried
};
bool Arranger(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, int cols, float cellH, Arrange& a, bool live); // x takes one off, a drag moves it, + brings one back; true when shown changed
constexpr float kArrangeGap = 8; // between its cells
Arrange TileSet(const Model& m); // the tools page's tiles as an editor sees them: only those this pc can use
void KeepTiles(Model& m, const Ids& set); // saves an edited set, keeping tiles this pc can't use
const wchar_t* TileShort(int id); // a tile's name in one short word

}
