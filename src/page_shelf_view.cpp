// The shelf's overlays: a card open full size (stacks with their files to pick from), the right-click and clear menus,
// the note being typed, and what every action does.
#include <cmath>

#include "anim.h"
#include "shelf_ui.h"
#include "starfall.h"

namespace shelfui {

namespace {

using K = sys::ShelfKind;
constexpr float kMenuW = 206, kMenuRow = 27, kMenuPad = 6, kMenuSep = 9, kMenuCap = 24;
constexpr float kHeadH = 46, kBarH = 30, kBarGap = 8; // the preview's title row and its row of actions
constexpr float kPillPad = 14;
constexpr float kTile = 104, kRowH = 42; // an open stack: picture tiles, file rows
constexpr float kNoteBarH = 50;
constexpr int64_t kHourSecs = 3600, kDaySecs = 24 * 3600;
constexpr size_t kPreviewText = 64 * 1024;

struct Entry {
    const wchar_t* label;
    Action act;
    bool danger = false, gap = false; // gap: a hairline above it
};

int64_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    constexpr int64_t kUnixDiff = 11644473600LL;
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ULL) - kUnixDiff;
}

Color Solid(const Theme& t) { // an overlay's body: never see-through, whatever the look
    Color b = t.bg;
    b.a = 1;
    return b.Mix(Color{t.cardHi.r, t.cardHi.g, t.cardHi.b, 1}, 0.55f);
}

std::vector<size_t> Picked(const View& v) {
    std::vector<size_t> out;
    for (size_t i = 0; i < v.picks.size(); ++i)
        if (v.picks[i]) out.push_back(i);
    return out;
}

bool Loose(const sys::ShelfItem& it) { // has a real file to show or copy the path of
    return std::any_of(it.files.begin(), it.files.end(), [](const sys::ShelfFile& f) { return !f.owned; });
}

std::vector<Entry> Entries(const Model& m, const sys::ShelfItem& it) {
    std::vector<Entry> e = {{L"Paste", kPaste}};
    if (m.shelf.CanPlain(it)) e.push_back({L"Paste as plain text", kPastePlain}); // files paste as their paths
    e.push_back({L"Copy", kCopy});
    size_t typed = e.size();
    if (it.kind == K::Link) {
        e.push_back({L"Open link", kOpen});
        if (sys::shelfkind::CleanLink(it.text) != it.text) e.push_back({L"Copy without tracking", kCopyClean});
    }
    if (it.kind == K::Color) e.insert(e.end(), {{L"Copy as HEX", kHex}, {L"Copy as RGB", kRgb}, {L"Copy as HSL", kHsl}});
    if (it.kind == K::Image) e.push_back({L"Save to Downloads", kSaveImages});
    if (it.HasFiles() && !it.Stack() && Loose(it)) e.push_back({L"Show in folder", kShowFolder});
    if (it.HasFiles() && Loose(it)) e.push_back({L"Copy path", kCopyPath});
    if (it.Stack()) e.push_back({L"Ungroup", kUngroup});
    if (e.size() > typed) e[typed].gap = true;
    e.push_back({it.note.empty() ? L"Add note" : L"Edit note", kNote, false, true});
    e.push_back({it.pinned ? L"Unpin" : L"Pin", kPin});
    e.push_back({L"Preview", kPreview});
    e.push_back({L"Delete", kDelete, true, true});
    return e;
}

bool Pill(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, bool primary, bool live) {
    return sheet::Act(ui, t, id, r, label.c_str(), primary, false, live);
}

