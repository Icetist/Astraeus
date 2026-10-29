// The live page's setup: a gallery of every sport by category, three to five picked with their league and men's or women's
// side, then world news. The same picks live in settings' advanced section.
#include <algorithm>
#include <cmath>
#include <cwctype>

#include "glyphs.h"
#include "live.h"
#include "live_page.h"
#include "live_settings.h"
#include "page_sheet.h"

namespace {

constexpr float kTop = 66; // below the sheet's title and its line
constexpr float kStripH = 26; // search and category tabs
constexpr float kCellH = 44, kCellGap = 8, kSectionH = 24;
constexpr int kGalCols = 3;
constexpr float kPickH = 28, kPickGap = 4;
constexpr float kChipH = 24;
constexpr float kMenuRowH = 24;
constexpr double kDoneSec = 3.2; // the confirmation stays this long, then the page shows
constexpr float kSettingsRowH = 34;

const wchar_t* SexName(int s) {
    static const wchar_t* const kNames[] = {L"Men", L"Women", L"Both"};
    return kNames[(std::clamp)(s, 0, 2)];
}

const wchar_t* Kind(const live::Sport& sp) { // what a gallery card promises
    switch (sp.src) {
    case live::kHeads: return L"Headlines";
    case live::kSkate: case live::kMoto: return L"Results";
    default: return L"Live scores";
    }
}

std::wstring Low(std::wstring s) {
    for (wchar_t& ch : s) ch = (wchar_t)std::towlower(ch);
    return s;
}

std::wstring LeagueText(const live::Sport& sp, const live::Pick& p) {
    if (!sp.men) return {};
    if (!sp.women || p.sex == 0) return sp.men[p.league[0]].name;
    if (p.sex == 1) return sp.women[p.league[1]].name;
    return std::wstring(sp.men[p.league[0]].name) + L" + " + sp.women[p.league[1]].name;
}

bool HasChoice(const live::Sport& sp) { return sp.men && (sp.nMen > 1 || (sp.women && sp.nWomen > 1)); }

struct Menu { // the league list over the picks column
    int pick = -1;
    float scroll = 0, scrollTo = 0;
};
Menu menu;

bool Toggle(live::Cfg& cfg, int sport) { // false when it would make six
    auto it = std::find_if(cfg.picks.begin(), cfg.picks.end(), [&](const live::Pick& p) { return p.sport == sport; });
    if (it != cfg.picks.end()) {
        cfg.picks.erase(it);
        return true;
    }
    if ((int)cfg.picks.size() >= live::kMaxPicks) return false;
    cfg.picks.push_back({sport, 0, {0, 0}});
    return true;
}

// one picked sport: its mark and name, men or women, the league, and an x; true when changed. leagueTap: the league chip was tapped
bool PickRow(Ui& ui, const Theme& t, const D2D1_RECT_F& r, live::Pick& p, int i, const char* id, bool live, bool& remove, bool& leagueTap) {
    Canvas& c = ui.C();
    const live::Sport& sp = live::SportAt(p.sport);
    bool changed = false;
    c.Fill(r, Height(r) / 2, t.track.Alpha(0.45f));
    c.Icon(sp.glyph, {r.left + 16, (r.top + r.bottom) / 2}, 16, t.accent);
    float x = r.right - 4;
    if (live && w::IconBtn(ui, t, Ui::Id(id, 100 + i), {x - 11, (r.top + r.bottom) / 2}, 22, glyph::Close, 14)) remove = true;
    else if (!live) c.Icon(glyph::Close, {x - 11, (r.top + r.bottom) / 2}, 14, t.textLow);
    x -= 26;
    float nameW = (std::min)(c.Measure(sp.name, Weight::Medium, 12) + 4, Width(r) * 0.36f);
    float chipsW = x - (r.left + 32 + nameW + 6);
    if (HasChoice(sp) && chipsW > 40) {
        float sexW = sp.women ? (std::min)(58.0f, chipsW * 0.4f) : 0;
        D2D1_RECT_F lc = {r.left + 32 + nameW + 6 + (sexW ? sexW + 6 : 0), r.top + 3, x, r.bottom - 3};
        if (w::Chip(ui, t, Ui::Id(id, 200 + i), lc, LeagueText(sp, p), false) && live) leagueTap = true;
        if (sexW > 0) {
            D2D1_RECT_F sc = Rect(r.left + 32 + nameW + 6, r.top + 3, sexW, Height(r) - 6);
            if (w::Chip(ui, t, Ui::Id(id, 300 + i), sc, SexName(p.sex), p.sex != 0) && live) {
                p.sex = (p.sex + 1) % 3;
                changed = true;
            }
        }
    } else if (sp.women && chipsW > 40) { // one league each side: only the sexes to choose
        D2D1_RECT_F sc = Rect(x - 64, r.top + 3, 64, Height(r) - 6);
        if (w::Chip(ui, t, Ui::Id(id, 300 + i), sc, SexName(p.sex), p.sex != 0) && live) {
            p.sex = (p.sex + 1) % 3;
            changed = true;
        }
    }
    c.Text(sp.name, Rect(r.left + 32, r.top, nameW, Height(r)), Weight::Medium, 12, t.text);
    return changed;
}

bool LeagueMenu(Ui& ui, const Theme& t, const D2D1_RECT_F& r, live::Pick& p) { // the open list; true when one was picked or it closed
    Canvas& c = ui.C();
    const live::Sport& sp = live::SportAt(p.sport);
    c.Fill(Inset(r, -4, -4), 18, t.veil.Alpha(0.35f));
    c.Fill(r, 14, t.cardHi);
    c.StrokeV(r, 14, t.hairTop, t.hairBot, 1);
    struct Item {
        int sex, idx; // idx -1: a section header
    };
    std::vector<Item> items;
    bool both = sp.women && p.sex == 2;
    if (!sp.women || p.sex != 1) {
        if (both) items.push_back({0, -1});
        for (int k = 0; k < sp.nMen; ++k) items.push_back({0, k});
    }
    if (sp.women && p.sex != 0) {
        if (both) items.push_back({1, -1});
        for (int k = 0; k < sp.nWomen; ++k) items.push_back({1, k});
    }
    D2D1_RECT_F view = Inset(r, 6, 8);
    float content = items.size() * kMenuRowH;
    float scroll = sheet::Scroll(ui, "live.menu", menu.scrollTo, view, content);
    c.PushClip(view);
    bool done = false;
    for (size_t i = 0; i < items.size(); ++i) {
        D2D1_RECT_F row = Rect(view.left, view.top + i * kMenuRowH - scroll, Width(view), kMenuRowH);
        if (row.bottom < view.top || row.top > view.bottom) continue;
        const Item& it = items[i];
        if (it.idx < 0) {
            c.TextSp(it.sex ? L"WOMEN" : L"MEN", Inset(row, 8, 0), Weight::Medium, 9, t.textLow, 1.3f);
            continue;
        }
        const live::League& lg = it.sex ? sp.women[it.idx] : sp.men[it.idx];
        bool cur = p.league[it.sex] == it.idx;
        Ui::Hit h = ui.Button(Ui::Id("live.menu.row", (int)i), row);
        if (h.hover || cur) c.Fill(row, 8, cur ? t.accent.Alpha(0.2f) : t.text.Alpha(0.06f));
        c.Text(lg.name, Inset(row, 10, 0), cur ? Weight::Medium : Weight::Regular, 11.5f, cur ? t.text : t.textDim);
        if (h.clicked) {
            p.league[it.sex] = it.idx;
            done = !both || it.sex == 1; // both: pick the women's too, the list stays
        }
    }
    c.PopClip();
    sheet::Fades(c, t, view, scroll, content);
    if (ui.In().pressed && !Inside(r, ui.In().mouse)) done = true; // clicked away
    return done;
}

void Gallery(Ui& ui, const Theme& t, const D2D1_RECT_F& g, live::Setup& s, bool live, double& fullAt) {
    Canvas& c = ui.C();
    // search field, then the category tabs
    D2D1_RECT_F field = Rect(g.left, g.top, (std::min)(150.0f, Width(g) * 0.36f), kStripH);
    Ui::Hit fh = live ? ui.Button(Ui::Id("live.search"), field) : Ui::Hit{};
    if (fh.clicked) s.typing = true;
    if (s.typing && ui.In().pressed && !Inside(field, ui.In().mouse)) s.typing = false;
    c.Fill(field, kStripH / 2, t.track.Alpha(0.7f));
    c.Stroke(field, kStripH / 2, s.typing ? t.accent : fh.hover ? t.text.Alpha(0.4f) : t.hairTop, s.typing ? 1.5f : 1);
    D2D1_RECT_F ft = Inset(field, 12, 0);
    if (s.search.empty() && !s.typing) c.Text(L"Search sports", ft, Weight::Light, 11.5f, t.textLow);
    float tw = c.TextOnce(s.search, ft, Weight::Regular, 11.5f, t.text);
    if (s.typing) {
        if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({ft.left + tw + 1.5f, field.top + 7}, {ft.left + tw + 1.5f, field.bottom - 7}, t.text, 1.5f); // caret
        ui.KeepAlive();
    }
    D2D1_RECT_F tabs = {field.right + 8, g.top, g.right, g.top + kStripH};
    static float tabScroll = 0;
    float tabsW = 0;
    for (int k = -1; k < live::kCats; ++k) tabsW += c.Measure(live::CatName(k), Weight::Medium, 11.5f) + 30;
    tabScroll = (std::clamp)(tabScroll - ui.Wheel(tabs) * 50, 0.0f, (std::max)(0.0f, tabsW - Width(tabs)));
    float ts = ui.Smooth(Ui::Id("live.tabs"), tabScroll, 18);
    c.PushClip(tabs);
    float x = tabs.left - ts;
    for (int k = -1; k < live::kCats; ++k) {
        float wv = c.Measure(live::CatName(k), Weight::Medium, 11.5f) + 22;
        D2D1_RECT_F chip = Rect(x, tabs.top + 1, wv, kStripH - 2);
        if (w::Chip(ui, t, Ui::Id("live.tab", k + 1), chip, live::CatName(k), s.cat == k) && live && chip.right <= tabs.right + 1 && chip.left >= tabs.left - 1) {
            s.cat = k;
            s.scrollTo = 0;
        }
        x += wv + 8;
    }
    c.PopClip();
    if (tabsW > Width(tabs)) c.FillH({tabs.right - 18, tabs.top, tabs.right, tabs.bottom}, 0, t.card.Alpha(0), t.card); // more to the right

    // the cards: by category, or one flat list while searching or on a tab
    D2D1_RECT_F view = {g.left, g.top + kStripH + 8, g.right, g.bottom};
    std::wstring q = Low(s.search);
    struct Slot {
        int sport; // -1: a section title for cat
        int cat;
    };
    std::vector<Slot> slots;
    bool grouped = s.cat < 0 && q.empty();
    for (int cat = 0; cat < live::kCats; ++cat) {
        if (s.cat >= 0 && cat != s.cat) continue;
        bool any = false;
        for (int i = 0; i < live::SportCount(); ++i) {
            const live::Sport& sp = live::SportAt(i);
            if (sp.cat != cat) continue;
            if (!q.empty() && Low(sp.name).find(q) == std::wstring::npos && Low(live::CatName(cat)).find(q) == std::wstring::npos) continue;
            if (grouped && !any) slots.push_back({-1, cat});
            any = true;
            slots.push_back({i, cat});
        }
    }
    float cw = (Width(view) - (kGalCols - 1) * kCellGap) / kGalCols;
    std::vector<D2D1_RECT_F> rects(slots.size());
    float y = 0;
    int col = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].sport < 0) {
            if (col) y += kCellH + kCellGap;
            col = 0;
            rects[i] = Rect(view.left, y, Width(view), kSectionH);
            y += kSectionH;
            continue;
        }
        rects[i] = Rect(view.left + col * (cw + kCellGap), y, cw, kCellH);
        if (++col == kGalCols) col = 0, y += kCellH + kCellGap;
    }
    float content = y + (col ? kCellH : 0);
    float scroll = sheet::Scroll(ui, "live.gal", s.scrollTo, view, content);
    c.PushClip(view);
    for (size_t i = 0; i < slots.size(); ++i) {
        D2D1_RECT_F r = rects[i];
        r.top += view.top - scroll, r.bottom += view.top - scroll;
        if (r.bottom < view.top || r.top > view.bottom) continue;
        if (slots[i].sport < 0) {
            c.TextSp(fmt::Upper(live::CatName(slots[i].cat)), Rect(r.left + 2, r.top + 4, Width(r), 14), Weight::Medium, 9.5f, t.textLow, 1.4f);
            continue;
        }
        int sport = slots[i].sport;
        const live::Sport& sp = live::SportAt(sport);
        bool on = s.draft.Has(sport);
        uint32_t id = Ui::Id("live.gal.card", sport);
        Ui::Hit h = live && Inside(view, ui.In().mouse) ? ui.Button(id, r) : Ui::Hit{};
        float sel = ui.Smooth(id, on ? 1.0f : 0, 14), hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        c.Fill(r, 12, t.cardHi.Mix(t.track, hov * 0.5f));
        if (sel > 0.001f) c.Fill(r, 12, t.accent.Alpha(0.2f * sel));
        c.Stroke(r, 12, t.accent.Alpha(0.6f * sel).Mix(t.hairTop, 1 - sel), 1);
        c.Icon(sp.glyph, {r.left + 20, (r.top + r.bottom) / 2}, 19, t.accent.Mix(t.accentHi, sel), on);
        c.Text(sp.name, Rect(r.left + 36, r.top + 6, Width(r) - 56, 17), Weight::Medium, 11.5f, t.text);
        c.Text(Kind(sp), Rect(r.left + 36, r.top + 22, Width(r) - 56, 14), Weight::Light, 9.5f, sp.src == live::kHeads ? t.textLow : t.textDim);
        if (sel > 0.01f) c.Icon(glyph::Check, {r.right - 14, r.top + 14}, 14, t.accentHi.Alpha(sel));
        if (h.clicked && !Toggle(s.draft, sport)) fullAt = ui.Now(); // a sixth: say why it didn't go in
    }
    c.PopClip();
    sheet::Fades(c, t, view, scroll, content);
    if (slots.empty()) c.Text(L"No sport by that name", Rect(view.left, view.top + 20, Width(view), 18), Weight::Light, 12, t.textLow, Align::Center);
}

