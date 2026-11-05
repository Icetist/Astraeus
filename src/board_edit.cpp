// The dashboard's tile editor: drag a tile to move it, its corner to resize it, x to take it off; a bar to add, start over or finish.
#include "board.h"

#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"

namespace {

constexpr float kBarW = 420, kBarH = 46, kBtnH = 32; // the editor's bar
constexpr float kGrip = 24; // the resize corner's hit box
constexpr float kPickCellW = 150, kPickCellH = 38; // a kind in the add list
constexpr int kPickCols = 4;
constexpr int kSizes[][2] = {{3, 2}, {2, 2}, {2, 1}, {1, 1}}; // a new tile takes the biggest of these that fits
double noRoomAt = -10; // a tile was picked with nowhere to go

D2D1_RECT_F PickBox(float w, const D2D1_RECT_F& bar, int n) { // the add list over the bar, sized for n kinds
    int cols = (std::min)(kPickCols, (std::max)(1, n));
    int lines = n == 0 ? 1 : (n + cols - 1) / cols;
    float pw = cols * kPickCellW + (cols + 1) * 8, ph = lines * kPickCellH + (lines + 1) * 8 + 26;
    return Rect((w - pw) / 2, bar.top - ph - 10, pw, ph);
}

bool Btn(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label, bool primary) { // a pill with an icon and a word
    Ui::Hit h = ui.Button(id, r);
    Canvas& c = ui.C();
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(r, Height(r) / 2, primary ? t.accent.Mix(t.accentHi, 0.4f * hov) : t.track.Mix(t.cardHi, 0.5f + 0.5f * hov));
    Color ink = primary ? t.onAccent : t.text;
    c.Icon(glyph, {r.left + 20, (r.top + r.bottom) / 2}, 16, ink, primary);
    c.Text(label, {r.left + 34, r.top, r.right - 10, r.bottom}, Weight::Medium, 12, ink);
    return h.clicked;
}

}

void Board::Edit() {
    editing = true;
    picking = false;
    grab = -1;
    Poke();
}

void Board::Save() { model.SetBoardTiles(name, tiles); } // this screen's own; the drawer passes the new set to polling on its next second

bool Board::Free(int x, int y, int w, int h, int skip) const {
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > Model::kBoardCols || y + h > rows) return false;
    for (int i = 0; i < (int)tiles.size(); ++i) {
        if (i == skip) continue;
        const auto& o = tiles[i];
        if (x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h) return false;
    }
    return true;
}

bool Board::Place(int kind) {
    for (const auto& s : kSizes)
        for (int y = 0; y + s[1] <= rows; ++y)
            for (int x = 0; x + s[0] <= Model::kBoardCols; ++x)
                if (Free(x, y, s[0], s[1], -1)) {
                    tiles.push_back({kind, x, y, s[0], s[1]});
                    Save();
                    return true;
                }
    return false;
}