void Header(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    View& v = V();
    if (w::IconBtn(ui, t, Ui::Id("sh.pv.close"), {r.left + 22, r.top + 23}, 32, glyph::Close, 19, live)) v.preview = 0;
    float right = r.right - 12;
    uint32_t pin = Ui::Id("sh.pv.pin");
    D2D1_RECT_F pr = Rect(right - 32, r.top + 7, 32, 32);
    Ui::Hit h = live ? ui.Button(pin, pr) : Ui::Hit{};
    float lit = ui.Smooth(pin + 1, it.pinned ? 1.0f : 0, 14);
    c.Dot({pr.left + 16, pr.top + 16}, 15, t.accent.Alpha(0.2f * lit).Mix(t.text.Alpha(0.09f), h.hover ? 0.5f : 0));
    PinMark(c, {pr.left + 16, pr.top + 16}, 15, t.textDim.Mix(t.accentHi, lit));
    if (h.clicked) Do(p, it.id, kPin);
    D2D1_RECT_F title = {r.left + 46, r.top + 6, pr.left - 8, r.top + 26};
    c.Text(it.Name(), title, Weight::Medium, 14, t.text);
    std::wstring sub = Detail(it) + (it.count > 1 ? L"  ·  added " + std::to_wstring(it.count) + L" times" : L"");
    c.Text(sub, {title.left, title.bottom, title.right, title.bottom + 16}, Weight::Light, 11, t.textDim);
}

std::vector<std::pair<std::wstring, Action>> BarActions(const Model& m, const sys::ShelfItem& it, size_t picked) {
    std::vector<std::pair<std::wstring, Action>> a = {{L"Paste", kPaste}, {L"Copy", kCopy}};
    std::wstring n = picked ? L" " + std::to_wstring(picked) : L"";
    if (it.kind == K::Link) a.push_back({L"Open", kOpen});
    if (it.kind == K::Link && sys::shelfkind::CleanLink(it.text) != it.text) a.push_back({L"Copy clean", kCopyClean});
    if (it.kind == K::Image) a.push_back({L"Save" + n, kSaveImages});
    if (it.Stack() && picked) a.insert(a.end(), {{L"Split" + n, kSplitPicked}, {L"Remove" + n, kDropPicked}});
    if (it.HasFiles() && !it.Stack() && Loose(it)) a.push_back({L"Show in folder", kShowFolder});
    if (it.HasFiles() && Loose(it)) a.push_back({L"Copy path", kCopyPath});
    a.push_back({it.note.empty() ? L"Add note" : L"Edit note", kNote});
    (void)m;
    return a;
}

void Bar(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it, bool live) { // what doesn't fit is still in the right-click menu
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    View& v = V();
    std::vector<size_t> picks = Picked(v);
    float x = r.left;
    int i = 0;
    for (const auto& [label, act] : BarActions(p.m, it, picks.size())) {
        float w = c.Measure(label, Weight::Medium, 12) + 2 * kPillPad;
        if (x + w > r.right) break;
        if (Pill(ui, p.shell, Ui::Id("sh.pv.act", i++), Rect(x, r.top, w, kBarH), label, act == kPaste, live)) Do(p, it.id, act, picks);
        x += w + kBarGap;
    }
}

void Scrolled(PageCtx& p, const D2D1_RECT_F& body, const sys::ShelfItem& it, bool live) { // a text or code, the whole of it
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    View& v = V();
    static float codeH = 0; // code measures itself as it draws: last frame's height scrolls this one
    bool code = it.kind == K::Code;
    const std::wstring& s = it.text.size() > kPreviewText ? it.text.substr(0, kPreviewText) : it.text;
    uint64_t key = it.id << 4 | 8;
    float h = code ? codeH : ParaHeight(s, Width(body), Weight::Regular, 13, key);
    float off = sheet::Scroll(ui, "sh.pv.scroll", v.pvScroll, body, h, live);
    c.PushClip(body);
    if (code) {
        c.Fill(body, 10, t.track.Alpha(0.35f));
        Code(c, t, s, {body.left + 10, body.top + 8 - off}, Width(body) - 20, key, codeH);
        codeH += 16;
    } else {
        Para(c, s, {body.left, body.top - off, body.right, body.top - off + 1e5f}, Weight::Regular, 13, t.text, key); // the same layout the height came from
    }
    c.PopClip();
    sheet::Fades(c, t, body, off, h);
}

