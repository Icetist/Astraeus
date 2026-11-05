// The widgets page: a bento board of small, wide and large tiles that pack together with no holes, arranged by hand on the page, added from a gallery with live previews.
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
#include "page_sheet.h"

using namespace sheet;

namespace {

enum Size { kSmall, kWide, kLarge };
constexpr float kCellMinW = 148, kCellH = 112, kGap = 10, kRadius = 18; // the board's grid
constexpr float kBarH = 36, kBarBtnW = 84, kBarBtnH = 26; // the row over the board
constexpr float kLargeZoom = 1.4f; // a large tile is its small face made bigger, not the same face with empty room around it
constexpr float kCtlSize = 24, kCtlGap = 3, kCtlInset = 7, kDragStart = 6; // arranging: the controls on a tile
constexpr double kSwapGap = 0.16; // a held tile moves its neighbours no faster than this: different sizes can't flip back and forth
constexpr float kGalMinW = 196, kGalCardH = 116, kGalThumbH = 60, kSearchW = 260, kSearchH = 28, kCatH = 26, kCatGap = 6, kCatPad = 22, kCharW = 6.6f; // the gallery
constexpr float kChipH = 26, kChipGap = 6, kChipPad = 22, kPickerHead = 34, kCardChrome = 44; // settings' card

bool adding = false, arranging = false;
int held = -1; // the tile being dragged
std::vector<int> heldOrder; // the board as it would land
D2D1_POINT_2F heldGrab{};
double lastSwap = 0;
int visit = 0; // the page visit all that belongs to
float boardScroll = 0, galScroll = 0;
int galCat = -1; // -1: every category
int category = 0; // settings' card

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

void DrawFace(Canvas& c, const Theme& t, int kind, int size, const D2D1_RECT_F& r, const faces::Host& host) {
    c.PushClip(r);
    if (size == kLarge) {
        ID2D1RenderTarget* rt = c.Target();
        D2D1_MATRIX_3X2_F was;
        rt->GetTransform(&was);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(kLargeZoom, kLargeZoom, {r.left, r.top}) * *D2D1::Matrix3x2F::ReinterpretBaseType(&was));
        faces::Draw(c, t, kind, 2, Rect(r.left, r.top, Width(r) / kLargeZoom, Height(r) / kLargeZoom), host);
        rt->SetTransform(was);
    } else {
        faces::Draw(c, t, kind, 2, r, host);
    }
    c.PopClip();
}

faces::Host MakeHost(Model& m) {
    faces::Host h;
    const auto& wx = m.wx;
    h.cpu = m.cpuHist.Last(), h.mem = m.ramHist.Last(), h.memUsedGb = m.stats.memUsedGb, h.memTotalGb = m.stats.memTotalGb;
    h.wxValid = wx.valid, h.wxDay = wx.isDay, h.fahrenheit = m.cfg.fahr, h.cards = true;
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

void Gallery(PageCtx& p, const faces::Host& host) { // every widget as a card with its real face on it: a tap puts it on the board or takes it off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    SearchField(p, Rect(a.left, a.top + 2, (std::min)(kSearchW, Width(a) - kBarBtnW - 16), kSearchH));
    if (Act(ui, t, Ui::Id("wg.gal.done"), Rect(a.right - kBarBtnW, a.top + 3, kBarBtnW, kBarBtnH), L"Done", true, false, true)) {
        adding = m.faceSearch = false;
        m.faceQuery.clear();
        return;
    }
    float tx = a.left, ty = a.top + kSearchH + 10;
    for (int k = -1; k < faces::kCats; ++k) { // the categories, wrapping when the panel is narrow
        const wchar_t* name = k < 0 ? L"All" : faces::CatName(k);
        float cw = kCatPad + kCharW * (float)wcslen(name);
        if (tx + cw > a.right) tx = a.left, ty += kCatH + kCatGap;
        if (w::Chip(ui, t, Ui::Id("wg.gal.cat", k + 1), Rect(tx, ty, cw, kCatH), name, galCat == k)) galCat = k, galScroll = 0;
        tx += cw + kCatGap;
    }
    D2D1_RECT_F view = {a.left, ty + kCatH + 8, a.right, a.bottom};
    std::vector<int> kinds;
    for (int k = 0; k < faces::Count(); ++k) {
        const faces::Meta& f = faces::Info(k);
        if (!Offered(k) || (m.faceQuery.empty() && galCat >= 0 && f.cat != galCat) || !Found(f, m.faceQuery)) continue; // a search looks everywhere
        kinds.push_back(k);
    }
    int cols = (std::max)(2, (int)((Width(view) + kGap) / (kGalMinW + kGap)));
    float cw = (Width(view) - (cols - 1) * kGap) / cols;
    int rows = ((int)kinds.size() + cols - 1) / cols;
    float content = rows * (kGalCardH + kGap) - kGap;
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
        Ui::Hit h = ui.Button(Ui::Id("wg.gal.card", k), card);
        ui.Mark(Ui::Role::Toggle, on);
        ui.Name(f.name);
        w::Card(ui, t, card, 16);
        if (h.hover) c.Fill(card, 16, t.text.Alpha(0.05f));
        D2D1_RECT_F thumb = Rect(card.left + 8, card.top + 8, cw - 16, kGalThumbH);
        c.Fill(thumb, 10, t.track.Alpha(0.35f));
        c.PushClip(thumb);
        faces::Draw(c, t, k, 1, thumb, host);
        c.PopClip();
        c.Text(f.name, Rect(card.left + 12, thumb.bottom + 5, cw - 44, 18), Weight::Semi, 12, t.text);
        c.Text(f.about, Rect(card.left + 12, thumb.bottom + 23, cw - 22, 15), Weight::Regular, 10, t.textDim);
        D2D1_POINT_2F o = {card.right - 18, thumb.bottom + 14}; // on the board or not
        c.Dot(o, 9, on ? t.accent : t.text.Alpha(0.1f));
        c.Icon(on ? glyph::Check : glyph::Add, o, 13, on ? t.onAccent : t.textDim);
        if (on) c.Stroke(card, 16, t.accent.Alpha(0.8f), 1.5f);
        if (h.clicked) toggle = k;
    }
    c.PopClip();
    Fades(c, t, view, scroll, content);
    if (toggle >= 0) m.ToggleFace(toggle);
    if (ui.RClick(a)) {
        adding = m.faceSearch = false;
        m.faceQuery.clear();
    }
}

