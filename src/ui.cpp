#include "ui.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"
#include "chrome.h"
#include "starfall.h"

namespace {
constexpr float kPi = 3.14159265f;
}

void Ui::Begin(Canvas& c, const Input& input, float frameDt, double time) {
    canvas = &c;
    in = input;
    dt = frameDt;
    now = time;
    animating = false; // widgets set these again during the frame
    ambient = false;
    hand = false;
    claimed = false;
    c.Target()->GetTransform(&frameXf); // base transform, hit rects stored relative to it
}

void Ui::End() {
    if (in.released) active = 0; // release ends capture
    hits.swap(nextHits); // double buffer: hit test uses last frame's rects
    nextHits.clear();
    lastHover = HitAt(in.mouse);
    fire = stepId = setId = 0; // keyboard presses last one frame
    stepDir = 0;
    if (focus && std::none_of(hits.begin(), hits.end(), [this](const Item& h) { return h.id == focus && h.stop; })) reveal = false; // gone with its page or sheet
}

void Ui::Tab(int dir) {
    std::vector<const Item*> stops;
    for (const Item& h : hits)
        if (h.stop) stops.push_back(&h);
    if (stops.empty()) return;
    int at = -1;
    for (int i = 0; i < (int)stops.size(); ++i)
        if (stops[i]->id == focus) at = i;
    int n = (int)stops.size();
    at = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n; // lost its place: from the ends, and round again
    focus = stops[at]->id;
    reveal = true;
}

bool Ui::SliderFocused() const {
    return std::any_of(hits.begin(), hits.end(), [this](const Item& h) { return h.id == focus && h.role == Role::Slider; });
}

bool Ui::KeyMove(uint32_t id, float& value, float step) {
    bool moved = false;
    if (id == setId) value = anim::Clamp01(setTo), moved = true;
    if (id == stepId && stepDir) {
        float was = value;
        value = anim::Clamp01(value + stepDir * step);
        moved = value != was;
    }
    if (moved) animating = true;
    return moved;
}

void Ui::Mark(Role role, bool on, float value) {
    if (nextHits.empty()) return;
    Item& it = nextHits.back();
    it.role = role;
    it.on = on;
    it.value = value;
}

void Ui::Name(const std::wstring& name, const std::wstring& valueText) {
    if (!listening || nextHits.empty()) return;
    nextHits.back().name = name;
    nextHits.back().valueText = valueText;
}

void Ui::NoStop() {
    if (!nextHits.empty()) nextHits.back().stop = false;
}

void Ui::DrawFocus(Color ring, Color edge) {
    if (!focus) return;
    for (const Item& h : nextHits) {
        if (h.id != focus || !h.stop) continue;
        D2D1_MATRIX_3X2_F was;
        canvas->Target()->GetTransform(&was);
        canvas->Target()->SetTransform(frameXf);
        D2D1_RECT_F r = Inset(h.r, -3, -3);
        float rad = (std::min)((std::min)(Width(r), Height(r)) / 2, 12.0f);
        canvas->Stroke(r, rad, edge, 4.5f); // two tones: it shows over any color
        canvas->Stroke(r, rad, ring, 2.5f);
        canvas->Target()->SetTransform(was);
        return;
    }
}

void Ui::Reveal(const D2D1_RECT_F& view, float shown, float& target) {
    if (!reveal || !focus) return;
    D2D1::Matrix3x2F m, inv = *D2D1::Matrix3x2F::ReinterpretBaseType(&frameXf);
    canvas->Target()->GetTransform(&m);
    if (!inv.Invert()) return;
    D2D1::Matrix3x2F toFrame = m * inv;
    D2D1_POINT_2F a = toFrame.TransformPoint({view.left, view.top}), b = toFrame.TransformPoint({view.right, view.bottom});
    for (const Item& h : hits) {
        if (h.id != focus) continue;
        float mid = (h.r.left + h.r.right) / 2;
        if (mid < a.x || mid > b.x) return; // not in this list: the rail, say
        constexpr float kAir = 14; // a little room around it
        if (h.r.top < a.y + kAir) target = shown - (a.y + kAir - h.r.top);
        else if (h.r.bottom > b.y - kAir) target = shown + (h.r.bottom - b.y + kAir);
        else reveal = false; // in view
        return;
    }
}