void LinkView(PageCtx& p, const D2D1_RECT_F& b, const sys::ShelfItem& it) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    sys::shelfkind::Link l = sys::shelfkind::ReadLink(it.text);
    D2D1_POINT_2F o = {b.left + 22, b.top + 22};
    c.Dot(o, 22, t.accent.Alpha(0.18f));
    c.Icon(kLinkGlyph, o, 26, t.accentHi);
    c.Text(l.site, Rect(b.left + 56, b.top + 4, Width(b) - 56, 22), Weight::Medium, 16, t.text);
    c.Text(l.type, Rect(b.left + 56, b.top + 26, Width(b) - 56, 16), Weight::Light, 11.5f, t.textDim);
    uint64_t key = it.id << 4;
    Para(c, l.title, Rect(b.left, b.top + 56, Width(b), 44), Weight::Regular, 15, t.text, key | 9);
    Para(c, it.text, Rect(b.left, b.top + 104, Width(b), 46), Weight::Light, 12, t.textDim, key | 10);
    std::wstring clean = sys::shelfkind::CleanLink(it.text);
    if (clean != it.text && Height(b) > 190) {
        c.TextSp(L"WITHOUT TRACKING", Rect(b.left, b.top + 158, Width(b), 12), Weight::Medium, 9.5f, t.textLow, 1.4f);
        Para(c, clean, Rect(b.left, b.top + 174, Width(b), Height(b) - 174), Weight::Light, 12, t.accentHi, key | 11);
    }
}

void ColorView(PageCtx& p, const D2D1_RECT_F& b, const sys::ShelfItem& it, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    float side = (std::min)(Height(b), Width(b) * 0.42f);
    D2D1_RECT_F sw = Rect(b.left, b.top, side, side);
    c.Fill(sw, 18, Color::Rgb(it.rgb));
    c.Stroke(sw, 18, Color{1, 1, 1, 0.2f}, 1);
    float x = sw.right + 20, w = b.right - x;
    const Action acts[3] = {kHex, kRgb, kHsl};
    const wchar_t* names[3] = {L"HEX", L"RGB", L"HSL"};
    for (int i = 0; i < 3; ++i) {
        float y = b.top + 6 + i * 44;
        c.TextSp(names[i], Rect(x, y, 60, 12), Weight::Medium, 9.5f, t.textLow, 1.4f);
        c.Text(sys::shelfkind::ColorText(it.rgb, i), Rect(x, y + 14, w - 74, 22), Weight::Medium, 14, t.text);
        if (sheet::Act(ui, t, Ui::Id("sh.pv.col", i), Rect(b.right - 64, y + 8, 64, 26), L"Copy", false, false, live)) Do(p, it.id, acts[i]);
    }
}

void Single(PageCtx& p, const D2D1_RECT_F& b, const sys::ShelfItem& it) { // one picture big, or one file on glass
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    const Art* big = Thumb(c, p.m, it.id, 0, true);
    const Art* little = big ? nullptr : Thumb(c, p.m, it.id, 0, false); // while the big one is made
    const Art* art = big ? big : little;
    if (it.kind == K::Image && art && !art->icon) {
        Cover(c, art->bmp.Get(), art->brush.Get(), b, 10, 1, true);
        return;
    }
    constexpr float kBigTile = 96;
    D2D1_RECT_F tile = Rect(b.left, b.top, kBigTile, kBigTile);
    Frosted(c, tile, 22, art, KindGlyph(it), t);
    const sys::ShelfFile& f = it.files[0];
    float x = tile.right + 18;
    c.Text(f.name, Rect(x, b.top + 10, b.right - x, 22), Weight::Medium, 15, t.text);
    c.Text(f.dir ? L"Folder" : f.owned ? L"Image kept by the shelf, encrypted" : L"File", Rect(x, b.top + 34, b.right - x, 16), Weight::Light, 11.5f, t.textDim);
    if (!f.owned) Para(c, f.path, Rect(x, b.top + 56, b.right - x, Height(b) - 56), Weight::Light, 12, t.textDim, it.id << 4 | 12);
}