void Done(PageCtx& p, const D2D1_RECT_F& s, live::Setup& st) { // saved: where to change it later
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    float k = ui.SmoothFrom(Ui::Id("live.done"), 1, 0, 10);
    float cx = (s.left + s.right) / 2, cy = (s.top + s.bottom) / 2 - 18;
    c.Dot({cx, cy - 22}, 24 * (0.8f + 0.2f * k), t.accent.Alpha(0.9f * k));
    c.Icon(glyph::Check, {cx, cy - 22}, 26, t.onAccent.Alpha(k));
    c.Text(L"You're all set", Rect(s.left, cy + 12, Width(s), 26), Weight::Semi, 18, t.text.Alpha(k), Align::Center);
    c.Text(L"You can change this anytime in Settings → Advanced.", Rect(s.left + 20, cy + 40, Width(s) - 40, 20), Weight::Regular, 12.5f, t.textDim.Alpha(k),
           Align::Center);
    D2D1_RECT_F b = Rect(cx - 70, cy + 76, 140, 32);
    bool go = sheet::Act(ui, t, Ui::Id("live.done.go"), b, L"Show me", true, false, true);
    ui.KeepAlive();
    if (go || ui.Now() - st.doneAt > kDoneSec) {
        st.open = false;
        st.done = false;
        if (p.m.HasPage(Model::kPgLive)) {
            p.wantPage = Model::kPgLive;
            if (p.m.settingsOpen) p.m.CloseSettings();
        }
    }
}

}

