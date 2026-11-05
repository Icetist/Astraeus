// The settings that came after the first instruments: smaller cards of switches, chips and sliders, two across, under the others.
#include <algorithm>
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "starfall.h"
#include "entry.h"
#include "report.h"
#include "stageprefs.h"
#include "tray.h"
#include "version.h"

using namespace sheet;

namespace {

constexpr float kRowH = 30; // one switch or slider line inside a card
constexpr float kKnobH = 22;
constexpr float kChipH = 28;
constexpr float kNoteH = 17; // one line of a two-line label
constexpr int kBatLowMin = 5, kBatLowMax = 50; // the low battery pill, in percent
constexpr int kScaleMin = 80, kScaleMax = 130; // ui scale, percent
constexpr int kLyricStep = 5; // lyric size moves in 5% steps
constexpr float kHoldSec = 1.2f; // hold to reset everything
constexpr float kOffAlpha = 0.4f; // a control that does nothing right now, greyed

bool SwitchRow(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* label, bool on, bool live, bool enabled = true) { // label left, switch right; true when flipped
    Canvas& c = ui.C();
    float cy = (r.top + r.bottom) / 2;
    Ui::Hit h = live && enabled ? ui.Button(Ui::Id(id, 1), {r.left, r.top, r.right - 52, r.bottom}) : Ui::Hit{}; // the words flip it too
    if (live && enabled) ui.NoStop(); // one control, one stop: the switch's
    c.Text(label, {r.left, r.top, r.right - 56, r.bottom}, Weight::Regular, 11.5f, !enabled ? t.textLow : h.hover ? t.text : t.text.Mix(t.textDim, 0.25f));
    bool flip = w::Switch(ui, t, Ui::Id(id), {r.right - 24, cy}, on, live && enabled);
    if (live && enabled) ui.Name(label);
    return flip || h.clicked;
}

void Note(Canvas& c, const Theme& t, const D2D1_RECT_F& in, float y, const std::wstring& text) { // a quiet line under the controls
    c.Text(text, Rect(in.left, y, Width(in), 16), Weight::Light, 10.5f, t.textLow);
}

void Flip(Model& m, int& field, const wchar_t* key) { m.SetPref(field, key, field ? 0 : 1); }

void Power(PageCtx& p, const D2D1_RECT_F& r, bool live) { // battery pills: plugged in, unplugged, running low
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"POWER");
    if (!m.portable) {
        c.Icon(glyph::Power, {in.left + 10, in.top + 12}, 18, t.textLow);
        c.Text(L"No battery on this PC", Rect(in.left + 28, in.top + 3, Width(in) - 28, 18), Weight::Regular, 11.5f, t.textDim);
        Note(c, t, in, in.top + 30, L"Charging and battery pills are for laptops");
        return;
    }
    if (SwitchRow(ui, t, "set.pwr.pills", Rect(in.left, in.top, Width(in), kRowH), L"Charging and on battery pills", m.powerPills != 0, live))
        Flip(m, m.powerPills, L"PowerPills");
    float v = (m.batLow - kBatLowMin) / (float)(kBatLowMax - kBatLowMin);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.pwr.low"), Rect(in.left, in.top + kRowH + 6, Width(in), kSlideH), L"Low at", std::to_wstring(m.batLow) + L"%", v, live, done,
             1.0f / (kBatLowMax - kBatLowMin)))
        m.batLow = kBatLowMin + (int)std::lround(v * (kBatLowMax - kBatLowMin));
    if (done) m.SetPref(m.batLow, L"BatteryLow", m.batLow);
    Note(c, t, in, in.top + kRowH + kSlideH + 12, L"The low pill shows even in do not disturb");
}

const wchar_t* WidgetName(int w) {
    static const wchar_t* const kNames[Model::kWidgets] = {L"Nothing", L"Weather", L"Battery", L"CPU", L"Memory", L"Network", L"Workspace", L"Language", L"Window", L"Disk"};
    return kNames[(w >= 0 && w < Model::kWidgets) ? w : 0];
}

void Pill(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the resting pill: always up, which two widgets flank its clock, the activity bubble
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"STATUS PILL");
    if (SwitchRow(ui, t, "set.pill.always", Rect(in.left, in.top, Width(in), kRowH), L"Always show the pill", m.pillAlways != 0, live)) Flip(m, m.pillAlways, L"PillAlways");
    float cw = (Width(in) - 8) / 2, y = in.top + kRowH + 2;
    int* sides[2] = {&m.pillLeft, &m.pillRight};
    const wchar_t* keys[2] = {L"PillLeft", L"PillRight"};
    for (int i = 0; i < 2; ++i) { // tap to step through the widgets
        int& wv = *sides[i];
        bool noWork = wv == Model::kWWork && m.tiler == sys::Tiler::kNone; // saved from when a tiling manager was here: it says nothing now
        std::wstring label = std::wstring(i ? L"Right: " : L"Left: ") + WidgetName(noWork ? Model::kWNone : wv);
        if (w::Chip(ui, t, Ui::Id("set.pill.w", i), Rect(in.left + i * (cw + 8), y, cw, kChipH), label, wv != Model::kWNone && !noWork) && live) {
            int next = (wv + 1) % Model::kWidgets;
            if (next == Model::kWBattery && !m.portable) next = (next + 1) % Model::kWidgets; // no battery to show
            if (next == Model::kWWork && m.tiler == sys::Tiler::kNone) next = (next + 1) % Model::kWidgets; // no workspaces without a tiling manager
            m.SetPref(wv, keys[i], next);
        }
    }
    if (SwitchRow(ui, t, "set.pill.bubble", Rect(in.left, y + kChipH + 4, Width(in), kRowH), L"Timer bubble beside the pill", m.activityBubble != 0, live))
        Flip(m, m.activityBubble, L"ActivityBubble");
}

