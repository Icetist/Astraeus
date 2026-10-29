#include "page_sheet.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <unordered_map>

#include "anim.h"
#include "glyphs.h"

namespace sheet {

Frame Begin(PageCtx& p, float k, bool split) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    const D2D1_RECT_F& s = p.area;
    Frame f;
    f.tall = Height(s) > Width(s);
    if (!f.tall) { // hero column on the left, list on the right
        float hx = s.left + kHero / 2;
        f.badge = {hx, s.top + 84};
        f.title = Rect(s.left + 16, s.top + 134, kHero - 32, 28);
        f.sub = Rect(s.left + 16, s.top + 162, kHero - 32, 18);
        f.sw = {hx, s.top + 204};
        f.pill = Rect(s.left + 28, s.bottom - 62, kHero - 56, 38);
        f.list = {s.left + kHero + 18, s.top + 16, s.right - 16, s.bottom - 14};
    } else { // header row: badge, title, switch, then the action pill, list below
        f.badge = {s.left + 46, s.top + 48};
        f.title = Rect(s.left + 92, s.top + 26, Width(s) - 170, 26);
        f.sub = Rect(s.left + 92, s.top + 52, Width(s) - 170, 18);
        f.sw = {s.right - 40, s.top + 48};
        f.pill = Rect(s.left + 16, s.top + 98, Width(s) - 32, 36);
        f.list = {s.left + 10, s.top + 146, s.right - 10, s.bottom - 10};
        f.align = Align::Left;
        f.titleSize = 19;
    }
    f.layered = k < 0.999f && SUCCEEDED(c.Target()->QueryInterface(IID_PPV_ARGS(&f.dc))); // layer only while animating
    c.Target()->GetTransform(&f.base);
    c.Target()->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - k) * 16) * *D2D1::Matrix3x2F::ReinterpretBaseType(&f.base)); // rise in
    if (f.layered) f.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), k), nullptr);
    c.Fill(Inset(s, -10, -10), 30, t.veil.Alpha(0.5f)); // dim what's behind
    c.Fill(s, 24, t.card); // sheet body
    if (split) {
        c.PushClip(s);
        c.Glow(f.badge, 200, 160, t.accent.Alpha(0.09f)); // tint around the hero badge
        c.PopClip();
    }
    c.FillV({s.left, s.top, s.right, s.top + 80}, 24, t.sheen, t.sheen.Alpha(0)); // top sheen
    c.StrokeV(s, 24, t.hairTop, t.hairBot, 1);
    if (!split) return f; // the page lays out the whole sheet itself
    if (f.tall) c.Line({s.left + 22, f.list.top - 6}, {s.right - 22, f.list.top - 6}, t.line, 1, false); // header / list divider
    else c.Line({s.left + kHero, s.top + 22}, {s.left + kHero, s.bottom - 22}, t.line, 1, false);
    return f;
}

void End(PageCtx& p, const Frame& f) {
    if (f.layered) f.dc->PopLayer();
    p.ui.C().Target()->SetTransform(f.base);
}

int Key(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t ch : s) h = (h ^ ch) * 16777619u; // fnv-1a
    return static_cast<int>(h & 0x7fffffff);
}

void Spinner(Canvas& c, D2D1_POINT_2F o, float r, Color col, double now) {
    float a = static_cast<float>(std::fmod(now * 5.5, 2 * kPi)); // ~0.9 turns/s
    c.Arc(o, r, a, kPi * 1.3f, col, 2);
}

