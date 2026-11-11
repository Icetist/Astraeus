// The widgets page: a bento board of small, wide and large tiles that pack together with no holes.
// They're arranged and added to in the studio: the same stage as the long pill's editor, the board over the wallpaper and a gallery of live previews popped out under it.
// Everything here is Astraeus' own: nothing asks HaloBar for anything.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "anim.h"
#include "faces.h"
#include "faces_state.h"
#include "feeds.h"
#include "glyphs.h"
#include "history.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

enum Size { kSmall, kWide, kLarge };
constexpr float kCellMinW = 148, kCellH = 112, kGap = 10, kRadius = 18; // the board's grid
constexpr float kBarH = 36, kBarBtnW = 110, kBarBtnH = 26; // the row over the board
constexpr float kCtlSize = 24, kCtlGap = 3, kCtlInset = 7, kDragStart = 6; // arranging: the controls on a tile
constexpr double kSwapGap = 0.16; // a held tile moves its neighbours no faster than this: different sizes can't flip back and forth
constexpr float kGalMinW = 176, kGalCardH = 146, kGalThumbH = 98, kSearchW = 240, kSearchH = 30, kCatH = 26, kCatGap = 6, kCatPad = 22, kCharW = 6.6f; // the gallery
constexpr float kShownW = 168, kShownWideW = 340, kShownLargeH = 234; // a preview's tile before it's scaled down: about what it is on the board
constexpr float kPanelMaxW = 1060, kPanelMaxH = 372, kPanelShare = 0.47f; // the studio's gallery

int held = -1; // the tile being dragged
std::vector<int> heldOrder; // the board as it would land
D2D1_POINT_2F heldGrab{};
double lastSwap = 0;
int visit = 0; // the page visit all that belongs to
float boardScroll = 0, studioScroll = 0, galScroll = 0;
int galCat = -1; // -1: every category
int shownKind = -1, fromKind = -1; // the widget open in full, kept while it folds away; the one whose tile it grew out of
D2D1_RECT_F openFrom{}; // that tile, where it was on the board
int scoresKind = -1; // the scores tile, where it was last drawn: the wheel over it scrolls its games
D2D1_RECT_F scoresAt{};
History hist; // the editor's undo: the board's widgets in order, then each one's size

History::Snap Snap(const Model& m) {
    History::Snap s{(float)m.faceOn.size()};
    for (int k : m.faceOn) s.push_back((float)k);
    for (const auto& [k, z] : m.faceSize) s.push_back((float)k), s.push_back((float)z);
    return s;
}

void Load(Model& m, const History::Snap& s) {
    if (s.empty()) return;
    size_t n = (size_t)s[0], i = 1;
    std::vector<int> on;
    for (; i <= n && i < s.size(); ++i) on.push_back((int)s[i]);
    std::map<int, int> sizes;
    for (; i + 1 < s.size(); i += 2) sizes[(int)s[i]] = (int)s[i + 1];
    held = -1;
    m.SetFaces(on, sizes);
}

bool Offered(int kind) { return strcmp(faces::Info(kind).key, "sys.tray") != 0; } // the tray is HaloBar's to show; here windows' own stays

int DefaultSize(int kind) { // the first arrangement already has some shape to it: faces with a lot to say get the room for it
    static const char* const kBig[] = {"clock.analog", "clock.analogdark", "clock.analogmin", "prog.year", "web.githubgrid"};
    const faces::Meta& f = faces::Info(kind);
    for (const char* key : kBig)
        if (!strcmp(f.key, key)) return kLarge;
    constexpr float kWideFrom = 176; // along a dock, this wide or more: it reads across
    return f.wMedium >= kWideFrom ? kWide : kSmall;
}

int SizeOf(const Model& m, int kind) {
    auto it = m.faceSize.find(kind);
    return it != m.faceSize.end() ? it->second : DefaultSize(kind);
}

struct Cell {
    int x = 0, y = 0, w = 1, h = 1;
};

