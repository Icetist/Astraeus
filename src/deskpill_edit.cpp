// Arranging the desktop pill, as a phone's lock screen: the desktop comes up behind it and everything is moved with the mouse, on the pill itself.
#include "deskpill.h"

#include "pages.h"

#include <algorithm>
#include <cmath>

#include "faces.h"
#include "glyphs.h"

namespace {

constexpr float kLenMin = 90, kThickMin = 24, kThickMax = 56;
constexpr float kGapMin = 4, kGapMax = 120, kStickAt = 8; // nearer the edge than this it sticks
constexpr float kMidPull = 14; // the edge's middle holds a drag this far out
constexpr float kLenPull = 9; // natural lengths hold it this far out
constexpr float kThickPull = 1.6f;
constexpr float kOtherEdge = 60; // how much nearer another edge must be to go over to it
constexpr float kGripLen = 15, kGripThick = 12; // reach of the handles from the pill's outline
constexpr float kPullOff = 34; // an item dragged this far off the pill is out
constexpr float kBarW = 780, kBarPad = 14, kChipH = 30, kRowGap = 8, kBarBottom = 28, kDragStart = 6;
constexpr float kEdgeSpare = 24; // the longest it gets leaves this of its edge
constexpr float kBackRate = 9, kFollow = 22, kTurn = 13;
constexpr float kShakeRate = 7, kShakeReach = 7;
constexpr float kBackFrom = 1.05f; // the wallpaper settles from a little closer

constexpr int kAll[] = {Model::kDClock, Model::kDDate, Model::kWWeather, Model::kDMusic, Model::kDCover, Model::kDBars, Model::kDTimer, Model::kDVolume, Model::kDWifi, Model::kWCpu, Model::kWGpu, Model::kWRam, Model::kWNet, Model::kWDisk, Model::kWBattery, Model::kWWork, Model::kWLang, Model::kWTitle, Model::kWMarket, Model::kDScore};

constexpr float kDefLen = 560, kDefThick = 30, kDefGap = 10;
constexpr float kSetMark = -1; // in an undo snapshot: between the desktop's set and the other one; no item is negative

float Pull(float v, float to, float reach) { return std::fabs(v - to) < reach ? to : v; }

}

std::vector<int> DeskPill::Offered() const {
    std::vector<int> out;
    bool sport = std::any_of(model.faceOn.begin(), model.faceOn.end(), [](int f) { // the score only with a sports widget on the board: its leagues are picked there
        std::string key = faces::Info(f).key;
        return key == "sport.scores" || key == "sport.fixtures";
    });
    for (int k : kAll)
        if ((k != Model::kWBattery || model.portable) && (k != Model::kWWork || model.tiler != sys::Tiler::kNone) && (k != Model::kWTitle || spec.apps) && (k != Model::kDScore || sport)) out.push_back(k);
    return out;
}

void DeskPill::Edit(HMONITOR on, int from) {
    if (Editing() || !hwnd) return;
    Measure(on); // the saved values, one last time
    model.WantDeskAudio(); // the volume and the visualizer are live in here too
    ++editRun;
    // the tab it opens on: where the pill that asked was, unless only the other place wears the long pill
    bool deskLong = spec.desk && model.pillDesk, wantApps = from == Model::kEditFromApps || (from == Model::kEditFromLong && onApp && spec.apps);
    if (wantApps && !spec.apps && deskLong) wantApps = false;
    if (!wantApps && !deskLong && spec.apps) wantApps = true;
    askLong = from != Model::kEditFromLong && !(wantApps ? spec.apps : deskLong); // the long pill nowhere: ask before anything changes
    editing = true; // from here the working copy rules
    wantUp = false;
    drag = Drag::None;
    previewApps = wantApps;
    model.deskPreview = wantApps ? 1 : 0; // it wears that tab's theme whatever is behind it now
    SyncItems();
    FitWindow();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RegisterHotKey(hwnd, kEscKey, 0, VK_ESCAPE); // taken by another app: Done and a click away still finish
    pose = Goal();
    intro = 0;
    inkDue = 0;
    if (onLook) onLook();
    inkAfterIntro = FillNow() == kFillClear;
    wallFor = 0;
    input = Input{};
    cursor = IDC_ARROW;
    Poke();
}

void DeskPill::SetPreview(bool apps) {
    previewApps = apps;
    model.deskPreview = apps ? 1 : 0;
    SyncItems(); // the tab's own set, when it has one
    if (onLook) onLook();
    if (FillNow() == kFillClear) inkDue = lastFrame + 0.05;
    Poke();
}