void Board::Picker(const D2D1_RECT_F& bar) { // the kinds not on the board, as chips over the bar
    Canvas& c = *canvas;
    std::vector<int> left;
    for (int k = 0; k < Model::kBoardKinds; ++k)
        if (model.BoardOffered(k) && std::none_of(tiles.begin(), tiles.end(), [k](const Model::BoardTile& tl) { return tl.kind == k; })) left.push_back(k);
    int cols = (std::min)(kPickCols, (std::max)(1, (int)left.size()));
    D2D1_RECT_F box = PickBox(W, bar, (int)left.size());
    float pw = Width(box), ph = Height(box);
    c.Glow({(box.left + box.right) / 2, box.bottom}, pw * 0.7f, ph, Color{0, 0, 0, 0.35f});
    c.Fill(box, 18, t.cardHi.Alpha(1));
    c.StrokeV(box, 18, t.hairTop, t.hairBot, 1);
    c.TextSp(L"ADD A TILE", Rect(box.left + 14, box.top + 9, pw - 28, 14), Weight::Medium, 10, t.textLow, 1.4f);
    if (left.empty()) {
        c.Text(L"Every tile is on the board", Rect(box.left, box.top + 30, pw, kPickCellH), Weight::Light, 12, t.textDim, Align::Center);
        return;
    }
    for (int i = 0; i < (int)left.size(); ++i) {
        D2D1_RECT_F cell = Rect(box.left + 8 + (i % cols) * (kPickCellW + 8), box.top + 30 + (i / cols) * (kPickCellH + 8), kPickCellW, kPickCellH);
        Ui::Hit h = ui.Button(Ui::Id("bd.pick", left[i]), cell);
        c.Fill(cell, 12, t.track.Mix(t.accent.Alpha(0.35f), h.hover ? 1.0f : 0));
        c.Icon(Model::BoardGlyph(left[i]), {cell.left + 20, (cell.top + cell.bottom) / 2}, 18, h.hover ? t.accentHi : t.textDim);
        c.Text(Model::BoardName(left[i]), {cell.left + 36, cell.top, cell.right - 6, cell.bottom}, Weight::Medium, 12.5f, t.text);
        if (h.clicked) {
            picking = !Place(left[i]); // no room: the list stays and says so
            if (picking) noRoomAt = ui.Now();
        }
    }
    if (ui.Now() - noRoomAt < 2.5) {
        c.Text(L"No room for it: take a tile off or make one smaller", Rect(box.left, box.bottom + 2, pw, 16), Weight::Regular, 11, Color{0.95f, 0.45f, 0.45f, 1},
               Align::Center);
        ui.KeepAlive();
    }
}

