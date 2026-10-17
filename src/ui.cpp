#include "ui.h"

#include <cmath>

#include "anim.h"
#include "glyphs.h"

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
}

void Ui::AddHit(uint32_t id, const D2D1_RECT_F& r) {
    D2D1::Matrix3x2F m, inv = *D2D1::Matrix3x2F::ReinterpretBaseType(&frameXf);
    canvas->Target()->GetTransform(&m); // current transform may include scroll/slide offsets
    if (!inv.Invert()) inv = D2D1::Matrix3x2F::Identity();
    D2D1::Matrix3x2F toLocal = m * inv; // map hit rect back to frame space
    D2D1_POINT_2F a = toLocal.TransformPoint({r.left, r.top});
    D2D1_POINT_2F b = toLocal.TransformPoint({r.right, r.bottom}); // corners only, assumes no rotation
    nextHits.push_back({id, D2D1_RECT_F{a.x, a.y, b.x, b.y}});
}

uint32_t Ui::HitAt(D2D1_POINT_2F p) const {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) // reverse: last drawn is on top
        if (Inside(it->second, p)) return it->first;
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
    return h;
}

bool Ui::Slider(uint32_t id, const D2D1_RECT_F& r, float& value, bool vertical) {
    Hit h = Button(id, r);
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
    bool changed = ui.Slider(id, track, value, false);
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
    float x = track.left + Width(track) * shown * (1 - mute); // muted: the bar empties, the level is kept for when it comes back
    if (x > track.left + th) { // skip fill when too short to round
        c.Fill({track.left - 2, cy - th / 2 - 3, x + 2, cy + th / 2 + 3}, th / 2 + 3, t.accent.Alpha((0.10f + 0.10f * glow) * (1 - mute)));
        c.FillH({track.left, cy - th / 2, x, cy + th / 2}, th / 2, t.accent.Mix(t.textLow, mute), t.accentHi.Mix(t.textLow, mute)); // muted: the level is kept, greyed
    }
    float knob = 6.5f + 2.5f * glow;
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
    ComPtr<ID2D1PathGeometry> area, stroke;
};
std::vector<SparkGeo> sparkCache; // small fifo, geometry rebuild is costly

}

void Spark(Canvas& c, const D2D1_RECT_F& r, const std::vector<float>& values, uint64_t version, float maxV,
               Color line, bool flip) {
    if (values.size() < 2 || maxV <= 0) return;
    uint64_t key = (uint64_t)(uintptr_t)values.data() * 31 + version; // buffer identity + data version
    key = key * 131 + (uint64_t)(r.left * 2) * 7 + (uint64_t)(r.top * 2) * 13 + (uint64_t)(Width(r) * 2) * 17 + // half px rect precision
          (uint64_t)(Height(r) * 2) * 19 + (uint64_t)maxV * 23 + (flip ? 1 : 0);
    for (const SparkGeo& g : sparkCache) {
        if (g.key != key) continue;
        c.FillGeoV(g.area.Get(), r, line.Alpha(flip ? 0.02f : 0.32f), line.Alpha(flip ? 0.32f : 0.02f)); // area fades toward baseline
        c.StrokeGeo(g.stroke.Get(), line, 1.6f);
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

    c.FillGeoV(area.Get(), r, line.Alpha(flip ? 0.02f : 0.32f), line.Alpha(flip ? 0.32f : 0.02f));
    c.StrokeGeo(stroke.Get(), line, 1.6f);
    if (sparkCache.size() >= 8) sparkCache.erase(sparkCache.begin()); // drop oldest
    sparkCache.push_back({key, area, stroke});
}

bool Chip(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, bool selected) {
    Ui::Hit h = ui.Button(id, r);
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
    D2D1_RECT_F r = {c.x - 23, c.y - 13, c.x + 23, c.y + 13};
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    float k = ui.Smooth(id, on ? 1.0f : 0, 18); // knob travel 0..1
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    float a = enabled ? 1.0f : 0.4f;
    Canvas& cv = ui.C();
    cv.Fill(r, 13, t.track.Mix(t.accent, k).Alpha(a));
    cv.Stroke(r, 13, t.hairTop.Mix(t.accentHi.Alpha(0.6f), k), 1);
    float x = r.left + 13 + 20 * k;
    float rad = 6.5f + 3 * k + hover; // m3 style: knob grows when on
    if (hover > 0.01f) cv.Dot({x, c.y}, rad + 6, t.text.Alpha(0.06f * hover));
    cv.Dot({x, c.y}, rad, t.textDim.Mix(t.onAccent, k).Alpha(a)); // dark knob on accent track
    return h.clicked;
}

std::wstring Pct(float v) {
    return std::to_wstring((int)std::lround(anim::Clamp01(v) * 100)) + L"%"; // round, not truncate
}

}