void Ui::AddHit(uint32_t id, const D2D1_RECT_F& r) {
    D2D1::Matrix3x2F m, inv = *D2D1::Matrix3x2F::ReinterpretBaseType(&frameXf);
    canvas->Target()->GetTransform(&m); // current transform may include scroll/slide offsets
    if (!inv.Invert()) inv = D2D1::Matrix3x2F::Identity();
    D2D1::Matrix3x2F toLocal = m * inv; // map hit rect back to frame space
    D2D1_POINT_2F a = toLocal.TransformPoint({r.left, r.top});
    D2D1_POINT_2F b = toLocal.TransformPoint({r.right, r.bottom}); // corners only, assumes no rotation
    Item item;
    item.id = id;
    item.r = {a.x, a.y, b.x, b.y};
    nextHits.push_back(std::move(item));
}

uint32_t Ui::HitAt(D2D1_POINT_2F p) const {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) // reverse: last drawn is on top
        if (Inside(it->r, p)) return it->id;
    return 0;
}

uint32_t Ui::Id(const char* s, int index) {
    uint32_t h = 2166136261u; // fnv-1a 32 bit
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return (h ^ (uint32_t)index) * 16777619u; // fold index in for repeated widgets
}

float Ui::Smooth(uint32_t id, float target, float rate) {
    auto it = values.find(id);
    if (it == values.end()) {
        values[id] = target; // first sight snaps, no intro animation
        return target;
    }
    if (anim::Approach(it->second, target, rate, dt)) animating = true; // still moving, request another frame
    return it->second;
}

float Ui::Soft(uint32_t id, float target) {
    auto it = values.find(id);
    if (it == values.end()) it = values.emplace(id, 0.0f).first; // starts at 0, eases in
    if (anim::Approach(it->second, target, 9, dt, 0.003f)) ambient = true; // ambient = low priority redraw
    return it->second;
}

float Ui::Env(uint32_t id, float target) {
    auto it = values.find(id);
    if (it == values.end()) it = values.emplace(id, 0.0f).first;
    anim::Approach(it->second, target, target > it->second ? 26.0f : 4.0f, dt, 0.002f); // envelope: fast attack, slow release
    return it->second;
}

float Ui::SmoothFrom(uint32_t id, float target, float initial, float rate) {
    if (values.find(id) == values.end()) values[id] = initial; // seed so first frame animates from initial
    return Smooth(id, target, rate);
}

Ui::Hit Ui::Button(uint32_t id, const D2D1_RECT_F& r) {
    if (blocked) return {}; // covered by overlay
    AddHit(id, r);
    Hit h;
    h.hover = Inside(r, in.mouse) && (active == 0 || active == id); // no hover while another widget holds capture
    if (in.pressed && Inside(r, in.pressPos) && active == 0) { // press must start inside to capture
        active = id;
        claimed = true; // tells drawer the press was consumed
    }
    h.down = active == id && in.down;
    if (active == id && in.released) h.clicked = Inside(r, in.mouse); // click only if released inside
    if (h.hover) hand = true; // hand cursor
    if (id == fire) h.clicked = true, animating = true; // the keyboard or a reader pressed it: what it changed shows next frame
    if (id == focus && keyHeld) h.down = h.hover = true;
    return h;
}