void StackView(PageCtx& p, const D2D1_RECT_F& b, const sys::ShelfItem& it, bool live) { // every file of a stack, to pick from or carry out
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    View& v = V();
    if (v.picks.size() != it.files.size()) v.picks.assign(it.files.size(), false);
    bool grid = it.kind == K::Image;
    int cols = grid ? (std::max)(2, (int)((Width(b) + kGap) / (kTile + kGap))) : 1;
    float cw = grid ? (Width(b) - (cols - 1) * kGap) / cols : Width(b), ch = grid ? (std::min)(cw, kTile) : kRowH;
    int rows = ((int)it.files.size() + cols - 1) / cols;
    float content = rows * (ch + (grid ? kGap : 4));
    float off = sheet::Scroll(ui, "sh.pv.scroll", v.pvScroll, b, content, live && !v.carry);
    D2D1_POINT_2F ms = ui.In().mouse, ps = ui.In().pressPos;
    c.PushClip(b);
    for (size_t i = 0; i < it.files.size(); ++i) {
        D2D1_RECT_F r = Rect(b.left + (i % cols) * (cw + kGap), b.top - off + (i / cols) * (ch + (grid ? kGap : 4)), cw, ch);
        if (r.bottom < b.top || r.top > b.bottom) continue;
        uint32_t id = Ui::Id("sh.pv.file", (int)i);
        Ui::Hit h = live && !v.carry ? ui.Button(id, {r.left, (std::max)(r.top, b.top), r.right, (std::min)(r.bottom, b.bottom)}) : Ui::Hit{};
        if (ui.IsActive(id) && ui.In().down && !v.carried && std::hypot(ms.x - ps.x, ms.y - ps.y) > kDragStart) {
            v.carry = it.id; // a file carried out of the stack: off the list it splits, out of the panel it drops anywhere
            v.carrySlot = (int)i;
            v.carried = true;
        }
        bool picked = v.picks[i];
        float lit = ui.Smooth(id + 1, picked ? 1.0f : 0, 16), hover = ui.Smooth(id + 2, h.hover ? 1.0f : 0, 16);
        const Art* a = Thumb(c, p.m, it.id, i, false);
        if (grid) {
            if (a) Cover(c, a->bmp.Get(), a->brush.Get(), r, 12, v.carrySlot == (int)i ? 0.4f : 1);
            else c.Fill(r, 12, t.track);
            if (hover > 0.01f) c.Fill(r, 12, Color{1, 1, 1, 0.08f * hover});
            c.Stroke(Inset(r, 1, 1), 11, t.accent.Alpha(lit), 2.5f);
        } else {
            c.Fill(r, 12, t.cardHi.Mix(t.track, 0.5f * hover).Mix(t.accent.Alpha(0.25f), 0.4f * lit));
            D2D1_RECT_F ic = Rect(r.left + 40, r.top + 9, 24, 24);
            if (a) Cover(c, a->bmp.Get(), a->brush.Get(), ic, 4, 1, true);
            else c.Icon(it.files[i].dir ? glyph::Folder : glyph::File, {ic.left + 12, ic.top + 12}, 20, t.accentHi);
            c.Text(it.files[i].name, Rect(r.left + 74, r.top + 4, Width(r) - 84, 18), Weight::Medium, 12.5f, t.text);
            c.Text(it.files[i].owned ? L"Kept by the shelf" : L"in " + Folder(it.files[i].path), Rect(r.left + 74, r.top + 22, Width(r) - 84, 15), Weight::Light, 11,
                   t.textDim);
        }
        D2D1_POINT_2F dot = grid ? D2D1_POINT_2F{r.right - 15, r.top + 15} : D2D1_POINT_2F{r.left + 20, (r.top + r.bottom) / 2};
        if (picked || hover > 0.01f || !grid) { // a pick mark: filled when picked
            c.Dot(dot, 9, picked ? t.accent : grid ? Color{0, 0, 0, 0.4f} : t.track);
            c.Ring(dot, 9, picked ? t.accent : Color{1, 1, 1, grid ? 0.8f : 0.25f}, 1.5f);
            if (picked) c.Icon(glyph::Check, dot, 13, t.onAccent, true);
        }
        if (h.clicked && !v.carried) v.picks[i] = !v.picks[i];
    }
    c.PopClip();
    sheet::Fades(c, t, b, off, content);
    if (v.carrySlot < 0 || v.carry != it.id) return;
    std::vector<size_t> picks = v.picks[(size_t)v.carrySlot] ? Picked(v) : std::vector<size_t>{(size_t)v.carrySlot}; // the picked ones go together
    if (ui.In().released) {
        if (!Inside(b, ui.In().mouse) && Inside(p.area, ui.In().mouse)) Do(p, it.id, kSplitPicked, picks); // dropped beside the list: their own cards
        v.carry = 0;
        v.carrySlot = -1;
    } else if (ui.In().down && !Inside(Inset(p.area, -8, -8), ui.In().mouse)) {
        p.m.shelfDrag = it.id; // out of the panel: the shell's drag takes them
        p.m.shelfPicks = picks;
        v.carry = 0;
        v.carrySlot = -1;
    }
}

}