void Radar(Ui& ui, const Theme& t, const char* id, D2D1_POINT_2F o, bool on, bool scanning, int glyphOn, int glyphOff, int glyphScan) {
    Canvas& c = ui.C();
    float scan = ui.Smooth(Ui::Id(id, 0), scanning ? 1.0f : 0, 6);
    float lit = ui.Smooth(Ui::Id(id, 1), on ? 1.0f : 0, 10);
    c.Glow(o, 96, 80, t.accent.Alpha(0.05f + 0.13f * lit)); // soft halo behind the badge
    if (scan > 0.01f) {
        float ph = static_cast<float>(std::fmod(ui.Now() * 0.55, 1.0)); // ring cycle ~1.8s
        for (int i = 0; i < 3; ++i) {
            float f = std::fmod(ph + i / 3.0f, 1.0f); // 3 staggered rings expand and fade
            c.Ring(o, 34 + f * 22, t.accent.Alpha((1 - f) * 0.5f * scan), 1.5f); // stays clear of the title
        }
        ui.KeepAlive(); // ambient rate is plenty for rings
    }
    c.Dot(o, 32, t.track.Mix(t.accent.Alpha(0.24f), lit));
    c.Ring(o, 32, t.hairTop.Mix(t.accentHi.Alpha(0.45f), lit), 1);
    int g = scanning ? glyphScan : on ? glyphOn : glyphOff;
    c.Icon(g, o, 30, t.textDim.Mix(t.accentHi, lit), on); // filled glyph when on
}

bool ScanBtn(Ui& ui, const Theme& t, const char* idName, const D2D1_RECT_F& r, bool scanning, bool enabled, int icon, const wchar_t* label,
             const wchar_t* stopLabel) {
    uint32_t id = Ui::Id(idName);
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 16);
    float press = ui.Smooth(id + 1, h.down ? 1.0f : 0, 26);
    float sc = ui.Smooth(id + 2, scanning ? 1.0f : 0, 12); // morphs primary pill -> ghost pill
    float a = enabled ? 1.0f : 0.4f;
    Canvas& c = ui.C();
    D2D1_RECT_F b = Inset(r, 1.5f * press, 1.5f * press); // press shrink
    float rad = Height(b) / 2;
    if (sc < 0.999f) {
        float k = (1 - sc) * a;
        c.Fill(Inset(b, -4, -4), rad + 4, t.accent.Alpha((0.10f + 0.14f * hover) * k)); // halo grows on hover
        c.FillH(b, rad, t.accent.Alpha(k), t.accentHi.Mix(t.accent, 0.4f * (1 - hover)).Alpha(k));
    }
    if (sc > 0.001f) {
        c.Fill(b, rad, t.cardHi.Mix(t.track, 0.5f * hover).Alpha(sc * a));
        c.Stroke(b, rad, t.hairTop.Mix(t.accent.Alpha(0.5f), 0.5f).Alpha(sc), 1);
    }
    Color ink = t.onAccent.Mix(t.text, sc).Alpha(a); // dark on accent, light on ghost
    const wchar_t* text = scanning ? stopLabel : label;
    float est = std::wcslen(text) * 7.0f; // rough text width, keeps icon+label centered
    float x0 = (b.left + b.right) / 2 - (est + 26) / 2, cy = (b.top + b.bottom) / 2;
    if (scanning) Spinner(c, {x0 + 8, cy}, 7, ink, ui.Now());
    else c.Icon(icon, {x0 + 8, cy}, 20, ink);
    c.Text(text, Rect(x0 + 24, b.top, est + 12, Height(b)), Weight::Medium, 13, ink);
    if (scanning) ui.KeepAlive();
    return h.clicked;
}

bool Act(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool busy, bool live) {
    Ui::Hit h = live && !busy ? ui.Button(id, r) : Ui::Hit{}; // no clicks while pending
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 16);
    float pr = ui.Smooth(id + 1, primary ? 1.0f : 0, 12); // filled accent vs quiet outline
    Canvas& c = ui.C();
    float rad = Height(r) / 2;
    Color base = t.track.Mix(t.accent, pr);
    Color hi = t.track.Mix(t.text, 0.12f).Mix(t.accentHi, pr);
    c.Fill(r, rad, base.Mix(hi, 0.6f * hover)); // brighten on hover
    if (pr < 0.99f) c.Stroke(r, rad, t.hairTop.Alpha(1 - pr), 1);
    Color ink = t.text.Mix(t.onAccent, pr);
    if (busy) Spinner(c, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 7, ink, ui.Now());
    else c.Text(label, r, Weight::Medium, 12, ink, Align::Center);
    return h.clicked;
}

void Head(Ui& ui, const Theme& t, const D2D1_RECT_F& r, const wchar_t* label, bool spin) {
    ui.C().TextSp(label, {r.left + 4, r.top + 8, r.left + 160, r.top + 22}, Weight::Medium, 10, t.textLow, 1.4f);
    if (spin) Spinner(ui.C(), {r.left + 14 + std::wcslen(label) * 8.0f, r.top + 15}, 5, t.accent, ui.Now());
}