float LiveSheetK(Ui& ui) { return ui.Smooth(Ui::Id("live.sheet"), live::Sheet().open ? 1.0f : 0, 14); }

void DrawLiveSetup(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    const D2D1_RECT_F& s = p.area;
    live::Setup& st = live::Sheet();
    sheet::Frame frame = sheet::Begin(p, k, false);
    ui.Button(Ui::Id("live.sheet.body"), s); // nothing under the sheet takes a click
    bool live = st.open && k > 0.9f && !st.done;
    c.Text(L"Live events", Rect(s.left + 24, s.top + 12, 300, 28), Weight::Semi, 21, t.text);
    c.Text(L"Pick 3 to 5 sports to follow, then world news", Rect(s.left + 24, s.top + 40, Width(s) * 0.5f, 16), Weight::Light, 12, t.textDim);
    if (w::IconBtn(ui, t, Ui::Id("live.close"), {s.right - 30, s.top + 30}, 30, glyph::Close, 18, st.open)) {
        st.open = st.typing = false;
        menu.pick = -1;
    }
    if (st.done) {
        Done(p, s, st);
        sheet::End(p, frame);
        return;
    }
    bool tall = Height(s) > Width(s);
    float gw = tall ? Width(s) - 24 : (Width(s) - 48) * 0.57f;
    D2D1_RECT_F gal = tall ? D2D1_RECT_F{s.left + 12, s.top + kTop, s.right - 12, s.top + kTop + (Height(s) - kTop) * 0.5f}
                           : D2D1_RECT_F{s.left + 16, s.top + kTop, s.left + 16 + gw, s.bottom - 14};
    D2D1_RECT_F side = tall ? D2D1_RECT_F{s.left + 12, gal.bottom + 14, s.right - 12, s.bottom - 12} : D2D1_RECT_F{gal.right + 16, s.top + kTop, s.right - 16, s.bottom - 14};
    if (!tall) c.Line({gal.right + 8, gal.top + 4}, {gal.right + 8, gal.bottom - 4}, t.line, 1, false);
    static double fullAt = -10;
    bool menuOpen = menu.pick >= 0 && menu.pick < (int)st.draft.picks.size();
    Gallery(ui, t, gal, st, live && !menuOpen, fullAt);

    // the picks
    int n = (int)st.draft.picks.size();
    c.TextSp(L"YOUR SPORTS", Rect(side.left + 2, side.top + 4, 120, 14), Weight::Medium, 9.5f, t.textLow, 1.4f);
    std::wstring count = std::to_wstring(n) + L" of " + std::to_wstring(live::kMaxPicks);
    c.Text(count, Rect(side.right - 80, side.top + 2, 80, 16), Weight::Medium, 11, n >= live::kMinPicks ? t.accentHi : t.textDim, Align::Right);
    float y = side.top + 24;
    int remove = -1;
    for (int i = 0; i < n; ++i) {
        bool rm = false, tap = false;
        PickRow(ui, t, Rect(side.left, y, Width(side), kPickH), st.draft.picks[i], i, "live.pick", live && !menuOpen, rm, tap);
        if (rm) remove = i;
        if (tap) menu = {i, 0, 0};
        y += kPickH + kPickGap;
    }
    if (remove >= 0) st.draft.picks.erase(st.draft.picks.begin() + remove);
    for (int i = n; i < live::kMinPicks; ++i) { // empty places still to fill
        D2D1_RECT_F r = Rect(side.left, y, Width(side), kPickH);
        c.Stroke(Inset(r, 0.5f, 0.5f), kPickH / 2, t.hairTop, 1);
        c.Text(L"Tap a sport to add it", Inset(r, 16, 0), Weight::Light, 11, t.textLow);
        y += kPickH + kPickGap;
    }
    D2D1_RECT_F newsRow = Rect(side.left, y + 2, Width(side), kPickH); // news closes the list; save sits up by the title
    c.Icon(glyph::Globe, {newsRow.left + 16, (newsRow.top + newsRow.bottom) / 2}, 16, t.accent);
    c.Text(L"World news", Rect(newsRow.left + 32, newsRow.top, 140, kPickH), Weight::Medium, 12, t.text);
    if (w::Switch(ui, t, Ui::Id("live.news"), {newsRow.right - 26, (newsRow.top + newsRow.bottom) / 2}, st.draft.news, live && !menuOpen)) st.draft.news = !st.draft.news;

    bool ok = n >= live::kMinPicks && n <= live::kMaxPicks;
    D2D1_RECT_F save = Rect(s.right - 160, s.top + 16, 96, 30);
    std::wstring hint = ui.Now() - fullAt < 2.5 ? L"Five at most: take one off first"
                      : n < live::kMinPicks ? L"Pick " + std::to_wstring(live::kMinPicks - n) + L" more to save"
                      : n == live::kMaxPicks ? L"Five picked, the most there can be"
                                             : L"Up to " + std::to_wstring(live::kMaxPicks - n) + L" more if you like";
    D2D1_RECT_F hr = tall ? Rect(side.left + 2, side.bottom - 16, Width(side), 16) : Rect(save.left - 260, save.top, 250, 30); // a sidebar has no room by the title
    c.Text(hint, hr, Weight::Light, 11, ui.Now() - fullAt < 2.5 ? t.accentHi : t.textLow, tall ? Align::Left : Align::Right);
    if (ui.Now() - fullAt < 2.5) ui.KeepAlive();
    if (sheet::Act(ui, t, Ui::Id("live.save"), save, L"Save", ok, false, live && ok && !menuOpen)) {
        live::Config() = st.draft;
        live::Config().setup = true;
        live::Changed();
        st.done = true;
        st.typing = false;
        st.doneAt = ui.Now();
        ui.Forget();
    }
    if (menuOpen) {
        D2D1_RECT_F mr = {side.left - 6, side.top, side.right + 6, side.bottom};
        if (LeagueMenu(ui, t, mr, st.draft.picks[menu.pick])) menu.pick = -1;
    }
    sheet::End(p, frame);
}