void Do(PageCtx& p, uint64_t id, Action a, const std::vector<size_t>& picks) {
    Model& m = p.m;
    View& v = V();
    double now = p.ui.Now();
    const sys::ShelfItem* it = m.shelf.Find(id);
    if (!it && a < kClearHour) return;
    using S = sys::Shelf;
    auto copy = [&](S::CopyHow how, const wchar_t* done) {
        if (!m.shelf.Copy(id, how, picks)) return Toast(v, now, L"Couldn't copy that");
        v.copiedId = id;
        v.copiedAt = now;
        Toast(v, now, done);
    };
    switch (a) {
    case kPaste:
    case kPastePlain: // after the frame: the drawer hands the keyboard back to the app first
        m.shelfPaste = id;
        m.shelfPasteHow = a == kPaste ? S::kCopy : S::kPlain;
        m.shelfPicks = picks;
        v.preview = 0;
        break;
    case kCopy: copy(S::kCopy, L"Copied"); break;
    case kCopyClean: copy(S::kClean, L"Copied without tracking"); break;
    case kHex: copy(S::kHex, L"Copied as HEX"); break;
    case kRgb: copy(S::kRgb, L"Copied as RGB"); break;
    case kHsl: copy(S::kHsl, L"Copied as HSL"); break;
    case kCopyPath: copy(S::kPath, picks.size() > 1 || (picks.empty() && it->Stack()) ? L"Paths copied" : L"Path copied"); break;
    case kNote:
        if (m.shelfField == Model::kShelfNote) m.CommitShelfNote();
        m.shelfField = Model::kShelfNote;
        m.shelfNoteFor = id;
        m.shelfEdit = it->note;
        break;
    case kPin:
        Toast(v, now, it->pinned ? L"Unpinned" : L"Pinned to the top");
        m.shelf.Pin(id, !it->pinned);
        break;
    case kPreview:
        v.preview = id;
        v.picks.assign(it->files.size(), false);
        v.pvScroll = 0;
        break;
    case kDelete:
        if (v.preview == id) v.preview = 0;
        m.shelf.Remove(id);
        break;
    case kOpen:
        if (m.shelf.OpenLink(id)) p.wantClose = true;
        break;
    case kShowFolder:
        m.shelf.ShowInFolder(id, picks.empty() ? 0 : picks[0]);
        p.wantClose = true;
        break;
    case kSaveImages: {
        int n = m.shelf.SaveImages(id, picks);
        Toast(v, now, n == 0 ? L"Nothing saved" : n == 1 ? L"Saved to Downloads" : L"Saved " + std::to_wstring(n) + L" to Downloads");
        break;
    }
    case kUngroup:
        m.shelf.Split(id, {});
        Toast(v, now, L"Ungrouped");
        break;
    case kSplitPicked:
        m.shelf.Split(id, picks);
        v.picks.assign(m.shelf.Find(id) ? m.shelf.Find(id)->files.size() : 0, false);
        if (!m.shelf.Find(id)) v.preview = 0;
        Toast(v, now, L"Split off");
        break;
    case kDropPicked:
        m.shelf.DropFiles(id, picks);
        v.picks.assign(m.shelf.Find(id) ? m.shelf.Find(id)->files.size() : 0, false);
        break;
    case kClearHour:
    case kClearDay:
    case kClearAll: { // the filter and the search say what; pinned and noted cards always stay
        int64_t from = a == kClearAll ? INT64_MIN : UnixNow() - (a == kClearHour ? kHourSecs : kDaySecs);
        int filter = v.filter;
        std::wstring query = m.shelfQuery;
        size_t n = m.shelf.ClearWhere([&](const sys::ShelfItem& x) { return x.at >= from && Matches(x, filter, query); });
        Toast(v, now, n ? L"Cleared " + std::to_wstring(n) : L"Nothing to clear");
        break;
    }
    }
}

