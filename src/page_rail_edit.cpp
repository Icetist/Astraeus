// The rail's editor: the pages as the rail lays them out, over the wallpaper, with a gallery of every page beside them.
#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "history.h"
#include "page_sheet.h"

namespace {

History hist; // the pages, the one it opens on and the last-page switch, together

History::Snap Snap(const Model& m) {
    History::Snap s{(float)m.rememberPage, (float)m.defaultPage};
    for (int id : m.pages) s.push_back((float)id);
    return s;
}

History::Snap Defaults() {
    History::Snap s{1, (float)Model::kPgHome};
    for (int id : Model::DefaultPages()) s.push_back((float)id);
    return s;
}

void Load(Model& m, const History::Snap& s) {
    constexpr size_t kHead = 2; // the switch and the opening page come first
    if (s.size() <= kHead) return; // one page always stays
    if (m.rememberPage != (int)s[0]) m.SetPref(m.rememberPage, L"RememberPage", (int)s[0]);
    if (m.defaultPage != (int)s[1]) m.SetPref(m.defaultPage, L"DefaultPage", (int)s[1]);
    std::vector<int> pages;
    for (size_t i = kHead; i < s.size(); ++i) pages.push_back((int)s[i]);
    m.SetPages(pages);
}

constexpr float kStep = 54, kRailPad = 14, kCellHalf = 22, kCellInset = 10, kEndPad = 14; // the rail's own spacing, and its cells
constexpr float kRemoveSize = 18, kDragStart = 6, kLitH = 40; // a page being arranged, and the mark on the one it opens on
constexpr float kPanelW = 560, kPad = 18, kHeadH = 30, kHeadGap = 12, kBtnW = 84, kSwitchH = 30, kNoteH = 16; // the gallery
constexpr int kCols = 2;

int heldPage = -1; // the one being dragged
sheet::Ids heldPages; // the set as it would land
float heldGrab = 0; // where along the rail the mouse holds it

D2D1_POINT_2F Spot(const D2D1_RECT_F& box, bool across, int n, int i) { // the middle of the i-th of n
    if (!across) return {(box.left + box.right) / 2, box.top + kRailPad + kStep * (i + 0.5f)};
    float step = (Width(box) - 2 * kEndPad) / n;
    return {box.left + kEndPad + step * (i + 0.5f), (box.top + box.bottom) / 2};
}

D2D1_RECT_F CellAt(const D2D1_RECT_F& box, bool across, int n, D2D1_POINT_2F o) {
    float halfW = across ? (std::min)(28.0f, (Width(box) - 2 * kEndPad) / n / 2) : Width(box) / 2 - kCellInset;
    float halfH = across ? Height(box) / 2 - 4 : kCellHalf;
    return {o.x - halfW, o.y - halfH, o.x + halfW, o.y + halfH};
}

void Rail(PageCtx& p, const D2D1_RECT_F& box, bool across) { // tap: it opens on that page. hover for the x, drag to move
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();
    sheet::Ids shown = m.pages;
    const sheet::Ids& order = heldPage >= 0 ? heldPages : shown;
    int n = (int)order.size(), removed = -1, tapped = -1;
    int opens = m.HasPage(m.defaultPage) ? m.defaultPage : m.pages.front();
    float pick = ui.Smooth(Ui::Id("rl.pick"), m.rememberPage ? 0.35f : 1.0f, 14); // the pick dims while the last page wins
    D2D1_RECT_F local = Rect(0, 0, Width(box), Height(box)); // the eased places don't count the rise it comes in on
    for (int i = 0; i < n; ++i) {
        int id = order[(size_t)i];
        D2D1_POINT_2F want = Spot(local, across, n, i);
        float x = ui.Smooth(Ui::Id("rl.x", id), want.x, 18), y = ui.Smooth(Ui::Id("rl.y", id), want.y, 18);
        if (id == heldPage) continue; // drawn last, on top of the rest
        D2D1_POINT_2F o = {box.left + x, box.top + y};
        D2D1_RECT_F cell = CellAt(box, across, n, o);
        bool over = heldPage < 0 && Inside(cell, in.mouse), canGo = n > 1; // one page always stays
        D2D1_POINT_2F corner = {cell.right - kRemoveSize / 2 + 2, cell.top + kRemoveSize / 2 - 2};
        Ui::Hit xh = over && canGo ? ui.Button(Ui::Id("rl.remove", id), Rect(corner.x - kRemoveSize / 2, corner.y - kRemoveSize / 2, kRemoveSize, kRemoveSize)) : Ui::Hit{}; // first: the first button under a press takes it
        if (over && canGo) ui.Name(std::wstring(L"Remove ") + Model::PageName(id));
        uint32_t body = Ui::Id("rl.body", id);
        Ui::Hit h = ui.Button(body, cell);
        ui.Name(Model::PageName(id));
        float lit = ui.Smooth(Ui::Id("rl.lit", id), id == opens ? pick : 0, 14), hover = ui.Smooth(Ui::Id("rl.hov", id), h.hover ? 1.0f : 0, 16);
        if (lit > 0.01f) { // as the rail marks a page
            float half = kLitH / 2;
            D2D1_RECT_F mark = across ? D2D1_RECT_F{cell.left, o.y - half + 1, cell.right, o.y + half - 1} : D2D1_RECT_F{cell.left + 3, o.y - half, cell.right - 3, o.y + half};
            c.Fill(mark, 20, t.accent.Alpha(0.13f * lit));
            if (across) c.Fill({o.x - 11, box.bottom - 6, o.x + 11, box.bottom - 3}, 1.5f, t.accent.Alpha(lit));
            else c.Fill({box.left + 5, o.y - 10, box.left + 8, o.y + 10}, 1.5f, t.accent.Alpha(lit));
        }
        c.Icon(Model::PageGlyph(id), o, across ? 22.0f : 23.0f, lit > 0.5f ? t.accentHi : t.textDim.Mix(t.text, hover), lit > 0.5f);
        if (over) {
            c.Stroke(cell, 14, t.accent.Alpha(0.7f), 1.5f);
            if (canGo) {
                c.Dot(corner, kRemoveSize / 2, xh.hover ? sheet::kDanger : t.bg);
                c.Icon(glyph::Close, corner, 11, xh.hover ? Color{1, 1, 1, 1} : t.text);
            }
        }
        if (xh.clicked) removed = id;
        else if (h.clicked) tapped = id;
        if (heldPage < 0 && ui.IsActive(body) && in.down && std::hypot(in.mouse.x - in.pressPos.x, in.mouse.y - in.pressPos.y) > kDragStart) { // picked up
            heldPage = id;
            heldPages = shown;
            heldGrab = across ? in.pressPos.x - o.x : in.pressPos.y - o.y;
        }
    }
    if (heldPage >= 0) { // it slides along the rail; the nearest place is the one it would land in
        ui.Wake();
        float along = (across ? in.mouse.x : in.mouse.y) - heldGrab;
        int best = 0;
        float bestD = 1e12f;
        for (int k = 0; k < n; ++k) {
            D2D1_POINT_2F q = Spot(box, across, n, k);
            float d = std::fabs((across ? q.x : q.y) - along);
            if (d < bestD) bestD = d, best = k;
        }
        heldPages.erase(std::find(heldPages.begin(), heldPages.end(), heldPage));
        heldPages.insert(heldPages.begin() + best, heldPage);
        D2D1_POINT_2F mid = Spot(box, across, n, best);
        D2D1_POINT_2F o = across ? D2D1_POINT_2F{along, mid.y} : D2D1_POINT_2F{mid.x, along};
        D2D1_RECT_F cell = CellAt(box, across, n, o);
        c.Fill(cell, 14, t.cardHi);
        c.Stroke(cell, 14, t.accent, 1.8f);
        ui.Button(Ui::Id("rl.body", heldPage), cell); // the press stays with it
        ui.NoStop();
        c.Icon(Model::PageGlyph(heldPage), o, across ? 22.0f : 23.0f, t.text);
        if (!in.down) { // let go: the order it landed in is kept
            heldPage = -1;
            if (!(heldPages == shown)) m.SetPages(heldPages.Vec());
        }
    }
    if (removed >= 0) {
        shown.erase(std::find(shown.begin(), shown.end(), removed));
        m.SetPages(shown.Vec());
    } else if (tapped >= 0) {
        m.SetPref(m.defaultPage, L"DefaultPage", tapped);
        if (m.rememberPage) m.SetPref(m.rememberPage, L"RememberPage", 0); // picking one means you want it
    }
}

float GalleryH() {
    int rows = (Model::kPageIds + kCols - 1) / kCols;
    return 2 * kPad + kHeadH + kHeadGap + kSwitchH + 6 + rows * sheet::kPickH + (rows - 1) * sheet::kPickGap + 10 + kNoteH;
}

void Gallery(PageCtx& p, const D2D1_RECT_F& panel) { // every page: a tap puts it on the rail or takes it off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F a = Inset(panel, kPad, kPad);
    float resetX = a.right - 2 * kBtnW - 16 - history::kResetW, histX = resetX - 8 - history::kW;
    c.Text(L"Pages", Rect(a.left + 2, a.top, histX - a.left - 10, kHeadH), Weight::Semi, 15, t.text);
    if (sheet::Act(ui, t, Ui::Id("rl.done"), Rect(a.right - kBtnW, a.top, kBtnW, kHeadH), L"Done", true, false, true)) p.wantClose = true;
    if (sheet::Act(ui, t, Ui::Id("rl.settings"), Rect(a.right - 2 * kBtnW - 8, a.top, kBtnW, kHeadH), L"Settings", false, false, true)) m.studioToSettings = true, p.wantClose = true; // what the rail's right click used to open
    if (sheet::Act(ui, t, Ui::Id("rl.reset"), Rect(resetX, a.top, history::kResetW, kHeadH), L"Reset", false, false, Snap(m) != Defaults())) Load(m, Defaults()); // undo brings it back
    int go = history::Buttons(ui, t, "rl.hist", histX, a.top + kHeadH / 2, hist, true);
    if (go < 0) Load(m, hist.Undo());
    else if (go > 0) Load(m, hist.Redo());
    float y = a.top + kHeadH + kHeadGap;
    c.Text(L"Open on the last page", Rect(a.left + 2, y, Width(a) - 60, kSwitchH), Weight::Regular, 11.5f, t.text);
    if (w::Switch(ui, t, Ui::Id("rl.last"), {a.right - 24, y + kSwitchH / 2}, m.rememberPage != 0)) m.SetPref(m.rememberPage, L"RememberPage", m.rememberPage ? 0 : 1);
    ui.Name(L"Open on the last page");
    y += kSwitchH + 6;
    float cw = (Width(a) - (kCols - 1) * sheet::kPickGap) / kCols;
    int toggle = -1;
    for (int id = 0; id < Model::kPageIds; ++id) {
        D2D1_RECT_F card = Rect(a.left + (id % kCols) * (cw + sheet::kPickGap), y + (id / kCols) * (sheet::kPickH + sheet::kPickGap), cw, sheet::kPickH);
        if (sheet::PickCard(ui, t, Ui::Id("rl.pick.card", id), card, Model::PageGlyph(id), Model::PageName(id), Model::PageAbout(id), m.HasPage(id))) toggle = id;
    }
    c.Text(L"Tap a page on the rail to open on it. Past five, the rail scrolls", Rect(a.left + 2, a.bottom - kNoteH, Width(a) - 4, kNoteH), Weight::Light, 10.5f, t.textDim);
    if (toggle < 0) return;
    sheet::Ids shown = m.pages;
    auto at = std::find(shown.begin(), shown.end(), toggle);
    if (at == shown.end()) shown.push_back(toggle);
    else if (shown.size() > 1) shown.erase(at); // one page always stays
    m.SetPages(shown.Vec());
}

}

