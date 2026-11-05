// The display dashboard's settings: whether it's on, which screen, and its tiles, kept in the registry as one line of text.
#include "model.h"

#include <algorithm>
#include <cmath>

#include "glyphs.h"

namespace {

constexpr wchar_t kBoardVal[] = L"Boards"; // the screens with a dashboard, a name a line
const std::wstring kTilesVal = L"BoardTiles "; // and each one's tiles, under this and its screen's name
const wchar_t* const kKeys[Model::kBoardKinds] = {L"clock", L"calendar", L"weather", L"media", L"lyrics", L"cpu", L"gpu", L"memory",
                                                  L"network", L"disk", L"processes", L"volume", L"todo", L"workspaces", L"shelf", L"timer", L"servers",
                                                  L"battery"}; // saved by name: new kinds never shift old ones
const wchar_t* const kNames[Model::kBoardKinds] = {L"Clock", L"Calendar", L"Weather", L"Media", L"Lyrics", L"CPU", L"GPU", L"Memory",
                                                   L"Network", L"Disk", L"Processes", L"Volume", L"To-do", L"Workspaces", L"Shelf", L"Timer", L"Servers",
                                                   L"Battery"};
constexpr int kPresetRows = 7; // the preset is drawn for 12 by 7, a 1024 x 600 screen; other shapes stretch it

const Model::BoardTile kPreset[] = { // time and date, weather, the month, the song, the gauges and the network: what a small side screen is for
    {Model::kBdClock, 0, 0, 4, 2},  {Model::kBdWeather, 4, 0, 4, 2}, {Model::kBdCalendar, 8, 0, 4, 4},
    {Model::kBdMedia, 0, 2, 4, 5},  {Model::kBdCpu, 4, 2, 2, 2},     {Model::kBdRam, 6, 2, 2, 2},
    {Model::kBdNet, 4, 4, 4, 3},    {Model::kBdVolume, 8, 4, 2, 3},  {Model::kBdDisk, 10, 4, 2, 3}, // no gpu gauge out of the box: windows' gpu counter is the one costly reading
};

int KindOf(const std::wstring& key) {
    for (int i = 0; i < Model::kBoardKinds; ++i)
        if (key == kKeys[i]) return i;
    return -1;
}

std::wstring SavedTiles(const std::wstring& screen) { // "clock,0,0,4,2;media,0,2,4,5", or nothing if this monitor was never arranged
    std::wstring s = sys::ReadText((kTilesVal + screen).c_str());
    if (!s.empty()) return s;
    for (const sys::Screen& o : sys::ListScreens()) { // kept under the windows device name by an earlier build: it moves to the monitor's own
        if (o.id != screen) continue;
        s = sys::ReadText((kTilesVal + o.name).c_str());
        if (!s.empty()) sys::SaveText((kTilesVal + screen).c_str(), s);
    }
    return s;
}

void LoadSpot(Model::BoardSpot& spot) {
    std::wstring s = SavedTiles(spot.screen);
    spot.edited = !s.empty();
    for (size_t a = 0; a < s.size();) {
        size_t e = (std::min)(s.find(L';', a), s.size());
        std::wstring item = s.substr(a, e - a);
        a = e + 1;
        size_t comma = item.find(L',');
        Model::BoardTile t;
        if (comma == std::wstring::npos || (t.kind = KindOf(item.substr(0, comma))) < 0) continue; // a kind from a newer version: dropped
        if (swscanf_s(item.c_str() + comma + 1, L"%d,%d,%d,%d", &t.x, &t.y, &t.w, &t.h) != 4) continue;
        bool twice = std::any_of(spot.tiles.begin(), spot.tiles.end(), [&](const Model::BoardTile& o) { return o.kind == t.kind; });
        if (t.w < 1 || t.h < 1 || t.x < 0 || t.y < 0 || t.x + t.w > Model::kBoardCols || twice) continue; // hand-edited or repeated
        spot.tiles.push_back(t);
    }
}

}

