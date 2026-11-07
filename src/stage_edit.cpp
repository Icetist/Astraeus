// The fullscreen player's editor: the layout itself, dragged and resized where it is, with a panel of options for whichever part is picked.
// One version for songs without lyrics and one for songs with them.
#include <algorithm>
#include <cmath>

#include "anim.h"
#include "page_sheet.h"
#include "stage.h"

namespace {

enum Page { kPageCover, kPageSong, kPageLyrics, kPageClock, kPageScene, kPageKeys, kPages };
constexpr float kOutset = 8, kPartRad = 18; // a part's outline sits a little clear of it
constexpr float kGripInset = 9, kGripLen = 11, kGripHit = 28, kGripThick = 2.5f; // the corner that resizes it
constexpr float kSnap = 8, kSnapOut = 14; // how near a screen's middle catches it, and how far it takes to pull away
constexpr float kTagH = 22, kTagPad = 10, kTagGap = 6; // its name, while the mouse is on it
constexpr float kPanelW = 336, kEdge = 24, kPad = 16, kHeadH = 32, kGap = 10, kTabsH = 30, kChipH = 30, kChipGap = 8, kKeyW = 132, kKeyH = 34, kNoteH = 18, kFaded = 0.25f, kRise = 18;
constexpr float kSlideRow = sheet::kSlideH + kGap, kChipRow = kChipH + kChipGap;
constexpr float kPanelChrome = 2 * kPad + kHeadH + kGap + kTabsH + kGap + kTabsH + kGap;
constexpr float kNudgeSmall = 1, kNudgeBig = 10; // dips an arrow moves a part, and with shift
constexpr int kLyricStep = 5;

constexpr float Clamped(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float Overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    float w = (std::min)(a.right, b.right) - (std::max)(a.left, b.left), h = (std::min)(a.bottom, b.bottom) - (std::max)(a.top, b.top);
    return w > 0 && h > 0 ? w * h : 0;
}

float PageH(int page, int tab) {
    switch (page) {
    case kPageCover: return 3 * kSlideRow;
    case kPageSong: return kSlideRow + (tab ? 1 : 2) * kChipRow;
    case kPageLyrics: return kChipRow + 4 * kSlideRow;
    case kPageClock: return kChipRow + 2 * kSlideRow;
    case kPageScene: return 3 * kChipRow + 3 * kSlideRow;
    default: return 2 * (kKeyH + kChipGap) + kNoteH;
    }
}

}

namespace {
constexpr size_t kShapeFloats = sizeof(StagePrefs::Shape) / sizeof(float);
static_assert(sizeof(StagePrefs::Shape) == kShapeFloats * sizeof(float), "a layout is floats only: it goes into a snapshot as they are");
}

History::Snap Stage::EditSnap() const { // both versions, then the scene's own switches and numbers
    const StagePrefs& sp = StagePrefs::Get();
    History::Snap s;
    s.reserve(2 * kShapeFloats + 8);
    for (const StagePrefs::Shape& sh : sp.shape) {
        const float* f = reinterpret_cast<const float*>(&sh);
        s.insert(s.end(), f, f + kShapeFloats);
    }
    for (int v : {model.stageArt, model.stageThump, model.stageHide, model.vizOn, model.vizWave, model.vizFull, model.vizGain, model.vizAlpha}) s.push_back((float)v);
    return s;
}

void Stage::EditLoad(const History::Snap& s) {
    StagePrefs& sp = StagePrefs::Get();
    if (s.size() != 2 * kShapeFloats + 8) return;
    for (int i = 0; i < 2; ++i) std::copy(s.begin() + i * kShapeFloats, s.begin() + (i + 1) * kShapeFloats, reinterpret_cast<float*>(&sp.shape[i]));
    sp.SaveShape();
    struct Pref {
        int* field;
        const wchar_t* key;
    } prefs[] = {{&model.stageArt, L"StageArtBg"}, {&model.stageThump, L"StageThump"}, {&model.stageHide, L"StageHide"}, {&model.vizOn, L"Visualizer"},
                 {&model.vizWave, L"VizWave"}, {&model.vizFull, L"VizFull"}, {&model.vizGain, L"VizGain"}, {&model.vizAlpha, L"VizAlpha"}};
    size_t at = 2 * kShapeFloats;
    for (const Pref& p : prefs) {
        int v = (int)s[at++];
        if (*p.field != v) model.SetPref(*p.field, p.key, v); // only what changed is written
    }
}