void PanelOpts(PageCtx& p, const D2D1_RECT_F& r, bool live) { // how the panel behaves, in either layout: pinned open, floating clear of the edge
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"PANEL");
    if (SwitchRow(ui, t, "set.pinned", Rect(in.left, in.top, Width(in), kRowH), L"Pinned open", m.pinned != 0, live)) Flip(m, m.pinned, L"Pinned");
    if (SwitchRow(ui, t, "set.floating", Rect(in.left, in.top + kRowH + 4, Width(in), kRowH), L"Floating card", m.floating != 0, live)) Flip(m, m.floating, L"Floating");
    Note(c, t, in, in.top + 2 * kRowH + 12, L"Pinned: stays open, maximized windows end where it starts");
    Note(c, t, in, in.top + 2 * kRowH + 30, L"Floating: a card with a gap from the edge");
}

void Layout(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the panel's shape: the wide island, or a narrow column
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"LAYOUT");
    const wchar_t* names[Model::kLayouts];
    for (int i = 0; i < Model::kLayouts; ++i) names[i] = Model::LayoutName(i);
    int v = Segmented(ui, t, "set.layout", Rect(in.left, in.top + 2, Width(in), 28), names, Model::kLayouts, m.layout, live);
    if (v >= 0 && v != m.layout) m.SetPref(m.layout, L"Layout", v); // the panel goes up and comes back in the new shape
    static const wchar_t* const kNotes[Model::kLayouts][2] = {
        {L"The wide island, pages side by side", L"Pinned and floating are under Advanced"},
        {L"A narrow column, the sidebar's layout", L"On the top edge; the sides are always narrow"},
    };
    Note(c, t, in, in.top + 40, kNotes[m.layout][0]);
    Note(c, t, in, in.top + 58, kNotes[m.layout][1]);
}

void Dashboard(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a screen of tiles that stays open behind your windows
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"FULLSCREEN DASHBOARD");
    if (m.screenList.empty()) m.screenList = sys::ListScreens();
    const auto& list = m.screenList;
    RECT box = list.empty() ? RECT{0, 0, 16, 9} : list[0].rc; // your screens as they're arranged, to scale
    for (const auto& s : list) {
        box.left = (std::min)(box.left, s.rc.left), box.top = (std::min)(box.top, s.rc.top);
        box.right = (std::max)(box.right, s.rc.right), box.bottom = (std::max)(box.bottom, s.rc.bottom);
    }
    D2D1_RECT_F map = {in.left + 4, in.top + 2, in.right - 4, in.bottom - 38};
    float bw = (float)(box.right - box.left), bh = (float)(box.bottom - box.top), k = (std::min)(Width(map) / bw, Height(map) / bh);
    float ox = (map.left + map.right - bw * k) / 2, oy = (map.top + map.bottom - bh * k) / 2;
    int on = 0;
    for (int i = 0; i < (int)list.size(); ++i) { // tap a screen: dashboard there, or the desktop again
        const auto& s = list[i];
        D2D1_RECT_F q = {ox + (s.rc.left - box.left) * k + 2, oy + (s.rc.top - box.top) * k + 2, ox + (s.rc.right - box.left) * k - 2, oy + (s.rc.bottom - box.top) * k - 2};
        uint32_t id = Ui::Id("set.board.scr", i);
        Ui::Hit h = live ? ui.Button(id, q) : Ui::Hit{};
        bool dash = m.Spot(s.id) != nullptr;
        if (live) {
            ui.Mark(Ui::Role::Toggle, dash);
            ui.Name(L"Dashboard on screen " + std::to_wstring(s.number ? s.number : i + 1));
        }
        on += dash;
        float lit = ui.Smooth(id + 10, dash ? 1.0f : 0, 14);
        c.FillV(q, 6, t.track.Alpha(0.9f).Mix(t.accent.Alpha(0.30f), lit), t.track.Alpha(0.5f).Mix(t.accent.Alpha(0.16f), lit));
        c.Stroke(q, 6, t.accent.Alpha(lit).Mix(t.text.Alpha(0.5f), h.hover ? 0.5f * (1 - lit) : 0), lit > 0.5f ? 1.5f : 1);
        if (lit > 0.01f) // a few little tiles: this one is a dashboard
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 3; ++x) {
                    float tw = (Width(q) - 16) / 3, th = (Height(q) - 13) / 2;
                    c.Fill(Rect(q.left + 4 + x * (tw + 4), q.top + 4 + y * (th + 5), tw, th), 2, t.accentHi.Alpha(0.35f * lit));
                }
        c.Text(std::to_wstring(s.number ? s.number : i + 1), q, Weight::Medium, (std::min)(14.0f, Height(q) * 0.45f), t.text.Alpha(0.55f + 0.45f * lit), Align::Center);
        if (h.clicked) m.SetBoardOn(s.id, !dash);
    }
    Note(c, t, in, in.bottom - 34, on ? L"Ctrl+Alt+D: that screen back to the desktop and again" : L"Tap a screen to fill it with live tiles");
    Note(c, t, in, in.bottom - 16, on ? L"Right click a dashboard to arrange its tiles" : L"Each one has its own layout, any size of screen");
}

constexpr float kPageCellH = 48, kTileCellH = 34; // the editors' cells: a page's icon over its name, a tile's beside it
constexpr float kWideCols = 480; // an editor at least this wide puts every page, or four tiles, in one row
constexpr float kBoxChrome = 30 + 10; // a card's title above and margin below its inner area

int PageCols(float w) { return w >= kWideCols ? Model::kPageIds : 3; }
int TileCols(float w) { return w >= kWideCols ? 4 : 2; }
float GridH(int items, int cols, float cellH) {
    int rows = (std::max)(1, (items + cols - 1) / cols);
    return rows * cellH + (rows - 1) * kArrangeGap;
}
int TilesHere(const Model& m) {
    int n = 0;
    for (int i = 0; i < Model::kToolTiles; ++i) n += m.TileHere(i) ? 1 : 0;
    return n;
}
float PagesH(float w) { return kBoxChrome + kRowH + 6 + GridH(Model::kPageIds, PageCols(w - 24), kPageCellH) + 6 + kNoteH; }
float TilesH(const Model& m, float w) { return kBoxChrome + GridH(TilesHere(m), TileCols(w - 24), kTileCellH) + 6 + kNoteH; }