void Empty(Canvas& c, const Theme& t, const D2D1_RECT_F& v, int g, const wchar_t* title, const wchar_t* sub) {
    float cx = (v.left + v.right) / 2, cy = (v.top + v.bottom) / 2 - 10;
    c.Icon(g, {cx, cy - 26}, 34, t.textLow);
    c.Text(title, {v.left, cy - 2, v.right, cy + 20}, Weight::Medium, 14, t.text, Align::Center);
    c.Text(sub, {v.left, cy + 22, v.right, cy + 40}, Weight::Light, 12, t.textDim, Align::Center);
}

void Bars(Canvas& c, const Theme& t, float x, float bottom, int quality, Color lit) {
    for (int i = 0; i < 4; ++i) {
        float h = 6.0f + i * 4; // bars step up 4px each
        bool on = quality >= 25 * i + 10; // thresholds 10/35/60/85%
        c.Fill(Rect(x + i * 7, bottom - h, 4, h), 2, on ? lit : t.track); // 7px pitch, 4px wide
    }
}

float Scroll(Ui& ui, const char* id, float& target, const D2D1_RECT_F& view, float content, bool wheel) {
    float maxS = (std::max)(0.0f, content - Height(view));
    target = (std::clamp)(target - (wheel ? ui.Wheel(view) : 0.0f) * 60, 0.0f, maxS); // 60 dips per notch
    return ui.Smooth(Ui::Id(id), target, 18);
}

void Fades(Canvas& c, const Theme& t, const D2D1_RECT_F& view, float scroll, float content) {
    float maxS = content - Height(view);
    if (scroll > 1) c.FillV({view.left, view.top, view.right, view.top + 18}, 0, t.card, t.card.Alpha(0));
    if (scroll < maxS - 1) c.FillV({view.left, view.bottom - 18, view.right, view.bottom}, 0, t.card.Alpha(0), t.card);
}

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title) { // a settings card, returns its inner area
    c.Fill(r, 16, t.cardHi.Alpha(0.55f));
    c.StrokeV(r, 16, t.hairTop, t.hairBot, 1);
    c.TextSp(title, Rect(r.left + 14, r.top + 11, Width(r) - 28, 12), Weight::Medium, 9.5f, t.textLow, 1.4f);
    return {r.left + 12, r.top + 30, r.right - 12, r.bottom - 10};
}

int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur, bool live) {
    Canvas& c = ui.C();
    float seg = Width(r) / n, rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    float x = ui.Smooth(Ui::Id(id, 99), r.left + seg * cur, 18); // thumb slides between options
    c.FillV({x + 3, r.top + 3, x + seg - 3, r.bottom - 3}, rad - 3, t.accentHi, t.accent);
    int picked = -1;
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F cell = {r.left + seg * i, r.top, r.left + seg * (i + 1), r.bottom};
        Ui::Hit h = live ? ui.Button(Ui::Id(id, i), cell) : Ui::Hit{};
        if (h.clicked && i != cur) picked = i;
        c.Text(labels[i], cell, Weight::Medium, 11.5f, i == cur ? t.onAccent : t.textDim.Mix(t.text, h.hover ? 0.6f : 0), Align::Center);
    }
    return picked;
}

namespace {

constexpr float kSlideLabelH = 16; // the label and value line over the track
constexpr float kThumbR = 8, kThumbGrow = 2.5f; // the thumb at rest, and how much it swells under the mouse
constexpr float kTrackH = 6, kTrackGrow = 2; // the same for the track
constexpr float kBubbleH = 22; // the value riding over the thumb while dragging
constexpr int kSliderCap = 24; // more sliders than settings has
uint32_t sliderIds[kSliderCap] = {}; // every slider seen, so the page can leave the wheel to the one under the mouse
int sliderCount = 0;

void Seen(uint32_t id) {
    for (int i = 0; i < sliderCount; ++i)
        if (sliderIds[i] == id) return;
    if (sliderCount < kSliderCap) sliderIds[sliderCount++] = id;
}

}