void Stage::StartEdit() {
    ++editRun;
    editing = barAuto = true;
    lyrMenu = false;
    editDrag = -1;
    editTab = lyrMix > 0.5f ? 1 : 0; // the one it's showing
    editPart = kPartCover;
    editPage = kPageCover;
    ++editPageNth;
}

void Stage::EndEdit() {
    editing = false;
    editDrag = -1;
    StagePrefs& sp = StagePrefs::Get();
    sp.capture = 0;
    sp.SaveShape();
}

void Stage::EditNudge(int dx, int dy, bool big) {
    float step = big ? kNudgeBig : kNudgeSmall, ox = dx * step / W, oy = dy * step / H;
    StagePrefs::Shape& s = StagePrefs::Get().shape[editTab];
    if (editPart == kPartCover) s.coverX += ox, s.coverY += oy;
    else if (editPart == kPartText) s.textX += ox, s.textY += oy;
    else if (editPart == kPartLyrics) s.lyrX += ox, s.lyrY += oy;
    else s.clockX += ox, s.clockY += oy;
    StagePrefs::Get().SaveShape();
}

D2D1_RECT_F Stage::BarRect(float show) {
    float want = (std::min)(kPanelChrome + PageH(editPage, editTab), H - 2 * kEdge);
    float h = ui.Smooth(Ui::Id("st.ed.h"), want, 16), w = (std::min)(kPanelW, W - 2 * kEdge);
    if (barAuto) { // it keeps out of the way of what's picked, on its own, until it's been put somewhere by hand
        D2D1_RECT_F parts[kEditParts] = {partClock, partLyrics, partText, partCover};
        const D2D1_RECT_F& part = parts[editPart];
        D2D1_RECT_F right = Rect(W - w - kEdge, (H - h) / 2, w, h), left = Rect(kEdge, (H - h) / 2, w, h);
        float onRight = Overlap(right, part), onLeft = Overlap(left, part);
        if (editDrag < 0 && (barLeft ? onLeft > onRight : onRight > onLeft)) barLeft = !barLeft;
        barAt = {ui.Smooth(Ui::Id("st.ed.x"), barLeft ? left.left : right.left, 12), (H - h) / 2};
    }
    barAt = {Clamped(barAt.x, 0, W - w), Clamped(barAt.y, 0, H - h)};
    return Rect(barAt.x + (1 - show) * (barLeft ? -kRise : kRise), barAt.y, w, h); // it slides in from its side
}

