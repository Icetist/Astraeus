#include "history.h"

#include <cctype>
#include <cstdlib>

#include "gfx.h"
#include "ui.h"

namespace history {
namespace {

// material symbols' undo and redo, outlined, weight 400 (apache 2.0, as the bundled icon fonts): the subset font never held them
constexpr const char* kUndoPath = "M280-200v-80h284q63 0 109.5-40T720-420q0-60-46.5-100T564-560H312l104 104-56 56-200-200 200-200 56 56-104 104h252q97 0 166.5 63T800-420q0 94-69.5 157T564-200H280Z";
constexpr const char* kRedoPath = "M396-200q-97 0-166.5-63T160-420q0-94 69.5-157T396-640h252L544-744l56-56 200 200-200 200-56-56 104-104H396q-63 0-109.5 40T240-420q0 60 46.5 100T396-280h284v80H396Z";
constexpr float kGrid = 960, kIconPx = 20; // the symbols' own grid: 0..960 across, -960..0 down

ComPtr<ID2D1PathGeometry> Shape(const char* d, D2D1_POINT_2F o, float px) { // an svg path of moves, lines and quadratics, centred on o
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    float k = px / kGrid;
    auto at = [&](float x, float y) { return D2D1_POINT_2F{o.x + (x - kGrid / 2) * k, o.y + (y + kGrid / 2) * k}; };
    float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0; // the pen, the figure's start, the last control point
    bool open = false;
    char cmd = 0, prev = 0;
    const char* s = d;
    auto num = [&]() {
        while (*s == ' ' || *s == ',') ++s;
        char* end = nullptr;
        float v = std::strtof(s, &end);
        s = end;
        return v;
    };
    while (*s) {
        while (*s == ' ' || *s == ',') ++s;
        if (!*s) break;
        if (std::isalpha((unsigned char)*s)) cmd = *s++;
        bool rel = std::islower((unsigned char)cmd) != 0;
        float bx = rel ? cx : 0, by = rel ? cy : 0;
        switch (std::toupper((unsigned char)cmd)) {
        case 'M': {
            float x = bx + num(), y = by + num();
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(at(x, y), D2D1_FIGURE_BEGIN_FILLED);
            open = true, cx = sx = x, cy = sy = y;
            cmd = rel ? 'l' : 'L'; // more pairs after a move are lines
            break;
        }
        case 'L': cx = bx + num(), cy = by + num(), sink->AddLine(at(cx, cy)); break;
        case 'H': cx = bx + num(), sink->AddLine(at(cx, cy)); break;
        case 'V': cy = by + num(), sink->AddLine(at(cx, cy)); break;
        case 'Q': {
            qx = bx + num(), qy = by + num();
            cx = bx + num(), cy = by + num();
            sink->AddQuadraticBezier({at(qx, qy), at(cx, cy)});
            break;
        }
        case 'T': { // the control mirrors the last one's
            bool chained = prev == 'Q' || prev == 'T';
            qx = chained ? 2 * cx - qx : cx, qy = chained ? 2 * cy - qy : cy;
            cx = bx + num(), cy = by + num();
            sink->AddQuadraticBezier({at(qx, qy), at(cx, cy)});
            break;
        }
        case 'Z':
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false, cx = sx, cy = sy;
            break;
        default: return nullptr; // nothing else is used
        }
        prev = (char)std::toupper((unsigned char)cmd);
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

bool One(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F o, bool back, bool enabled) { // a round chip as the quiet pills beside it, the arrow in the accent
    Canvas& c = ui.C();
    D2D1_RECT_F r = {o.x - kBtn / 2, o.y - kBtn / 2, o.x + kBtn / 2, o.y + kBtn / 2};
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    if (enabled) ui.Name(back ? L"Undo" : L"Redo"); // a greyed one registers nothing to name
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 16), press = ui.Smooth(id + 1, h.down ? 0.88f : 1.0f, 26);
    float on = ui.Smooth(id + 2, enabled ? 1.0f : 0, 14); // greys out and back softly
    Color base = t.track.Mix(t.track.Mix(t.text, 0.12f), 0.6f * hover);
    c.Dot(o, kBtn / 2, base.Alpha(0.45f + 0.55f * on));
    if (ComPtr<ID2D1PathGeometry> g = Shape(back ? kUndoPath : kRedoPath, o, kIconPx * press)) c.FillGeometry(g.Get(), t.textLow.Alpha(0.6f).Mix(t.accent, on));
    return h.clicked;
}

}

int Buttons(Ui& ui, const Theme& t, const char* id, float left, float midY, const History& h, bool live) {
    int go = 0;
    if (One(ui, t, Ui::Id(id, 1), {left + kBtn / 2, midY}, true, live && h.CanUndo())) go = -1;
    if (One(ui, t, Ui::Id(id, 2), {left + kBtn + kGap + kBtn / 2, midY}, false, live && h.CanRedo())) go = 1;
    return go;
}

}