std::vector<Cell> Pack(const std::vector<int>& order, const Model& m, int cols, int& rows) { // each in turn takes the first place it fits: small ones fall back into any gap a big one left
    std::vector<char> used;
    auto taken = [&](int x, int y) { size_t at = (size_t)y * cols + x; return at < used.size() && used[at]; };
    std::vector<Cell> out;
    rows = 0;
    for (int kind : order) {
        int size = SizeOf(m, kind);
        Cell c;
        c.w = size == kSmall ? 1 : (std::min)(2, cols);
        c.h = size == kLarge ? 2 : 1;
        for (bool placed = false; !placed; ++c.y) {
            for (c.x = 0; c.x + c.w <= cols && !placed; ++c.x) {
                placed = true;
                for (int yy = c.y; yy < c.y + c.h; ++yy)
                    for (int xx = c.x; xx < c.x + c.w; ++xx) placed = placed && !taken(xx, yy);
                if (placed) break;
            }
            if (placed) break;
        }
        used.resize((std::max)(used.size(), (size_t)(c.y + c.h) * cols), 0);
        for (int yy = c.y; yy < c.y + c.h; ++yy)
            for (int xx = c.x; xx < c.x + c.w; ++xx) used[(size_t)yy * cols + xx] = 1;
        rows = (std::max)(rows, c.y + c.h);
        out.push_back(c);
    }
    return out;
}

void DrawFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r, const faces::Host& host) {
    c.PushClip(r);
    faces::Draw(c, t, kind, 2, r, host);
    c.PopClip();
}

void Preview(Ui& ui, const Theme& t, int kind, const D2D1_RECT_F& box, const faces::Host& host) { // the tile as it will be on the board, made small: never a squeezed layout of its own
    Canvas& c = ui.C();
    int size = DefaultSize(kind);
    float w = size == kSmall ? kShownW : kShownWideW, h = size == kLarge ? kShownLargeH : kCellH;
    float k = (std::min)((std::min)(Width(box) / w, Height(box) / h), 1.0f);
    ID2D1RenderTarget* rt = c.Target();
    D2D1_MATRIX_3X2_F was;
    rt->GetTransform(&was);
    D2D1_POINT_2F at = {(box.left + box.right - w * k) / 2, (box.top + box.bottom - h * k) / 2};
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(at.x, at.y) * *D2D1::Matrix3x2F::ReinterpretBaseType(&was));
    D2D1_RECT_F tile = Rect(0, 0, w, h);
    w::Card(ui, t, tile, kRadius);
    faces::Sampling(true); // made-up numbers: what it looks like, before anything's connected
    DrawFace(c, t, kind, tile, host);
    faces::Sampling(false);
    rt->SetTransform(was);
}

faces::Host MakeHost(Model& m) {
    faces::Host h;
    const auto& wx = m.wx;
    h.cpu = m.cpuHist.Last(), h.mem = m.ramHist.Last(), h.memUsedGb = m.stats.memUsedGb, h.memTotalGb = m.stats.memTotalGb;
    h.wxValid = wx.valid, h.wxDay = wx.isDay, h.fahrenheit = (m.fahr != 0), h.cards = true;
    wchar_t format[32] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, format, 32);
    h.h24 = wcschr(format, L'H') != nullptr;
    h.tempC = wx.tempC, h.highC = wx.highC, h.lowC = wx.lowC, h.wxCode = wx.code, h.place = wx.place;
    h.nHours = (std::min)(wx.nHours, 8), h.nDays = (std::min)(wx.nDays, 5);
    for (int i = 0; i < h.nHours; ++i) h.hours[i] = {wx.hours[i].hour, wx.hours[i].code, wx.hours[i].tempC, wx.hours[i].day};
    for (int i = 0; i < h.nDays; ++i) h.days[i] = {wx.days[i].y, wx.days[i].m, wx.days[i].d, wx.days[i].code, wx.days[i].hiC, wx.days[i].loC};
    h.hasTrack = m.hasTrack, h.playing = m.hasTrack && m.track.playing, h.title = m.track.title, h.artist = m.track.artist;
    h.progress = m.hasTrack ? m.track.Progress() : 0;
    h.bright = m.brightPct;
    h.ctx = &m;
    h.setBright = [](void* self, int percent) { static_cast<Model*>(self)->SetBright(percent / 100.0f); };
    h.resync = [](void* self) { static_cast<Model*>(self)->SyncFaces(); }; // new leagues followed: fetched from now
    return h;
}

