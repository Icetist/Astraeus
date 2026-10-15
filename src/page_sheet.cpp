#include "page_sheet.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"

namespace sheet {

Frame Begin(PageCtx& p, float k) {
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
    c.PushClip(s);
    c.Glow(f.badge, 200, 160, t.accent.Alpha(0.09f)); // tint around the hero badge
    c.PopClip();
    c.FillV({s.left, s.top, s.right, s.top + 80}, 24, t.sheen, t.sheen.Alpha(0)); // top sheen
    c.StrokeV(s, 24, t.hairTop, t.hairBot, 1);
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

float Scroll(Ui& ui, const char* id, float& target, const D2D1_RECT_F& view, float content) {
    float maxS = (std::max)(0.0f, content - Height(view));
    target = (std::clamp)(target - ui.Wheel(view) * 60, 0.0f, maxS); // 60 dips per notch
    return ui.Smooth(Ui::Id(id), target, 18);
}

void Fades(Canvas& c, const Theme& t, const D2D1_RECT_F& view, float scroll, float content) {
    float maxS = content - Height(view);
    if (scroll > 1) c.FillV({view.left, view.top, view.right, view.top + 18}, 0, t.card, t.card.Alpha(0));
    if (scroll < maxS - 1) c.FillV({view.left, view.bottom - 18, view.right, view.bottom}, 0, t.card.Alpha(0), t.card);
}

bool Knob(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value, float& v, bool live,
          bool& released) { // label, track, value text; true while dragging
    Canvas& c = ui.C();
    float cy = (r.top + r.bottom) / 2;
    D2D1_RECT_F track = {r.left + 62, r.top, r.right - 70, r.bottom};
    bool changed = live && ui.Slider(id, track, v, false);
    released = ui.IsActive(id) && ui.In().released;
    float glow = ui.Smooth(id + 1, ui.IsActive(id) || (live && Inside(r, ui.In().mouse)) ? 1.0f : 0, 14);
    c.Text(label, {r.left, r.top, track.left - 8, r.bottom}, Weight::Regular, 11.5f, t.textDim);
    float th = 4 + 2 * glow;
    c.Fill({track.left, cy - th / 2, track.right, cy + th / 2}, th / 2, t.track);
    float x = track.left + Width(track) * anim::Clamp01(v);
    if (x > track.left + th) c.FillH({track.left, cy - th / 2, x, cy + th / 2}, th / 2, t.accent, t.accentHi);
    c.Dot({x, cy}, 6 + 1.5f * glow, t.accentHi);
    c.Text(value, {track.right + 8, r.top, r.right, r.bottom}, Weight::Medium, 11.5f, t.text, Align::Right);
    return changed;
}

}
