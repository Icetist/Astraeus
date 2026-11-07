#include "page_sheet.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

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
        c.Fill(b, rad, t.accent.Mix(t.accentHi, 0.35f * hover).Alpha(k)); // solid accent, a shade lighter under the mouse
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
    float shown = ui.Smooth(Ui::Id(id), target, 18);
    ui.Reveal(view, shown, target); // the keyboard moved to something out of view
    target = (std::clamp)(target, 0.0f, maxS);
    return shown;
}

void FadesIn(Canvas& c, Color ink, const D2D1_RECT_F& view, float scroll, float content) {
    float maxS = content - Height(view);
    if (scroll > 1) c.FillV({view.left, view.top, view.right, view.top + 18}, 0, ink, ink.Alpha(0));
    if (scroll < maxS - 1) c.FillV({view.left, view.bottom - 18, view.right, view.bottom}, 0, ink.Alpha(0), ink);
}

void Fades(Canvas& c, const Theme& t, const D2D1_RECT_F& view, float scroll, float content) { FadesIn(c, t.card, view, scroll, content); }

void Surface(Canvas& c, const Theme& t, const D2D1_RECT_F& r, Color fill) {
    c.Fill(Rect(r.left, r.top + 3, Width(r), Height(r)), kSurfaceR, Color{0, 0, 0, 0.22f});
    c.Fill(r, kSurfaceR, fill);
    c.StrokeV(r, kSurfaceR, t.hairTop, t.hairBot, 1);
}

constexpr float kCardLift = 0.25f; // toward the track: about 1.12:1 on the sheet, enough to read with no border, and the controls on it still stand out

Color CardFill(const Theme& t) { return t.glass ? t.cardHi.Alpha(0.55f) : t.cardHi.Mix(t.track, kCardLift); } // glass already reads, and a brighter pane would cost its text

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title) { // a settings card, returns its inner area
    c.Fill(r, 16, CardFill(t));
    if (theme::Contrast()) c.StrokeV(r, 16, t.hairTop, t.hairBot, 1); // card and sheet are one color there: only an edge shows it
    c.Text(title, Rect(r.left + 14, r.top + 9, Width(r) - 28, 16), Weight::Medium, 12, t.text);
    return {r.left + 12, r.top + 30, r.right - 12, r.bottom - 10};
}

int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur, bool live) {
    Canvas& c = ui.C();
    float seg = Width(r) / n, rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    float x = ui.Smooth(Ui::Id(id, 99), r.left + seg * cur, 18); // thumb slides between options
    c.Fill({x + 3, r.top + 3, x + seg - 3, r.bottom - 3}, rad - 3, t.accent); // solid, as every other accented control in settings
    int picked = -1;
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F cell = {r.left + seg * i, r.top, r.left + seg * (i + 1), r.bottom};
        Ui::Hit h = live ? ui.Button(Ui::Id(id, i), cell) : Ui::Hit{};
        if (live) ui.Mark(Ui::Role::Toggle, i == cur);
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
    float cy = (grab.top + r.bottom) / 2 + 1;
    float x0 = r.left + kThumbR, x1 = r.right - kThumbR; // the mouse moves the thumb's middle, which stops short of the ends
    Ui::Hit h = live ? ui.Button(id, grab) : Ui::Hit{};
    if (live) {
        ui.Mark(Ui::Role::Slider, false, v);
        ui.Name(label, value);
    }
    bool dragging = ui.IsActive(id) && (in.down || in.released);
    bool changed = false;
    bool keyed = live && ui.KeyMove(id, v, step); // arrows: a step each, saved at once
    if (dragging && x1 > x0) { // a press jumps there, a drag follows
        float f = anim::Clamp01((in.mouse.x - x0) / (x1 - x0));
        if (std::fabs(f - v) >= 0.0005f) v = f, changed = true;
    }
    released = ui.IsActive(id) && in.released;
    if (keyed) changed = released = true;
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
    float tr = kThumbR + kThumbGrow * hot + drag, th = kTrackH + kTrackGrow * hot, x = w::ThumbAt(r.left, r.right, tr, shown);
    c.Fill({r.left, cy - th / 2, r.right, cy + th / 2}, th / 2, t.track);
    c.Fill({r.left, cy - th / 2, (std::max)(r.left + Width(r) * shown, r.left + th), cy + th / 2}, th / 2, t.accent); // solid, the song's or the shell's accent; the whole track at 100%
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

bool PickCard(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& card, int icon, const wchar_t* name, const wchar_t* about, bool on) {
    Canvas& c = ui.C();
    Ui::Hit h = ui.Button(id, card);
    ui.Mark(Ui::Role::Toggle, on);
    ui.Name(name);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), sel = ui.Smooth(id + 2, on ? 1.0f : 0, 16);
    c.Fill(card, 16, t.text.Alpha(0.04f + 0.05f * hov));
    if (sel > 0.01f) c.Stroke(card, 16, t.accent.Alpha(0.8f * sel), 1.5f);
    float mid = (card.top + card.bottom) / 2, room = Width(card) - 82;
    c.Icon(icon, {card.left + 26, mid}, 21, t.text.Mix(t.accentHi, sel));
    c.Text(name, Rect(card.left + 48, mid - 18, room, 18), Weight::Semi, 12, t.text);
    c.Text(about, Rect(card.left + 48, mid, room, 16), Weight::Regular, 10.5f, t.textDim);
    D2D1_POINT_2F o = {card.right - 20, mid};
    c.Dot(o, 9, t.text.Alpha(0.1f).Mix(t.accent, sel));
    c.Icon(on ? glyph::Check : glyph::Add, o, 13, on ? t.onAccent : t.textDim);
    return h.clicked;
}

void KeepTiles(Model& m, const Ids& set) {
    std::vector<int> shown = set.Vec();
    for (int id : m.tiles)
        if (!m.TileHere(id)) shown.push_back(id); // a laptop's tile stays saved on a desktop
    bool had = std::find(m.tiles.begin(), m.tiles.end(), (int)Model::kTlWork) != m.tiles.end();
    bool has = std::find(shown.begin(), shown.end(), (int)Model::kTlWork) != shown.end();
    if (had != has) sys::SaveNum(L"WorkTileOff", has ? 0 : 1); // taken off by hand: start-up doesn't put it back
    m.SetTiles(shown);
}

}