void DeskPill::Studio(HMONITOR on) {
    if (Editing() || !hwnd) return;
    Measure(on);
    editing = studio = true;
    wantUp = false;
    drag = Drag::None;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~(LONG_PTR)WS_EX_NOACTIVATE); // its search takes the keyboard
    FitWindow();
    ShowWindow(hwnd, SW_SHOW);
    RegisterHotKey(hwnd, kEscKey, 0, VK_ESCAPE);
    pose = Goal();
    intro = 0;
    inkDue = 0;
    inkAfterIntro = false;
    wallFor = 0;
    input = Input{};
    cursor = IDC_ARROW;
    model.studioOpen = model.studioKind == Model::kStudioWidgets;
    ++model.studioRun;
    model.SyncFaces(); // the panel's closed for this: what's on the board keeps fetching
    Poke();
}

void DeskPill::DrawStudio(const Theme& t) {
    constexpr float kScrim = 0.3f, kRise = 30;
    cursor = IDC_ARROW;
    canvas->Fill({-1, -1, MW + 1, MH + 1}, 0, Color{0, 0, 0, kScrim * intro});
    bool fading = intro < 0.999f;
    if (fading) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), intro), nullptr);
    float rise = (1 - anim::EaseOut(intro)) * kRise, bottom = (work.bottom - monitor.top) / scale;
    PageCtx pc{ui, model, t, t, nullptr, nullptr, 1.0f, Rect(0, rise, MW, bottom)};
    if (model.studioKind == Model::kStudioTiles) DrawTileStudio(pc);
    else if (model.studioKind == Model::kStudioPages) DrawPageStudio(pc);
    else DrawWidgetStudio(pc);
    if (fading) ctx->PopLayer();
    if (editing && pc.wantClose) Finish();
}

History::Snap DeskPill::SpecSnap() const { // its place and size, both shapes, its bodies, then its items
    History::Snap s{(float)spec.edge, spec.pos, spec.thick, spec.apps ? 1.0f : 0, (float)spec.fill, (float)spec.fillApps, spec.desk ? 1.0f : 0, model.pillDesk ? 1.0f : 0}; // picking long here can turn the desktop's pill on: undone with it
    for (const Spec::Shape& sh : spec.shape) s.push_back(sh.len), s.push_back(sh.gap), s.push_back(sh.floats ? 1.0f : 0);
    for (int k : DeskSet()) s.push_back((float)k);
    s.push_back(kSetMark), s.push_back(spec.split ? 1.0f : 0); // then whether it splits, and the set for when not on desktop
    for (int k : AppsSet()) s.push_back((float)k);
    return s;
}

void DeskPill::SpecLoad(const History::Snap& s) {
    constexpr size_t kHead = 8, kPerShape = 3, kItems = kHead + 2 * kPerShape;
    if (s.size() < kItems) return;
    spec.edge = (int)s[0], spec.pos = s[1], spec.thick = s[2], spec.apps = s[3] > 0.5f, spec.fill = (int)s[4], spec.fillApps = (int)s[5], spec.desk = s[6] > 0.5f;
    if (int pill = s[7] > 0.5f ? 1 : 0; pill != model.pillDesk) model.SetPref(model.pillDesk, L"PillDesktop", pill);
    for (int i = 0; i < 2; ++i) {
        const float* f = &s[kHead + i * kPerShape];
        spec.shape[i] = {f[0], f[1], f[2] > 0.5f};
    }
    std::vector<int> desk, apps;
    size_t i = kItems;
    for (; i < s.size() && s[i] != kSetMark; ++i) desk.push_back((int)s[i]);
    spec.split = i + 1 < s.size() && s[i + 1] > 0.5f;
    for (i += 2; i < s.size(); ++i) apps.push_back((int)s[i]);
    spec.items = itemsCtx == 0 ? desk : apps, spec.itemsOther = itemsCtx == 0 ? apps : desk; // into the context showing
    SyncItems();
    drag = Drag::None;
    Commit();
}