bool OverSlider(const Ui& ui) {
    uint32_t h = ui.Hovered();
    for (int i = 0; h && i < sliderCount; ++i)
        if (sliderIds[i] == h) return true;
    return false;
}

bool Knob(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value, float& v, bool live,
          bool& released, float step) { // label and value over a roomy track; true when v changed
    Canvas& c = ui.C();
    const Input& in = ui.In();
    Seen(id);
    D2D1_RECT_F grab = {r.left, r.top + kSlideLabelH - 2, r.right, r.bottom + 4}; // the whole lower band takes a press, not just the line
    float pad = kThumbR + kThumbGrow + 1, cy = (grab.top + r.bottom) / 2 + 1;
    float x0 = r.left + pad, x1 = r.right - pad; // the thumb stays inside the row
    Ui::Hit h = live ? ui.Button(id, grab) : Ui::Hit{};
    bool dragging = ui.IsActive(id) && (in.down || in.released);
    bool changed = false;
    if (dragging && x1 > x0) { // a press jumps there, a drag follows
        float f = anim::Clamp01((in.mouse.x - x0) / (x1 - x0));
        if (std::fabs(f - v) >= 0.0005f) v = f, changed = true;
    }
    released = ui.IsActive(id) && in.released;
    if (live && !ui.Captured()) {
        if (float notches = ui.Wheel(grab)) { // a notch is a step, saved at once
            float was = v;
            v = anim::Clamp01(v + notches * step);
            changed = released = v != was;
        }
    }
    bool held = ui.IsActive(id) && in.down;
    float hot = ui.Smooth(id + 1, held || h.hover ? 1.0f : 0, 16);
    float drag = ui.Smooth(id + 2, held ? 1.0f : 0, 18);
    float shown = ui.Smooth(id + 3, anim::Clamp01(v), 13 + 19 * drag); // a jump glides there, then a drag stays under the finger
    c.Text(label, {r.left, r.top, r.right - 70, r.top + kSlideLabelH}, Weight::Regular, 11.5f, t.textDim.Mix(t.text, 0.5f * hot));
    c.Text(value, {r.right - 90, r.top, r.right, r.top + kSlideLabelH}, Weight::Semi, 11.5f, t.textDim.Mix(t.accentHi, 0.4f + 0.6f * hot), Align::Right);
    float th = kTrackH + kTrackGrow * hot, x = x0 + (x1 - x0) * shown;
    c.Fill({r.left, cy - th / 2, r.right, cy + th / 2}, th / 2, t.track);
    c.Fill({r.left, cy - th / 2, (std::max)(x, r.left + th), cy + th / 2}, th / 2, t.accent); // solid, the song's or the shell's accent
    float tr = kThumbR + kThumbGrow * hot + drag;
    if (hot > 0.01f) c.Dot({x, cy}, tr + 6 * hot, t.accent.Alpha(0.16f * hot)); // a soft halo under the mouse
    c.Dot({x, cy}, tr, t.text);
    c.Ring({x, cy}, tr - 1, t.accent, 2.5f);
    if (drag > 0.01f) { // the value rides over the thumb while it moves
        float bw = (std::max)(kBubbleH * 1.6f, c.Measure(value, Weight::Semi, 11.5f) + 18), lift = 6 * drag;
        D2D1_RECT_F b = Rect(x - bw / 2, cy - tr - kBubbleH - lift, bw, kBubbleH);
        c.Fill(b, kBubbleH / 2, t.accent.Alpha(drag));
        c.Text(value, b, Weight::Semi, 11.5f, t.onAccent.Alpha(drag), Align::Center);
    }
    return changed;
}