bool Small(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool on, const std::wstring& said) { // a control on a tile being arranged
    bool hit = w::Chip(ui, t, id, r, label, on);
    ui.Name(said);
    return hit;
}

void Board(PageCtx& p, const D2D1_RECT_F& view, const faces::Host& host) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();
    const std::vector<int>& order = held >= 0 ? heldOrder : m.faceOn;
    int cols = (std::max)(2, (int)((Width(view) + kGap) / (kCellMinW + kGap)));
    float cw = (Width(view) - (cols - 1) * kGap) / cols;
    int rows = 0;
    std::vector<Cell> laid = Pack(order, m, cols, rows);
    float content = rows * (kCellH + kGap) - kGap;
    float scroll = Scroll(ui, "wg.scroll", boardScroll, view, content);
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
        DrawFace(c, t, kind, size, r, host);
        if (!arranging) {
            Ui::Hit h = ui.Button(Ui::Id("wg.card", kind), r);
            ui.Name(f.name);
            if (h.hover) c.Stroke(r, kRadius, t.accent.Alpha(0.5f), 1.2f);
            if (h.clicked) picked = kind;
            if (ui.RClick(r)) picked = kind, right = true;
            continue;
        }
        c.Fill(r, kRadius, t.bg.Alpha(0.55f)); // the face steps back behind its controls
        c.Stroke(r, kRadius, t.accent.Alpha(0.45f), 1.2f);
        if (w::IconBtn(ui, t, Ui::Id("wg.remove", kind), {r.right - 8 - kCtlSize / 2, r.top + 8 + kCtlSize / 2}, kCtlSize, glyph::Close, 13)) removed = kind;
        ui.Name(std::wstring(L"Remove ") + f.name);
        float row = 5 * kCtlSize + 4 * kCtlGap, cx = (r.left + r.right) / 2 - row / 2, cy = r.bottom - kCtlInset - kCtlSize;
        auto spot = [&](int n) { return Rect(cx + n * (kCtlSize + kCtlGap), cy, kCtlSize, kCtlSize); };
        std::wstring named = f.name;
        if (Small(ui, t, Ui::Id("wg.earlier", kind), spot(0), L"‹", false, L"Move " + named + L" earlier") && i > 0) moved = kind, moveTo = i - 1;
        static const wchar_t* const kLabels[3] = {L"S", L"W", L"L"};
        static const wchar_t* const kSaid[3] = {L" small", L" wide", L" large"};
        for (int z = 0; z < 3; ++z)
            if (Small(ui, t, Ui::Id("wg.size", kind * 3 + z), spot(1 + z), kLabels[z], size == z, L"Make " + named + kSaid[z]) && size != z) sized = kind, sizeTo = z;
        if (Small(ui, t, Ui::Id("wg.later", kind), spot(4), L"›", false, L"Move " + named + L" later") && i + 1 < (int)order.size()) moved = kind, moveTo = i + 1;
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
        int size = SizeOf(m, held);
        if (spotOf != order.end()) {
            D2D1_RECT_F g = home(laid[spotOf - order.begin()]);
            D2D1_RECT_F r = Rect(in.mouse.x - heldGrab.x, in.mouse.y - heldGrab.y, Width(g), Height(g));
            c.Glow({(r.left + r.right) / 2, (r.top + r.bottom) / 2 + 8}, Width(r) * 0.75f, Height(r) * 0.75f, Color{0, 0, 0, 0.45f});
            w::Card(ui, t, r, kRadius);
            DrawFace(c, t, held, size, r, host);
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
    Fades(c, t, view, scroll, content);
    if (held < 0) {
        if (removed >= 0) m.ToggleFace(removed);
        else if (sized >= 0) m.SetFaceSize(sized, sizeTo);
        else if (moved >= 0) {
            std::vector<int> next = m.faceOn;
            next.erase(std::find(next.begin(), next.end(), moved));
            next.insert(next.begin() + moveTo, moved);
            m.PlaceFaces(next);
        } else if (picked >= 0) {
            DoAct(m, faces::Click(picked, right, host), picked);
        }
    }
    if (arranging && m.faceOn.empty()) arranging = false;
    if (arranging && ui.RClick(view)) arranging = false;
}

}