void DrawPageStudio(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    const Input& in = ui.In();
    hist.Sync(m.studioRun, Snap(m), !in.down && heldPage < 0); // a drag counts once it lands
    bool across = m.railAcross;
    float panelW = (std::min)(kPanelW, Width(a) - 2 * sheet::kStudioEdge), panelH = GalleryH();
    float along = across ? m.railLen : ui.Smooth(Ui::Id("rl.len"), kStep * (float)m.pages.size() + 2 * kRailPad, 18); // down the side it holds every page, a step each
    float railW = across ? along : m.railThick, railH = across ? m.railThick : along;
    float wholeW = across ? (std::max)(railW, panelW) : railW + sheet::kPanelGap + panelW;
    float wholeH = across ? railH + sheet::kPanelGap + panelH : (std::max)(railH, panelH);
    float left = (a.left + a.right - wholeW) / 2, top = a.top + (Height(a) - wholeH) / 2; // the two together, in the middle of the screen
    D2D1_RECT_F rail = across ? Rect((a.left + a.right - railW) / 2, top, railW, railH) : Rect(left, top + (wholeH - railH) / 2, railW, railH);
    D2D1_RECT_F panel = across ? Rect((a.left + a.right - panelW) / 2, rail.bottom + sheet::kPanelGap, panelW, panelH) : Rect(rail.right + sheet::kPanelGap, top + (wholeH - panelH) / 2, panelW, panelH);
    sheet::Surface(c, t, panel, t.cardHi);
    Gallery(p, panel);
    sheet::Surface(c, t, rail, t.bg.Alpha(sheet::kBoardFill));
    Rail(p, rail, across);
    bool onSheet = Inside(rail, in.pressPos) || Inside(panel, in.pressPos);
    if (heldPage < 0 && in.released && !onSheet) p.wantClose = true; // a click away, as in the long pill's editor
    if (in.rclick && !Inside(rail, in.mouse) && !Inside(panel, in.mouse)) p.wantClose = true;
}