bool Ui::Slider(uint32_t id, const D2D1_RECT_F& r, float& value, bool vertical) {
    Hit h = Button(id, r);
    Mark(Role::Slider, false, value);
    constexpr float kKeyStep = 0.05f; // 5% a press, as a wheel notch
    if (KeyMove(id, value, kKeyStep)) return true;
    if (active != id || !(in.down || in.released)) return false; // only while dragging
    (void)h;
    float f = vertical ? 1 - (in.mouse.y - r.top) / Height(r) : (in.mouse.x - r.left) / Width(r); // vertical: bottom = 0
    f = anim::Clamp01(f);
    if (std::fabs(f - value) < 0.0005f) return false; // ignore jitter, avoid spamming setters
    value = f;
    return true;
}

float Ui::Wheel(const D2D1_RECT_F& r) {
    if (blocked || in.wheel == 0 || !Inside(r, in.mouse)) return 0;
    float notches = in.wheel;
    in.wheel = 0; // consume so only one widget scrolls
    return notches;
}

bool Ui::Hold(uint32_t id, const D2D1_RECT_F& r, float seconds, float& progress) {
    Hit h = Button(id, r);
    progress = 0;
    if (!h.down || !h.hover) {
        holdT.erase(id); // released or slid off, reset
        return false;
    }
    auto it = holdT.find(id);
    if (it == holdT.end()) it = holdT.emplace(id, now).first; // remember press start time
    if (it->second < 0) { // -1 = already fired this press
        progress = 1;
        return false;
    }
    progress = anim::Clamp01((float)((now - it->second) / seconds));
    animating = true; // keep frames coming while holding
    if (progress < 1) return false;
    it->second = -1; // fire once per press
    return true;
}

void Ui::Forget() {
    values.clear();
    holdT.clear();
    active = 0;
}

namespace w {

void Card(Ui& ui, const Theme& t, const D2D1_RECT_F& r, float radius) {
    Canvas& c = ui.C();
    c.Fill(r, radius, t.card);
    c.FillV(D2D1_RECT_F{r.left, r.top, r.right, r.top + (std::min)(Height(r), 70.0f)}, radius, // top sheen, max 70px tall
                   t.sheen, t.sheen.Alpha(0));
    c.StrokeV(r, radius, t.hairTop, t.hairBot, 1); // hairline brighter on top, fake bevel
    if (t.stars) starfall::Seam(c, {r.left + radius, r.top + 0.5f}, {r.right - radius, r.top + 0.5f}, t, 0.8f); // starfall: a faint line of light along the top edge
    else if (t.chrome) chrome::Seam(c, {r.left + radius, r.top + 0.5f}, {r.right - radius, r.top + 0.5f}, 1.2f); // chrome: the bevel catches the light
}

bool IconBtn(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float size, int glyph, float iconPx,
                bool enabled, bool lit) {
    D2D1_RECT_F r = {c.x - size / 2, c.y - size / 2, c.x + size / 2, c.y + size / 2};
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{}; // disabled: no hit registered
    float hover = ui.Smooth(id, h.down ? 1.0f : (h.hover ? 0.6f : 0), 18);
    if (hover > 0.001f) ui.C().Dot(c, size / 2, t.text.Alpha(0.09f * hover));
    float press = ui.Smooth(id + 1, h.down ? 0.88f : 1.0f, 26); // id+1: separate anim slot, shrink on press
    Color ink = !enabled ? t.textLow.Alpha(0.6f) : (lit ? t.accent : t.text);
    ui.C().Icon(glyph, c, iconPx * press, ink, lit); // lit uses filled glyph
    return h.clicked;
}

bool PlayBtn(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float size, bool playing, bool enabled) {
    D2D1_RECT_F r = {c.x - size / 2, c.y - size / 2, c.x + size / 2, c.y + size / 2};
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 14);
    float press = ui.Smooth(id + 1, h.down ? 0.9f : 1.0f, 26);
    float mode = ui.Smooth(id + 2, playing ? 1.0f : 0, 14); // 0 = play icon, 1 = pause icon
    Canvas& cv = ui.C();
    float rad = size / 2 * press;
    if (enabled) cv.Dot(c, rad + 5 + 3 * hover, t.glow.Alpha(0.5f + 0.5f * hover)); // halo grows on hover
    cv.Dot(c, rad, enabled ? t.accent.Mix(t.accentHi, hover * 0.5f) : t.track);
    Color ink = enabled ? t.onAccent : t.textLow;
    float s = size * 0.5f * press;
    if (mode < 0.999f) cv.Icon(glyph::Play, c, s * (1 - 0.3f * mode), ink.Alpha(1 - mode), true); // crossfade + scale between icons
    if (mode > 0.001f) cv.Icon(glyph::Pause, c, s * (0.7f + 0.3f * mode), ink.Alpha(mode), true);
    return h.clicked;
}