void Menu(PageCtx& p, const D2D1_RECT_F& area) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    View& v = V();
    const sys::ShelfItem* it = v.menu ? p.m.shelf.Find(v.menu) : nullptr;
    std::vector<Entry> list;
    std::wstring cap;
    if (v.clearMenu) {
        static const wchar_t* const kNames[] = {L"everything", L"text", L"links", L"images", L"files", L"colours", L"code"};
        list = {{L"Last hour", kClearHour}, {L"Last 24 hours", kClearDay}, {L"Everything", kClearAll, true}};
        cap = std::wstring(L"Clears ") + kNames[(std::clamp)(v.filter, 0, 6)] + (p.m.shelfQuery.empty() ? L"" : L" found");
    } else if (it) {
        list = Entries(p.m, *it);
    } else {
        v.menu = 0;
        return;
    }
    float h = 2 * kMenuPad + (cap.empty() ? 0 : kMenuCap);
    for (const Entry& e : list) h += kMenuRow + (e.gap ? kMenuSep : 0);
    float x = v.clearMenu ? v.menuAt.x - kMenuW : v.menuAt.x, y = v.menuAt.y;
    x = (std::clamp)(x, area.left, area.right - kMenuW);
    if (y + h > area.bottom) y = (std::max)(area.top, v.clearMenu ? area.bottom - h : v.menuAt.y - h); // no room below: it opens upward
    float k = ui.SmoothFrom(Ui::Id("sh.menu.in", (int)(v.menu ^ (v.clearMenu ? 0x5A5A : 0))), 1, 0, 24);
    D2D1_RECT_F box = Rect(x, y + (1 - k) * 6, kMenuW, h);
    c.Fill(Rect(box.left, box.top + 4, kMenuW, h), 14, Color{0, 0, 0, 0.22f * k}); // a soft drop below
    c.Fill(box, 14, Solid(t));
    c.StrokeV(box, 14, t.hairTop.Mix(t.accent.Alpha(0.5f), 0.4f), t.hairBot, 1);
    float ry = box.top + kMenuPad;
    if (!cap.empty()) {
        c.Text(cap, Rect(box.left + 14, ry, kMenuW - 28, kMenuCap - 4), Weight::Light, 11, t.textDim);
        c.Text(L"pinned and noted stay", Rect(box.left + 14, ry + 13, kMenuW - 28, 12), Weight::Light, 9.5f, t.textLow);
        ry += kMenuCap + 6;
    }
    int i = 0;
    for (const Entry& e : list) {
        if (e.gap) {
            c.Line({box.left + 12, ry + kMenuSep / 2}, {box.right - 12, ry + kMenuSep / 2}, t.line, 1, false);
            ry += kMenuSep;
        }
        D2D1_RECT_F row = Rect(box.left + 5, ry, kMenuW - 10, kMenuRow);
        uint32_t id = Ui::Id("sh.menu.row", i++);
        Ui::Hit hit = ui.Button(id, row);
        float hv = ui.Smooth(id, hit.hover ? 1.0f : 0, 20);
        if (hv > 0.01f) c.Fill(row, 9, (e.danger ? sheet::kDanger.Alpha(0.22f) : t.track).Alpha(hv));
        c.Text(e.label, Inset(row, 10, 0), Weight::Regular, 12, e.danger ? sheet::kDanger : t.text);
        if (hit.clicked) {
            uint64_t target = v.menu;
            v.menu = 0;
            v.clearMenu = false;
            Do(p, target, e.act);
            return;
        }
        ry += kMenuRow;
    }
    bool away = (ui.In().pressed || ui.RClick(area)) && !Inside(box, ui.In().mouse);
    if (away && ui.Now() != v.menuSince) { // a click anywhere else closes it
        v.menu = 0;
        v.clearMenu = false;
    }
}