void DoAct(Model& m, faces::Act act, int kind) {
    switch (act) {
    case faces::Act::kPanel:
        m.OpenFace(kind);
        break;
    case faces::Act::kPlay:
        m.media.TogglePlay();
        break;
    case faces::Act::kNext:
        m.media.Next();
        break;
    case faces::Act::kPrev:
        m.media.Previous();
        break;
    default:
        break;
    }
}

bool Found(const faces::Meta& f, const std::wstring& query) { // what's typed, anywhere in its name or what it says of itself
    if (query.empty()) return true;
    auto has = [&](const wchar_t* text) {
        std::wstring hay = text, needle = query;
        CharLowerBuffW(hay.data(), (DWORD)hay.size());
        CharLowerBuffW(needle.data(), (DWORD)needle.size());
        return hay.find(needle) != std::wstring::npos;
    };
    return has(f.name) || has(f.about);
}

void SearchField(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    constexpr int kSearchGlyph = 0xE8B6;
    bool clear = !m.faceQuery.empty();
    D2D1_RECT_F field = {r.left, r.top, r.right - (clear ? 30 : 0), r.bottom};
    Ui::Hit h = ui.Button(Ui::Id("wg.search"), field);
    ui.Name(L"Search widgets", m.faceQuery);
    float rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    c.Stroke(r, rad, m.faceSearch ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, m.faceSearch ? 1.5f : 1);
    c.Icon(kSearchGlyph, {r.left + 15, (r.top + r.bottom) / 2}, 16, m.faceSearch ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {r.left + 30, r.top, field.right - 8, r.bottom};
    if (m.faceSearch) {
        float w = m.faceQuery.empty() ? 0 : c.TextOnce(m.faceQuery, text, Weight::Regular, 12, t.text); // typed text isn't cached
        if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({text.left + w + 1.5f, r.top + 7}, {text.left + w + 1.5f, r.bottom - 7}, t.text, 1.5f); // caret
        ui.KeepAlive();
        if (ui.In().pressed && !Inside(r, ui.In().mouse)) m.faceSearch = false; // clicked away: the search stays, the keyboard goes back
    } else {
        c.Text(clear ? m.faceQuery : L"Search widgets", text, Weight::Regular, 12, clear ? t.text : t.textLow);
    }
    if (h.clicked) m.faceSearch = true;
    if (clear && w::IconBtn(ui, t, Ui::Id("wg.search.x"), {r.right - 15, (r.top + r.bottom) / 2}, 26, glyph::Close, 15)) m.faceQuery.clear();
}

void Gallery(PageCtx& p, const D2D1_RECT_F& panel, const faces::Host& host) { // every widget with its real tile on it: a tap puts it on the board or takes it off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    constexpr float kPadX = 18, kPadY = 14, kTitleW = 112, kDoneW = 84, kDoneH = 30;
    D2D1_RECT_F a = Inset(panel, kPadX, kPadY);
    float resetX = a.right - kDoneW - 8 - history::kResetW, histX = resetX - 8 - history::kW;
    c.Text(L"Add widgets", Rect(a.left + 2, a.top, kTitleW, kDoneH), Weight::Semi, 15, t.text);
    SearchField(p, Rect(a.left + kTitleW, a.top, (std::min)(kSearchW, histX - a.left - kTitleW - 12), kSearchH));
    if (Act(ui, t, Ui::Id("wg.gal.done"), Rect(a.right - kDoneW, a.top, kDoneW, kDoneH), L"Done", true, false, true)) p.wantClose = true;
    if (Act(ui, t, Ui::Id("wg.gal.reset"), Rect(resetX, a.top, history::kResetW, kDoneH), L"Reset", false, false, !m.faceOn.empty() || !m.faceSize.empty())) Load(m, {0}); // an empty board, as it starts; undo brings it back
    int go = history::Buttons(ui, t, "wg.gal.hist", histX, a.top + kDoneH / 2, hist, true);
    if (go < 0) Load(m, hist.Undo());
    else if (go > 0) Load(m, hist.Redo());
    float tx = a.left, ty = a.top + kDoneH + 10;
    for (int k = -1; k < faces::kCats; ++k) { // the categories, wrapping when the screen is narrow
        const wchar_t* name = k < 0 ? L"All" : faces::CatName(k);
        float cw = kCatPad + kCharW * (float)wcslen(name);
        if (tx + cw > a.right) tx = a.left, ty += kCatH + kCatGap;
        if (w::Chip(ui, t, Ui::Id("wg.gal.cat", k + 1), Rect(tx, ty, cw, kCatH), name, galCat == k)) galCat = k, galScroll = 0;
        tx += cw + kCatGap;
    }
    D2D1_RECT_F view = {a.left, ty + kCatH + 10, a.right, panel.bottom - 4};
    std::vector<int> kinds;
    for (int k = 0; k < faces::Count(); ++k) {
        const faces::Meta& f = faces::Info(k);
        if (!Offered(k) || (m.faceQuery.empty() && galCat >= 0 && f.cat != galCat) || !Found(f, m.faceQuery)) continue; // a search looks everywhere
        kinds.push_back(k);
    }
    int cols = (std::max)(2, (int)((Width(view) + kGap) / (kGalMinW + kGap)));
    float cw = (Width(view) - (cols - 1) * kGap) / cols;
    int rows = ((int)kinds.size() + cols - 1) / cols;
    float content = rows * (kGalCardH + kGap);
    float scroll = Scroll(ui, "wg.gal.scroll", galScroll, view, content);
    c.PushClip(view);
    int toggle = -1;
    if (kinds.empty()) c.Text(L"No widget by that name", Rect(view.left, view.top + 40, Width(view), 20), Weight::Light, 13, t.textDim, Align::Center);
    for (int i = 0; i < (int)kinds.size(); ++i) {
        float y = view.top + (i / cols) * (kGalCardH + kGap) - scroll;
        if (y + kGalCardH < view.top || y > view.bottom) continue; // out of view: not drawn, not hit
        int k = kinds[i];
        const faces::Meta& f = faces::Info(k);
        D2D1_RECT_F card = Rect(view.left + (i % cols) * (cw + kGap), y, cw, kGalCardH);
        bool on = std::find(m.faceOn.begin(), m.faceOn.end(), k) != m.faceOn.end();
        uint32_t id = Ui::Id("wg.gal.card", k);
        Ui::Hit h = ui.Button(id, card);
        ui.Mark(Ui::Role::Toggle, on);
        ui.Name(f.name);
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), sel = ui.Smooth(id + 2, on ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(card, 16, t.text.Alpha(0.06f * hov));
        D2D1_RECT_F thumb = Rect(card.left + 6, card.top + 6, cw - 12, kGalThumbH);
        Preview(ui, t, k, thumb, host);
        c.Text(f.name, Rect(card.left + 10, thumb.bottom + 5, cw - 40, 18), Weight::Semi, 12, t.text);
        c.Text(f.about, Rect(card.left + 10, thumb.bottom + 23, cw - 20, 15), Weight::Regular, 10.5f, t.textDim);
        D2D1_POINT_2F o = {card.right - 18, thumb.bottom + 14}; // on the board or not
        c.Dot(o, 9, t.text.Alpha(0.1f).Mix(t.accent, sel));
        c.Icon(on ? glyph::Check : glyph::Add, o, 13, on ? t.onAccent : t.textDim);
        if (sel > 0.01f) c.Stroke(card, 16, t.accent.Alpha(0.8f * sel), 1.5f);
        if (h.clicked) toggle = k;
    }
    c.PopClip();
    FadesIn(c, t.cardHi, view, scroll, content);
    if (toggle >= 0) m.ToggleFace(toggle);
}

bool Small(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool on, const std::wstring& said) { // a control on a tile being arranged
    bool hit = w::Chip(ui, t, id, r, label, on);
    ui.Name(said);
    return hit;
}

void Board(PageCtx& p, const D2D1_RECT_F& view, const faces::Host& host, bool editing) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();
    const std::vector<int>& order = held >= 0 ? heldOrder : m.faceOn;
    int cols = (std::max)(2, (int)((m.faceAreaW + kGap) / (kCellMinW + kGap))); // the page's, in the editor too: a tile lands where it was put
    float cw = (Width(view) - (cols - 1) * kGap) / cols;
    int rows = 0;
    std::vector<Cell> laid = Pack(order, m, cols, rows);
    float content = rows * (kCellH + kGap) - kGap;
    if (!editing && scoresKind >= 0 && Inside(scoresAt, in.mouse) && Inside(view, in.mouse) && faces::Wheel(scoresKind, 0)) { // over the scores: the wheel goes through its games, not the board
        if (float n = ui.Wheel(scoresAt); n != 0) faces::Wheel(scoresKind, n), ui.Wake();
    }
    scoresKind = -1; // found again as the tiles draw
    float scroll = Scroll(ui, editing ? "wg.studio.scroll" : "wg.scroll", editing ? studioScroll : boardScroll, view, content);
    auto home = [&](const Cell& g) { return Rect(g.x * (cw + kGap), g.y * (kCellH + kGap), g.w * cw + (g.w - 1) * kGap, g.h * kCellH + (g.h - 1) * kGap); }; // from the board's corner, before scrolling
    c.PushClip(view);
    int picked = -1, removed = -1, sized = -1, sizeTo = 0, moved = -1, moveTo = 0;
    bool right = false;
    for (int i = 0; i < (int)order.size(); ++i) {
        int kind = order[i];
        D2D1_RECT_F want = home(laid[i]);
        float x = ui.Smooth(Ui::Id("wg.x", kind), want.left, 18), y = ui.Smooth(Ui::Id("wg.y", kind), want.top, 18); // the others slide over as one moves or grows
        float wd = ui.Smooth(Ui::Id("wg.w", kind), Width(want), 18), ht = ui.Smooth(Ui::Id("wg.h", kind), Height(want), 18);
        if (kind == held) continue; // drawn last, on top of the rest
        D2D1_RECT_F r = Rect(view.left + x, view.top + y - scroll, wd, ht);
        if (r.bottom < view.top || r.top > view.bottom) continue; // out of view: not drawn, not hit
        const faces::Meta& f = faces::Info(kind);
        int size = SizeOf(m, kind);
        w::Card(ui, t, r, kRadius);
        DrawFace(c, t, kind, r, host);
        if (!editing) {
            Ui::Hit h = ui.Button(Ui::Id("wg.card", kind), r);
            ui.Name(f.name);
            if (h.hover) c.Stroke(r, kRadius, t.accent.Alpha(0.5f), 1.2f);
            if (h.clicked) picked = kind, openFrom = r;
            if (!strcmp(f.key, "sport.scores")) scoresKind = kind, scoresAt = r; // the wheel over it scrolls its games, next frame
            if (ui.RClick(r)) picked = kind, right = true, openFrom = r;
            continue;
        }
        bool over = held < 0 && Inside(r, in.mouse) && Inside(view, in.mouse); // the tile stays itself: its controls come with the mouse
        if (over) {
            c.Stroke(r, kRadius, t.accent.Alpha(0.7f), 1.5f);
            D2D1_POINT_2F corner = {r.right - 8 - kCtlSize / 2, r.top + 8 + kCtlSize / 2};
            c.Dot(corner, kCtlSize / 2, t.bg.Alpha(0.92f));
            if (w::IconBtn(ui, t, Ui::Id("wg.remove", kind), corner, kCtlSize, glyph::Close, 13)) removed = kind;
            ui.Name(std::wstring(L"Remove ") + f.name);
            float row = 5 * kCtlSize + 4 * kCtlGap, cx = (r.left + r.right) / 2 - row / 2, cy = r.bottom - kCtlInset - kCtlSize;
            D2D1_RECT_F tray = {cx - 5, cy - 4, cx + row + 5, cy + kCtlSize + 4};
            c.Fill(tray, Height(tray) / 2, t.bg.Alpha(0.92f));
            c.Stroke(tray, Height(tray) / 2, t.hairTop, 1);
            auto spot = [&](int n) { return Rect(cx + n * (kCtlSize + kCtlGap), cy, kCtlSize, kCtlSize); };
            std::wstring named = f.name;
            if (Small(ui, t, Ui::Id("wg.earlier", kind), spot(0), L"‹", false, L"Move " + named + L" earlier") && i > 0) moved = kind, moveTo = i - 1;
            static const wchar_t* const kLabels[3] = {L"S", L"W", L"L"};
            static const wchar_t* const kSaid[3] = {L" small", L" wide", L" large"};
            for (int z = 0; z < 3; ++z)
                if (Small(ui, t, Ui::Id("wg.size", kind * 3 + z), spot(1 + z), kLabels[z], size == z, L"Make " + named + kSaid[z]) && size != z) sized = kind, sizeTo = z;
            if (Small(ui, t, Ui::Id("wg.later", kind), spot(4), L"›", false, L"Move " + named + L" later") && i + 1 < (int)order.size()) moved = kind, moveTo = i + 1;
        }
        uint32_t body = Ui::Id("wg.body", kind);
        ui.Button(body, r);
        ui.NoStop();
        if (held < 0 && ui.IsActive(body) && in.down && std::hypot(in.mouse.x - in.pressPos.x, in.mouse.y - in.pressPos.y) > kDragStart) { // picked up
            held = kind;
            heldOrder = m.faceOn;
            heldGrab = {in.pressPos.x - r.left, in.pressPos.y - r.top};
            lastSwap = ui.Now();
        }
    }
    if (held >= 0) { // it follows the mouse; the tile under the mouse gives up its place
        ui.Wake();
        for (int i = 0; i < (int)order.size() && ui.Now() - lastSwap > kSwapGap; ++i) {
            D2D1_RECT_F g = home(laid[i]);
            D2D1_RECT_F at = Rect(view.left + g.left, view.top + g.top - scroll, Width(g), Height(g));
            if (order[i] == held || !Inside(at, in.mouse)) continue;
            auto from = std::find(heldOrder.begin(), heldOrder.end(), held);
            heldOrder.erase(from);
            heldOrder.insert(heldOrder.begin() + i, held);
            lastSwap = ui.Now();
            break;
        }
        auto spotOf = std::find(order.begin(), order.end(), held);
        if (spotOf != order.end()) {
            D2D1_RECT_F g = home(laid[spotOf - order.begin()]);
            D2D1_RECT_F r = Rect(in.mouse.x - heldGrab.x, in.mouse.y - heldGrab.y, Width(g), Height(g));
            c.Glow({(r.left + r.right) / 2, (r.top + r.bottom) / 2 + 8}, Width(r) * 0.75f, Height(r) * 0.75f, Color{0, 0, 0, 0.45f});
            w::Card(ui, t, r, kRadius);
            DrawFace(c, t, held, r, host);
            c.Stroke(r, kRadius, t.accent, 1.8f);
            ui.Button(Ui::Id("wg.body", held), r); // the press stays with it
            ui.NoStop();
        }
        if (!in.down) { // let go: the order it landed in is kept
            std::vector<int> landed = heldOrder;
            held = -1;
            heldOrder.clear();
            if (landed != m.faceOn) m.PlaceFaces(landed);
        }
    }
    c.PopClip();
    if (editing) FadesIn(c, t.bg.Alpha(kBoardFill), view, scroll, content);
    else Fades(c, t, view, scroll, content);
    if (held < 0) {
        if (removed >= 0) m.ToggleFace(removed);
        else if (sized >= 0) m.SetFaceSize(sized, sizeTo);
        else if (moved >= 0) {
            std::vector<int> next = m.faceOn;
            next.erase(std::find(next.begin(), next.end(), moved));
            next.insert(next.begin() + moveTo, moved);
            m.PlaceFaces(next);
        } else if (picked >= 0) {
            fromKind = picked; // its details grow out of this tile, and go back into it
            DoAct(m, faces::Click(picked, right, host), picked);
        }
    }
}

}