void Pages(PageCtx& p, const D2D1_RECT_F& r, bool live) { // which pages the rail shows and in what order, and the one the panel opens on
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"PAGES");
    if (SwitchRow(ui, t, "set.page.last", Rect(in.left, in.top, Width(in), kRowH), L"Open on the last page", m.rememberPage != 0, live)) Flip(m, m.rememberPage, L"RememberPage");
    Arrange a;
    a.shown = m.pages;
    for (int i = 0; i < Model::kPageIds; ++i)
        if (!m.HasPage(i)) a.hidden.push_back(i);
    a.glyph = Model::PageGlyph;
    a.name = Model::PageName;
    a.keep = 1; // one page always stays
    a.lit = m.HasPage(m.defaultPage) ? m.defaultPage : m.pages.front(); // the picked page, or the first while that one is hidden
    a.litMix = ui.Smooth(Ui::Id("set.page.fixed"), m.rememberPage ? 0.35f : 1.0f, 14); // the pick dims while the last page wins
    int cols = PageCols(Width(in));
    float gy = in.top + kRowH + 6, gh = GridH(Model::kPageIds, cols, kPageCellH);
    if (Arranger(ui, t, "set.pages", Rect(in.left, gy, Width(in), gh), cols, kPageCellH, a, live)) m.SetPages(a.shown.Vec());
    if (a.tapped >= 0) {
        m.SetPref(m.defaultPage, L"DefaultPage", a.tapped);
        if (m.rememberPage) m.SetPref(m.rememberPage, L"RememberPage", 0); // picking one means you want it
    }
    Note(c, t, in, gy + gh + 6, L"Tap a page to open on it, drag to reorder. Right-click the rail for settings");
}

void Tiles(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the tools page's quick tiles: which, and in what order
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"TOOLS TILES");
    Arrange a = TileSet(m);
    int cols = TileCols(Width(in));
    float gh = GridH(TilesHere(m), cols, kTileCellH);
    if (Arranger(ui, t, "set.tiles", Rect(in.left, in.top, Width(in), gh), cols, kTileCellH, a, live)) KeepTiles(m, a.shown);
    Note(c, t, in, in.top + gh + 6, L"Right-click the tiles on the Tools page to edit them there too");
}

void Media(PageCtx& p, const D2D1_RECT_F& r, bool live) { // what the player is made of
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"MEDIA");
    struct Opt {
        const wchar_t* label;
        int* field;
        const wchar_t* key;
    } opts[] = {{L"Compact player", &m.mediaCompact, L"MediaCompact"}, {L"Music in the pill", &m.pillMusic, L"PillMusic"},
                {L"Album art", &m.showArt, L"ShowArt"}, {L"Visualizer", &m.vizOn, L"Visualizer"}};
    float cw = (Width(in) - 8) / 2;
    for (int i = 0; i < 4; ++i) { // lit chips: on or off at a glance
        D2D1_RECT_F q = Rect(in.left + (i % 2) * (cw + 8), in.top + 2 + (i / 2) * (kChipH + 8), cw, kChipH);
        if (w::Chip(ui, t, Ui::Id("set.media", i), q, opts[i].label, *opts[i].field != 0) && live) Flip(m, *opts[i].field, opts[i].key);
    }
    if (SwitchRow(ui, t, "set.stage.hide", Rect(in.left, in.top + 2 * (kChipH + 8) - 2, Width(in), kKnobH + 4), L"Fullscreen controls hide at rest", m.stageHide != 0, live))
        Flip(m, m.stageHide, L"StageHide"); // off: the fullscreen player keeps its buttons up
}

void ArtBack(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the cover behind the media page and the fullscreen player, each on its own switch, and how strongly
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"ALBUM ART BACKGROUND");
    bool can = m.showArt != 0; // no art shown: nothing to lay behind anything
    if (SwitchRow(ui, t, "set.media.back", Rect(in.left, in.top, Width(in), kRowH), can ? L"Media page" : L"Album art is switched off", m.artBack != 0, live && can, can))
        m.SetArtBack(m.artBack == 0); // shared: halobar's player follows
    if (SwitchRow(ui, t, "set.stage.back", Rect(in.left, in.top + kRowH, Width(in), kRowH), L"Fullscreen player", m.stageArt != 0, live && can, can))
        Flip(m, m.stageArt, L"StageArtBg"); // apart from the media page: either, both or neither
    bool any = can && (m.artBack || m.stageArt);
    float dim = ui.Smooth(Ui::Id("set.art.dim"), any ? 1.0f : kOffAlpha, 14);
    bool layered = dim < 0.999f;
    if (layered) c.Target()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), dim), nullptr);
    constexpr int lo = Model::kVizAlphaMin, hi = 100, step = Model::kSlideStep;
    float v = (m.vizAlpha - lo) / (float)(hi - lo);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.viz.alpha"), Rect(in.left, in.top + 2 * kRowH + 4, Width(in), kSlideH), L"Visualizer opacity", std::to_wstring(m.vizAlpha) + L"%", v, live && any, done,
             (float)step / (hi - lo)))
        m.vizAlpha = lo + (int)std::lround(v * (hi - lo) / step) * step; // live: the page behind shows it as it moves
    if (done) m.SetPref(m.vizAlpha, L"VizAlpha", m.vizAlpha); // shared: halobar's player follows
    if (layered) c.Target()->PopLayer();
}

void Visualizer(PageCtx& p, const D2D1_RECT_F& r, bool live) { // how hard the music's light answers, everywhere it shows
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"VISUALIZER");
    constexpr int lo = Model::kVizGainMin, hi = Model::kVizGainMax, step = Model::kSlideStep;
    float v = (m.vizGain - lo) / (float)(hi - lo);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.viz.gain"), Rect(in.left, in.top + 2, Width(in), kSlideH), L"Sensitivity", std::to_wstring(m.vizGain) + L"%", v, live, done, (float)step / (hi - lo)))
        m.vizGain = lo + (int)std::lround(v * (hi - lo) / step) * step; // live: the bars answer as it moves
    if (done) m.SetPref(m.vizGain, L"VizGain", m.vizGain); // shared: halobar's follow
    float cw = (Width(in) - 8) / 2, y = in.top + kSlideH + 4; // its options: two chips is all a card's width can name
    if (w::Chip(ui, t, Ui::Id("set.viz.wave"), Rect(in.left, y, cw, kChipH), L"Waveform", m.vizWave != 0) && live) Flip(m, m.vizWave, L"VizWave");
    if (w::Chip(ui, t, Ui::Id("set.viz.full"), Rect(in.left + cw + 8, y, cw, kChipH), L"Any volume", m.vizFull != 0) && live) Flip(m, m.vizFull, L"VizFull");
    if (SwitchRow(ui, t, "set.viz.bob", Rect(in.left, y + kChipH + 2, Width(in), kRowH), L"Cover thump in fullscreen", m.stageThump != 0, live)) Flip(m, m.stageThump, L"StageThump"); // the fullscreen cover, on the beat
}

