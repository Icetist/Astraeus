// The shelf: whatever was dropped on the island and, when switched on, everything copied. Cards newest first, pinned ones on top of
// each filter. Tap a card to copy it, right-click for the rest, carry one onto another to stack them, out of the panel to drop it anywhere.
#include <cmath>

#include "anim.h"
#include "shelf_ui.h"

using namespace shelfui;

namespace {

constexpr float kRowH = 30; // the search field and the clear button
constexpr float kChipH = 26;
constexpr float kSearchW = 210; // a wide panel: the search beside the filters
constexpr float kClearW = 66;
constexpr float kChipPadMin = 10, kChipPadMax = 22, kChipGap = 6;
constexpr float kCardMin = 132, kCardH = 116, kCardTallH = 124;
constexpr float kHeadH = 22; // the pinned and recent labels
constexpr float kBtn = 24; // a card's hover buttons
constexpr double kCopiedSec = 1.4; // the tick that says it's on the clipboard
constexpr double kAgeEvery = 1.0; // seconds between auto-delete looks while the page shows
constexpr float kGhostW = 150, kGhostH = 40; // a carried card under the mouse
constexpr float kOutSlack = 8; // past the page this far while carrying: the drag leaves the panel
const wchar_t* const kFilters[] = {L"All", L"Text", L"Links", L"Images", L"Files", L"Colors", L"Code"};
constexpr int kFilterN = (int)(sizeof(kFilters) / sizeof(kFilters[0]));

void Refilter(View& v, Model& m) { // only when the shelf, the filter or the search changed
    if (v.seenVer == m.shelf.Version() && v.seenFilter == v.filter && v.seenQuery == m.shelfQuery) return;
    v.seenVer = m.shelf.Version();
    v.seenFilter = v.filter;
    v.seenQuery = m.shelfQuery;
    v.pinned.clear();
    v.recent.clear();
    const auto& items = m.shelf.Items();
    for (size_t i = 0; i < items.size(); ++i)
        if (Matches(items[i], v.filter, m.shelfQuery)) (items[i].pinned ? v.pinned : v.recent).push_back(i);
}

void SearchField(PageCtx& p, const D2D1_RECT_F& r, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool focus = m.shelfField == Model::kShelfSearch;
    bool clear = !m.shelfQuery.empty();
    D2D1_RECT_F field = {r.left, r.top, r.right - (clear ? 30 : 0), r.bottom};
    Ui::Hit h = live ? ui.Button(Ui::Id("sh.search"), field) : Ui::Hit{};
    float rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    c.Stroke(r, rad, focus ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, focus ? 1.5f : 1);
    c.Icon(kSearchGlyph, {r.left + 15, (r.top + r.bottom) / 2}, 16, focus ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {r.left + 30, r.top, field.right - 8, r.bottom};
    if (focus) {
        float w = m.shelfQuery.empty() ? 0 : c.TextOnce(m.shelfQuery, text, Weight::Regular, 12, t.text); // typed text isn't cached
        if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({text.left + w + 1.5f, r.top + 8}, {text.left + w + 1.5f, r.bottom - 8}, t.text, 1.5f); // caret
        ui.KeepAlive();
        if (ui.In().pressed && !Inside(r, ui.In().mouse)) m.shelfField = Model::kShelfNone; // clicked away: the search stays, the keyboard goes back
    } else {
        c.Text(clear ? m.shelfQuery : L"Search", text, Weight::Regular, 12, clear ? t.text : t.textLow);
    }
    if (h.clicked && !focus) {
        if (m.shelfField == Model::kShelfNote) m.CommitShelfNote();
        m.shelfField = Model::kShelfSearch;
    }
    if (clear && w::IconBtn(ui, t, Ui::Id("sh.search.x"), {r.right - 15, (r.top + r.bottom) / 2}, 26, glyph::Close, 15, live)) m.shelfQuery.clear();
}

float ChipsWidth(Canvas& c, float pad) {
    float w = (kFilterN - 1) * kChipGap;
    for (const wchar_t* f : kFilters) w += c.Measure(f, Weight::Medium, 11.5f) + 2 * pad;
    return w;
}

void Filters(PageCtx& p, const D2D1_RECT_F& r, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    View& v = V();
    float text = ChipsWidth(c, 0);
    float pad = (std::clamp)((Width(r) - text) / (2 * kFilterN), kChipPadMin, kChipPadMax); // as roomy as the row allows
    float x = r.left;
    for (int i = 0; i < kFilterN; ++i) {
        float w = c.Measure(kFilters[i], Weight::Medium, 11.5f) + 2 * pad;
        if (w::Chip(ui, p.shell, Ui::Id("sh.filter", i), Rect(x, r.top, w, Height(r)), kFilters[i], v.filter == i) && live) {
            v.filter = i;
            v.scroll = 0;
        }
        x += w + kChipGap;
    }
}

void HoverButtons(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it, const Ui::Hit& xh, const Ui::Hit& oh) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    D2D1_POINT_2F xc = {r.right - 16, r.top + 16}, oc = {r.right - 16, r.bottom - 16};
    bool art = it.kind == sys::ShelfKind::Image;
    Color back = art ? Color{0, 0, 0, 0.5f} : t.track.Alpha(0.9f);
    c.Dot(xc, 11, back);
    c.Icon(glyph::Close, xc, 14, (art ? Color{1, 1, 1, 0.8f} : t.textDim).Mix(art ? Color{1, 1, 1, 1} : t.text, xh.hover ? 1.0f : 0));
    c.Dot(oc, 11, oh.hover ? t.accent : back);
    c.Icon(kOpenGlyph, oc, 16, oh.hover ? t.onAccent : art ? Color{1, 1, 1, 0.9f} : t.textDim);
}

bool Slot(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, const sys::ShelfItem& it, bool live, uint64_t& target) { // false once the card is gone
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    View& v = V();
    uint32_t id = Ui::Id("sh.card", (int)it.id);
    D2D1_POINT_2F ms = ui.In().mouse, ps = ui.In().pressPos;
    D2D1_RECT_F seen = {r.left, (std::max)(r.top, view.top), r.right, (std::min)(r.bottom, view.bottom)}; // only the part in view takes clicks
    bool over = live && !v.carry && Inside(seen, ms);
    D2D1_RECT_F xr = Rect(r.right - 16 - kBtn / 2, r.top + 16 - kBtn / 2, kBtn, kBtn), orr = Rect(r.right - 16 - kBtn / 2, r.bottom - 16 - kBtn / 2, kBtn, kBtn);
    Ui::Hit xh = over && Inside(view, {xr.left, xr.top}) ? ui.Button(Ui::Id("sh.x", (int)it.id), xr) : Ui::Hit{}; // first, so a press on them isn't the card's
    Ui::Hit oh = over && Inside(view, {orr.right - 1, orr.bottom - 1}) ? ui.Button(Ui::Id("sh.open", (int)it.id), orr) : Ui::Hit{};
    Ui::Hit h = live ? ui.Button(id, seen) : Ui::Hit{};
    h.hover = h.hover && !xh.hover && !oh.hover;
    if (ui.IsActive(id) && ui.In().down && !v.carried && std::hypot(ms.x - ps.x, ms.y - ps.y) > kDragStart) {
        v.carry = it.id; // carried inside the panel first: onto another card it stacks, out of the panel it drops anywhere
        v.carried = true;
    }
    bool aimed = v.carry && v.carrySlot < 0 && v.carry != it.id && Inside(seen, ms) && m.shelf.CanMerge(v.carry, it.id);
    if (aimed) target = it.id;
    float hover = ui.Smooth(id + 1, h.hover || (ui.IsActive(id) && !v.carry) ? 1.0f : 0, 16);
    float press = ui.Smooth(id + 2, h.down && !v.carried ? 1.0f : 0, 26);
    D2D1_RECT_F body = Inset(r, 1.5f * press, 1.5f * press);
    Card(p, body, it, hover, aimed);
    if (v.carry == it.id) c.Fill(body, kRadius, t.card.Alpha(0.6f)); // its place, while it's carried
    if (v.copiedId == it.id && ui.Now() - v.copiedAt < kCopiedSec) { // a tick while it's fresh on the clipboard
        D2D1_POINT_2F o = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        c.Dot(o, 20, t.accent);
        c.Icon(glyph::Check, o, 22, t.onAccent, true);
        ui.KeepAlive();
    }
    if (over || xh.hover || oh.hover) HoverButtons(p, r, it, xh, oh);
    if (xh.clicked) {
        if (v.preview == it.id) v.preview = 0;
        m.shelf.Remove(it.id); // the card goes; a file stays where it was
        ui.Wake();
        return false;
    }
    if (oh.clicked) {
        v.preview = it.id;
        v.picks.assign(it.files.size(), false);
        v.pvScroll = 0;
    }
    if (h.clicked && !v.carried) Do(p, it.id, kCopy);
    if (live && !v.carry && ui.RClick(seen)) {
        v.menu = it.id;
        v.clearMenu = false;
        v.menuAt = ms;
        v.menuSince = ui.Now();
    }
    return true;
}

void Grid(PageCtx& p, const D2D1_RECT_F& view, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    View& v = V();
    const auto& items = m.shelf.Items();
    bool tall = Height(p.area) > Width(p.area);
    int cols = tall ? 2 : (std::max)(2, (int)((Width(view) + kGap) / (kCardMin + kGap)));
    float cw = (Width(view) - (cols - 1) * kGap) / cols, ch = tall ? kCardTallH : kCardH;
    auto rows = [&](size_t n) { return (float)((n + cols - 1) / cols); };
    bool heads = !v.pinned.empty();
    float content = (heads ? kHeadH + rows(v.pinned.size()) * (ch + kGap) + (v.recent.empty() ? 0 : kHeadH) : 0) + rows(v.recent.size()) * (ch + kGap) - kGap;
    float off = sheet::Scroll(ui, "sh.scroll", v.scroll, view, content, live);
    c.PushClip({view.left - 4, view.top, view.right + 4, view.bottom});
    float y = view.top - off;
    uint64_t target = 0;
    bool alive = true;
    auto section = [&](const std::vector<size_t>& list, const wchar_t* label) {
        if (heads && !list.empty()) {
            if (y + kHeadH > view.top) c.TextSp(label, Rect(view.left + 4, y + 4, 160, 14), Weight::Medium, 10, t.textLow, 1.4f);
            y += kHeadH;
        }
        for (size_t k = 0; k < list.size() && alive; ++k) {
            D2D1_RECT_F r = Rect(view.left + (k % cols) * (cw + kGap), y + (k / cols) * (ch + kGap), cw, ch);
            if (r.bottom < view.top || r.top > view.bottom) continue; // out of view: not drawn, not asked for its preview
            if (list[k] < items.size()) alive = Slot(p, r, view, items[list[k]], live, target);
        }
        y += rows(list.size()) * (ch + kGap);
    };
    section(v.pinned, L"PINNED");
    if (alive) section(v.recent, L"RECENT");
    c.PopClip();
    sheet::Fades(c, t, view, off, content);
    if (!alive || v.carrySlot >= 0) return; // one went: the rest moves up next frame; a file carried out of the open stack is the preview's
    if (v.carry && ui.In().released) { // let go inside the panel: onto a card it stacks, anywhere else nothing happens
        if (target && m.shelf.Merge(v.carry, target)) Toast(v, ui.Now(), L"Stacked");
        v.carry = 0;
    } else if (v.carry && ui.In().down && !Inside(Inset(p.area, -kOutSlack, -kOutSlack), ui.In().mouse)) {
        m.shelfDrag = v.carry; // out of the panel: the shell's drag takes over, after this frame
        m.shelfPicks.clear();
        v.carry = 0;
    }
}

void Ghost(PageCtx& p) { // the carried card, small, under the mouse
    View& v = V();
    const sys::ShelfItem* it = v.carry ? p.m.shelf.Find(v.carry) : nullptr;
    if (!it) return;
    bool file = v.carrySlot >= 0 && (size_t)v.carrySlot < it->files.size(); // one file out of an open stack
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    D2D1_POINT_2F ms = p.ui.In().mouse;
    D2D1_RECT_F g = Rect(ms.x - kGhostW / 2, ms.y - kGhostH - 6, kGhostW, kGhostH);
    c.Fill(Inset(g, -2, -2), kGhostH / 2 + 2, Color{0, 0, 0, 0.18f});
    c.Fill(g, kGhostH / 2, t.cardHi.Mix(t.accent, 0.18f));
    c.Stroke(g, kGhostH / 2, t.accent.Alpha(0.7f), 1);
    c.Icon(file ? (it->kind == sys::ShelfKind::Image ? glyph::Image : glyph::File) : KindGlyph(*it), {g.left + 20, (g.top + g.bottom) / 2}, 17, t.accentHi);
    c.Text(file ? it->files[(size_t)v.carrySlot].name : it->Name(), Rect(g.left + 36, g.top, kGhostW - 48, kGhostH), Weight::Medium, 12, t.text);
    p.ui.Wake();
}

void Hot(PageCtx& p, const D2D1_RECT_F& grid) { // something is being carried over from outside: the shelf lights up to take it
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    float hot = ui.Smooth(Ui::Id("sh.hot"), p.m.shelfHot ? 1.0f : 0, 14);
    if (hot <= 0.01f) return;
    c.PushClip(grid);
    c.Glow({(grid.left + grid.right) / 2, grid.bottom}, Width(grid) * 0.6f, Height(grid) * 0.9f, t.accent.Alpha(0.22f * hot));
    c.PopClip();
    c.Stroke(Inset(grid, 1, 1), 22, t.accent.Alpha(0.8f * hot), 2);
}

void ToastPill(PageCtx& p) {
    View& v = V();
    Ui& ui = p.ui;
    double age = ui.Now() - v.toastAt;
    if (age > kToastSec || v.toast.empty()) return;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    float a = anim::Clamp01((float)(kToastSec - age) / 0.3f) * anim::Clamp01((float)age / 0.12f); // in quickly, out softly
    float w = c.Measure(v.toast, Weight::Medium, 12) + 40;
    D2D1_RECT_F r = Rect((p.area.left + p.area.right - w) / 2, p.area.bottom - 40, w, 30);
    c.Fill(r, 15, t.accent.Alpha(a));
    c.Icon(glyph::Check, {r.left + 18, (r.top + r.bottom) / 2}, 15, t.onAccent.Alpha(a), true);
    c.Text(v.toast, {r.left + 30, r.top, r.right - 10, r.bottom}, Weight::Medium, 12, t.onAccent.Alpha(a));
    ui.Wake();
}

}