namespace {

constexpr float kBadgeR = 8; // the x and + marks on a cell's corner
constexpr float kDragStart = 6; // dips moved before a press is a drag
constexpr float kStackH = 44; // cells at least this tall stack the icon over the name
struct Carry {
    int item = -1; // what's being dragged, -1 nothing
    D2D1_POINT_2F grab{}; // where on it the mouse holds it
};
std::unordered_map<uint32_t, Carry> carries; // one per editor, a few at most

void Badge(Canvas& c, const Theme& t, D2D1_POINT_2F o, bool add, float hover) {
    if (add) c.Dot(o, kBadgeR, t.accent.Mix(t.accentHi, hover));
    else c.Dot(o, kBadgeR, hover > 0.5f ? kDanger : t.textLow.Mix(t.textDim, 0.5f));
    c.Icon(add ? glyph::Add : glyph::Close, o, 12, add ? t.onAccent : Color{1, 1, 1, 1});
}

void Cell(Canvas& c, const Theme& t, const D2D1_RECT_F& q, const Arrange& a, int item, bool ghost, float hover, float lit, float lift) {
    float rad = (std::min)(Height(q) / 2, 14.0f);
    if (lift > 0.01f) c.Fill(Inset(q, -2, -2), rad + 2, Color{0, 0, 0, 0.22f * lift}); // carried: it floats over the others
    if (ghost) { // could be added: an outline waiting to be filled
        c.Fill(q, rad, t.track.Alpha(0.20f + 0.30f * hover));
        c.Stroke(q, rad, t.accent.Alpha(0.30f + 0.40f * hover), 1);
    } else {
        c.Fill(q, rad, t.cardHi.Mix(t.track, 0.5f * hover));
        if (lit > 0.01f) c.Fill(q, rad, t.accent.Alpha(0.22f * lit));
        c.StrokeV(q, rad, t.hairTop.Mix(t.accentHi.Alpha(0.75f), lit), t.hairBot, 1);
    }
    bool stack = Height(q) >= kStackH;
    float size = stack ? 11 : 11.5f, room = stack ? Width(q) - 10 : Width(q) - 32 - kBadgeR - 4;
    const wchar_t* label = a.name(item);
    if (a.shortName && c.Measure(label, Weight::Medium, size) > room) label = a.shortName(item); // narrow: the short word, never a cut one
    Color ink = ghost ? t.textDim.Mix(t.text, hover) : t.text;
    Color iconInk = lit > 0.5f ? t.accentHi : ink;
    if (stack) {
        c.Icon(a.glyph(item), {(q.left + q.right) / 2, q.top + Height(q) * 0.38f}, 19, iconInk, lit > 0.5f);
        c.Text(label, Rect(q.left + 5, q.top + Height(q) * 0.62f - 8, Width(q) - 10, 16), Weight::Medium, size, ink, Align::Center);
    } else {
        c.Icon(a.glyph(item), {q.left + 18, (q.top + q.bottom) / 2}, 17, iconInk, lit > 0.5f);
        c.Text(label, {q.left + 32, q.top, q.left + 32 + room, q.bottom}, Weight::Medium, size, ink);
    }
}

}