void NoteBar(PageCtx& p, const D2D1_RECT_F& area) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const sys::ShelfItem* it = m.shelf.Find(m.shelfNoteFor);
    if (!it) {
        m.shelfField = Model::kShelfNone;
        return;
    }
    D2D1_RECT_F bar = Rect(area.left, area.bottom - kNoteBarH, Width(area), kNoteBarH);
    c.Fill(Rect(bar.left, bar.top - 4, Width(bar), kNoteBarH + 4), 18, Color{0, 0, 0, 0.18f});
    c.Fill(bar, 18, Solid(t));
    c.Stroke(bar, 18, t.accent.Alpha(0.7f), 1.5f);
    c.Icon(glyph::Edit, {bar.left + 22, (bar.top + bar.bottom) / 2}, 17, t.accentHi);
    constexpr float kSaveW = 64;
    D2D1_RECT_F text = {bar.left + 42, bar.top, bar.right - kSaveW - 70, bar.bottom};
    float w = m.shelfEdit.empty() ? 0 : c.TextOnce(m.shelfEdit, text, Weight::Regular, 12.5f, t.text);
    if (m.shelfEdit.empty()) c.Text(L"A note for " + it->Name(), text, Weight::Light, 12.5f, t.textLow);
    if (std::fmod(ui.Now(), 1.0) < 0.55) c.Line({text.left + w + 1.5f, bar.top + 15}, {text.left + w + 1.5f, bar.bottom - 15}, t.text, 1.5f); // caret
    ui.KeepAlive();
    std::wstring left = std::to_wstring(sys::Shelf::kNoteMax - (std::min)(m.shelfEdit.size(), sys::Shelf::kNoteMax));
    c.Text(left, Rect(text.right + 4, bar.top, 56, kNoteBarH), Weight::Light, 11, t.textLow, Align::Right);
    if (sheet::Act(ui, t, Ui::Id("sh.note.save"), Rect(bar.right - kSaveW - 10, bar.top + 10, kSaveW, kNoteBarH - 20), L"Save", true, false, true)) {
        m.CommitShelfNote();
        return;
    }
    if (ui.In().pressed && !Inside(bar, ui.In().mouse)) m.CommitShelfNote(); // clicked away: kept, like the city field
}

void Preview(PageCtx& p, const D2D1_RECT_F& area, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    View& v = V();
    const sys::ShelfItem* it = p.m.shelf.Find(v.preview);
    if (!it) return;
    float k = ui.SmoothFrom(Ui::Id("sh.pv.in", (int)it->id), 1, 0, 18);
    D2D1_RECT_F r = {area.left, area.top + (1 - k) * 14, area.right, area.bottom + (1 - k) * 14};
    c.Fill(r, 20, Solid(t));
    c.StrokeV(r, 20, t.hairTop, t.hairBot, 1);
    if (t.stars) starfall::Seam(c, {r.left + 20, r.top + 0.5f}, {r.right - 20, r.top + 0.5f}, t, 0.8f);
    Header(p, Rect(r.left, r.top, Width(r), kHeadH), *it, live);
    if (!v.preview) return; // closed by its own button
    D2D1_RECT_F body = {r.left + 16, r.top + kHeadH + 8, r.right - 16, r.bottom - kBarH - 24};
    if (it->Stack()) StackView(p, body, *it, live);
    else if (it->HasFiles()) Single(p, body, *it);
    else if (it->kind == K::Link) LinkView(p, body, *it);
    else if (it->kind == K::Color) ColorView(p, body, *it, live);
    else Scrolled(p, body, *it, live);
    if (!p.m.shelf.Find(v.preview)) return; // split or removed from inside
    if (!it->note.empty()) c.Text(L"“" + it->note + L"”", Rect(r.left + 16, r.bottom - kBarH - 22, Width(r) - 32, 16), Weight::Light, 11, t.accentHi);
    Bar(p, Rect(r.left + 16, r.bottom - kBarH - 10, Width(r) - 32, kBarH), *it, live);
    if (live && ui.RClick(r)) v.preview = 0; // right click closes, like the sheets
}

}