void ShelfClosed() {
    View& v = V();
    v.preview = v.menu = v.carry = 0;
    v.clearMenu = false;
    v.carrySlot = -1;
}

void DrawShelf(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    View& v = V();
    const D2D1_RECT_F& a = p.area;
    Tick(m);
    if (ui.Now() - v.agedAt > kAgeEvery) { // a timer does this too; the clock may have jumped past it in sleep
        v.agedAt = ui.Now();
        m.shelf.Age();
    }
    if (ui.In().pressed) v.carried = false; // a new press starts clean
    if (!ui.In().down && !ui.In().released) v.carry = 0, v.carrySlot = -1;
    if (v.preview && !m.shelf.Find(v.preview)) v.preview = 0;
    if (v.menu && !m.shelf.Find(v.menu)) v.menu = 0;
    bool noting = m.shelfField == Model::kShelfNote;
    bool live = !v.preview && !v.menu && !v.clearMenu && !noting; // an overlay up: the grid under it is still
    const auto& items = m.shelf.Items();

    // the head: search, filters, clear. one row on a wide panel, two in the sidebar
    float wideNeed = kSearchW + 10 + ChipsWidth(c, kChipPadMin) + 10 + kClearW;
    bool oneRow = Width(a) >= wideNeed;
    D2D1_RECT_F search = oneRow ? Rect(a.left, a.top, kSearchW, kRowH) : Rect(a.left, a.top, Width(a) - kClearW - 10, kRowH);
    D2D1_RECT_F clearR = Rect(a.right - kClearW, a.top + (kRowH - 28) / 2, kClearW, 28);
    D2D1_RECT_F chips = oneRow ? D2D1_RECT_F{search.right + 10, a.top + (kRowH - kChipH) / 2, clearR.left - 10, a.top + (kRowH + kChipH) / 2}
                               : Rect(a.left, a.top + kRowH + 8, Width(a), kChipH);
    SearchField(p, search, live);
    if (sheet::Act(ui, t, Ui::Id("sh.clear"), clearR, L"Clear", false, false, live && !items.empty())) {
        v.clearMenu = true;
        v.menuSince = ui.Now();
        v.menu = 0;
        v.menuAt = {clearR.right, clearR.bottom + 4};
    }
    Filters(p, chips, live);
    Refilter(v, m);

    D2D1_RECT_F grid = {a.left, chips.bottom + 12, a.right, a.bottom};
    Hot(p, grid);
    if (items.empty()) {
        if (!m.shelfHot) c.Stroke(Inset(grid, 1, 1), 22, t.hairTop, 1);
        sheet::Empty(c, t, grid, m.shelfHot ? glyph::Drop : glyph::Shelf, m.shelfHot ? L"Let go to keep it here" : L"Drop files, links or text here",
                     m.shelfCapture ? L"Everything you copy lands here too" : L"Onto the top of the screen, anytime");
    } else if (v.pinned.empty() && v.recent.empty()) {
        sheet::Empty(c, t, grid, kSearchGlyph, L"Nothing matches", m.shelfQuery.empty() ? L"Nothing of this kind yet" : L"Try another word, or All");
    } else {
        Grid(p, grid, live);
    }
    if (v.preview) Preview(p, a, !v.menu && !v.clearMenu && !noting);
    Ghost(p); // over the open card too: a file carried out of it
    if (v.menu || v.clearMenu) Menu(p, a);
    if (noting) NoteBar(p, a);
    ToastPill(p);
}