void LivePagesChanged(const std::vector<int>& was, const std::vector<int>& now, int livePage) {
    bool before = std::find(was.begin(), was.end(), livePage) != was.end(), after = std::find(now.begin(), now.end(), livePage) != now.end();
    if (after && !before && !live::Config().setup) live::OpenSetup(); // turned on for the first time: pick what to follow
}

float LiveSettings(PageCtx& p, const D2D1_RECT_F& r, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    live::Cfg& cfg = live::Config();
    int n = (int)cfg.picks.size();
    float h = 40 + n * kSettingsRowH + kSettingsRowH + 44;
    D2D1_RECT_F box = Rect(r.left, r.top, Width(r), h);
    D2D1_RECT_F in = sheet::Box(c, t, box, L"LIVE EVENTS");
    float y = in.top;
    bool changed = false;
    int remove = -1;
    for (int i = 0; i < n; ++i) {
        bool rm = false, tap = false;
        changed |= PickRow(ui, t, Rect(in.left, y, Width(in), kPickH), cfg.picks[i], i, "set.live.pick", live, rm, tap);
        if (rm && n > live::kMinPicks) remove = i; // three always stay
        if (tap) { // the full list is in the setup sheet
            live::OpenSetup();
            menu = {i, 0, 0};
        }
        y += kSettingsRowH;
    }
    if (remove >= 0) {
        cfg.picks.erase(cfg.picks.begin() + remove);
        changed = true;
    }
    D2D1_RECT_F nr = Rect(in.left, y, Width(in), kPickH);
    c.Icon(glyph::Globe, {nr.left + 16, (nr.top + nr.bottom) / 2}, 16, t.accent);
    c.Text(L"World news", Rect(nr.left + 32, nr.top, 160, kPickH), Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.25f));
    if (w::Switch(ui, t, Ui::Id("set.live.news"), {nr.right - 24, (nr.top + nr.bottom) / 2}, cfg.news, live)) {
        cfg.news = !cfg.news;
        changed = true;
    }
    y += kSettingsRowH + 4;
    D2D1_RECT_F b = Rect(in.left, y, 150, 30);
    if (sheet::Act(ui, t, Ui::Id("set.live.edit"), b, L"Change sports…", false, false, live)) live::OpenSetup();
    const wchar_t* note = p.m.HasPage(Model::kPgLive) ? L"3 to 5 sports. Scores refresh only while the page shows" : L"Turn on the Live page above to see them";
    c.Text(note, Rect(b.right + 12, y, in.right - b.right - 12, 30), Weight::Light, 10.5f, t.textLow);
    if (changed) live::Changed();
    return h;
}