void Stage::EditParts(const D2D1_RECT_F& bar, float show) {
    Canvas& c = *canvas;
    const Input& in = ui.In();
    StagePrefs& sp = StagePrefs::Get();
    StagePrefs::Shape& s = sp.shape[editTab];
    const D2D1_RECT_F parts[kEditParts] = {partClock, partLyrics, partText, partCover};
    static const wchar_t* const kNames[kEditParts] = {L"Clock", L"Lyrics", L"Song", L"Cover"};
    static const int kPageOf[kEditParts] = {kPageClock, kPageLyrics, kPageSong, kPageCover};
    bool has[kEditParts] = {s.clockOn > 0.5f, editTab == 1, true, true}; // what's in this version
    if (!has[editPart]) editPart = kPartCover;
    D2D1_RECT_F boxes[kEditParts];
    for (int i = 0; i < kEditParts; ++i) boxes[i] = Inset(parts[i], -kOutset, -kOutset);
    float rads[kEditParts] = {kPartRad, kPartRad, kPartRad, Width(partCover) * shapeNow[editTab].coverRound + kOutset};
    bool free = editDrag >= 0 || (!barHeld && !Inside(bar, in.down ? in.pressPos : in.mouse));
    auto grip = [&](int i) { return D2D1_POINT_2F{boxes[i].right - kGripInset, boxes[i].bottom - kGripInset}; };
    Ui::Hit sizes[kEditParts] = {}, moves[kEditParts] = {};
    for (int i = 0; i < kEditParts && free; ++i) { // the corners before the bodies: the first button under a press takes it
        if (!has[i]) continue;
        D2D1_POINT_2F o = grip(i);
        sizes[i] = ui.Button(Ui::Id("st.ed.grip", i), Rect(o.x - kGripHit / 2, o.y - kGripHit / 2, kGripHit, kGripHit));
        ui.Name(std::wstring(L"Resize ") + kNames[i]);
    }
    for (int i = 0; i < kEditParts && free; ++i) {
        if (!has[i]) continue;
        moves[i] = ui.Button(Ui::Id("st.ed.body", i), boxes[i]);
        ui.Name(std::wstring(L"Move ") + kNames[i]);
    }
    if (editDrag < 0) {
        editCursor = nullptr; // the top one under the mouse says how it moves
        for (int i = 0; i < kEditParts && !editCursor; ++i)
            if (sizes[i].hover) editCursor = IDC_SIZENWSE;
        for (int i = 0; i < kEditParts && !editCursor; ++i)
            if (moves[i].hover) editCursor = IDC_SIZEALL;
    }
    int top = -1; // only the top one lights: parts overlap, and a stack of highlights reads as a mess
    for (int i = 0; i < kEditParts && top < 0 && editDrag < 0; ++i)
        if (sizes[i].hover) top = i;
    for (int i = 0; i < kEditParts && top < 0 && editDrag < 0; ++i)
        if (moves[i].hover) top = i;
    for (int i = kEditParts - 1; i >= 0; --i) { // the lowest first, so the top one's marks lie over it
        float there = ui.Smooth(Ui::Id("st.ed.has", i), has[i] ? 1.0f : 0, 16) * show;
        if (there < 0.01f) continue;
        bool held = editDrag >= 0 && editDrag / 2 == i, picked = i == editPart;
        float lit = ui.Smooth(Ui::Id("st.ed.lit", i), held || i == top ? 1.0f : 0, 16), sel = ui.Smooth(Ui::Id("st.ed.sel", i), picked ? 1.0f : 0, 14);
        if (lit > 0.01f) c.Fill(boxes[i], rads[i], look.text.Alpha(0.04f * lit * there));
        float mark = (std::max)(lit, 0.6f * sel);
        c.Stroke(boxes[i], rads[i], look.text.Alpha(0.14f * there).Mix(look.accentHi.Alpha(0.85f * there), mark), 1); // a hairline at rest: the song stays the thing to look at
        D2D1_POINT_2F o = grip(i);
        Color ink = look.text.Alpha(0.3f * there).Mix(look.accentHi.Alpha(there), mark);
        c.Line({o.x, o.y - kGripLen}, o, ink, kGripThick);
        c.Line({o.x - kGripLen, o.y}, o, ink, kGripThick);
        if (lit > 0.01f) { // its name, just over it, or inside when there's no room above
            float tw = c.Measure(kNames[i], Weight::Medium, 11.5f) + 2 * kTagPad, ty = boxes[i].top - kTagH - kTagGap;
            D2D1_RECT_F tag = Rect(boxes[i].left, ty < 0 ? boxes[i].top + kTagGap : ty, tw, kTagH);
            Color back = look.bg;
            back.a = 0.92f;
            c.Fill(tag, kTagH / 2, back.Alpha(lit * there));
            c.Text(kNames[i], tag, Weight::Medium, 11.5f, look.text.Alpha(lit * there), Align::Center);
        }
    }
    for (int i = 0; i < kEditParts && editDrag < 0 && in.down; ++i) { // picked up, and its options come to the panel
        if (!has[i]) continue;
        bool bySize = ui.IsActive(Ui::Id("st.ed.grip", i));
        if (!bySize && !ui.IsActive(Ui::Id("st.ed.body", i))) continue;
        editDrag = i * 2 + (bySize ? 1 : 0);
        editPart = i;
        if (editPage != kPageOf[i]) editPage = kPageOf[i], ++editPageNth;
        editFrom = s;
        editMid = {(parts[i].left + parts[i].right) / 2, (parts[i].top + parts[i].bottom) / 2};
        editSize = {Width(parts[i]), Height(parts[i])};
        editCursor = bySize ? IDC_SIZENWSE : IDC_SIZEALL;
        editSnapX = editSnapY = false;
    }
    bool moving = editDrag >= 0 && !(editDrag & 1);
    float lineX = ui.Smooth(Ui::Id("st.ed.lineX"), moving && editSnapX ? 1.0f : 0, 18), lineY = ui.Smooth(Ui::Id("st.ed.lineY"), moving && editSnapY ? 1.0f : 0, 18); // the guides fade, nothing pops
    if (lineX > 0.01f) c.Line({W / 2, 0}, {W / 2, H}, look.accentHi.Alpha(0.45f * lineX * show), 1);
    if (lineY > 0.01f) c.Line({0, H / 2}, {W, H / 2}, look.accentHi.Alpha(0.45f * lineY * show), 1);
    if (editDrag < 0) return;
    ui.Wake();
    if (!in.down) { // let go: it's kept
        editDrag = -1;
        sp.SaveShape();
        return;
    }
    int part = editDrag / 2;
    float dx = in.mouse.x - in.pressPos.x, dy = in.mouse.y - in.pressPos.y;
    if (editDrag & 1) { // the corner: a part grows with the mouse's reach; the lyrics' column takes each way apart, and its type stays as it is
        float from = std::hypot(in.pressPos.x - editMid.x, in.pressPos.y - editMid.y), to = std::hypot(in.mouse.x - editMid.x, in.mouse.y - editMid.y), k = to / (std::max)(from, 1.0f);
        if (part == kPartCover) s.coverS = Clamped(editFrom.coverS * k, StagePrefs::kCoverLo, StagePrefs::kCoverHi);
        else if (part == kPartText) s.textS = Clamped(editFrom.textS * k, StagePrefs::kTextLo, StagePrefs::kTextHi);
        else if (part == kPartClock) s.clockS = Clamped(editFrom.clockS * k, StagePrefs::kClockLo, StagePrefs::kClockHi);
        else {
            s.lyrW = Clamped(editFrom.lyrW * (1 + 2 * dx / editSize.width), StagePrefs::kLyrWLo, StagePrefs::kLyrWHi);
            s.lyrH = Clamped(editFrom.lyrH * (1 + 2 * dy / editSize.height), StagePrefs::kLyrHLo, StagePrefs::kLyrHHi);
        }
        return;
    }
    float x = Clamped(editMid.x + dx, editSize.width / 2, W - editSize.width / 2), y = Clamped(editMid.y + dy, editSize.height / 2, H - editSize.height / 2);
    editSnapX = std::fabs(x - W / 2) < (editSnapX ? kSnapOut : kSnap); // a magnet: once caught it takes a little more to leave, and the glide makes the catch soft
    editSnapY = std::fabs(y - H / 2) < (editSnapY ? kSnapOut : kSnap);
    if (editSnapX) x = W / 2;
    if (editSnapY) y = H / 2;
    float ox = (x - editMid.x) / W, oy = (y - editMid.y) / H;
    if (part == kPartCover) s.coverX = editFrom.coverX + ox, s.coverY = editFrom.coverY + oy;
    else if (part == kPartText) s.textX = editFrom.textX + ox, s.textY = editFrom.textY + oy;
    else if (part == kPartClock) s.clockX = editFrom.clockX + ox, s.clockY = editFrom.clockY + oy;
    else s.lyrX = editFrom.lyrX + ox, s.lyrY = editFrom.lyrY + oy;
}