bool Arranger(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, int cols, float cellH, Arrange& a, bool live) {
    Canvas& c = ui.C();
    const Input& in = ui.In();
    Carry& carry = carries[Ui::Id(id)];
    cols = (std::max)(1, cols);
    float cw = (Width(r) - kArrangeGap * (cols - 1)) / cols;
    auto cell = [&](int k) { return Rect(r.left + (k % cols) * (cw + kArrangeGap), r.top + (k / cols) * (cellH + kArrangeGap), cw, cellH); };
    int n = (int)a.shown.size();
    Ids order = a.shown; // this frame's order: a carried item sits where it would land
    auto held = std::find(order.begin(), order.end(), carry.item);
    if (held == order.end() || !live) carry.item = -1;
    if (carry.item >= 0) {
        order.erase(held);
        int best = 0;
        float bestD = 1e12f;
        for (int k = 0; k < n; ++k) { // the nearest of the shown cells
            D2D1_RECT_F q = cell(k);
            float dx = in.mouse.x - (q.left + q.right) / 2, dy = in.mouse.y - (q.top + q.bottom) / 2;
            if (dx * dx + dy * dy < bestD) bestD = dx * dx + dy * dy, best = k;
        }
        order.insert(order.begin() + best, carry.item);
    }
    bool changed = false, canDrop = n > (int)a.keep;
    int removeItem = -1, addItem = -1, top = -1;
    D2D1_RECT_F topRect{};
    for (int k = 0; k < n; ++k) {
        int item = order[(size_t)k];
        D2D1_RECT_F home = cell(k);
        bool carried = item == carry.item;
        D2D1_RECT_F q = carried ? Rect(in.mouse.x - carry.grab.x, in.mouse.y - carry.grab.y, cw, cellH) : home; // cells sit still: an eased place lagged the page's scroll
        D2D1_POINT_2F bo = {q.right - 3, q.top + 3};
        D2D1_RECT_F br = {bo.x - kBadgeR - 2, bo.y - kBadgeR - 2, bo.x + kBadgeR + 2, bo.y + kBadgeR + 2};
        Ui::Hit xh = live && canDrop && carry.item < 0 ? ui.Button(Ui::Id(id, 200 + item), br) : Ui::Hit{}; // before the cell's: the first button under a press takes it
        uint32_t cid = Ui::Id(id, 100 + item);
        Ui::Hit h = live ? ui.Button(cid, carried ? q : home) : Ui::Hit{};
        if (live && ui.IsActive(cid) && in.down && carry.item < 0 &&
            std::fabs(in.mouse.x - in.pressPos.x) + std::fabs(in.mouse.y - in.pressPos.y) > kDragStart) { // pulled far enough: it's carried
            carry.item = item;
            carry.grab = {in.pressPos.x - home.left, in.pressPos.y - home.top};
        }
        if (xh.clicked) removeItem = item;
        else if (h.clicked && !carried) a.tapped = item;
        if (carried) {
            top = item, topRect = q;
            continue; // drawn last, over the rest
        }
        float hover = ui.Smooth(Ui::Id(id, 600 + item), h.hover ? 1.0f : 0, 16);
        float lit = ui.Smooth(Ui::Id(id, 700 + item), item == a.lit ? a.litMix : 0, 14);
        Cell(c, t, q, a, item, false, hover, lit, 0);
        if (canDrop) Badge(c, t, bo, false, xh.hover ? 1.0f : 0);
    }
    for (size_t j = 0; j < a.hidden.size(); ++j) { // the ones that could come back, after the rest
        int item = a.hidden[j];
        D2D1_RECT_F q = cell(n + (int)j);
        Ui::Hit h = live && carry.item < 0 ? ui.Button(Ui::Id(id, 300 + item), q) : Ui::Hit{};
        float hover = ui.Smooth(Ui::Id(id, 600 + item), h.hover ? 1.0f : 0, 16);
        Cell(c, t, q, a, item, true, hover, 0, 0);
        Badge(c, t, {q.right - 3, q.top + 3}, true, hover);
        if (h.clicked) addItem = item;
    }
    if (top >= 0) {
        ui.Wake(); // it follows the mouse
        Cell(c, t, topRect, a, top, false, 1, top == a.lit ? a.litMix : 0, 1);
    }
    if (carry.item >= 0 && !in.down) { // let go: the order it showed is kept
        if (order != a.shown) a.shown = order, changed = true;
        carry.item = -1;
    }
    if (removeItem >= 0) {
        a.shown.erase(std::remove(a.shown.begin(), a.shown.end(), removeItem), a.shown.end());
        changed = true;
    }
    if (addItem >= 0) {
        a.shown.push_back(addItem);
        changed = true;
    }
    return changed;
}

const wchar_t* TileShort(int id) {
    static const wchar_t* const kShort[Model::kToolTiles] = {L"Snip", L"Color", L"Awake", L"Alerts", L"Quiet", L"Saver", L"Airplane", L"Settings"};
    return kShort[(std::clamp)(id, 0, Model::kToolTiles - 1)];
}

Arrange TileSet(const Model& m) {
    Arrange a;
    for (int id : m.tiles)
        if (m.TileHere(id)) a.shown.push_back(id);
    for (int i = 0; i < Model::kToolTiles; ++i)
        if (m.TileHere(i) && std::find(m.tiles.begin(), m.tiles.end(), i) == m.tiles.end()) a.hidden.push_back(i);
    a.glyph = Model::TileGlyph;
    a.name = Model::TileName;
    a.shortName = TileShort;
    return a;
}

void KeepTiles(Model& m, const Ids& set) {
    std::vector<int> shown = set.Vec();
    for (int id : m.tiles)
        if (!m.TileHere(id)) shown.push_back(id); // a laptop's tile stays saved on a desktop
    m.SetTiles(shown);
}

}
