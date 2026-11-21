// The fullscreen player's editor: the layout itself, dragged and resized where it is, with a panel of options for whichever part is picked.
// One version for songs without lyrics and one for songs with them.
#include <algorithm>
#include <cmath>
#include <tuple>

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

constexpr float kFaceH = 58, kFaceRow = kFaceH + kChipGap; // a clock face's tile: the time in that face over its name
constexpr int kFaceCols = 3;
constexpr float kFaceTime = 22, kFaceName = 10.5f; // the tile's sample time and its name
constexpr float kGroupGap = 8; // a breath between the room's own options and the visualizer's: room, not a caption
const wchar_t* const kFaceNames[gfx::kClockFaces] = {L"Default", L"Thin", L"Bold", L"Rounded", L"Serif", L"Narrow"};

bool Worded(int tab) { return tab == 1 || tab == StagePrefs::kVinylLyr; } // a version with words: it has a lyrics page

int FacesThere() {
    int n = 0;
    for (int i = 0; i < gfx::kClockFaces; ++i) n += gfx::ClockFaceThere(i) ? 1 : 0;
    return n;
}

float PageH(int page, int tab, int songChips, const Model& m) {
    bool vinyl = m.stageVinyl != 0;
    switch (page) {
    case kPageCover: return (vinyl ? 2 : 1) * kChipRow + 3 * kSlideRow; // vinyl adds its color row, and trades the corners for its spin speed
    case kPageSong: return kSlideRow + (songChips + 1) / 2 * kChipRow; // two to a row
    case kPageLyrics:
        if (tab < StagePrefs::kVinyl) return kChipRow + 4 * kSlideRow;
        return m.stageVinylClassic ? 2 * kChipRow + 4 * kSlideRow : 2 * kChipRow + kSlideRow; // vinyl: its words' two switches, then the column's numbers or the line's size
    case kPageClock: return kChipRow + (FacesThere() + kFaceCols - 1) / kFaceCols * kFaceRow + 2 * kSlideRow;
    case kPageScene: { // the room's own, then the visualizer's: what hangs off the visualizer shows only while it's on
        float h = 2 * kChipRow + kSlideRow + kGroupGap + kChipRow;
        if (m.vizOn) h += 2 * kChipRow + 2 * kSlideRow + ((m.vizLooks & Model::kLookRim) || m.mediaWave == rim::kMediaRim ? kSlideRow : 0);
        return h;
    }
    default: return StagePrefs::kKeys * (kKeyH + kChipGap) + kNoteH;
    }
}

}

namespace {
constexpr size_t kShapeFloats = sizeof(StagePrefs::Shape) / sizeof(float);
constexpr size_t kScenePrefs = 19;
constexpr int kSpinStep = 5; // the spin speed slider's steps, in percent // the switches and numbers after the two layouts in a snapshot: EditSnap and EditLoad keep the same order
static_assert(sizeof(StagePrefs::Shape) == kShapeFloats * sizeof(float), "a layout is floats only: it goes into a snapshot as they are");
}

History::Snap Stage::EditSnap() const { // both versions, then the scene's own switches and numbers
    const StagePrefs& sp = StagePrefs::Get();
    History::Snap s;
    s.reserve(StagePrefs::kShapes * kShapeFloats + kScenePrefs);
    for (const StagePrefs::Shape& sh : sp.shape) {
        const float* f = reinterpret_cast<const float*>(&sh);
        s.insert(s.end(), f, f + kShapeFloats);
    }
    for (int v : {model.stageArt, model.stageThump, model.stageHide, model.vizOn, model.vizLooks, model.vizFull, model.vizGain, model.vizAlpha, model.stageVinyl, model.stageVinylCtl, model.stageVinylSpeed, model.stageVinylSound, model.stageVinylLabel, model.stageVinylGlow, model.mediaWave, model.rimGlow, model.lyricScale, model.stageVinylClassic, model.stageClockFace}) s.push_back((float)v);
    return s;
}