void Stage::EditPage(const D2D1_RECT_F& body, bool live) { // the picked page's options
    Canvas& c = *canvas;
    const Input& in = ui.In();
    StagePrefs& sp = StagePrefs::Get();
    StagePrefs::Shape& s = sp.shape[editTab];
    float x = body.left, wide = Width(body), y = body.top, cw = (wide - kChipGap) / 2;
    int col = 0;
    auto percent = [&](const char* id, const wchar_t* label, float& f, float lo, float hi, float showAs, bool on = true) { // a number kept with this version of the layout
        float v = (f - lo) / (hi - lo);
        bool done = false;
        if (sheet::Knob(ui, look, Ui::Id(id), Rect(x, y, wide, sheet::kSlideH), label, std::to_wstring(std::lround(f * showAs)) + L"%", v, live && on, done, 0.01f)) f = lo + v * (hi - lo);
        if (done) sp.SaveShape();
        y += kSlideRow;
    };
    auto setting = [&](const char* id, const wchar_t* label, int& f, const wchar_t* key, int lo, int hi, int step, bool on = true) { // a number kept for the whole app
        float v = (f - lo) / (float)(hi - lo);
        bool done = false;
        if (sheet::Knob(ui, look, Ui::Id(id), Rect(x, y, wide, sheet::kSlideH), label, std::to_wstring(f) + L"%", v, live && on, done, (float)step / (hi - lo))) f = lo + (int)std::lround(v * (hi - lo) / step) * step;
        if (done) model.SetPref(f, key, f);
        y += kSlideRow;
    };
    auto chip = [&](const char* id, const wchar_t* label, bool on) { // two to a row
        bool hit = w::Chip(ui, look, Ui::Id(id), Rect(x + col * (cw + kChipGap), y, cw, kChipH), label, on) && live;
        if (col) y += kChipRow;
        col = 1 - col;
        return hit;
    };
    auto flip = [&](float& f) {
        f = f > 0.5f ? 0.0f : 1.0f;
        sp.SaveShape();
    };
    auto pref = [&](int& f, const wchar_t* key) { model.SetPref(f, key, f ? 0 : 1); };
    switch (editPage) {
    case kPageCover:
        percent("st.ed.cover.size", L"Size", s.coverS, StagePrefs::kCoverLo, StagePrefs::kCoverHi, 100);
        percent("st.ed.cover.round", L"Corners", s.coverRound, 0, StagePrefs::kRoundHi, 200); // 100%: a circle
        percent("st.ed.cover.glow", L"Glow", s.coverGlow, 0, StagePrefs::kGlowHi, 100);
        break;
    case kPageSong:
        percent("st.ed.song.size", L"Size", s.textS, StagePrefs::kTextLo, StagePrefs::kTextHi, 100);
        if (chip("st.ed.song.extras", L"Shuffle and repeat", s.extrasOn > 0.5f)) flip(s.extrasOn);
        if (chip("st.ed.song.times", L"Times", s.timesOn > 0.5f)) flip(s.timesOn);
        if (editTab == 0) { // the layout with lyrics has no room for these anyway
            if (chip("st.ed.song.app", L"App name", s.appOn > 0.5f)) flip(s.appOn);
            if (chip("st.ed.song.album", L"Album name", s.albumOn > 0.5f)) flip(s.albumOn);
        }
        break;
    case kPageLyrics: {
        if (w::Chip(ui, look, Ui::Id("st.ed.lyr.on"), Rect(x, y, cw, kChipH), L"Lyrics", model.fullLyrics != 0) && live) pref(model.fullLyrics, L"FullLyrics");
        static const wchar_t* const kAlign[3] = {L"Left", L"Center", L"Right"};
        int was = (int)std::lround(s.lyrAlign), picked = sheet::Segmented(ui, look, "st.ed.lyr.align", Rect(x + cw + kChipGap, y, cw, kChipH), kAlign, 3, was, live);
        if (picked >= 0) s.lyrAlign = (float)picked, sp.SaveShape();
        y += kChipRow;
        setting("st.ed.lyr.type", L"Type size", model.lyricScale, L"LyricScale", Model::kLyricMin, Model::kLyricMax, kLyricStep, model.fullLyrics != 0);
        percent("st.ed.lyr.line", L"Sung line height", s.lyrAnchor, StagePrefs::kAnchorLo, StagePrefs::kAnchorHi, 100);
        percent("st.ed.lyr.dim", L"Other lines", s.lyrDim, StagePrefs::kLyrDimLo, StagePrefs::kLyrDimHi, 100);
        percent("st.ed.lyr.blur", L"Blur", s.lyrHaze, 0, StagePrefs::kHazeHi, 100);
        break;
    }
    case kPageClock:
        if (chip("st.ed.clock.on", L"Show the clock", s.clockOn > 0.5f)) flip(s.clockOn);
        y += kChipRow, col = 0;
        percent("st.ed.clock.size", L"Size", s.clockS, StagePrefs::kClockLo, StagePrefs::kClockHi, 100, s.clockOn > 0.5f);
        percent("st.ed.clock.alpha", L"Opacity", s.clockA, StagePrefs::kClockALo, 1, 100, s.clockOn > 0.5f);
        break;
    case kPageScene:
        if (chip("st.ed.sc.art", L"Album art", model.stageArt != 0)) pref(model.stageArt, L"StageArtBg");
        if (chip("st.ed.sc.thump", L"Cover thump", model.stageThump != 0)) pref(model.stageThump, L"StageThump");
        if (chip("st.ed.sc.hide", L"Hide at rest", model.stageHide != 0)) pref(model.stageHide, L"StageHide");
        if (chip("st.ed.sc.viz", L"Visualizer", model.vizOn != 0)) pref(model.vizOn, L"Visualizer");
        if (chip("st.ed.sc.wave", L"Waveform", model.vizWave != 0)) pref(model.vizWave, L"VizWave");
        if (chip("st.ed.sc.full", L"Any volume", model.vizFull != 0)) pref(model.vizFull, L"VizFull");
        setting("st.ed.sc.gain", L"Visualizer strength", model.vizGain, L"VizGain", Model::kVizGainMin, Model::kVizGainMax, Model::kSlideStep);
        setting("st.ed.sc.alpha", L"Visualizer opacity", model.vizAlpha, L"VizAlpha", Model::kVizAlphaMin, 100, Model::kSlideStep);
        percent("st.ed.sc.dim", L"Darken the background", s.dim, 0, StagePrefs::kDimHi, 100);
        break;
    default: {
        static const wchar_t* const kLabels[2] = {L"Two screens", L"Swap screens"};
        UINT keys[2] = {sp.keySplit, sp.keySwap};
        for (int i = 0; i < 2; ++i) { // the same two as in settings: click it, then press the new one
            bool waiting = sp.capture == i + 1;
            c.Text(kLabels[i], Rect(x, y, wide - kKeyW - 8, kKeyH), Weight::Regular, 12, look.text);
            if (w::Chip(ui, look, Ui::Id("st.ed.key", i), Rect(x + wide - kKeyW, y + 2, kKeyW, kKeyH - 4), waiting ? std::wstring(L"Press a key") : StagePrefs::Name(keys[i]), waiting) && live) sp.capture = waiting ? 0 : i + 1;
            y += kKeyH + kChipGap;
        }
        if (sp.capture && in.pressed && !Inside({x + wide - kKeyW, body.top, x + wide, y}, in.mouse)) sp.capture = 0; // clicked away: the key stays as it was
        c.Text(sp.capture ? L"Esc keeps the key it had" : L"With a second screen", Rect(x, y, wide, kNoteH), Weight::Light, 11, look.textDim);
        break;
    }
    }
}