void Opening(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the closed island: only the shortcut opens it, wherever it is
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"OPENING");
    if (SwitchRow(ui, t, "set.keyonly", Rect(in.left, in.top, Width(in), kRowH), L"Open only with the shortcut", m.keyOnly != 0, live)) Flip(m, m.keyOnly, L"KeyOnly");
    Note(c, t, in, in.top + kRowH + 6, L"Hovering or clicking the closed island");
    Note(c, t, in, in.top + kRowH + 24, L"does nothing. The shortcut still opens it");
}

void Apps(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the edge over an app window: only the shortcut opens it, or how long it waits
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"OVER APPS");
    D2D1_RECT_F row = Rect(in.left, in.top, Width(in), 2 * kNoteH);
    Ui::Hit h = live ? ui.Button(Ui::Id("set.apps.key", 1), {row.left, row.top, row.right - 52, row.bottom}) : Ui::Hit{}; // the words flip it too
    if (live) ui.NoStop();
    Color ink = h.hover ? t.text : t.text.Mix(t.textDim, 0.25f);
    c.Text(L"Open with the shortcut", Rect(row.left, row.top, Width(row) - 56, kNoteH), Weight::Regular, 11.5f, ink); // two lines: the whole name, nothing cut
    c.Text(L"only over apps", Rect(row.left, row.top + kNoteH, Width(row) - 56, kNoteH), Weight::Regular, 11.5f, ink);
    bool appFlip = w::Switch(ui, t, Ui::Id("set.apps.key"), {row.right - 24, (row.top + row.bottom) / 2}, m.appKeyOnly != 0, live);
    if (live) ui.Name(L"Open with the shortcut only over apps");
    if ((appFlip || h.clicked) && live) Flip(m, m.appKeyOnly, L"AppKeyOnly");
    bool can = m.appKeyOnly == 0; // the shortcut alone: no hover left to wait for
    float dim = ui.Smooth(Ui::Id("set.apps.dim"), can ? 1.0f : kOffAlpha, 14);
    bool layered = dim < 0.999f;
    if (layered) c.Target()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), dim), nullptr);
    constexpr int hi = Model::kAppHoverMax, step = Model::kAppHoverStep;
    float v = m.appHoverMs / (float)hi;
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.apps.ms"), Rect(in.left, row.bottom + 6, Width(in), kSlideH), L"Hover delay over apps", std::to_wstring(m.appHoverMs) + L" ms", v,
             live && can, done, (float)step / hi))
        m.appHoverMs = (int)std::lround(v * hi / step) * step; // 50 ms steps
    if (done) m.SetPref(m.appHoverMs, L"AppHoverMs", m.appHoverMs);
    if (layered) c.Target()->PopLayer();
}

constexpr float kKeyW = 128; // a fullscreen key's capture chip

void Fullscreen(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the keys that split the fullscreen player across two screens, and swap its halves
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    StagePrefs& sp = StagePrefs::Get();
    D2D1_RECT_F in = Box(c, t, r, L"FULLSCREEN KEYS");
    const wchar_t* labels[2] = {L"Two screens", L"Swap screens"};
    UINT keys[2] = {sp.keySplit, sp.keySwap};
    for (int i = 0; i < 2; ++i) { // label left, the key on the right: click it, then press the new one
        D2D1_RECT_F row = Rect(in.left, in.top + i * (kRowH + 4), Width(in), kRowH);
        bool waiting = sp.capture == i + 1;
        c.Text(labels[i], {row.left, row.top, row.right - kKeyW - 8, row.bottom}, Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.25f));
        D2D1_RECT_F chip = {row.right - kKeyW, row.top + 1, row.right, row.bottom - 1};
        if (w::Chip(ui, t, Ui::Id("set.fullkey", i), chip, waiting ? std::wstring(L"Press a key") : StagePrefs::Name(keys[i]), waiting) && live) sp.capture = waiting ? 0 : i + 1;
    }
    if (sp.capture && ui.In().pressed && !Inside({in.right - kKeyW, in.top, in.right, in.top + 2 * (kRowH + 4)}, ui.In().mouse)) sp.capture = 0; // clicked away: the key stays as it was
    Note(c, t, in, in.top + 2 * (kRowH + 4) + 2, sp.capture ? L"Esc keeps the key it had" : L"While fullscreen is open, with a second screen");
}

void Display(PageCtx& p, const D2D1_RECT_F& r, bool live) { // how big everything is
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"DISPLAY");
    static int scaleDrag = -1; // shown while dragging, applied on release: the panel resizing under the mouse would fight the drag
    if (!ui.IsActive(Ui::Id("set.scale"))) scaleDrag = -1; // let go anywhere, or the panel closed: back to what's saved
    float sv = ((scaleDrag >= 0 ? scaleDrag : m.uiScale) - kScaleMin) / (float)(kScaleMax - kScaleMin);
    bool sdone = false;
    if (Knob(ui, t, Ui::Id("set.scale"), Rect(in.left, in.top + 2, Width(in), kSlideH), L"Size",
             std::to_wstring(scaleDrag >= 0 ? scaleDrag : m.uiScale) + L"%", sv, live, sdone, 5.0f / (kScaleMax - kScaleMin)))
        scaleDrag = kScaleMin + (int)std::lround(sv * (kScaleMax - kScaleMin) / 5) * 5; // 5% steps
    if (sdone && scaleDrag >= 0) {
        m.SetPref(m.uiScale, L"UiScale", scaleDrag);
        scaleDrag = -1;
    }
    Note(c, t, in, in.top + kSlideH + 8, L"The panel, the pill and their text, 80 to 130%");
    Note(c, t, in, in.top + kSlideH + 26, L"Applied when you let go of the slider");
}