void Board::EditFrame() {
    Canvas& c = *canvas;
    const Input& in = ui.In();
    for (int y = 0; y < rows; ++y) // the grid, faint: where tiles can go
        for (int x = 0; x < Model::kBoardCols; ++x) c.Fill(CellRect(x, y, 1, 1), 12, t.text.Alpha(0.035f));

    D2D1_RECT_F bar = Rect((W - kBarW) / 2, H - kMargin - kBarH - 6, kBarW, kBarH);
    int missing = 0;
    for (int k = 0; k < Model::kBoardKinds; ++k)
        missing += model.BoardOffered(k) && std::none_of(tiles.begin(), tiles.end(), [k](const Model::BoardTile& tl) { return tl.kind == k; });
    auto over = [&](D2D1_POINT_2F pt) { return Inside(Inset(bar, -8, -8), pt) || (picking && Inside(Inset(PickBox(W, bar, missing), -6, -16), pt)); };
    bool overBar = over(in.mouse) || over(in.pressPos); // presses there are the bar's and the list's: the tiles under them never take them
    int remove = -1;
    for (int i = 0; i < (int)tiles.size(); ++i) {
        const auto& tl = tiles[i];
        D2D1_RECT_F r = CellRect(tl.x, tl.y, tl.w, tl.h);
        bool held = grab == i;
        float lift = ui.Smooth(Ui::Id("bd.lift", tl.kind), held ? 1.0f : 0, 16);
        c.Fill(r, 20, t.card.Alpha(held ? 0.45f : 0.9f));
        c.Stroke(r, 20, t.accent.Alpha(0.35f + 0.5f * lift), 1.5f);
        float k = std::clamp((std::min)(Width(r), Height(r)) / 150, 0.6f, 1.6f);
        D2D1_POINT_2F mid = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        c.Icon(Model::BoardGlyph(tl.kind), {mid.x, mid.y - 12 * k}, 28 * k, t.accentHi);
        c.Text(Model::BoardName(tl.kind), Rect(r.left, mid.y + 8 * k, Width(r), 20 * k), Weight::Medium, 13 * k, t.text, Align::Center);
        c.Text(std::to_wstring(tl.w) + L" × " + std::to_wstring(tl.h), Rect(r.left, mid.y + 28 * k, Width(r), 16 * k), Weight::Light, 10.5f * k, t.textLow,
               Align::Center);
        if (w::IconBtn(ui, t, Ui::Id("bd.x", tl.kind), {r.right - 18, r.top + 18}, 28, glyph::Close, 15, !overBar) && grab < 0) remove = i;
        D2D1_RECT_F grip = {r.right - kGrip, r.bottom - kGrip, r.right, r.bottom};
        for (int s = 0; s < 3; ++s) { // three short diagonals in the corner
            float o = 6 + s * 4.5f;
            c.Line({r.right - o, r.bottom - 5}, {r.right - 5, r.bottom - o}, t.textDim, 1.5f);
        }
        if (!overBar) ui.Button(Ui::Id("bd.body", tl.kind), r); // the hand over a tile; under the bar or the list, theirs
        bool onX = Inside({r.right - 32, r.top + 4, r.right - 4, r.top + 32}, in.mouse);
        if (in.pressed && grab < 0 && !overBar && Inside(r, in.mouse) && !onX) {
            grab = i;
            sizing = Inside(grip, in.mouse);
            D2D1_POINT_2F at = CellAt(in.mouse);
            grabAt = {at.x - tl.x, at.y - tl.y};
            toX = tl.x, toY = tl.y, toW = tl.w, toH = tl.h;
        }
    }
    if (grab >= 0 && grab < (int)tiles.size()) { // where it would land, lit if it fits
        const auto& tl = tiles[grab];
        D2D1_POINT_2F at = CellAt(in.mouse);
        if (sizing) {
            toX = tl.x, toY = tl.y;
            toW = (std::clamp)((int)std::ceil(at.x - tl.x - 0.2f), 1, Model::kBoardCols - tl.x);
            toH = (std::clamp)((int)std::ceil(at.y - tl.y - 0.2f), 1, rows - tl.y);
        } else {
            toW = tl.w, toH = tl.h;
            toX = (std::clamp)((int)std::lround(at.x - grabAt.x), 0, Model::kBoardCols - tl.w);
            toY = (std::clamp)((int)std::lround(at.y - grabAt.y), 0, rows - tl.h);
        }
        bool ok = Free(toX, toY, toW, toH, grab);
        D2D1_RECT_F ghost = CellRect(toX, toY, toW, toH);
        Color col = ok ? t.accent : Color{0.95f, 0.35f, 0.35f, 1};
        c.Fill(ghost, 20, col.Alpha(0.16f));
        c.Stroke(ghost, 20, col.Alpha(0.9f), 2);
        if (!in.down) { // let go: it moves there if it fits, else back where it was
            if (ok && (toX != tl.x || toY != tl.y || toW != tl.w || toH != tl.h)) {
                tiles[grab] = {tl.kind, toX, toY, toW, toH};
                Save();
            }
            grab = -1;
        }
        ui.Wake();
    }
    if (remove >= 0) {
        tiles.erase(tiles.begin() + remove);
        Save();
    }

    c.Glow({W / 2, bar.bottom}, kBarW * 0.8f, kBarH * 1.6f, Color{0, 0, 0, 0.4f});
    c.Fill(bar, kBarH / 2, t.cardHi.Alpha(1));
    c.StrokeV(bar, kBarH / 2, t.hairTop, t.hairBot, 1);
    float bx = bar.left + 8, by = bar.top + (kBarH - kBtnH) / 2;
    if (Btn(ui, t, Ui::Id("bd.add"), Rect(bx, by, 128, kBtnH), glyph::Add, L"Add a tile", false)) picking = !picking;
    if (Btn(ui, t, Ui::Id("bd.reset"), Rect(bx + 136, by, 128, kBtnH), glyph::Restart, L"Start over", false)) {
        tiles = model.BoardPreset(rows);
        Save();
        picking = false;
    }
    if (Btn(ui, t, Ui::Id("bd.done"), Rect(bar.right - 8 - 128, by, 128, kBtnH), glyph::Check, L"Done", true) || in.rclick) {
        editing = picking = false;
        grab = -1;
    }
    if (picking) Picker(bar);
    else c.Text(L"Drag a tile to move it, its corner to resize it, right click to finish", Rect(0, bar.top - 26, W, 18), Weight::Light, 11.5f, t.textDim, Align::Center);
}