void Stage::EditPanel(const D2D1_RECT_F& bar, bool canWith, bool covered) {
    Canvas& c = *canvas;
    const Input& in = ui.In();
    StagePrefs& sp = StagePrefs::Get();
    bool live = editDrag < 0;
    float a = ui.Smooth(Ui::Id("st.ed.bar"), covered ? kFaded : 1.0f, 14); // only when what's being moved goes under it
    bool layer = a < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(Inset(bar, -6, -6), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    Color fill = look.bg;
    fill.a = 0.96f;
    sheet::Surface(c, look, bar, fill);
    D2D1_RECT_F inner = Inset(bar, kPad, kPad);
    float y = inner.top, btnW = 60, histX = inner.right - 2 * btnW - 12 - history::kW;
    editHist.Sync(editRun, EditSnap(), live && !in.down); // a drag or a slider counts once it's let go
    c.Text(L"Edit fullscreen", Rect(inner.left + 2, y, histX - inner.left - 6, kHeadH), Weight::Semi, 13, look.text);
    if (sheet::Act(ui, look, Ui::Id("st.ed.done"), Rect(inner.right - btnW, y, btnW, kHeadH), L"Done", true, false, live)) EndEdit();
    if (sheet::Act(ui, look, Ui::Id("st.ed.reset"), Rect(inner.right - 2 * btnW - 6, y, btnW, kHeadH), L"Reset", false, false, live)) {
        sp.shape[editTab] = StagePrefs::Shape{}; // this version's layout; undo brings it back
        sp.SaveShape();
    }
    int go = history::Buttons(ui, look, "st.ed.hist", histX, y + kHeadH / 2, editHist, live);
    if (go < 0) EditLoad(editHist.Undo());
    else if (go > 0) EditLoad(editHist.Redo());
    y += kHeadH + kGap;
    if (canWith) { // both versions while a song has its words to show; one without has only the one
        static const wchar_t* const kTabs[2] = {L"Without lyrics", L"With lyrics"};
        int picked = sheet::Segmented(ui, look, "st.ed.tab", Rect(inner.left, y, Width(inner), kTabsH), kTabs, 2, editTab, live);
        if (picked >= 0) editTab = picked;
    } else {
        c.Text(L"Without lyrics", Rect(inner.left + 2, y, Width(inner), kTabsH), Weight::Medium, 12, look.textDim);
    }
    y += kTabsH + kGap;
    if (editTab == 0 && editPage == kPageLyrics) editPage = kPageCover, ++editPageNth;
    int ids[kPages], n = 0;
    const wchar_t* names[kPages];
    static const wchar_t* const kNames[kPages] = {L"Cover", L"Song", L"Lyrics", L"Clock", L"Scene", L"Keys"};
    int cur = 0;
    for (int i = 0; i < kPages; ++i) {
        if (i == kPageLyrics && editTab == 0) continue;
        if (i == editPage) cur = n;
        ids[n] = i, names[n++] = kNames[i];
    }
    int picked = sheet::Segmented(ui, look, "st.ed.page", Rect(inner.left, y, Width(inner), kTabsH), names, n, cur, live);
    if (picked >= 0) {
        editPage = ids[picked];
        ++editPageNth;
        sp.capture = 0;
        static const int kPartOf[kPages] = {kPartCover, kPartText, kPartLyrics, kPartClock, -1, -1};
        if (kPartOf[editPage] >= 0) editPart = kPartOf[editPage]; // the page's part is picked with it
    }
    y += kTabsH + kGap;
    D2D1_RECT_F body = {inner.left, y, inner.right, bar.bottom - 6};
    float pageIn = ui.SmoothFrom(Ui::Id("st.ed.page.in", editPageNth), 1, 0, 16); // each page drifts up into place
    c.PushClip(body);
    bool fading = pageIn < 0.999f;
    if (fading) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pageIn), nullptr);
    EditPage(Rect(body.left, body.top + (1 - pageIn) * 8, Width(body), Height(body)), live);
    if (fading) ctx->PopLayer();
    c.PopClip();
    uint32_t back = Ui::Id("st.ed.barbody"); // last: its controls take their presses first. the rest of it carries the panel off whatever's under it
    ui.Button(back, bar);
    ui.NoStop();
    if (!barHeld && ui.IsActive(back) && in.down) barHeld = true, barAuto = false, barGrab = {in.pressPos.x - bar.left, in.pressPos.y - bar.top};
    if (barHeld) {
        ui.Wake();
        barAt = {in.mouse.x - barGrab.x, in.mouse.y - barGrab.y};
        if (!in.down) barHeld = false;
    }
    if (layer) ctx->PopLayer();
}

void Stage::EditLayer(bool canWith, float show) {
    D2D1_RECT_F bar = BarRect(show);
    EditParts(bar, show);
    const D2D1_RECT_F parts[kEditParts] = {partClock, partLyrics, partText, partCover};
    bool covered = editDrag >= 0 && Overlap(bar, parts[editDrag / 2]) > 0;
    bool layer = show < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), show), nullptr);
    EditPanel(bar, canWith, covered);
    if (layer) ctx->PopLayer();
}