void DrawWidgetsPage(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    m.facesDue = true; // their seconds and minutes tick on the model's own second, only while this page shows
    if (visit != m.pageVisit) { // back on the page: it opens as the board, not where it was left
        visit = m.pageVisit;
        arranging = adding = false;
        held = -1;
    }
    faces::Host host = MakeHost(m);
    faces::state::TimerCheck();

    if (m.facePanel >= 0) { // one widget, in full
        w::Card(ui, t, a);
        D2D1_RECT_F in = Inset(a, 22, 18);
        if (w::IconBtn(ui, t, Ui::Id("wg.back"), {a.right - 28, a.top + 26}, 30, glyph::Close, 18)) {
            m.CloseFace();
            return;
        }
        faces::Panel(ui, t, m.facePanel, {in.left, in.top, in.right - 36, in.bottom}, host);
        if (ui.RClick(a)) m.CloseFace();
        return;
    }
    if (adding) {
        Gallery(p, host);
        return;
    }
    if (m.faceOn.empty()) {
        w::Card(ui, t, a);
        float mid = (a.top + a.bottom) / 2;
        c.Icon(glyph::Dashboard, {(a.left + a.right) / 2, mid - 40}, 34, t.textDim);
        c.Text(L"No widgets yet", Rect(a.left, mid - 14, Width(a), 24), Weight::Medium, 16, t.text, Align::Center);
        c.Text(L"Clocks, money, stocks, screen time and more", Rect(a.left, mid + 12, Width(a), 18), Weight::Light, 12, t.textDim, Align::Center);
        if (Act(ui, t, Ui::Id("wg.pick"), Rect((a.left + a.right) / 2 - 70, mid + 44, 140, 32), L"Add widgets", true, false, true)) adding = true;
        return;
    }
    if (arranging) {
        c.Text(L"Drag to move them  ·  S, W and L set the size", Rect(a.left + 4, a.top, Width(a) - kBarBtnW - 12, kBarBtnH + 4), Weight::Light, 12, t.textDim);
        if (Act(ui, t, Ui::Id("wg.done"), Rect(a.right - kBarBtnW, a.top + 2, kBarBtnW, kBarBtnH), L"Done", true, false, true)) arranging = false;
    } else {
        if (Act(ui, t, Ui::Id("wg.add"), Rect(a.right - kBarBtnW, a.top + 2, kBarBtnW, kBarBtnH), L"Add", true, false, true)) adding = true;
        if (Act(ui, t, Ui::Id("wg.arrange"), Rect(a.right - 2 * kBarBtnW - 8, a.top + 2, kBarBtnW, kBarBtnH), L"Arrange", false, false, true)) arranging = true;
    }
    Board(p, {a.left, a.top + kBarH, a.right, a.bottom}, host);
}

float WidgetsCardH(const Model&, float w) { // as tall as the chosen category's chips need: an estimate from its names, made the same way it's drawn
    float x = 0, y = kChipH;
    float room = w - 24;
    for (int k = 0; k < faces::Count(); ++k) {
        if (!Offered(k) || faces::Info(k).cat != category) continue;
        float cw = kChipPad + kCharW * (float)wcslen(faces::Info(k).name);
        if (x + cw > room) x = 0, y += kChipH + kChipGap;
        x += cw + kChipGap;
    }
    return kCardChrome + kPickerHead + y + 8;
}

void WidgetsCard(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a category, then every widget in it: a tap puts it on the widgets page or takes it off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"WIDGETS");
    float tx = in.left;
    for (int k = 0; k < faces::kCats; ++k) {
        float cw = kChipPad + kCharW * (float)wcslen(faces::CatName(k));
        if (tx + cw > in.right) break;
        if (w::Chip(ui, t, Ui::Id("wg.cat", k), Rect(tx, in.top, cw, kChipH), faces::CatName(k), category == k) && live) category = k;
        tx += cw + kChipGap;
    }
    float x = in.left, y = in.top + kPickerHead;
    for (int k = 0; k < faces::Count(); ++k) {
        if (!Offered(k) || faces::Info(k).cat != category) continue;
        const wchar_t* name = faces::Info(k).name;
        float cw = kChipPad + kCharW * (float)wcslen(name);
        if (x + cw > in.right) x = in.left, y += kChipH + kChipGap;
        bool on = std::find(m.faceOn.begin(), m.faceOn.end(), k) != m.faceOn.end();
        if (w::Chip(ui, t, Ui::Id("wg.opt", k), Rect(x, y, cw, kChipH), name, on) && live) m.ToggleFace(k);
        x += cw + kChipGap;
    }
}