void Stage::EditLoad(const History::Snap& s) {
    StagePrefs& sp = StagePrefs::Get();
    if (s.size() != StagePrefs::kShapes * kShapeFloats + kScenePrefs) return;
    for (int i = 0; i < StagePrefs::kShapes; ++i) std::copy(s.begin() + i * kShapeFloats, s.begin() + (i + 1) * kShapeFloats, reinterpret_cast<float*>(&sp.shape[i]));
    sp.SaveShape();
    struct Pref {
        int* field;
        const wchar_t* key;
    } prefs[] = {{&model.stageArt, L"StageArtBg"}, {&model.stageThump, L"StageThump"}, {&model.stageHide, L"StageHide"}, {&model.vizOn, L"Visualizer"},
                 {&model.vizLooks, L"VizLooks"}, {&model.vizFull, L"VizFull"}, {&model.vizGain, L"VizGain"}, {&model.vizAlpha, L"VizAlpha"}, {&model.stageVinyl, L"StageVinyl"}, {&model.stageVinylCtl, L"StageVinylCtl"}, {&model.stageVinylSpeed, L"StageVinylSpeed"}, {&model.stageVinylSound, L"StageVinylSound"}, {&model.stageVinylLabel, L"StageVinylLabel"}, {&model.stageVinylGlow, L"StageVinylGlow"}, {&model.mediaWave, L"MediaWave"}, {&model.rimGlow, L"RimGlow"}, {&model.lyricScale, L"LyricScale"}, {&model.stageVinylClassic, L"StageVinylClassic"}, {&model.stageClockFace, L"StageClockFace"}};
    size_t at = StagePrefs::kShapes * kShapeFloats;
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
    editTab = model.stageVinyl ? (vinylLyr > 0.5f ? StagePrefs::kVinylLyr : StagePrefs::kVinyl) : lyrMix > 0.5f ? 1 : 0; // the one it's showing, with words if they're up
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
    else if (editPart == kPartCtl) s.ctlX += ox, s.ctlY += oy;
    else if (editPart == kPartWave) s.waveX += ox, s.waveY += oy;
    else s.clockX += ox, s.clockY += oy;
    StagePrefs::Get().SaveShape();
}