void DeskPill::Commit() {
    auto num = [&](int& field, const wchar_t* key, int v) { if (field != v) model.SetPref(field, key, v); };
    num(model.deskEdge, L"DeskEdge", spec.edge);
    num(model.deskPos, L"DeskPos", (int)std::lround(spec.pos));
    num(model.deskLen, L"DeskLen", (int)std::lround(spec.shape[0].len));
    num(model.deskLenApps, L"DeskLenApps", (int)std::lround(spec.shape[1].len));
    num(model.deskThick, L"DeskThick", (int)std::lround(spec.thick));
    num(model.deskGap, L"DeskGap", (int)std::lround(spec.shape[0].gap));
    num(model.deskGapApps, L"DeskGapApps", (int)std::lround(spec.shape[1].gap));
    num(model.deskFloat, L"DeskFloat", spec.shape[0].floats ? 1 : 0);
    num(model.deskFloatApps, L"DeskFloatApps", spec.shape[1].floats ? 1 : 0);
    num(model.deskApps, L"DeskApps", spec.apps ? 1 : 0);
    num(model.deskPill, L"DeskPill", spec.desk ? 1 : 0);
    num(model.deskFill, L"DeskFill", spec.fill);
    num(model.deskFillApps, L"DeskFillApps", spec.fillApps);
    if (model.deskItems != DeskSet()) model.SetDeskItems(DeskSet());
    num(model.deskSplit, L"DeskSplit", spec.split ? 1 : 0);
    if (spec.split && model.deskItemsApps != AppsSet()) model.SetDeskItemsApps(AppsSet());
}

void DeskPill::Finish() {
    if (!editing) return;
    if (studio) {
        model.faceSearch = false;
        input = Input{}; // a last click can't land on what's fading
    } else {
        Commit();
    }
    UnregisterHotKey(hwnd, kEscKey);
    if (GetCapture() == hwnd) ReleaseCapture();
    drag = Drag::None;
    editing = false;
    leaving = true;
    cursor = IDC_HAND;
    Poke();
}

void DeskPill::Leave() {
    leaving = false;
    wallFor = 0;
    wall.Reset();
    wallBlur.Reset();
    bool wasStudio = studio;
    if (studio) {
        studio = false;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
        model.studioOpen = false;
        model.faceQuery.clear();
        model.SyncFaces();
    }
    model.deskPreview = -1;
    Measure(screen);
    shapeShift = false;
    rest = Goal();
    FitWindow();
    input = Input{};
    if (wasStudio) { // nothing of the pill was showing: the drawer decides again
        appear = anim::Spring{};
        wantUp = false;
        if (onStudioDone) onStudioDone();
        return;
    }
    if (!TabLong() || askLong) { // it showed no long pill: none drops back in, the drawer decides
        askLong = false;
        appear = anim::Spring{};
        wantUp = false;
        if (onDone) onDone();
        return;
    }
    appear.value = appear.target = 100; // the pill the editor just showed: no drop-in for what's already there
    appear.vel = 0;
    wantUp = true;
    if (FillNow() == kFillClear) inkDue = lastFrame + 0.2; // the real desktop again, not the editor's wallpaper
    if (onDone) onDone(); // the drawer's own look decides whether it stays
}

bool DeskPill::StepEdit(float dt) {
    bool calm = model.reduceMotion != 0;
    bool moving = anim::Approach(intro, editing ? 1.0f : 0.0f, calm ? 60.0f : kBackRate, dt, 0.002f);
    if (inkAfterIntro && intro > 0.98f && wall) inkAfterIntro = false, inkDue = lastFrame + 0.05; // the wallpaper's up: what the ink will sit on
    Pose g = Goal();
    float rate = calm ? 200.0f : kFollow;
    moving = anim::Approach(pose.ax, g.ax, rate, dt, 0.02f) || moving;
    moving = anim::Approach(pose.ay, g.ay, rate, dt, 0.02f) || moving;
    moving = anim::Approach(pose.len, g.len, rate, dt, 0.02f) || moving;
    moving = anim::Approach(pose.thick, g.thick, rate, dt, 0.02f) || moving;
    moving = anim::Approach(pose.flat, g.flat, calm ? 200.0f : kTurn, dt, 0.004f) || moving;
    moving = anim::Approach(pose.angle, g.angle, calm ? 200.0f : kTurn, dt, 0.02f) || moving;
    if (shake > 0.001f) {
        moving = anim::Approach(shake, 0, kShakeRate, dt, 0.002f) || moving;
        float wob = std::sin(shake * 18) * shake * kShakeReach, a = pose.angle * 3.14159265f / 180;
        pose.ax += std::cos(a) * wob * dt * 60, pose.ay += std::sin(a) * wob * dt * 60;
    }
    return moving;
}