void LyricType(PageCtx& p, const D2D1_RECT_F& r, bool live) { // how big the fullscreen player's words are
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"LYRICS");
    constexpr int lo = Model::kLyricMin, hi = Model::kLyricMax;
    float v = (m.lyricScale - lo) / (float)(hi - lo);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.lyr.size"), Rect(in.left, in.top + 2, Width(in), kSlideH), L"Lyric size", std::to_wstring(m.lyricScale) + L"%", v, live, done,
             (float)kLyricStep / (hi - lo)))
        m.lyricScale = lo + (int)std::lround(v * (hi - lo) / kLyricStep) * kLyricStep; // live: an open player re-lays as it moves
    if (done) m.SetPref(m.lyricScale, L"LyricScale", m.lyricScale);
    Note(c, t, in, in.top + kSlideH + 8, L"The fullscreen and split lyrics, 80 to 150%");
    Note(c, t, in, in.top + kSlideH + 26, m.fullLyrics ? L"100% is the usual size" : L"Synced lyrics are switched off for now");
}

void Weather(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a city typed in, instead of finding you by your ip address
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"WEATHER");
    std::wstring city = m.weather.City();
    D2D1_RECT_F field = Rect(in.left, in.top + 2, Width(in) - (city.empty() || m.cityFocus ? 0 : 36), 30);
    uint32_t id = Ui::Id("set.city");
    Ui::Hit h = live ? ui.Button(id, field) : Ui::Hit{};
    c.Fill(field, 15, t.track.Alpha(0.7f));
    c.Stroke(field, 15, m.cityFocus ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, m.cityFocus ? 1.5f : 1);
    c.Icon(glyph::Place, {field.left + 16, (field.top + field.bottom) / 2}, 16, m.cityFocus ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {field.left + 30, field.top, field.right - 10, field.bottom};
    if (m.cityFocus) {
        float wdt = c.TextOnce(m.cityEdit, text, Weight::Regular, 12, t.text);
        if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({text.left + wdt + 1.5f, field.top + 8}, {text.left + wdt + 1.5f, field.bottom - 8}, t.text, 1.5f); // caret
        if (m.cityEdit.empty()) c.Text(L"Type a city, enter to search", {text.left + 6, text.top, text.right, text.bottom}, Weight::Light, 12, t.textLow);
        ui.KeepAlive();
        if (ui.In().pressed && !Inside(field, ui.In().mouse)) m.CommitCity(); // clicked away: search what's there
    } else {
        c.Text(city.empty() ? L"Automatic, from your connection" : city, text, Weight::Regular, 12, city.empty() ? t.textDim : t.text);
    }
    if (h.clicked && !m.cityFocus) {
        m.cityFocus = true;
        m.cityEdit = city;
    }
    if (!city.empty() && !m.cityFocus &&
        w::IconBtn(ui, t, Ui::Id("set.city.x"), {in.right - 15, field.top + 15}, 28, glyph::Close, 16, live)) { // back to automatic
        m.cityEdit.clear();
        m.CommitCity();
    }
    int st = m.weather.State();
    const wchar_t* note = st == sys::WxSvc::kCitySearching ? L"Looking it up…"
                        : st == sys::WxSvc::kCityMissing ? L"No place by that name, try another spelling"
                        : city.empty() ? L"Your IP address finds the nearest city"
                                       : L"Your IP address is never used to look you up";
    Note(c, t, in, in.top + 40, note);
    Note(c, t, in, in.top + 58, L"Forecasts and city search by Open-Meteo");
}

void ShelfIntake(PageCtx& p, const D2D1_RECT_F& r, bool live) { // what the shelf takes in, and whether it outlasts a restart
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"SHELF");
    if (SwitchRow(ui, t, "set.shelf.cap", Rect(in.left, in.top, Width(in), kRowH), L"Everything you copy goes to the Shelf", m.shelfCapture != 0, live)) {
        Flip(m, m.shelfCapture, L"ShelfCapture");
        m.SyncShelf(); // starts or stops listening right away
    }
    bool keep = m.shelf.Keep();
    if (SwitchRow(ui, t, "set.shelf.keep", Rect(in.left, in.top + kRowH, Width(in), kRowH), L"Keep after restart, encrypted", keep, live)) m.shelf.SetKeep(!keep);
    if (SwitchRow(ui, t, "set.shelf.wipe", Rect(in.left, in.top + 2 * kRowH, Width(in), kRowH), L"Clear unpinned at each start", m.shelfClearStart != 0, live && keep, keep)) {
        Flip(m, m.shelfClearStart, L"ShelfClearStart");
        m.SyncShelf();
    }
    Note(c, t, in, in.top + 3 * kRowH + 2, m.shelfCapture ? L"Private and password copies are never taken" : L"Off: only what you drop on it");
}

void ShelfHistory(PageCtx& p, const D2D1_RECT_F& r, bool live) { // how much the shelf keeps, and for how long
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"SHELF HISTORY");
    constexpr int lo = sys::Shelf::kHistMin, hi = sys::Shelf::kHistMax, step = sys::Shelf::kHistStep;
    float v = (m.shelfHistory - lo) / (float)(hi - lo);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.shelf.hist"), Rect(in.left, in.top + 2, Width(in), kSlideH), L"Keep up to", std::to_wstring(m.shelfHistory) + L" cards", v, live, done,
             (float)step / (hi - lo)))
        m.shelfHistory = lo + (int)std::lround(v * (hi - lo) / step) * step;
    if (done) {
        m.SetPref(m.shelfHistory, L"ShelfHistory", m.shelfHistory);
        m.SyncShelf(); // trimmed on let go, never mid-drag
    }
    c.Text(L"Delete after", Rect(in.left, in.top + kSlideH + 2, Width(in), 16), Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.25f));
    static const wchar_t* const kAges[sys::Shelf::kExpireNever + 1] = {L"1 h", L"6 h", L"24 h", L"7 d", L"Never"};
    int pick = Segmented(ui, t, "set.shelf.age", Rect(in.left, in.top + kSlideH + 19, Width(in), kKnobH + 4), kAges, sys::Shelf::kExpireNever + 1, m.shelfExpire, live);
    if (pick >= 0) {
        m.SetPref(m.shelfExpire, L"ShelfExpire", pick);
        m.SyncShelf();
    }
    Note(c, t, in, in.top + kSlideH + kKnobH + 27, L"Pinned and noted cards always stay");
}