void DrawWidgetsPage(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    D2D1_RECT_F view = {a.left, a.top + kBarH, a.right, a.bottom};
    m.faceAreaW = Width(view), m.faceAreaH = Height(view);
    m.facesDue = true; // their seconds and minutes tick on the model's own second, only while this page shows
    if (visit != m.pageVisit) { // back on the page: nothing left in the hand
        visit = m.pageVisit;
        held = -1;
    }
    faces::Host host = MakeHost(m);
    faces::state::TimerCheck();

    constexpr float kOpenRate = 11, kSink = 0.035f, kFaceGone = 0.4f, kContentFrom = 0.45f, kContentRise = 14, kOpenRadius = 22; // a widget opening in full: its tile grows over the board
    float open = ui.Smooth(Ui::Id("wg.open"), m.facePanel >= 0 ? 1.0f : 0, kOpenRate);
    if (m.facePanel >= 0) shownKind = m.facePanel;
    else if (open < 0.002f) shownKind = -1;
    ID2D1RenderTarget* rt = c.Target();
    bool blocked = ui.Blocked(); // a sheet or the tour over the page: nothing here takes input either way
    auto board = [&] {
        if (m.faceOn.empty()) {
            w::Card(ui, t, a);
            float mid = (a.top + a.bottom) / 2;
            c.Icon(glyph::Dashboard, {(a.left + a.right) / 2, mid - 40}, 34, t.textDim);
            c.Text(L"No widgets yet", Rect(a.left, mid - 14, Width(a), 24), Weight::Medium, 16, t.text, Align::Center);
            c.Text(L"Clocks, money, stocks, screen time and more", Rect(a.left, mid + 12, Width(a), 18), Weight::Light, 12, t.textDim, Align::Center);
            p.spotAdd = Rect((a.left + a.right) / 2 - 70, mid + 44, 140, 32);
            if (Act(ui, t, Ui::Id("wg.pick"), p.spotAdd, L"Add widgets", true, false, true)) m.studioAsk = Model::kStudioWidgets;
            return;
        }
        p.spotAdd = Rect(a.right - kBarBtnW, a.top + 2, kBarBtnW, kBarBtnH);
        if (Act(ui, t, Ui::Id("wg.add"), p.spotAdd, L"Add widgets", true, false, true)) m.studioAsk = Model::kStudioWidgets;
        Board(p, view, host, false);
    };
    if (open < 0.999f) { // the board, settling back as a widget opens over it
        bool behind = open > 0.001f;
        D2D1_MATRIX_3X2_F was;
        rt->GetTransform(&was);
        if (behind) {
            float k = 1 - kSink * open;
            D2D1_POINT_2F mid = {(a.left + a.right) / 2, (a.top + a.bottom) / 2};
            rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, mid) * *D2D1::Matrix3x2F::ReinterpretBaseType(&was));
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1 - open), nullptr);
            ui.Block(true); // it can't be touched while the open one moves over it
        }
        board();
        if (behind) {
            ui.Block(blocked);
            rt->PopLayer();
            rt->SetTransform(was);
        }
    }
    if (shownKind < 0 || open <= 0.001f) return;
    D2D1_RECT_F from = fromKind == shownKind && Width(openFrom) > 1 ? openFrom : Inset(a, Width(a) * 0.3f, Height(a) * 0.3f); // opened from elsewhere: out of the middle
    D2D1_RECT_F r = {from.left + (a.left - from.left) * open, from.top + (a.top - from.top) * open, from.right + (a.right - from.right) * open, from.bottom + (a.bottom - from.bottom) * open};
    c.Glow({(r.left + r.right) / 2, (r.top + r.bottom) / 2 + 10}, Width(r) * 0.6f, Height(r) * 0.6f, Color{0, 0, 0, 0.35f * open}); // it lifts off the board
    w::Card(ui, t, r, kRadius + (kOpenRadius - kRadius) * open);
    float face = 1 - anim::Clamp01(open / kFaceGone); // its tile's face, gone as it grows
    if (face > 0.01f) {
        rt->PushLayer(D2D1::LayerParameters(r, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), face), nullptr);
        DrawFace(c, t, shownKind, r, host);
        rt->PopLayer();
    }
    float show = anim::Clamp01((open - kContentFrom) / (1 - kContentFrom)); // the details, once it's mostly open: they drift up into place
    if (m.facePanel < 0 || show <= 0.01f) return; // folding away: only the card goes back into its tile
    c.PushClip(r);
    D2D1_MATRIX_3X2_F was;
    rt->GetTransform(&was);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - show) * kContentRise) * *D2D1::Matrix3x2F::ReinterpretBaseType(&was));
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), show), nullptr);
    bool live = show > 0.9f && !blocked;
    ui.Block(!live);
    D2D1_RECT_F in = Inset(a, 22, 18);
    bool shut = w::IconBtn(ui, t, Ui::Id("wg.back"), {a.right - 28, a.top + 26}, 30, glyph::Close, 18);
    if (!shut) faces::Panel(ui, t, m.facePanel, {in.left, in.top, in.right - 36, in.bottom}, host);
    ui.Block(blocked);
    rt->PopLayer();
    rt->SetTransform(was);
    c.PopClip();
    if (live && (shut || ui.RClick(a))) m.CloseFace(); // back into its tile
}