void DeskPill::DrawBackdrop() {
    float k = kBackFrom + (1 - kBackFrom) * anim::EaseOut(intro); // the desktop comes forward, then settles
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {MW / 2, MH / 2}));
    canvas->Fill({-1, -1, MW + 1, MH + 1}, 0, wallColor.Alpha(intro));
    if (wall) ctx->DrawBitmap(wall.Get(), WallRect(), intro, D2D1_INTERPOLATION_MODE_LINEAR);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DeskPill::DrawGuides(const Theme& t) {
    if (drag != Drag::Move) return;
    Canvas& c = *canvas;
    float lo, hi;
    Span(spec.edge, lo, hi);
    Color line = t.accentHi.Alpha(0.9f);
    float mid = (lo + hi) / 2;
    if (magnetMid) { // held at the middle: a short tick says so
        if (spec.edge == 0) c.Line({mid, 2}, {mid, 20}, line, 2);
        else if (spec.edge == 1) c.Line({2, mid}, {20, mid}, line, 2);
        else c.Line({MW - 2, mid}, {MW - 20, mid}, line, 2);
    }
    Color glow = t.accent.Alpha(0.55f); // the edge it belongs to
    if (spec.edge == 0) c.Line({12, 1}, {MW - 12, 1}, glow, 2);
    else if (spec.edge == 1) c.Line({1, lo + 12}, {1, hi - 12}, glow, 2);
    else c.Line({MW - 1, lo + 12}, {MW - 1, hi - 12}, glow, 2);
}

void DeskPill::DrawGrips(const Theme& t) {
    Canvas& c = *canvas;
    if (!body) return;
    D2D1_POINT_2F m = ToLocal(pose, input.mouse);
    float half = pose.len / 2, th = pose.thick;
    bool overBar = Inside(BarRect(), input.mouse);
    bool onEnd = !overBar && std::fabs(std::fabs(m.x) - half) < kGripLen && m.y > -6 && m.y < th + 6;
    bool onThick = !overBar && std::fabs(m.x) < 34 && m.y > th - 2 && m.y < th + kGripThick + 8;
    bool inside = !overBar && std::fabs(m.x) < half && m.y > 0 && m.y < th;
    float e = ui.Smooth(Ui::Id("dk.grip.end"), drag == Drag::Len ? 1.0f : onEnd ? 0.6f : 0, 18), v = ui.Smooth(Ui::Id("dk.grip.th"), drag == Drag::Thick ? 1.0f : onThick ? 0.6f : 0, 18);
    ctx->SetTransform(Local(pose));
    c.StrokeGeo(body.Get(), t.accentHi.Alpha(0.75f), 1.5f); // it's picked up: an outline
    Color grip{1, 1, 1, 1}, edge{0, 0, 0, 0.35f};
    float gh = th * 0.42f + 5 * e;
    for (float s : {-1.0f, 1.0f}) { // its two ends
        D2D1_RECT_F r = Rect(s * (half + 7) - 2.5f, th / 2 - gh / 2, 5, gh);
        c.Fill(Inset(r, -1, -1), 3.5f, edge);
        c.Fill(r, 2.5f, grip);
    }
    float gw = 26 + 12 * v;
    D2D1_RECT_F r = Rect(-gw / 2, th + 6, gw, 5); // its thickness
    c.Fill(Inset(r, -1, -1), 3.5f, edge);
    c.Fill(r, 2.5f, grip);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-view.x, -view.y));
    if (drag == Drag::None) {
        bool across = spec.edge != 0; // on a side the pill's length runs up and down
        cursor = onEnd ? (across ? IDC_SIZENS : IDC_SIZEWE) : onThick ? (across ? IDC_SIZEWE : IDC_SIZENS) : inside ? IDC_SIZEALL : IDC_ARROW;
    }
}

D2D1_RECT_F DeskPill::BarRect() const {
    float w = (std::min)(kBarW, MW - 32), h = 2 * kBarPad + 4 * kChipH + 3 * kRowGap; // up to two rows of items, the choices, then its bodies
    float bottom = (work.bottom - monitor.top) / scale - kBarBottom;
    return Rect((MW - w) / 2, bottom - h, w, h);
}

bool DeskPill::Seg(const Theme& t, const char* key, const D2D1_RECT_F& r, const wchar_t* const* names, int n, int& value) {
    Canvas& c = *canvas;
    bool changed = false;
    c.Fill(r, Height(r) / 2, t.track);
    float w = Width(r) / n, x = ui.Smooth(Ui::Id(key, 99), r.left + w * value, 18);
    c.Fill(Rect(x + 2, r.top + 2, w - 4, Height(r) - 4), (Height(r) - 4) / 2, t.accent);
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F q = Rect(r.left + w * i, r.top, w, Height(r));
        Ui::Hit h = ui.Button(Ui::Id(key, i), q);
        ui.Mark(Ui::Role::Toggle, i == value);
        if (h.clicked && i != value) value = i, changed = true;
        c.Text(names[i], q, Weight::Medium, 11.5f, i == value ? t.onAccent : t.text, Align::Center);
    }
    return changed;
}