bool Tile(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label,
          const std::wstring& detail, bool on, bool enabled) {
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    float lit = ui.Smooth(id, on ? 1.0f : 0, 12);
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    float press = ui.Smooth(id + 2, h.down ? 1.0f : 0, 26);
    Canvas& c = ui.C();
    D2D1_RECT_F body = Inset(r, 1.5f * press, 1.5f * press); // sink slightly when pressed
    if (lit > 0.001f) c.Fill(Inset(body, -3, -3), 21, t.accent.Alpha(0.10f * lit)); // outer glow when on
    c.Fill(body, 18, t.cardHi.Mix(t.track, hover * 0.5f));
    if (lit > 0.001f) c.FillV(body, 18, t.accent.Alpha(0.32f * lit), t.accent.Alpha(0.12f * lit));
    c.StrokeV(body, 18, t.hairTop.Mix(t.accentHi.Alpha(0.75f), lit), t.hairBot.Mix(t.accent.Alpha(0.3f), lit), 1); // hairline tints toward accent as it lights

    float alpha = enabled ? 1.0f : 0.45f; // dim whole tile when disabled
    Color ink = t.text.Alpha(alpha);
    Color iconInk = t.text.Mix(t.accentHi, lit).Alpha(alpha);
    Color sub = t.textDim.Mix(t.accentHi, lit * 0.6f).Alpha(alpha);
    if (Width(r) < 140) { // narrow tile: stacked layout, icon above text
        c.Icon(glyph, {body.left + 24, body.top + 24}, 21, iconInk, on);
        c.Text(label, Rect(body.left + 14, body.top + 41, Width(body) - 20, 16), Weight::Medium, 12.5f, ink);
        c.Text(detail, Rect(body.left + 14, body.top + 56, Width(body) - 20, 13), Weight::Light, 10.5f, sub);
        return h.clicked;
    }
    D2D1_POINT_2F ic = {body.left + 26, (body.top + body.bottom) / 2}; // wide tile: icon left, two text rows right
    c.Icon(glyph, ic, 21, iconInk, on);
    float tx = body.left + 46, mid = (body.top + body.bottom) / 2; // label and detail stay one block, centered, however tall the tile
    c.Text(label, {tx, mid - 19, body.right - 8, mid + 1}, Weight::Medium, 13, ink);
    c.Text(detail, {tx, mid + 1, body.right - 8, mid + 19}, Weight::Light, 11.5f, sub);
    return h.clicked;
}