void Hud(PageCtx& p, const D2D1_RECT_F& r, bool live) { // astraeus' own volume pill in place of windows' popup
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"VOLUME AND BRIGHTNESS");
    D2D1_RECT_F row = Rect(in.left, in.top, Width(in), 2 * kNoteH);
    Ui::Hit h = live ? ui.Button(Ui::Id("set.hud", 1), {row.left, row.top, row.right - 52, row.bottom}) : Ui::Hit{}; // the words flip it too
    if (live) ui.NoStop();
    Color ink = h.hover ? t.text : t.text.Mix(t.textDim, 0.25f);
    c.Text(L"Replace Windows volume popup", Rect(row.left, row.top, Width(row) - 56, kNoteH), Weight::Regular, 11.5f, ink); // two lines: the whole name, nothing cut
    c.Text(L"(experimental)", Rect(row.left, row.top + kNoteH, Width(row) - 56, kNoteH), Weight::Regular, 11.5f, t.textDim);
    bool hudFlip = w::Switch(ui, t, Ui::Id("set.hud"), {row.right - 24, (row.top + row.bottom) / 2}, m.hud != 0, live);
    if (live) ui.Name(L"Replace Windows volume popup, experimental");
    if (hudFlip || h.clicked) Flip(m, m.hud, L"Hud");
    Note(c, t, in, row.bottom + 4, L"A pill on every volume change. While on, the");
    Note(c, t, in, row.bottom + 20, L"volume keys are taken from Windows.");
    Note(c, t, in, row.bottom + 36, L"Brightness keys still show its popup too.");
}

void Backup(PageCtx& p, const D2D1_RECT_F& r, bool live) { // export, import, start over
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    D2D1_RECT_F in = Box(c, t, r, L"BACKUP");
    float bw = (Width(in) - 8) / 2;
    static HWND host = nullptr; // the dialogs and the restart live there; looked up once, not every frame
    if (!IsWindow(host)) host = FindWindowW(L"AstraeusHost", nullptr);
    if (Act(ui, t, Ui::Id("set.export"), Rect(in.left, in.top + 2, bw, 30), L"Export…", false, false, live) && host) {
        p.wantClose = true;
        PostMessageW(host, WM_COMMAND, tray::kExport, 0);
    }
    if (Act(ui, t, Ui::Id("set.import"), Rect(in.left + bw + 8, in.top + 2, bw, 30), L"Import…", false, false, live) && host) {
        p.wantClose = true;
        PostMessageW(host, WM_COMMAND, tray::kImport, 0);
    }
    D2D1_RECT_F reset = Rect(in.left, in.top + 42, Width(in), 30); // held, like shut down: nothing to undo it
    uint32_t id = Ui::Id("set.reset");
    float progress = 0;
    bool fire = live && ui.Hold(id, reset, kHoldSec, progress);
    bool hover = live && Inside(reset, ui.In().mouse);
    float rad = Height(reset) / 2;
    c.Fill(reset, rad, t.track.Mix(kDanger.Alpha(0.25f), hover ? 1.0f : 0.4f));
    if (progress > 0) c.Fill({reset.left, reset.top, reset.left + Width(reset) * progress, reset.bottom}, rad, kDanger.Alpha(0.55f));
    c.Stroke(reset, rad, kDanger.Alpha(hover ? 0.8f : 0.4f), 1);
    c.Text(progress > 0 ? L"Keep holding…" : hover ? L"Hold to reset every setting" : L"Reset to defaults", reset, Weight::Medium, 12, hover ? t.text : t.textDim, Align::Center);
    if (fire && host) PostMessageW(host, WM_COMMAND, tray::kReset, 0); // astraeus restarts on its defaults
}

constexpr wchar_t kIssues[] = L"https://github.com/Icetist/Astraeus/issues/new"; // never prefilled: a report only travels if the user pastes it
constexpr double kCopiedSec = 2.5; // how long the button says what happened

void Problem(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a report for the clipboard, and where to take it; nothing is sent from here
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    D2D1_RECT_F in = Box(c, t, r, L"REPORT A PROBLEM");
    float bw = (Width(in) - 8) / 2;
    static double copiedAt = -10;
    static bool copyOk = false;
    bool told = ui.Now() - copiedAt < kCopiedSec;
    if (told) ui.Wake(); // it has to turn back with nothing else going on
    const wchar_t* label = !told ? L"Copy report" : copyOk ? L"Copied" : L"Couldn't copy";
    if (Act(ui, t, Ui::Id("set.report"), Rect(in.left, in.top + 2, bw, 30), label, true, false, live)) {
        copyOk = sys::CopyText(sys::ProblemReport());
        copiedAt = ui.Now();
    }
    if (Act(ui, t, Ui::Id("set.report.open"), Rect(in.left + bw + 8, in.top + 2, bw, 30), L"Open GitHub", false, false, live)) {
        p.wantClose = true;
        sys::OpenUri(kIssues);
    }
    Note(c, t, in, in.top + 42, L"Version, PC specs, main settings, recent log,");
    Note(c, t, in, in.top + 42 + kNoteH, L"with your name taken out. Nothing is sent:");
    Note(c, t, in, in.top + 42 + 2 * kNoteH, L"read it, then paste it where you choose.");
}

constexpr float kFieldH = 30; // the code card's field
constexpr double kShakeSec = 0.32; // a wrong code: a short shake, then still
constexpr float kShakeAmp = 5, kShakeHz = 4.5f;