void DeskPill::DrawBar(const Theme& t) {
    Canvas& c = *canvas;
    D2D1_RECT_F bar = BarRect();
    hist.Sync(editRun, SpecSnap(), drag == Drag::None && !input.down); // a drag counts once it lands
    float rise = (1 - anim::EaseOut(intro)) * 40; // comes up after the desktop
    bar.top += rise, bar.bottom += rise;
    pressedOnBar = Inside(bar, input.down || input.pressed ? input.pressPos : input.mouse);
    c.Fill(Rect(bar.left, bar.top + 3, Width(bar), Height(bar)), 24, Color{0, 0, 0, 0.22f * intro});
    c.Fill(bar, 24, t.cardHi.Alpha(intro));
    c.StrokeV(bar, 24, t.hairTop.Alpha(intro), t.hairBot.Alpha(intro), 1);

    float right = bar.right - kBarPad;
    DrawBarHead(t, bar);
    float x = bar.left + kBarPad, y = bar.top + kBarPad + kChipH + kRowGap; // the items: tap to add or take off, or drag onto the pill
    if (!TabLong()) { // the regular pill here: nothing of the long one's to arrange
        D2D1_RECT_F say = {bar.left + kBarPad, y, right, bar.bottom - kBarPad};
        bool none = !previewApps && !model.pillDesk; // pill on the desktop is off: nothing shows there at all
        c.Text(none ? L"No pill on the desktop" : L"The regular pill shows here", Rect(say.left, say.top + 14, Width(say), 22), Weight::Medium, 14, t.text, Align::Center);
        c.Text(none ? L"Pick Long above to put the long pill there" : L"Pick Long above to arrange a long pill for this place", Rect(say.left, say.top + 40, Width(say), 18), Weight::Regular, 11.5f, t.textDim, Align::Center);
        if (!input.down && !input.released) moved = false;
        return;
    }
    for (int kind : Offered()) {
        const wchar_t* name = Model::DeskItemName(kind);
        float w = c.Measure(name, Weight::Medium, 11.5f) + 24;
        if (x + w > right) x = bar.left + kBarPad, y += kChipH + kRowGap;
        D2D1_RECT_F r = Rect(x, y, w, kChipH);
        uint32_t id = Ui::Id("dk.chip", kind);
        Ui::Hit h = ui.Button(id, r);
        bool on = std::find(spec.items.begin(), spec.items.end(), kind) != spec.items.end();
        ui.Mark(Ui::Role::Toggle, on);
        float sel = ui.Smooth(id + 1, on ? 1.0f : 0, 16), hov = ui.Smooth(id + 2, h.hover ? 1.0f : 0, 16);
        c.Fill(r, kChipH / 2, t.track.Mix(t.accent, sel).Mix(t.text, 0.07f * hov));
        c.Text(name, r, Weight::Medium, 11.5f, t.text.Mix(t.onAccent, sel), Align::Center);
        if (h.down && drag == Drag::None && !moved) {
            float dx = input.mouse.x - input.pressPos.x, dy = input.mouse.y - input.pressPos.y;
            if (dx * dx + dy * dy > kDragStart * kDragStart) { // lifted off the tray
                drag = Drag::Item, dragKind = kind, moved = true, offPill = true;
                auto it = std::find(spec.items.begin(), spec.items.end(), kind);
                if (it != spec.items.end()) spec.items.erase(it); // it comes back wherever it lands
            }
        }
        if (h.clicked && !moved) {
            auto it = std::find(spec.items.begin(), spec.items.end(), kind);
            if (it != spec.items.end()) {
                spec.items.erase(it);
                Commit();
            } else {
                std::vector<int> more = spec.items;
                more.push_back(kind);
                float need = NeedLen(more, spec.thick), lo, hi;
                Span(spec.edge, lo, hi);
                if (need > hi - lo - kEdgeSpare) {
                    shake = 1; // no room on this edge
                } else {
                    spec.items = std::move(more);
                    Grow(need);
                    Commit();
                }
            }
        }
        x += w + 6;
    }

    float bodies = bar.bottom - kBarPad - kChipH, ox = bar.left + kBarPad; // the one picked: how it holds to its edge, and its body
    constexpr float kHoldW = 170, kFillW = 300, kGapX = 12;
    static const wchar_t* const kHolds[] = {L"Stuck", L"Floating"};
    Spec::Shape& shape = Sh();
    int hold = shape.floats ? 1 : 0;
    if (Seg(t, "dk.hold", Rect(ox, bodies, kHoldW, kChipH), kHolds, 2, hold)) { // the pill only: the island and the panel keep to the edge
        shape.floats = hold == 1;
        shape.gap = (std::max)(shape.gap, kDefGap);
        Commit();
    }
    ox += kHoldW + kGapX;
    static const wchar_t* const kFills[] = {L"Theme", L"Accent", L"Glass", L"Clear"};
    int& fill = Ctx() ? spec.fillApps : spec.fill;
    if (Seg(t, "dk.fill", Rect(ox, bodies, kFillW, kChipH), kFills, 4, fill)) {
        if (fill == kFillClear) inkDue = lastFrame + 0.05;
        Commit();
    }
    if (!input.down && !input.released) moved = false;
}