const wchar_t* Model::LayoutName(int l) {
    static const wchar_t* const kLayNames[kLayouts] = {L"Horizontal", L"Vertical"};
    return kLayNames[(std::clamp)(l, 0, kLayouts - 1)];
}

const wchar_t* Model::BoardName(int kind) { return kNames[(std::clamp)(kind, 0, kBoardKinds - 1)]; }

int Model::BoardGlyph(int kind) {
    static const int kGlyphs[kBoardKinds] = {glyph::Schedule, glyph::Calendar, glyph::PartlyDay, glyph::Music, glyph::Lyrics, glyph::Cpu, glyph::Gpu, glyph::Ram,
                                             glyph::Network, glyph::Disk, glyph::System, glyph::VolumeUp, glyph::BoxOn, glyph::Workspaces, glyph::Shelf, glyph::Timer,
                                             glyph::Server, glyph::Battery};
    return kGlyphs[(std::clamp)(kind, 0, kBoardKinds - 1)];
}

bool Model::BoardOffered(int kind) const {
    if (kind == kBdWork) return tiler != sys::Tiler::kNone; // workspaces mean a tiling manager here
    if (kind == kBdBattery) return portable;
    return kind >= 0 && kind < kBoardKinds;
}

bool Model::BoardHas(int kind) const {
    return std::any_of(boardShown.begin(), boardShown.end(), [kind](const BoardTile& t) { return t.kind == kind; });
}

Model::BoardSpot* Model::Spot(const std::wstring& screen) {
    for (auto& s : boards)
        if (s.screen == screen) return &s;
    return nullptr;
}

void Model::LoadBoard() {
    std::wstring all = sys::ReadText(kBoardVal);
    for (size_t at = 0, end; at < all.size(); at = end + 1) { // one screen name a line
        end = (std::min)(all.find(L'\n', at), all.size());
        BoardSpot spot;
        spot.screen = all.substr(at, end - at);
        if (spot.screen.empty() || Spot(spot.screen)) continue;
        LoadSpot(spot);
        boards.push_back(std::move(spot));
    }
}

void Model::SetBoardOn(const std::wstring& screen, bool on) {
    if (on == (Spot(screen) != nullptr)) return;
    if (on) { // back on: the layout it had; switching off never touches what's saved for this monitor
        BoardSpot spot;
        spot.screen = screen;
        LoadSpot(spot);
        boards.push_back(std::move(spot));
    } else boards.erase(std::remove_if(boards.begin(), boards.end(), [&](const BoardSpot& s) { return s.screen == screen; }), boards.end());
    boardsAway.erase(std::remove(boardsAway.begin(), boardsAway.end(), screen), boardsAway.end()); // switched on: it shows right away
    std::wstring list;
    for (const auto& s : boards) list += (list.empty() ? L"" : L"\n") + s.screen;
    sys::SaveText(kBoardVal, list);
}

void Model::SetBoardTiles(const std::wstring& screen, const std::vector<BoardTile>& tiles) {
    BoardSpot* spot = Spot(screen);
    if (!spot) return;
    spot->tiles = tiles;
    spot->edited = true;
    std::wstring s;
    for (const BoardTile& t : tiles) {
        wchar_t item[64];
        swprintf_s(item, L"%s,%d,%d,%d,%d", kKeys[t.kind], t.x, t.y, t.w, t.h);
        s += (s.empty() ? L"" : L";") + std::wstring(item);
    }
    sys::SaveText((kTilesVal + screen).c_str(), s.empty() ? L"none" : s); // all taken off: "none", so it doesn't read as never set
}

std::vector<Model::BoardTile> Model::BoardPreset(int rows) const {
    std::vector<BoardTile> out;
    for (BoardTile t : kPreset) { // stretched to the screen's rows, cells snapped
        int y0 = (int)std::lround(t.y * rows / (double)kPresetRows), y1 = (int)std::lround((t.y + t.h) * rows / (double)kPresetRows);
        t.y = y0;
        t.h = (std::max)(1, y1 - y0);
        if (t.y + t.h <= rows) out.push_back(t);
    }
    return out;
}