void Code(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a plain field: enter checks it, and it clears either way
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Entry& e = Entry::Get();
    D2D1_RECT_F in = Box(c, t, r, L"CODE");
    if (e.Settle(ui.Now())) ui.KeepAlive(); // a check running: frames enough to see it land
    float since = (float)(ui.Now() - e.wrongAt), shake = 0;
    if (since < kShakeSec) {
        shake = kShakeAmp * std::sin(since * kShakeHz * 2 * kPi) * (1 - since / (float)kShakeSec);
        ui.Wake();
    }
    D2D1_RECT_F home = Rect(in.left, in.top + 2, Width(in), kFieldH), field = Rect(in.left + shake, in.top + 2, Width(in), kFieldH);
    Ui::Hit h = live ? ui.Button(Ui::Id("set.code"), home) : Ui::Hit{};
    c.Fill(field, kFieldH / 2, t.track.Alpha(0.7f));
    c.Stroke(field, kFieldH / 2, e.focus ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, e.focus ? 1.5f : 1);
    D2D1_RECT_F text = {field.left + 14, field.top, field.right - 30, field.bottom};
    if (e.focus) {
        float wdt = e.text.empty() ? 0 : c.TextOnce(e.text, text, Weight::Regular, 12, t.text); // shown as typed, never cached
        if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({text.left + wdt + 1.5f, field.top + 8}, {text.left + wdt + 1.5f, field.bottom - 8}, t.text, 1.5f); // caret
        ui.KeepAlive();
        if (ui.In().pressed && !Inside(home, ui.In().mouse)) e.Drop(); // clicked away: nothing kept
    }
    if (e.Working()) Spinner(c, {field.right - 16, (field.top + field.bottom) / 2}, 6, t.textDim, ui.Now());
    if (h.clicked && !e.focus) e.focus = true;
}

constexpr float kAccessWide = 520; // at least this wide: its two groups side by side
constexpr float kAccessSeg = 16 + 28 + 6, kAccessLeft = kAccessSeg + kRowH + kSlideH + 8, kAccessRight = 2 * kRowH + 4 + 4 * kNoteH + 4; // their heights
float AccessH(float w) { return kBoxChrome + (w - 24 >= kAccessWide ? (std::max)(kAccessLeft, kAccessRight) : kAccessLeft + kAccessRight + 6); }

void Accessibility(PageCtx& p, const D2D1_RECT_F& r, bool live) { // for eyes that need more, and for hands and ears instead of the mouse
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"ACCESSIBILITY");
    bool wide = Width(in) >= kAccessWide;
    float colW = wide ? (Width(in) - 20) / 2 : Width(in);
    c.Text(L"High contrast", Rect(in.left, in.top, colW, 16), Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.25f));
    static const wchar_t* const kModes[] = {L"With Windows", L"On", L"Off"};
    int pick = Segmented(ui, t, "set.a11y.hc", Rect(in.left, in.top + 16, colW, 28), kModes, 3, m.a11yContrast, live);
    if (pick >= 0) m.SetPref(m.a11yContrast, L"A11yContrast", pick);
    float y = in.top + kAccessSeg;
    if (SwitchRow(ui, t, "set.a11y.follow", Rect(in.left, y, colW, kRowH), L"Follow Windows text size", m.a11yFollow != 0, live)) Flip(m, m.a11yFollow, L"A11yFollowText");
    y += kRowH;
    static int textDrag = -1; // shown while dragging, applied on release, as the display size: the panel resizing under the mouse would fight the drag
    uint32_t tid = Ui::Id("set.a11y.text");
    if (!ui.IsActive(tid)) textDrag = -1;
    constexpr int kTextStep = 10;
    int shown = textDrag >= 0 ? textDrag : m.a11yText;
    float v = (shown - Model::kTextMin) / (float)(Model::kTextMax - Model::kTextMin);
    bool done = false;
    if (Knob(ui, t, tid, Rect(in.left, y, colW, kSlideH), L"Text size", std::to_wstring(shown) + L"%", v, live, done, (float)kTextStep / (Model::kTextMax - Model::kTextMin)))
        textDrag = Model::kTextMin + (int)std::lround(v * (Model::kTextMax - Model::kTextMin) / kTextStep) * kTextStep;
    if (done && textDrag >= 0) {
        m.SetPref(m.a11yText, L"A11yText", textDrag);
        textDrag = -1;
    }
    float x = wide ? in.left + colW + 20 : in.left;
    y = wide ? in.top : y + kSlideH + 14;
    if (SwitchRow(ui, t, "set.a11y.keys", Rect(x, y, colW, kRowH), L"Keyboard navigation", m.a11yKeys != 0, live)) Flip(m, m.a11yKeys, L"A11yKeys");
    y += kRowH;
    if (SwitchRow(ui, t, "set.a11y.reader", Rect(x, y, colW, kRowH), L"Screen reader support", m.a11yReader != 0, live)) Flip(m, m.a11yReader, L"A11yReader");
    y += kRowH + 4;
    D2D1_RECT_F notes = Rect(x, 0, colW, 0);
    Note(c, t, notes, y, L"Open with the shortcut, then Tab moves, Enter presses,");
    Note(c, t, notes, y + kNoteH, L"arrows adjust, S opens settings, Esc steps back");
    Note(c, t, notes, y + 2 * kNoteH, L"Ctrl+Alt+Shift+D: the keyboard into a dashboard");
    Note(c, t, notes, y + 3 * kNoteH, L"For Narrator, NVDA and JAWS; nothing runs without one");
}

constexpr float kAdvHeadH = 56; // the advanced section's bar
constexpr float kAdvRow = 150; // its cards: room for a slider and its notes
constexpr float kCodeRow = 76;
bool advOpen = false; // folded each launch: the everyday settings come first
constexpr float kUpdH = 64, kUpdBtnW = 170; // the update card under advanced, and its button

void Updates(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the version, and a check right now: the tray menu's row, here too
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    D2D1_RECT_F in = Box(c, t, r, L"ASTRAEUS");
    static HWND host = nullptr; // the updater lives there; looked up once, not every frame
    if (!IsWindow(host)) host = FindWindowW(L"AstraeusHost", nullptr);
    bool busy = tray::UpdateBusy();
    float cy = (in.top + in.bottom) / 2;
    if (Act(ui, t, Ui::Id("set.update"), Rect(in.left, cy - 15, kUpdBtnW, 30), L"Check for updates", true, busy, live && !busy) && host) {
        tray::SetUpdate(tray::Upd::Checking);
        PostMessageW(host, WM_COMMAND, tray::kUpdate, 0); // the same check the tray menu runs
    }
    if (busy) ui.KeepAlive(); // the words follow the check as it goes
    std::wstring state = tray::UpdateText(), note = L"v" APP_VER_W;
    if (state != L"Check for updates") note += L"  ·  " + state; // idle: just the version
    c.Text(note, Rect(in.left + kUpdBtnW + 14, in.top, Width(in) - kUpdBtnW - 14, Height(in)), Weight::Regular, 11.5f, t.textDim);
}