bool DeskPill::BarButton(const Theme& t, const char* key, const D2D1_RECT_F& r, const wchar_t* label, bool primary) {
    Canvas& c = *canvas;
    Ui::Hit h = ui.Button(Ui::Id(key), r);
    float hov = ui.Smooth(Ui::Id(key, 1), h.hover ? 1.0f : 0, 16);
    c.Fill(r, kChipH / 2, primary ? t.accent.Mix(t.accentHi, 0.3f * hov) : t.track.Mix(t.text, 0.07f * hov));
    c.Text(label, r, Weight::Medium, 11.5f, primary ? t.onAccent : t.text, Align::Center);
    return h.clicked;
}

void DeskPill::DrawBarHead(const Theme& t, const D2D1_RECT_F& bar) { // which place, the pill it wears there, then undo, reset and done
    constexpr float kTabW = 210, kFormW = 150, kSplitW = 118, kGapX = 12, kResetW = 76, kDoneW = 88;
    float x = bar.left + kBarPad, y = bar.top + kBarPad, right = bar.right - kBarPad;
    static const wchar_t* const kTabs[] = {L"On desktop", L"Not on desktop"};
    int tab = previewApps ? 1 : 0;
    if (Seg(t, "dk.tab", Rect(x, y, kTabW, kChipH), kTabs, 2, tab)) SetPreview(tab == 1);
    x += kTabW + kGapX;
    static const wchar_t* const kForms[] = {L"Regular", L"Long"};
    int form = TabLong() ? 1 : 0;
    if (Seg(t, "dk.form", Rect(x, y, kFormW, kChipH), kForms, 2, form)) { // this place's pill: the regular one, or this long one
        if (previewApps) {
            spec.apps = form == 1;
            DropAppItems();
        } else {
            spec.desk = form == 1;
            if (spec.desk && !model.pillDesk) model.SetPref(model.pillDesk, L"PillDesktop", 1); // asked for a pill on the desktop: it shows there
        }
        SyncItems();
        Commit();
    }
    x += kFormW + kGapX;
    if (previewApps && spec.apps && BarButton(t, "dk.split", Rect(x, y, kSplitW, kChipH), L"Own widgets", spec.split)) { // off by default: the same widgets everywhere
        spec.split = !spec.split;
        if (spec.split) spec.itemsOther = spec.items; // it starts as a copy of the set showing
        SyncItems();
        Commit();
    }
    if (BarButton(t, "dk.done", Rect(right - kDoneW, y, kDoneW, kChipH), L"Done", true)) Finish();
    if (BarButton(t, "dk.reset", Rect(right - kDoneW - 8 - kResetW, y, kResetW, kChipH), L"Reset", false)) {
        spec.edge = 0, spec.pos = 500, spec.thick = kDefThick, spec.apps = false, spec.desk = true, spec.fill = spec.fillApps = kFillTheme;
        for (Spec::Shape& s : spec.shape) s = {kDefLen, kDefGap, true};
        spec.items = spec.itemsOther = Model::DeskDefaults();
        spec.split = false;
        SetPreview(false);
        Commit();
    }
    int go = history::Buttons(ui, t, "dk.hist", right - kDoneW - 8 - kResetW - 8 - history::kW, y + kChipH / 2, hist, true);
    if (go < 0) SpecLoad(hist.Undo());
    else if (go > 0) SpecLoad(hist.Redo());
}