bool Pill(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, float& value, bool* iconTap, bool off) {
    float h = Height(r);
    D2D1_RECT_F iconZone = {r.left, r.top, r.left + h, r.bottom}; // square at left, e.g. mute toggle
    Ui::Hit iconHit = iconTap ? ui.Button(id + 7, iconZone) : Ui::Hit{}; // no tap wanted: the icon is just a picture
    if (iconTap) *iconTap = iconHit.clicked;
    D2D1_RECT_F track = {r.left + h + 10, r.top, r.right - 50, r.bottom}; // 50px reserved for percent label
    constexpr float kKnob = 6.5f, kKnobHot = 2.5f; // the knob at rest, and how much it swells
    bool changed = ui.Slider(id, Inset(track, kKnob, 0), value, false); // the mouse moves the knob's middle, which stops short of the ends
    ui.Name(glyph == glyph::Bright ? L"Brightness" : L"Volume", off ? std::wstring(L"Muted") : Pct(value));
    float notches = ui.Wheel(r);
    if (notches != 0) {
        value = anim::Clamp01(value + notches * 0.05f); // 5% per wheel notch
        changed = true;
    }
    float shown = ui.Smooth(id, value, changed ? 40.0f : 14.0f); // follow drag tightly, ease external changes
    bool hovered = Inside(r, ui.In().mouse) || ui.IsActive(id); // stay lit while dragging outside
    float glow = ui.Smooth(id + 3, hovered ? 1.0f : 0, 14);
    float iconHover = ui.Smooth(id + 4, iconHit.hover ? 1.0f : 0, 16);

    Canvas& c = ui.C();
    float cy = (r.top + r.bottom) / 2;
    D2D1_POINT_2F ic = {r.left + h / 2, cy};
    if (iconHover > 0.001f) c.Dot(ic, h / 2, t.text.Alpha(0.08f * iconHover));
    c.Icon(glyph, ic, 20, t.textDim.Mix(t.text, 0.4f + 0.6f * glow));

    float th = 5 + 2 * glow; // track thickens on hover
    c.Fill({track.left, cy - th / 2, track.right, cy + th / 2}, th / 2, t.track);
    float mute = ui.Smooth(id + 5, off ? 1.0f : 0, 14);
    float level = shown * (1 - mute); // muted: the bar empties, the level is kept for when it comes back
    float knob = kKnob + kKnobHot * glow, x = ThumbAt(track.left, track.right, knob, level), end = track.left + Width(track) * level; // the fill runs the whole track as the knob runs its own shorter way
    if (end > track.left + th) { // skip fill when too short to round
        c.Fill({track.left - 2, cy - th / 2 - 3, end + 2, cy + th / 2 + 3}, th / 2 + 3, t.accent.Alpha((0.10f + 0.10f * glow) * (1 - mute)));
        c.FillH({track.left, cy - th / 2, end, cy + th / 2}, th / 2, t.accent.Mix(t.textLow, mute), t.accentHi.Mix(t.textLow, mute)); // muted: the level is kept, greyed
    }
    if (glow > 0.001f) c.Dot({x, cy}, knob + 5, t.accent.Alpha(0.20f * glow));
    c.Dot({x, cy}, knob, t.text);
    c.Text(off ? std::wstring(L"Muted") : Pct(value), {r.right - 44, r.top, r.right, r.bottom}, Weight::Medium, 12, t.textDim.Mix(t.text, glow), Align::Right);
    return changed;
}

void Gauge(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float radius, float thick, float value) {
    float v = ui.Soft(id, anim::Clamp01(value));
    Canvas& cv = ui.C();
    const float start = -kPi / 2; // 12 o'clock
    if (v < 0.002f) { // empty: plain track ring
        cv.Ring(c, radius, t.track, thick);
        return;
    }
    float gap = thick * 2.2f / radius; // gap angle so round caps don't touch
    float sweep = v * 2 * kPi;
    cv.Arc(c, radius, start, sweep, t.accent, thick);
    float rest = 2 * kPi - sweep - 2 * gap; // remaining track with a gap on each side
    if (rest > 0.01f) cv.Arc(c, radius, start + sweep + gap, rest, t.track, thick);
}