void DrawWidgetStudio(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    const Input& in = ui.In();
    hist.Sync(m.studioRun, Snap(m), !in.down && held < 0); // a drag counts once it lands
    faces::Host host = MakeHost(m);
    faces::state::TimerCheck();
    float mid = (a.left + a.right) / 2;
    float panelW = (std::min)(kPanelMaxW, Width(a) - 2 * kStudioEdge), panelH = (std::min)(kPanelMaxH, Height(a) * kPanelShare);
    D2D1_RECT_F panel = Rect(mid - panelW / 2, a.bottom - kStudioEdge - panelH, panelW, panelH);
    float sheetW = (std::min)(m.faceAreaW + 2 * kBoardPad, Width(a) - 2 * kStudioEdge); // just the board as the page has it, with a margin
    float room = panel.top - kPanelGap - a.top - kStudioEdge, sheetH = (std::min)(m.faceAreaH + 2 * kBoardPad, room);
    D2D1_RECT_F sheet = Rect(mid - sheetW / 2, a.top + kStudioEdge + (room - sheetH) / 2, sheetW, sheetH);
    Surface(c, t, sheet, t.bg.Alpha(kBoardFill));
    D2D1_RECT_F board = Inset(sheet, kBoardPad, kBoardPad);
    if (m.faceOn.empty()) {
        float y = (sheet.top + sheet.bottom) / 2;
        c.Text(L"Your widgets go here", Rect(sheet.left, y - 22, Width(sheet), 24), Weight::Medium, 16, t.text, Align::Center);
        c.Text(L"Tap one below to add it", Rect(sheet.left, y + 4, Width(sheet), 18), Weight::Regular, 12, t.textDim, Align::Center);
    } else {
        Board(p, board, host, true);
    }
    Surface(c, t, panel, t.cardHi);
    Gallery(p, panel, host);
    bool onSheet = Inside(sheet, in.pressPos) || Inside(panel, in.pressPos);
    if (held < 0 && in.released && !onSheet) p.wantClose = true; // a click away, as in the long pill's editor
    if (in.rclick && !Inside(sheet, in.mouse) && !Inside(panel, in.mouse)) p.wantClose = true;
}