void DeskPill::DrawAsk(const Theme& t) { // the regular pill was right clicked and the long one is nowhere: switch first, or leave
    constexpr float kAskW = 440, kAskH = 156, kBtnW = 150, kLaterW = 96;
    Canvas& c = *canvas;
    float rise = (1 - anim::EaseOut(intro)) * 30;
    D2D1_RECT_F r = Rect((MW - kAskW) / 2, MH / 2 - kAskH / 2 + rise, kAskW, kAskH);
    c.Fill(Rect(r.left, r.top + 4, kAskW, kAskH), 24, Color{0, 0, 0, 0.25f * intro});
    c.Fill(r, 24, t.cardHi.Alpha(intro));
    c.StrokeV(r, 24, t.hairTop.Alpha(intro), t.hairBot.Alpha(intro), 1);
    D2D1_RECT_F in = Inset(r, 22, 20);
    c.Text(L"Customizing needs the long pill", Rect(in.left, in.top, Width(in), 24), Weight::Semi, 15, t.text);
    c.Text(previewApps ? L"Use the long pill when not on desktop?" : L"Use the long pill on the desktop?", Rect(in.left, in.top + 30, Width(in), 18), Weight::Regular, 12.5f, t.textDim);
    c.Text(L"You can switch back here or in Settings.", Rect(in.left, in.top + 50, Width(in), 18), Weight::Regular, 12.5f, t.textDim);
    float by = in.bottom - kChipH;
    bool yes = BarButton(t, "dk.ask.yes", Rect(in.right - kBtnW, by, kBtnW, kChipH), L"Use the long pill", true);
    bool no = BarButton(t, "dk.ask.no", Rect(in.right - kBtnW - 8 - kLaterW, by, kLaterW, kChipH), L"Not now", false);
    if (input.released && !Inside(r, input.pressPos)) no = true; // a click away: as not now
    if (no) {
        Finish();
        return;
    }
    if (!yes) return;
    if (previewApps) {
        spec.apps = true;
    } else {
        spec.desk = true;
        if (!model.pillDesk) model.SetPref(model.pillDesk, L"PillDesktop", 1);
    }
    askLong = false;
    SyncItems();
    Commit();
    pose = Goal();
    Poke();
}

void DeskPill::PressPill() {
    D2D1_POINT_2F m = ToLocal(pose, input.pressPos);
    float half = pose.len / 2, th = pose.thick;
    moved = false;
    bareDown = false;
    if (std::fabs(std::fabs(m.x) - half) < kGripLen && m.y > -6 && m.y < th + 6) { // its ends first: they sit over the outermost items
        drag = Drag::Len;
        grabLen = std::fabs(m.x) * 2 - Sh().len;
        return;
    }
    if (std::fabs(m.x) < 34 && m.y > th - 2 && m.y < th + kGripThick + 8) {
        drag = Drag::Thick;
        grabY = m.y - spec.thick;
        return;
    }
    if (std::fabs(m.x) < half && m.y >= 0 && m.y < th) {
        for (const Placed& it : placed)
            if (m.x >= it.x - 4 && m.x <= it.x + it.w + 4) { // on an item it's the item that moves; the pill moves from the space between
                drag = Drag::Item, dragKind = it.kind, offPill = false;
                return;
            }
        drag = Drag::Move;
        grabX = m.x, grabY = m.y;
        return;
    }
    bareDown = true; // a press on the bare desktop: its release finishes
}