namespace {

struct SparkGeo {
    uint64_t key;
    ComPtr<ID2D1Geometry> area, stroke;
    bool cut = false; // clipped: stroke is the line's widened outline cut to the card, filled rather than stroked
};
std::vector<SparkGeo> sparkCache; // small fifo, geometry rebuild is costly
constexpr float kSparkLine = 1.6f;

ComPtr<ID2D1Geometry> Cut(ID2D1Geometry* g, ID2D1Geometry* mask, float tol) { // g inside mask, as one path: built once per new sample, not masked through a layer every frame
    ComPtr<ID2D1PathGeometry> out;
    ComPtr<ID2D1GeometrySink> s;
    if (!g || !mask || FAILED(gfx::D2D()->CreatePathGeometry(&out)) || FAILED(out->Open(&s))) return nullptr;
    if (FAILED(g->CombineWithGeometry(mask, D2D1_COMBINE_MODE_INTERSECT, nullptr, tol, s.Get())) || FAILED(s->Close())) return nullptr;
    return out;
}

bool Clip(Canvas& c, SparkGeo& g, const D2D1_RECT_F& r, const D2D1_ROUNDED_RECT& in) { // the card's rounded corners and the chart's box, as the layer they replace did
    float dx = 96, dy = 96;
    c.Target()->GetDpi(&dx, &dy);
    float tol = D2D1::ComputeFlatteningTolerance(D2D1::Matrix3x2F::Identity(), dx, dy); // what a plain draw flattens to at this dpi
    ComPtr<ID2D1RoundedRectangleGeometry> card;
    ComPtr<ID2D1RectangleGeometry> box;
    if (FAILED(gfx::D2D()->CreateRoundedRectangleGeometry(in, &card)) || FAILED(gfx::D2D()->CreateRectangleGeometry(r, &box))) return false;
    ComPtr<ID2D1Geometry> mask = Cut(card.Get(), box.Get(), tol);
    ComPtr<ID2D1PathGeometry> wide;
    ComPtr<ID2D1GeometrySink> s;
    if (!mask || FAILED(gfx::D2D()->CreatePathGeometry(&wide)) || FAILED(wide->Open(&s))) return false;
    if (FAILED(g.stroke->Widen(kSparkLine, c.RoundCaps(), nullptr, tol, s.Get())) || FAILED(s->Close())) return false;
    ComPtr<ID2D1Geometry> area = Cut(g.area.Get(), mask.Get(), tol), line = Cut(wide.Get(), mask.Get(), tol);
    if (!area || !line) return false;
    g.area = area;
    g.stroke = line;
    g.cut = true;
    return true;
}

}

