#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "theme.h"
#include "ui.h"

// Widgets that only need a rectangle to draw in: clocks, the weather's other views, system rings, accounts, stocks, recent files and small tools.
// A host places them, passes what it already knows in Host, and routes clicks and typing here. What they keep, they keep under the host's own settings.
namespace faces {

enum Cat { kClocks, kWeather, kWork, kSystem, kMedia, kMoney, kOnline, kFiles, kSports, kCats };

struct Meta {
    const char* key; // saved by name
    const wchar_t* name;
    const wchar_t* about;
    int cat;
    int glyph;
    int sizes; // bits: 1 small, 2 medium, 4 large
    int defSize;
    float wMedium, wLarge; // along a dock
    const char* feed; // what it reads from feeds, null: nothing
    bool anotherFace; // the bar has its own richer one of these: only the panel shows this one
};

int Count();
const Meta& Info(int kind);
int Find(const char* key); // -1 if none
const wchar_t* CatName(int cat);

struct Host { // what the host already reads; faces never start a source of their own for these
    double cpu = 0, mem = 0, gpu = -1, memUsedGb = 0, memTotalGb = 0;
    double netDown = 0, netUp = 0;
    bool wxValid = false, wxDay = true, fahrenheit = false, h24 = false;
    bool cards = false; // shown as big cards while something is open anyway: a clock's seconds are worth a frame
    double tempC = 0, highC = 0, lowC = 0;
    int wxCode = 0;
    std::wstring place;
    struct Hour {
        int hour = 0, code = 0;
        double tempC = 0;
        bool day = true;
    } hours[8];
    int nHours = 0;
    struct Day {
        int y = 0, m = 0, d = 0, code = 0;
        double hiC = 0, loC = 0;
    } days[5];
    int nDays = 0;
    bool hasTrack = false, playing = false;
    std::wstring title, artist;
    double progress = 0;
    int bright = -1; // percent, -1 if this display has no control
    void (*setBright)(void* ctx, int percent) = nullptr;
    void (*resync)(void* ctx) = nullptr; // what's fetched changed: the host asks for it again
    void* ctx = nullptr;
};

enum class Act { kNone, kPanel, kPlay, kNext, kPrev, kTray };

// size: 0 small, 1 medium, 2 large; the rectangle's shape decides how much it says
void Draw(Canvas& c, const Theme& t, int kind, int size, const D2D1_RECT_F& r, const Host& h);
void Sampling(bool on); // while on, feeds draw made-up numbers: a preview of what a widget looks like before it's connected
Act Click(int kind, bool right, const Host& h); // a click on its face
bool Wheel(int kind, float notches); // the wheel over its face: true when it scrolled something of its own (the scores' games)
int NextMs(const std::vector<int>& kinds, const Host& h); // how long before any of these has something new to draw, 0: nothing to wait for
std::vector<std::string> Feeds(const std::vector<int>& kinds); // what they need fetched

// the panel behind a face: more detail, and whatever it needs set up
void OpenPanel(int kind); // reads what it lists, once
float PanelH(int kind, float w);
void Panel(Ui& ui, const Theme& t, int kind, const D2D1_RECT_F& r, const Host& h);
bool WantsKeys(int kind); // it has a field or a list to type into
bool Char(int kind, wchar_t ch); // true when typing went to it; esc with nothing being typed: false, the host closes it
void ClosePanel(); // what's typed is kept

struct Market {
    const wchar_t* label;
    const wchar_t* tick;
};
const std::vector<Market>& UsMarkets(); // the us markets widget's: its tile shows the first few, its panel all of them
std::vector<std::wstring> UsMoreTickers(); // and the ones added in its panel
std::wstring Hours(int seconds); // "2h 14m"
int WxIcon(int code, bool day);
const wchar_t* WxText(int code);

}