void AdvancedHead(PageCtx& p, const D2D1_RECT_F& r, bool live, float open) { // a bar that unfolds the rest
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    uint32_t id = Ui::Id("set.adv");
    Ui::Hit h = live ? ui.Button(id, r) : Ui::Hit{};
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(r, 16, t.cardHi.Alpha(0.55f).Mix(t.track, 0.4f * hover));
    c.StrokeV(r, 16, t.hairTop.Mix(t.accent.Alpha(0.6f), open), t.hairBot, 1);
    D2D1_POINT_2F badge = {r.left + 30, (r.top + r.bottom) / 2};
    c.Dot(badge, 16, t.track.Mix(t.accent.Alpha(0.3f), open));
    c.Icon(glyph::Settings, badge, 19, t.textDim.Mix(t.accentHi, open), open > 0.5f);
    c.Text(L"Advanced", Rect(r.left + 56, r.top + 9, Width(r) - 110, 20), Weight::Medium, 13, t.text);
    c.Text(L"Accessibility, power, weather, shelf, volume keys, backup and more",Rect(r.left + 56, r.top + 29, Width(r) - 110, 16), Weight::Light, 10.5f, t.textLow);
    D2D1_POINT_2F o = {r.right - 26, (r.top + r.bottom) / 2}; // a chevron that turns over as it opens
    float a = open * kPi, s = 5, cs = std::cos(a), sn = std::sin(a);
    auto turn = [&](float x, float y) { return D2D1_POINT_2F{o.x + x * cs - y * sn, o.y + x * sn + y * cs}; };
    Color ink = t.textDim.Mix(t.text, hover);
    c.Line(turn(-s, -s * 0.5f), turn(0, s * 0.5f), ink, 2);
    c.Line(turn(0, s * 0.5f), turn(s, -s * 0.5f), ink, 2);
    if (h.clicked) advOpen = !advOpen;
}

float Advanced(PageCtx& p, const D2D1_RECT_F& area, bool live, int cols) { // power, weather, volume keys, backup, display and lyric sizes, the live page's options, the code; returns its full height
    float cw = (Width(area) - (cols - 1) * kPrefGap) / cols, y = area.top;
    int n = 0;
    auto next = [&] {
        D2D1_RECT_F r = Rect(area.left + (n % cols) * (cw + kPrefGap), y + (n / cols) * (kAdvRow + kPrefGap), cw, kAdvRow);
        ++n;
        return r;
    };
    PanelOpts(p, next(), live); // pinned and floating, for either layout
    Power(p, next(), live);
    Weather(p, next(), live);
    Hud(p, next(), live);
    Backup(p, next(), live);
    Display(p, next(), live); // the two sizes side by side
    LyricType(p, next(), live);
    Opening(p, next(), live); // how the closed island opens: anywhere, then over apps
    Apps(p, next(), live);
    ArtBack(p, next(), live); // the album art behind, and the music's light
    Visualizer(p, next(), live);
    ShelfIntake(p, next(), live); // the shelf's intake and its history, side by side
    ShelfHistory(p, next(), live);
    Problem(p, next(), live);
    y +=((n + cols - 1) / cols) * (kAdvRow + kPrefGap);
    float ah = AccessH(Width(area));
    Accessibility(p, Rect(area.left, y, Width(area), ah), live);
    y += ah + kPrefGap;
    Code(p, Rect(area.left, y, Width(area), kCodeRow), live); // always last
    y += kCodeRow + kPrefGap;
    return y - area.top;
}

}

float DrawPrefs(PageCtx& p, const D2D1_RECT_F& area, bool live, bool tall) {
    int cols = tall ? 1 : 2;
    float cw = (Width(area) - (cols - 1) * kPrefGap) / cols, y = area.top;
    float ph = PagesH(Width(area)), th = TilesH(p.m, Width(area)); // the two editors run across, as tall as their cells need
    Pages(p, Rect(area.left, y, Width(area), ph), live);
    y += ph + kPrefGap;
    Tiles(p, Rect(area.left, y, Width(area), th), live);
    y += th + kPrefGap;
    float wh = WidgetsCardH(p.m, Width(area)); // clocks, money, stocks and the rest, for the widgets page
    WidgetsCard(p, Rect(area.left, y, Width(area), wh), live);
    y += wh + kPrefGap;
    int n = 0;
    auto next = [&] { // the next card's place, filling rows left to right
        D2D1_RECT_F r = Rect(area.left + (n % cols) * (cw + kPrefGap), y + (n / cols) * (kPrefRow + kPrefGap), cw, kPrefRow);
        ++n;
        return r;
    };
    Layout(p, next(), live); // how the panel shows, and the dashboard beside it
    Dashboard(p, next(), live);
    Pill(p, next(), live);
    Media(p, next(), live);
    Fullscreen(p, next(), live);
    y += ((n + cols - 1) / cols) * (kPrefRow + kPrefGap);

    Ui& ui = p.ui;
    float open = ui.Smooth(Ui::Id("set.adv", 2), advOpen ? 1.0f : 0, 11);
    if (!advOpen && Entry::Get().focus) Entry::Get().Drop(); // folded away: nothing left typed
    AdvancedHead(p, Rect(area.left, y, Width(area), kAdvHeadH), live, open);
    y += kAdvHeadH + kPrefGap;
    static float advH = 0; // its height as it measured last frame
    if (open > 0.001f) { // unfolds downward, the cards revealed as it goes
        float shown = advH * open;
        ui.C().PushClip({area.left - 4, y - 4, area.right + 4, y + shown});
        advH = Advanced(p, {area.left, y - (1 - open) * 24, area.right, area.bottom}, live && open > 0.99f, cols); // and rises into place
        ui.C().PopClip();
        y += shown;
    }
    Updates(p, Rect(area.left, y, Width(area), kUpdH), live); // under advanced, open or folded
    y += kUpdH + kPrefGap;
    return y - area.top;
}