void Spark(Canvas& c, const D2D1_RECT_F& r, const std::vector<float>& values, uint64_t version, float maxV,
               Color line, bool flip, const D2D1_ROUNDED_RECT* in) {
    if (values.size() < 2 || maxV <= 0) return;
    uint64_t key = (uint64_t)(uintptr_t)values.data() * 31 + version; // buffer identity + data version
    key = key * 131 + (uint64_t)(r.left * 2) * 7 + (uint64_t)(r.top * 2) * 13 + (uint64_t)(Width(r) * 2) * 17 + // half px rect precision
          (uint64_t)(Height(r) * 2) * 19 + (uint64_t)maxV * 23 + (flip ? 1 : 0);
    if (in) key = key * 37 + (uint64_t)(in->rect.left * 2) * 41 + (uint64_t)(in->rect.bottom * 2) * 43 + (uint64_t)(Width(in->rect) * 2) * 47 + (uint64_t)(in->radiusX * 2) * 53;
    auto draw = [&](const SparkGeo& g) { // kept: the same geometry every frame until the next sample, tessellated once
        c.FillGeoV(g.area.Get(), r, line.Alpha(flip ? 0.02f : 0.32f), line.Alpha(flip ? 0.32f : 0.02f), true); // area fades toward baseline
        if (g.cut) c.FillGeometry(g.stroke.Get(), line, true);
        else c.StrokeGeo(g.stroke.Get(), line, kSparkLine, true);
    };
    for (const SparkGeo& g : sparkCache) {
        if (g.key != key) continue;
        draw(g);
        return;
    }
    ComPtr<ID2D1PathGeometry> area, stroke;
    ComPtr<ID2D1GeometrySink> a, s;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&area)) || FAILED(area->Open(&a))) return;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&stroke)) || FAILED(stroke->Open(&s))) return;
    float base = flip ? r.top : r.bottom; // flip: graph hangs down from top (upload vs download)
    float step = Width(r) / (values.size() - 1);
    auto y = [&](size_t i) {
        float f = anim::Clamp01(values[i] / maxV);
        return flip ? r.top + f * Height(r) : r.bottom - f * Height(r);
    };
    a->BeginFigure({r.left, base}, D2D1_FIGURE_BEGIN_FILLED);
    a->AddLine({r.left, y(0)});
    s->BeginFigure({r.left, y(0)}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < values.size(); ++i) {
        float x0 = r.left + step * (i - 1), x1 = r.left + step * i;
        D2D1_BEZIER_SEGMENT seg = {{x0 + step / 2, y(i - 1)}, {x1 - step / 2, y(i)}, {x1, y(i)}}; // flat tangents at points, smooth curve no overshoot
        a->AddBezier(seg);
        s->AddBezier(seg);
    }
    a->AddLine({r.right, base});
    a->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    a->Close();
    s->Close();

    SparkGeo g{key, area, stroke};
    if (in && !Clip(c, g, r, *in)) return; // can't cut it: nothing rather than spilling past the card
    draw(g);
    if (sparkCache.size() >= 8) sparkCache.erase(sparkCache.begin()); // drop oldest
    sparkCache.push_back(g);
}

bool Chip(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, bool selected) {
    Ui::Hit h = ui.Button(id, r);
    ui.Mark(Ui::Role::Toggle, selected);
    float sel = ui.Smooth(id, selected ? 1.0f : 0, 14);
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    Canvas& c = ui.C();
    float rad = Height(r) / 2; // full pill shape
    c.Fill(r, rad, t.cardHi.Mix(t.track, hover * 0.6f));
    if (sel > 0.001f) c.Fill(r, rad, t.accent.Alpha(0.22f * sel));
    c.Stroke(r, rad, t.accent.Alpha(0.55f * sel).Mix(t.hairTop, 1 - sel), 1);
    c.Text(label, Inset(r, 10, 0), Weight::Medium, 11.5f, t.textDim.Mix(t.text, sel), Align::Center);
    return h.clicked;
}

bool Switch(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, bool on, bool enabled) {
    constexpr float kHalfW = 20, kHalfH = 11; // rows are 30 tall: this leaves air between stacked ones
    D2D1_RECT_F r = {c.x - kHalfW, c.y - kHalfH, c.x + kHalfW, c.y + kHalfH};
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    if (enabled) ui.Mark(Ui::Role::Toggle, on);
    float k = ui.Smooth(id, on ? 1.0f : 0, 18); // knob travel 0..1
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    float a = enabled ? 1.0f : 0.4f;
    Canvas& cv = ui.C();
    cv.Fill(r, kHalfH, t.track.Mix(t.accent, k).Alpha(a));
    cv.Stroke(r, kHalfH, t.hairTop.Mix(t.accentHi.Alpha(0.6f), k), 1);
    float x = r.left + kHalfH + 2 * (kHalfW - kHalfH) * k;
    float rad = 5.5f + 2.5f * k + hover; // m3 style: knob grows when on
    if (hover > 0.01f) cv.Dot({x, c.y}, rad + 6, t.text.Alpha(0.06f * hover));
    cv.Dot({x, c.y}, rad, t.textDim.Mix(t.onAccent, k).Alpha(a)); // dark knob on accent track
    return h.clicked;
}

std::wstring Pct(float v) {
    return std::to_wstring((int)std::lround(anim::Clamp01(v) * 100)) + L"%"; // round, not truncate
}

}