void DeskPill::DragNow() {
    float lo, hi;
    Span(spec.edge, lo, hi);
    D2D1_POINT_2F mm = input.mouse, m = ToLocal(pose, mm);
    magnetMid = false;
    switch (drag) {
    case Drag::Len: {
        float raw = std::fabs(m.x) * 2 - grabLen, least = (std::max)(kLenMin, NeedLen(spec.items, spec.thick)), most = hi - lo - kEdgeSpare;
        for (float pull : {least, (hi - lo) / 3, (hi - lo) / 2}) raw = Pull(raw, pull, kLenPull);
        if (raw < least) raw = least - (least - raw) * 0.12f; // resists: the items are in the way
        Sh().len = (std::min)(raw, most);
        cursor = spec.edge ? IDC_SIZENS : IDC_SIZEWE;
        break;
    }
    case Drag::Thick: {
        float raw = m.y - grabY;
        for (float pull : {kDefThick, 34.0f, 40.0f}) raw = Pull(raw, pull, kThickPull);
        spec.thick = (std::clamp)(raw, kThickMin, kThickMax);
        cursor = spec.edge ? IDC_SIZEWE : IDC_SIZENS;
        break;
    }
    case Drag::Move: {
        float dTop = mm.y, dLeft = mm.x, dRight = MW - mm.x, here = spec.edge == 0 ? dTop : spec.edge == 1 ? dLeft : dRight; // the nearest edge, once it's clearly nearer
        int best = spec.edge;
        float bestD = here - kOtherEdge;
        if (dTop < bestD) best = 0, bestD = dTop;
        if (dLeft < bestD) best = 1, bestD = dLeft;
        if (dRight < bestD) best = 2, bestD = dRight;
        if (best != spec.edge) {
            spec.edge = best;
            Span(best, lo, hi);
            grabX = 0; // turning over: the cursor holds its middle
            grabY = (std::min)(grabY, spec.thick);
            here = best == 0 ? dTop : best == 1 ? dLeft : dRight;
        }
        float along = best == 0 ? mm.x - grabX : best == 1 ? mm.y + grabX : mm.y - grabX, mid = (lo + hi) / 2;
        float room = (std::min)(Sh().len, hi - lo - kEdgeSpare) / 2 + kEdgeSpare / 2;
        along = (std::clamp)(along, lo + room, hi - room);
        if (std::fabs(along - mid) < kMidPull) along = mid, magnetMid = true;
        spec.pos = (along - lo) / (hi - lo) * 1000;
        float depth = here - grabY;
        if (depth < kStickAt) Sh().floats = false; // near the edge it sticks; pulled off, it floats
        else Sh().floats = true, Sh().gap = (std::clamp)(depth, kGapMin, kGapMax);
        cursor = IDC_SIZEALL;
        break;
    }
    case Drag::Item: {
        float dx = mm.x - input.pressPos.x, dy = mm.y - input.pressPos.y;
        if (!moved && dx * dx + dy * dy < kDragStart * kDragStart) break; // not pulled yet
        moved = true;
        cursor = IDC_HAND;
        bool over = std::fabs(m.x) < pose.len / 2 + kPullOff && m.y > -kPullOff && m.y < pose.thick + kPullOff;
        auto it = std::find(spec.items.begin(), spec.items.end(), dragKind);
        if (!over) {
            if (it != spec.items.end()) spec.items.erase(it);
            offPill = true;
            break;
        }
        size_t at = 0;
        for (const Placed& p : placed)
            if (p.kind != dragKind && m.x > p.x + p.w / 2) ++at; // past this one's middle: it goes after it
        std::vector<int> next = spec.items;
        auto was = std::find(next.begin(), next.end(), dragKind);
        if (was != next.end()) next.erase(was);
        next.insert(next.begin() + (std::min)(at, next.size()), dragKind);
        float need = NeedLen(next, spec.thick);
        if (need <= hi - lo - kEdgeSpare) {
            spec.items = std::move(next);
            Grow(need);
            offPill = false;
        } else {
            if (!offPill) shake = 1; // no room: it stays in the hand
            offPill = true;
        }
        break;
    }
    default:
        break;
    }
}

void DeskPill::Land() {
    bool bare = bareDown;
    bareDown = false;
    Drag kind = drag;
    drag = Drag::None;
    dragKind = -1;
    magnetMid = false;
    moved = false;
    if (bare) { // a click away
        Finish();
        return;
    }
    if (kind == Drag::None) return;
    Grow((std::max)(kLenMin, NeedLen(spec.items, spec.thick))); // past what the items allow, it springs back
    Commit();
    if (FillNow() == kFillClear) inkDue = lastFrame + 0.15; // settled somewhere new
    Poke();
}

void DeskPill::DrawFloater(const Theme& t) { // the item in the hand
    if (drag != Drag::Item || dragKind < 0 || !moved) return;
    Canvas& c = *canvas;
    std::wstring text;
    int icon = 0;
    bool idle = false;
    Text(dragKind, text, icon, idle);
    if (idle) text = Model::DeskItemName(dragKind);
    float w = (icon ? 20.0f : 0) + (text.empty() ? 0 : c.Measure(text, Weight::Medium, 12)) + 26, h = 30;
    D2D1_RECT_F r = Rect(input.mouse.x - w / 2, input.mouse.y - h / 2, w, h);
    c.Fill(Rect(r.left, r.top + 3, w, h), h / 2, Color{0, 0, 0, 0.28f});
    c.Fill(r, h / 2, offPill ? t.track.Mix(t.cardHi, 0.3f) : t.accent);
    Color ink = offPill ? t.text : t.onAccent;
    float x = r.left + 13;
    if (icon) c.Icon(icon, {x + 8, input.mouse.y}, 15, ink, true), x += 20;
    if (!text.empty()) c.Text(text, Rect(x, r.top, r.right - x, h), Weight::Medium, 12, ink);
}