D2D1_RECT_F Stage::BarRect(float show) {
    bool extras = !model.stageVinyl || model.stageVinylCtl; // as the song page lays its switches out
    int songChips = (model.stageVinyl ? 1 : 0) + (extras ? 2 : 0) + (editTab != 1 ? 2 : 0);
    float want = (std::min)(kPanelChrome + PageH(editPage, editTab, songChips, model), H - 2 * kEdge);
    float h = ui.Smooth(Ui::Id("st.ed.h"), want, 16), w = (std::min)(kPanelW, W - 2 * kEdge);
    if (barAuto) { // it keeps out of the way of what's picked, on its own, until it's been put somewhere by hand
        D2D1_RECT_F parts[kEditParts] = {partClock, partCtl, partLyrics, partText, partCover, partWave};
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
    const D2D1_RECT_F parts[kEditParts] = {partClock, partCtl, partLyrics, partText, partCover, partWave};
    static const wchar_t* const kNames[kEditParts] = {L"Clock", L"Controls", L"Lyrics", L"Song", L"Cover", L"Waveform"};
    static const int kPageOf[kEditParts] = {kPageClock, kPageSong, kPageLyrics, kPageSong, kPageCover, kPageScene}; // the buttons' switches live with the song's, the waveform's with the scene's
    bool vinylLine = model.stageVinyl && model.fullLyrics && editTab == StagePrefs::kVinylLyr; // the vinyl room's one sung line: a part of its own
    bool has[kEditParts] = {s.clockOn > 0.5f, model.stageVinyl && model.stageVinylCtl && editTab >= StagePrefs::kVinyl, editTab == 1 || vinylLine, true, true, model.vizOn && (model.vizLooks & Model::kLookBars) != 0}; // what's in this version; the waveform only while it's worn
    if (!has[editPart]) editPart = kPartCover;
    D2D1_RECT_F boxes[kEditParts];
    for (int i = 0; i < kEditParts; ++i) boxes[i] = Inset(parts[i], -kOutset, -kOutset);
    float rads[kEditParts] = {kPartRad, kPartRad, kPartRad, kPartRad, (model.stageVinyl ? kPartRad : Width(partCover) * shapeNow[editTab].coverRound + kOutset), kPartRad}; // vinyl: the sleeve and its record together, a plain box
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
        int page = kPageOf[i]; // vinyl's words have their own lyrics page too
        if (editPage != page) editPage = page, ++editPageNth;
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
        else if (part == kPartCtl) s.ctlS = Clamped(editFrom.ctlS * k, StagePrefs::kCtlLo, StagePrefs::kCtlHi);
        else if (part == kPartWave) { // the waveform as the lyrics' column: wider and taller each way apart, about its middle
            s.waveW = Clamped(editFrom.waveW * (1 + 2 * dx / editSize.width), StagePrefs::kWaveWLo, StagePrefs::kWaveWHi);
            s.waveH = Clamped(editFrom.waveH * (1 + 2 * dy / editSize.height), StagePrefs::kWaveHLo, StagePrefs::kWaveHHi);
        } else {
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
    else if (part == kPartCtl) s.ctlX = editFrom.ctlX + ox, s.ctlY = editFrom.ctlY + oy;
    else if (part == kPartWave) s.waveX = editFrom.waveX + ox, s.waveY = editFrom.waveY + oy;
    else s.lyrX = editFrom.lyrX + ox, s.lyrY = editFrom.lyrY + oy;
}

void Stage::ResetPage(int page) {
    StagePrefs& sp = StagePrefs::Get();
    StagePrefs::Shape& s = sp.shape[editTab];
    const StagePrefs::Shape d{};
    auto num = [&](int& f, const wchar_t* key, int v) {
        if (f != v) model.SetPref(f, key, v);
    };
    switch (page) {
    case kPageCover:
        s.coverS = d.coverS, s.coverX = d.coverX, s.coverY = d.coverY, s.coverRound = d.coverRound, s.coverGlow = d.coverGlow;
        if (model.stageVinyl) num(model.stageVinylSpeed, L"StageVinylSpeed", Model::kVinylSpeedDef); // its slider lives here
        break;
    case kPageSong: // the song, and the vinyl room's buttons, which this page holds too
        s.textS = d.textS, s.textX = d.textX, s.textY = d.textY, s.appOn = d.appOn, s.albumOn = d.albumOn, s.extrasOn = d.extrasOn, s.timesOn = d.timesOn;
        s.ctlS = d.ctlS, s.ctlX = d.ctlX, s.ctlY = d.ctlY;
        break;
    case kPageLyrics:
        s.lyrX = d.lyrX, s.lyrY = d.lyrY, s.lyrW = d.lyrW, s.lyrH = d.lyrH, s.lyrAlign = d.lyrAlign, s.lyrDim = d.lyrDim, s.lyrHaze = d.lyrHaze, s.lyrAnchor = d.lyrAnchor;
        num(model.lyricScale, L"LyricScale", Model::kLyricDef);
        break;
    case kPageClock:
        s.clockOn = d.clockOn, s.clockS = d.clockS, s.clockX = d.clockX, s.clockY = d.clockY, s.clockA = d.clockA;
        num(model.stageClockFace, L"StageClockFace", 0);
        break;
    case kPageScene: // the numbers and the waveform's place; the switches are kept as they were set
        s.dim = d.dim, s.waveX = d.waveX, s.waveY = d.waveY, s.waveW = d.waveW, s.waveH = d.waveH;
        num(model.vizGain, L"VizGain", Model::kVizGainDef);
        num(model.vizAlpha, L"VizAlpha", Model::kVizAlphaDef);
        num(model.rimGlow, L"RimGlow", Model::kRimGlowDef);
        break;
    default: { // the keys, back to the ones it came with
        const StagePrefs fresh;
        for (int i = 1; i <= StagePrefs::kKeys; ++i) sp.Pick(i, fresh.KeyOf(i));
        sp.capture = 0;
        break;
    }
    }
    sp.SaveShape();
}

void Stage::ClockFaces(float x, float y, float wide, bool live) {
    Canvas& c = *canvas;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t time[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER, &st, nullptr, time, 32); // the time now, as the clock shows it, without am/pm to keep the tile calm
    float tw = (wide - (kFaceCols - 1) * kChipGap) / kFaceCols;
    int n = 0;
    for (int f = 0; f < gfx::kClockFaces; ++f) {
        if (!gfx::ClockFaceThere(f)) continue; // missing on this pc: not offered
        D2D1_RECT_F r = Rect(x + n % kFaceCols * (tw + kChipGap), y + n / kFaceCols * kFaceRow, tw, kFaceH);
        ++n;
        uint32_t id = Ui::Id("st.ed.face", f);
        Ui::Hit h = {};
        if (live) {
            h = ui.Button(id, r);
            ui.Name(std::wstring(L"Clock face: ") + kFaceNames[f]);
        }
        bool on = model.stageClockFace == f;
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), sel = ui.Smooth(id + 2, on ? 1.0f : 0, 14);
        float a = live ? 1.0f : kFaded * 2;
        c.Fill(r, 14, look.track.Alpha((0.55f + 0.25f * hov) * a).Mix(look.accent.Alpha(0.22f * a), sel));
        if (sel > 0.01f) c.Stroke(r, 14, look.accentHi.Alpha(0.9f * sel * a), 1.5f);
        c.TextIn(time, Rect(r.left, r.top + 6, tw, 32), gfx::ClockFmt(f, kFaceTime), look.text.Alpha(a));
        c.Text(kFaceNames[f], Rect(r.left, r.bottom - 20, tw, 14), Weight::Medium, kFaceName, (on ? look.accentHi : look.textDim).Alpha(a), Align::Center);
        if (h.clicked && !on) model.SetPref(model.stageClockFace, L"StageClockFace", f);
    }
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
        if (chip("st.ed.cover.vinyl", L"Vinyl", model.stageVinyl != 0)) pref(model.stageVinyl, L"StageVinyl"); // the cover as a record on a turntable
        if (model.stageVinyl) { // beside it: the arm's sound, then the finer touches
            if (chip("st.ed.cover.sound", L"Needle sound", model.stageVinylSound != 0)) pref(model.stageVinylSound, L"StageVinylSound");
            if (chip("st.ed.cover.label", L"Label details", model.stageVinylLabel != 0)) pref(model.stageVinylLabel, L"StageVinylLabel");
            y += kChipRow, col = 0;
        } else {
            y += kChipRow, col = 0;
        }
        if (model.stageVinyl) setting("st.ed.cover.spin", L"Spin speed", model.stageVinylSpeed, L"StageVinylSpeed", Model::kVinylSpeedMin, Model::kVinylSpeedMax, kSpinStep); // 100%: a real 33 1/3
        percent("st.ed.cover.size", L"Size", s.coverS, StagePrefs::kCoverLo, StagePrefs::kCoverHi, 100);
        if (!model.stageVinyl) percent("st.ed.cover.round", L"Corners", s.coverRound, 0, StagePrefs::kRoundHi, 200); // 100%: a circle; the sleeve and record have their own shapes
        percent("st.ed.cover.glow", L"Glow", s.coverGlow, 0, StagePrefs::kGlowHi, 100);
        break;
    case kPageSong:
        percent("st.ed.song.size", L"Size", s.textS, StagePrefs::kTextLo, StagePrefs::kTextHi, 100);
        if (model.stageVinyl && chip("st.ed.song.ctl", L"Controls  (C)", model.stageVinylCtl != 0)) pref(model.stageVinylCtl, L"StageVinylCtl"); // off: the record is the control
        if (!model.stageVinyl || model.stageVinylCtl) { // with the vinyl room's buttons off there's neither to show
            if (chip("st.ed.song.extras", L"Shuffle and repeat", s.extrasOn > 0.5f)) flip(s.extrasOn);
            if (chip("st.ed.song.times", L"Times", s.timesOn > 0.5f)) flip(s.timesOn);
        }
        if (editTab != 1) { // the layout with lyrics has no room for these anyway
            if (chip("st.ed.song.app", L"App name", s.appOn > 0.5f)) flip(s.appOn);
            if (chip("st.ed.song.album", L"Album name", s.albumOn > 0.5f)) flip(s.albumOn);
        }
        break;
    case kPageLyrics: {
        static const wchar_t* const kAlign[3] = {L"Left", L"Center", L"Right"};
        auto align = [&](const D2D1_RECT_F& r) {
            int was = (int)std::lround(s.lyrAlign), picked = sheet::Segmented(ui, look, "st.ed.lyr.align", r, kAlign, 3, was, live);
            if (picked >= 0) s.lyrAlign = (float)picked, sp.SaveShape();
        };
        auto column = [&] { // the scrolling words' own numbers, the full player's and vinyl's classic look alike
            setting("st.ed.lyr.type", L"Type size", model.lyricScale, L"LyricScale", Model::kLyricMin, Model::kLyricMax, kLyricStep, model.fullLyrics != 0);
            percent("st.ed.lyr.line", L"Sung line height", s.lyrAnchor, StagePrefs::kAnchorLo, StagePrefs::kAnchorHi, 100);
            percent("st.ed.lyr.dim", L"Other lines", s.lyrDim, StagePrefs::kLyrDimLo, StagePrefs::kLyrDimHi, 100);
            percent("st.ed.lyr.blur", L"Blur", s.lyrHaze, 0, StagePrefs::kHazeHi, 100);
        };
        if (editTab < StagePrefs::kVinyl) {
            if (w::Chip(ui, look, Ui::Id("st.ed.lyr.on"), Rect(x, y, cw, kChipH), L"Lyrics", model.fullLyrics != 0) && live) pref(model.fullLyrics, L"FullLyrics");
            align(Rect(x + cw + kChipGap, y, cw, kChipH));
            y += kChipRow;
            column();
            break;
        }
        // vinyl: the one sung line under the record, or the full player's scrolling words beside it
        if (chip("st.ed.lyr.on", L"Lyrics", model.fullLyrics != 0)) pref(model.fullLyrics, L"FullLyrics");
        if (chip("st.ed.lyr.classic", L"Classic lyrics", model.stageVinylClassic != 0)) pref(model.stageVinylClassic, L"StageVinylClassic");
        if (model.stageVinylClassic) {
            align(Rect(x, y, wide, kChipH));
            y += kChipRow;
            column();
        } else {
            if (chip("st.ed.lyr.glow", L"Lyric glow", model.stageVinylGlow != 0)) pref(model.stageVinylGlow, L"StageVinylGlow");
            y += kChipRow, col = 0;
            percent("st.ed.lyr.size", L"Size", s.lyrW, StagePrefs::kLyrWLo, StagePrefs::kLyrWHi, 100);
        }
        break;
    }
    case kPageClock:
        if (chip("st.ed.clock.on", L"Show the clock", s.clockOn > 0.5f)) flip(s.clockOn);
        y += kChipRow, col = 0;
        ClockFaces(x, y, wide, live && s.clockOn > 0.5f);
        y += (FacesThere() + kFaceCols - 1) / kFaceCols * kFaceRow;
        percent("st.ed.clock.size", L"Size", s.clockS, StagePrefs::kClockLo, StagePrefs::kClockHi, 100, s.clockOn > 0.5f);
        percent("st.ed.clock.alpha", L"Opacity", s.clockA, StagePrefs::kClockALo, 1, 100, s.clockOn > 0.5f);
        break;
    case kPageScene: {
        // the room: what's behind the song
        if (chip("st.ed.sc.art", L"Album art", model.stageArt != 0)) pref(model.stageArt, L"StageArtBg");
        if (chip("st.ed.sc.thump", L"Cover thump", model.stageThump != 0)) pref(model.stageThump, L"StageThump");
        if (chip("st.ed.sc.hide", L"Hide at rest", model.stageHide != 0)) pref(model.stageHide, L"StageHide");
        if (chip("st.ed.sc.lyrics", L"Lyrics", model.fullLyrics != 0)) pref(model.fullLyrics, L"FullLyrics"); // always here: the lyrics page shows only while there are lyrics, so off there was off for good
        percent("st.ed.sc.dim", L"Darken the background", s.dim, 0, StagePrefs::kDimHi, 100);
        y += kGroupGap;
        // the visualizer: its switch, then what hangs off it, only while it's on
        if (chip("st.ed.sc.viz", L"Visualizer", model.vizOn != 0)) pref(model.vizOn, L"Visualizer");
        if (chip("st.ed.sc.full", L"Any volume", model.vizFull != 0)) pref(model.vizFull, L"VizFull");
        if (!model.vizOn) break;
        float cw3 = (wide - 2 * kChipGap) / 3;
        int i = 0;
        for (auto [id, label, bit] : {std::tuple{"st.ed.sc.glow2", L"Glow", Model::kLookGlow}, std::tuple{"st.ed.sc.wave", L"Waveform", Model::kLookBars}, std::tuple{"st.ed.sc.rim", L"Edge glow", Model::kLookRim}}) { // its looks, any of them together: one row, read as a set
            if (w::Chip(ui, look, Ui::Id(id), Rect(x + i++ * (cw3 + kChipGap), y, cw3, kChipH), label, (model.vizLooks & bit) != 0) && live) model.SetPref(model.vizLooks, L"VizLooks", model.vizLooks ^ bit);
        }
        y += kChipRow;
        { // the media tab shows one visualizer at a time: which of them, the soft glow by default
            static const wchar_t* const kMedia[3] = {L"Glow", L"Wave", L"Edge"};
            c.Text(L"On the media tab", Rect(x + 2, y, cw, kChipH), Weight::Regular, 12, look.textDim);
            int picked = sheet::Segmented(ui, look, "st.ed.sc.media", Rect(x + cw + kChipGap, y, cw, kChipH), kMedia, 3, model.mediaWave, live);
            if (picked >= 0 && picked != model.mediaWave) model.SetPref(model.mediaWave, L"MediaWave", picked);
            y += kChipRow;
        }
        setting("st.ed.sc.gain", L"Strength", model.vizGain, L"VizGain", Model::kVizGainMin, Model::kVizGainMax, Model::kSlideStep);
        setting("st.ed.sc.alpha", L"Opacity", model.vizAlpha, L"VizAlpha", Model::kVizAlphaMin, 100, Model::kSlideStep);
        if ((model.vizLooks & Model::kLookRim) || model.mediaWave == rim::kMediaRim) // only while it's worn somewhere
            setting("st.ed.sc.rimglow", L"Edge glow", model.rimGlow, L"RimGlow", Model::kRimGlowMin, Model::kRimGlowMax, Model::kSlideStep);
        break;
    }
    default: {
        static const wchar_t* const kLabels[StagePrefs::kKeys] = {L"Two screens", L"Swap screens", L"Lyrics only"};
        for (int i = 0; i < StagePrefs::kKeys; ++i) { // the same ones as in settings: click it, then press the new one
            bool waiting = sp.capture == i + 1;
            c.Text(kLabels[i], Rect(x, y, wide - kKeyW - 8, kKeyH), Weight::Regular, 12, look.text);
            if (w::Chip(ui, look, Ui::Id("st.ed.key", i), Rect(x + wide - kKeyW, y + 2, kKeyW, kKeyH - 4), waiting ? std::wstring(L"Press a key") : StagePrefs::Name(sp.KeyOf(i + 1)), waiting) && live) sp.capture = waiting ? 0 : i + 1, sp.clash = false;
            y += kKeyH + kChipGap;
        }
        if (sp.capture && in.pressed && !Inside({x + wide - kKeyW, body.top, x + wide, y}, in.mouse)) sp.capture = 0; // clicked away: the key stays as it was
        c.Text(sp.capture ? (sp.clash ? L"That key is already used in fullscreen" : L"Esc keeps the key it had") : L"The first two need a second screen", Rect(x, y, wide, kNoteH), Weight::Light, 11, look.textDim);
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
    if (sheet::Act(ui, look, Ui::Id("st.ed.reset"), Rect(inner.right - 2 * btnW - 6, y, btnW, kHeadH), L"Reset", false, false, live))
        ResetPage(editPage); // only the page that's open, in this version; undo brings it back
    int go = history::Buttons(ui, look, "st.ed.hist", histX, y + kHeadH / 2, editHist, live);
    if (go < 0) EditLoad(editHist.Undo());
    else if (go > 0) EditLoad(editHist.Redo());
    y += kHeadH + kGap;
    if (canWith) { // both versions while a song has its words to show; one without has only the one
        static const wchar_t* const kTabs[2] = {L"Without lyrics", L"With lyrics"};
        int base = editTab >= StagePrefs::kVinyl ? StagePrefs::kVinyl : 0; // vinyl's pair or the player's
        int picked = sheet::Segmented(ui, look, "st.ed.tab", Rect(inner.left, y, Width(inner), kTabsH), kTabs, 2, editTab - base, live);
        if (picked >= 0) editTab = base + picked;
    } else {
        c.Text(editTab == StagePrefs::kVinyl ? L"Vinyl" : L"Without lyrics", Rect(inner.left + 2, y, Width(inner), kTabsH), Weight::Medium, 12, look.textDim); // vinyl is arranged on its own
    }
    y += kTabsH + kGap;
    if (!Worded(editTab) && editPage == kPageLyrics) editPage = kPageCover, ++editPageNth;
    int ids[kPages], n = 0;
    const wchar_t* names[kPages];
    static const wchar_t* const kNames[kPages] = {L"Cover", L"Song", L"Lyrics", L"Clock", L"Scene", L"Keys"};
    int cur = 0;
    for (int i = 0; i < kPages; ++i) {
        if (i == kPageLyrics && !Worded(editTab)) continue;
        if (i == editPage) cur = n;
        ids[n] = i, names[n++] = kNames[i];
    }
    int picked = sheet::Segmented(ui, look, "st.ed.page", Rect(inner.left, y, Width(inner), kTabsH), names, n, cur, live);
    if (picked >= 0) {
        editPage = ids[picked];
        ++editPageNth;
        sp.capture = 0;
        static const int kPartOf[kPages] = {kPartCover, kPartText, kPartLyrics, kPartClock, kPartWave, -1}; // the scene's part is the waveform, while it's worn
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
    const D2D1_RECT_F parts[kEditParts] = {partClock, partCtl, partLyrics, partText, partCover, partWave};
    bool covered = editDrag >= 0 && Overlap(bar, parts[editDrag / 2]) > 0;
    bool layer = show < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), show), nullptr);
    EditPanel(bar, canWith, covered);
    if (layer) ctx->PopLayer();
}
