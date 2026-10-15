// Settings as a set of small visual instruments: a color orb, material swatches, a miniature monitor to dock on,
// miniature screens, a miniature island. You touch the thing itself instead of reading rows of options.
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "version.h"

using namespace sheet;

namespace {

constexpr float kGap = 10;
constexpr int kPresets[] = {0x5B8CFF, 0x9B7BFF, 0xFF6FAE, 0xFF6B6B, 0xFF9F43, 0xF5C542, 0x3DDC97, 0x2EC4D6}; // curated accents
bool ringDrag = false; // dragging around the hue ring

float Rad(float deg) { return deg * kPi / 180; }

int ToHex(Color c) {
    auto ch = [](float v) { return (int)(anim::Clamp01(v) * 255 + 0.5f); };
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

std::wstring HexText(int rgb) {
    wchar_t buf[8];
    swprintf_s(buf, L"%06X", rgb & 0xFFFFFF);
    return buf;
}

Seed Base(const Model& m, const Theme& t) { // the hue/chroma everything is previewed in
    return theme::FromCol(m.accentMode ? Color::Rgb((uint32_t)m.accentHex) : t.accent);
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

void Orb(PageCtx& p, D2D1_POINT_2F o, float R, bool live) { // hue ring around a glowing orb of the accent; tap the center to type hex
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool custom = m.accentMode == 1;
    Seed cur = Base(m, t);
    float ringW = R * 0.17f;
    for (int i = 0; i < 36; ++i) { // rainbow ring, dimmed while the wallpaper decides
        float deg = i * 10.0f;
        c.Arc(o, R, Rad(deg - 90.5f), Rad(11), theme::Hsl(deg, 0.80f, 0.60f).Alpha(custom ? 1.0f : 0.45f), ringW);
    }
    uint32_t id = Ui::Id("set.orb");
    float reach = R + ringW;
    D2D1_RECT_F box = {o.x - reach, o.y - reach, o.x + reach, o.y + reach};
    Ui::Hit h = live ? ui.Button(id, box) : Ui::Hit{};
    D2D1_POINT_2F ms = ui.In().mouse, ps = ui.In().pressPos;
    float dist = std::hypot(ms.x - o.x, ms.y - o.y), pdist = std::hypot(ps.x - o.x, ps.y - o.y);
    if (ui.IsActive(id) && ui.In().pressed) {
        if (std::fabs(pdist - R) < ringW + 8) ringDrag = true; // grabbed the ring
        else if (pdist < R - ringW && !m.hexFocus) { // tapped the orb: type a color
            m.hexFocus = true;
            m.hexEdit = HexText(custom ? m.accentHex : ToHex(t.accent));
        }
    }
    if (m.hexFocus && ui.In().pressed && pdist > R - ringW) m.CommitHex(); // clicked away: keep it if it's a color
    if (ringDrag && ui.IsActive(id) && ui.In().down) { // whole panel recolors live while you drag
        float hue = std::fmod(std::atan2(ms.y - o.y, ms.x - o.x) * 180 / kPi + 90 + 360, 360.0f);
        m.accentHex = ToHex(theme::Hsl(hue, 0.78f, 0.62f));
        m.accentMode = 1;
        cur = theme::FromCol(Color::Rgb((uint32_t)m.accentHex));
    }
    if (ringDrag && ui.In().released) {
        ringDrag = false;
        m.SetAccent(1, m.accentHex); // remember it
    }
    (void)h;

    custom = m.accentMode == 1;
    Theme tinted = Theme::From(Seed{cur.hue, cur.chroma, m.look}); // what the theme makes of it
    Color accent = custom ? tinted.accent : t.accent;
    float lift = ui.Smooth(id + 1, live && dist < R - ringW ? 1.0f : 0, 14);
    c.Glow(o, R * 1.5f, R * 1.5f, accent.Alpha(0.22f + 0.10f * lift));
    c.Dot(o, R - ringW - 3 + 2 * lift, accent);
    c.Dot({o.x - R * 0.22f, o.y - R * 0.26f}, R * 0.30f, Color{1, 1, 1, 0.16f}); // specular highlight, gives it a glassy orb look
    D2D1_POINT_2F k = {o.x + R * std::cos(Rad(cur.hue - 90)), o.y + R * std::sin(Rad(cur.hue - 90))};
    c.Dot(k, ringW * 0.75f + 3, t.text);
    c.Dot(k, ringW * 0.75f, theme::Hsl(cur.hue, 0.80f, 0.60f));

    Color ink = custom ? tinted.onAccent : t.onAccent;
    D2D1_RECT_F label = {o.x - R, o.y - 10, o.x + R, o.y + 10};
    if (m.hexFocus) {
        std::wstring shown = L"#" + m.hexEdit;
        c.Text(shown, label, Weight::Medium, 13, ink, Align::Center);
        if (std::fmod(ui.Now(), 1.0) < 0.55) { // caret blinks while typing
            float cx = o.x + shown.size() * 4.3f + 1;
            c.Line({cx, o.y - 8}, {cx, o.y + 8}, ink, 1.5f);
        }
        ui.KeepAlive();
    } else {
        c.Text(custom ? L"#" + HexText(m.accentHex) : std::wstring(L"Wallpaper"), label, Weight::Medium, custom ? 13.0f : 11.5f, ink, Align::Center);
    }
}

void Presets(PageCtx& p, float x, float y, float step, bool live) { // one-tap accents
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    for (int i = 0; i < 8; ++i) {
        D2D1_POINT_2F o = {x + i * step, y};
        Ui::Hit h = live ? ui.Button(Ui::Id("set.preset", i), {o.x - 10, o.y - 10, o.x + 10, o.y + 10}) : Ui::Hit{};
        bool on = p.m.accentMode == 1 && (p.m.accentHex & 0xFFFFFF) == kPresets[i];
        if (on || h.hover) c.Ring(o, 10, p.shell.text.Alpha(on ? 0.9f : 0.4f), 1.5f);
        c.Dot(o, 7, Color::Rgb((uint32_t)kPresets[i]));
        if (h.clicked) p.m.SetAccent(1, kPresets[i]);
    }
}

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title) { // card shell, returns its inner area
    c.Fill(r, 16, t.cardHi.Alpha(0.55f));
    c.StrokeV(r, 16, t.hairTop, t.hairBot, 1);
    c.TextSp(title, Rect(r.left + 14, r.top + 11, Width(r) - 28, 12), Weight::Medium, 9.5f, t.textLow, 1.4f);
    return {r.left + 12, r.top + 30, r.right - 12, r.bottom - 10};
}

void Material(PageCtx& p, const D2D1_RECT_F& r, bool live) { // three swatches drawn in the actual materials
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"MATERIAL");
    bool glass = m.look == 2;
    Seed s = Base(m, t);
    float sw = (Width(in) - 16) / 3, sh = (std::min)(Height(in) - (glass ? 48 : 20), 74.0f);
    static const wchar_t* const kNames[] = {L"Dark", L"Light", L"Glass"};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F q = Rect(in.left + i * (sw + 8), in.top + 2, sw, sh);
        uint32_t id = Ui::Id("set.mat", i);
        Ui::Hit h = live ? ui.Button(id, q) : Ui::Hit{};
        float on = ui.Smooth(id + 10, m.look == i ? 1.0f : 0, 14), hov = ui.Smooth(id + 20, h.hover ? 1.0f : 0, 16);
        q = Inset(q, -2 * on, -2 * on); // the chosen one lifts
        Theme look = Theme::From(Seed{s.hue, s.chroma, i});
        if (on > 0.01f) c.Glow({(q.left + q.right) / 2, (q.top + q.bottom) / 2}, sw, sh, t.accent.Alpha(0.20f * on));
        if (i == 2) { // glass: color under frost, a light streak, a bright rim
            c.PushClip(q);
            c.Dot({q.left + sw * 0.3f, q.top + sh * 0.7f}, sh * 0.55f, look.accent.Alpha(0.65f));
            c.Dot({q.right - sw * 0.25f, q.top + sh * 0.25f}, sh * 0.45f, theme::Hsl(s.hue + 60, 0.7f, 0.6f, 0.55f));
            c.FillV(q, 12, Color{1, 1, 1, 0.22f}, Color{1, 1, 1, 0.06f});
            c.Line({q.left + sw * 0.15f, q.bottom + 6}, {q.left + sw * 0.75f, q.top - 6}, Color{1, 1, 1, 0.22f}, 9);
            c.PopClip();
            c.Stroke(q, 12, Color{1, 1, 1, 0.40f}, 1.2f);
        } else {
            c.FillV(q, 12, look.cardHi, look.bg.Alpha(1));
            c.Fill(Rect(q.left + 10, q.top + 12, sw * 0.5f, 6), 3, look.text.Alpha(0.85f)); // a line of "text"
            c.Fill(Rect(q.left + 10, q.top + 24, sw * 0.32f, 5), 2.5f, look.textDim.Alpha(0.8f));
            c.Fill(Rect(q.left + 10, q.bottom - 16, sw - 20, 6), 3, look.accent); // and an accent bar
            c.StrokeV(q, 12, look.hairTop, look.hairBot, 1);
        }
        if (on > 0.01f || hov > 0.01f) c.Stroke(Inset(q, -3, -3), 14, t.accent.Alpha((std::max)(on, 0.5f * hov)), 2);
        c.Text(kNames[i], Rect(q.left, q.bottom + 4, Width(q), 14), Weight::Medium, 11, t.textDim.Mix(t.text, on), Align::Center);
        if (h.clicked && m.look != i) m.SetLook(i); // drawer crossfades, glass swaps its pipeline
    }
    if (glass) { // how frosted: fades between clear glass and windows' full backdrop blur
        float blur = m.glassBlur / 100.0f;
        bool done = false;
        if (Knob(ui, t, Ui::Id("set.blur"), Rect(in.left, in.bottom - 22, Width(in), 22), L"Blur", std::to_wstring(m.glassBlur) + L"%", blur, live, done))
            m.glassBlur = (int)(blur * 100 + 0.5f); // live preview
        if (done) m.SetPref(m.glassBlur, L"GlassBlur", m.glassBlur);
    }
}

void Monitor(Canvas& c, const Theme& t, const D2D1_RECT_F& s) { // a little screen on a stand
    float mx = (s.left + s.right) / 2;
    c.Fill(Rect(mx - 6, s.bottom, 12, 5), 2, t.track);
    c.Fill(Rect(mx - 15, s.bottom + 5, 30, 3), 1.5f, t.track);
    c.FillV(s, 7, t.track.Alpha(0.9f), t.track.Alpha(0.5f));
    c.Stroke(s, 7, t.hairTop, 1);
}

void Position(PageCtx& p, const D2D1_RECT_F& r, bool live) { // tap an edge of the miniature monitor to dock there
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"POSITION");
    float mw = (std::min)(Width(in) - 40, (Height(in) - 34) / 0.6f), mh = mw * 0.6f;
    D2D1_RECT_F scr = Rect((in.left + in.right - mw) / 2, in.top + 4, mw, mh);
    Monitor(c, t, scr);
    for (int e = 0; e < 3; ++e) {
        D2D1_RECT_F hot = e == 0 ? D2D1_RECT_F{scr.left + mw * 0.2f, scr.top - 8, scr.right - mw * 0.2f, scr.top + mh * 0.35f}
                        : e == 1 ? D2D1_RECT_F{scr.left - 8, scr.top + mh * 0.15f, scr.left + mw * 0.3f, scr.bottom - mh * 0.15f}
                                 : D2D1_RECT_F{scr.right - mw * 0.3f, scr.top + mh * 0.15f, scr.right + 8, scr.bottom - mh * 0.15f};
        uint32_t id = Ui::Id("set.edge", e);
        Ui::Hit h = live ? ui.Button(id, hot) : Ui::Hit{};
        float on = ui.Smooth(id + 10, m.edge == e ? 1.0f : 0, 14), hov = h.hover ? 1.0f : 0;
        Color col = t.accent.Alpha(0.25f + 0.75f * on).Mix(t.text.Alpha(0.5f), hov * (1 - on));
        float d = 4 + (e == 0 ? mh * 0.30f : mw * 0.22f) * on; // the docked edge grows into a little panel
        D2D1_RECT_F bar = e == 0 ? D2D1_RECT_F{scr.left + mw * 0.3f, scr.top + 3, scr.right - mw * 0.3f, scr.top + 3 + d}
                        : e == 1 ? D2D1_RECT_F{scr.left + 3, scr.top + mh * 0.22f, scr.left + 3 + d, scr.bottom - mh * 0.22f}
                                 : D2D1_RECT_F{scr.right - 3 - d, scr.top + mh * 0.22f, scr.right - 3, scr.bottom - mh * 0.22f};
        c.Fill(bar, 3, col);
        if (h.clicked && m.edge != e) m.SetPref(m.edge, L"Edge", e); // panel retracts and re-docks
    }
    static const wchar_t* const kNames[] = {L"Top edge", L"Left edge", L"Right edge"};
    c.Text(kNames[m.edge], Rect(in.left, in.bottom - 14, Width(in), 14), Weight::Medium, 11, t.textDim, Align::Center);
}

void Screens(PageCtx& p, const D2D1_RECT_F& r, bool live) { // your monitors as they're arranged: tap one, or All
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"SCREENS");
    if (m.screenList.empty()) m.screenList = sys::ListScreens();
    const auto& list = m.screenList;
    int chosen = -1; // the screen it lives on in one-screen mode: the pick, else primary
    for (int i = 0; i < (int)list.size(); ++i)
        if (list[i].name == m.screen) chosen = i;
    for (int i = 0; chosen < 0 && i < (int)list.size(); ++i)
        if (list[i].primary) chosen = i;

    RECT box = list.empty() ? RECT{0, 0, 16, 9} : list[0].rc; // everything, to scale
    for (const auto& s : list) {
        box.left = (std::min)(box.left, s.rc.left);
        box.top = (std::min)(box.top, s.rc.top);
        box.right = (std::max)(box.right, s.rc.right);
        box.bottom = (std::max)(box.bottom, s.rc.bottom);
    }
    D2D1_RECT_F map = {in.left + 4, in.top + 2, in.right - 4, in.bottom - 26};
    float bw = (float)(box.right - box.left), bh = (float)(box.bottom - box.top);
    float k = (std::min)(Width(map) / bw, Height(map) / bh);
    float ox = (map.left + map.right - bw * k) / 2, oy = (map.top + map.bottom - bh * k) / 2;
    float all = ui.Smooth(Ui::Id("set.screens", 99), m.screens ? 1.0f : 0, 14);
    for (int i = 0; i < (int)list.size(); ++i) {
        const auto& s = list[i];
        D2D1_RECT_F q = {ox + (s.rc.left - box.left) * k + 2, oy + (s.rc.top - box.top) * k + 2, ox + (s.rc.right - box.left) * k - 2,
                         oy + (s.rc.bottom - box.top) * k - 2};
        uint32_t id = Ui::Id("set.screen", i);
        Ui::Hit h = live ? ui.Button(id, q) : Ui::Hit{};
        float on = (std::max)(all, ui.Smooth(id + 10, i == chosen ? 1.0f : 0, 14));
        c.FillV(q, 6, t.track.Alpha(0.9f).Mix(t.accent.Alpha(0.35f), on), t.track.Alpha(0.5f).Mix(t.accent.Alpha(0.18f), on));
        c.Stroke(q, 6, t.accent.Alpha(on).Mix(t.text.Alpha(0.5f), h.hover ? 0.5f * (1 - on) : 0), on > 0.5f ? 1.5f : 1);
        float mx = (q.left + q.right) / 2, my = (q.top + q.bottom) / 2; // where the panel docks on that screen
        D2D1_RECT_F dock = m.edge == 0 ? D2D1_RECT_F{mx - Width(q) * 0.18f, q.top + 3, mx + Width(q) * 0.18f, q.top + 6}
                         : m.edge == 1 ? D2D1_RECT_F{q.left + 3, my - Height(q) * 0.2f, q.left + 6, my + Height(q) * 0.2f}
                                       : D2D1_RECT_F{q.right - 6, my - Height(q) * 0.2f, q.right - 3, my + Height(q) * 0.2f};
        c.Fill(dock, 1.5f, t.accentHi.Alpha(on));
        c.Text(std::to_wstring(s.number ? s.number : i + 1), q, Weight::Medium, (std::min)(14.0f, Height(q) * 0.45f), t.text.Alpha(0.5f + 0.5f * on),
               Align::Center);
        if (h.clicked && (m.screens || i != chosen)) m.PickScreen(s.name);
    }
    D2D1_RECT_F pill = {in.right - 44, in.bottom - 18, in.right, in.bottom}; // every screen: the panel follows the mouse
    if (list.size() > 1) {
        Ui::Hit ph = live ? ui.Button(Ui::Id("set.screens.all"), pill) : Ui::Hit{};
        c.Fill(pill, 9, t.track.Alpha(0.8f).Mix(t.accent, all));
        c.Text(L"All", pill, Weight::Medium, 10.5f, t.textDim.Mix(t.onAccent, all).Mix(t.text, ph.hover ? 0.4f * (1 - all) : 0), Align::Center);
        if (ph.clicked) {
            if (m.screens) m.PickScreen(chosen >= 0 ? list[chosen].name : L"");
            else m.SetPref(m.screens, L"Screens", 1);
        }
    }
    int shown = chosen >= 0 && list[chosen].number ? list[chosen].number : chosen + 1;
    std::wstring label = list.size() <= 1 ? L"One screen" : m.screens ? L"Every screen" : L"Screen " + std::to_wstring(shown);
    c.Text(label, Rect(in.left, in.bottom - 16, Width(in) - 48, 14), Weight::Medium, 11, t.textDim);
}

void Notices(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a little screen with the island catching a notification
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"NOTIFICATIONS");
    Ui::Hit h = live ? ui.Button(Ui::Id("set.notice"), in) : Ui::Hit{};
    float on = ui.Smooth(Ui::Id("set.notice", 1), m.notifyPills ? 1.0f : 0, 12);
    float mw = (std::min)(Width(in) - 40, (Height(in) - 34) / 0.6f), mh = mw * 0.6f;
    D2D1_RECT_F scr = Rect((in.left + in.right - mw) / 2, in.top + 4, mw, mh);
    Monitor(c, t, scr);
    float pw = mw * (0.30f + 0.22f * on), ph = 7 + 7 * on; // the island grows when it carries a notification
    D2D1_RECT_F pill = Rect((scr.left + scr.right - pw) / 2, scr.top + 3, pw, ph);
    c.Fill(pill, ph / 2, Color{0.02f, 0.02f, 0.03f, 0.95f});
    if (on > 0.05f) {
        c.Dot({pill.left + ph / 2 + 1, (pill.top + pill.bottom) / 2}, ph * 0.26f, t.accent.Alpha(on));
        c.Fill(Rect(pill.left + ph + 2, pill.top + ph * 0.38f, pw * 0.5f, ph * 0.24f), ph * 0.12f, Color{1, 1, 1, 0.75f * on});
    }
    if (h.hover) c.Stroke(Inset(in, -4, -4), 12, t.accent.Alpha(0.35f), 1.5f);
    c.Text(m.notifyPills ? L"Shown as pills" : L"Off", Rect(in.left, in.bottom - 14, Width(in), 14), Weight::Medium, 11,
           m.notifyPills ? t.accentHi : t.textDim, Align::Center);
    if (h.clicked) m.SetNotifyPills(m.notifyPills == 0);
}

}

void DrawSettings(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    Frame frame = Begin(p, k);
    bool live = ui.In().mouse.x > -1e5f; // inputs only while the mouse is around
    static const wchar_t* const kSources[] = {L"Wallpaper", L"Custom"};

    D2D1_RECT_F grid;
    if (!frame.tall) { // hero column: the color orb
        float hx = s.left + kHero / 2;
        c.TextSp(std::wstring(L"ASTRAEUS  ·  ") + APP_VER_W, Rect(s.left + 10, s.top + 16, kHero - 20, 12), Weight::Medium, 9.5f, t.textLow, 1.4f,
                 Align::Center);
        Orb(p, {hx, s.top + 98}, 56, live);
        int v = Segmented(ui, t, "set.accent", Rect(hx - 90, s.top + 172, 180, 24), kSources, 2, m.accentMode, live);
        if (v >= 0) m.SetAccent(v, m.accentHex);
        Presets(p, hx - 84, s.top + 214, 24, live);
        if (ScanBtn(ui, t, "set.win", Rect(s.left + 28, s.bottom - 44, kHero - 56, 32), false, true, glyph::OpenNew, L"Windows settings", L"")) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:");
        }
        D2D1_RECT_F list = frame.list;
        c.TextSp(L"SETTINGS", Rect(list.left + 4, list.top + 8, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
        if (w::IconBtn(ui, t, Ui::Id("set.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseSettings();
        grid = {list.left, list.top + 34, list.right, list.bottom};
    } else { // sidebar header: orb on the left, source switch and presets beside it
        Orb(p, {s.left + 66, s.top + 66}, 44, live);
        float x0 = s.left + 130;
        int v = Segmented(ui, t, "set.accent", Rect(x0, s.top + 28, (std::min)(Width(s) - 176, 180.0f), 24), kSources, 2, m.accentMode, live);
        if (v >= 0) m.SetAccent(v, m.accentHex);
        Presets(p, x0 + 10, s.top + 76, 22, live);
        c.Text(L"Tap the orb to type a hex", Rect(x0, s.top + 96, Width(s) - 146, 16), Weight::Light, 10.5f, t.textLow);
        if (w::IconBtn(ui, t, Ui::Id("set.close"), {s.right - 22, s.top + 22}, 30, glyph::Close, 18)) m.CloseSettings();
        if (ScanBtn(ui, t, "set.win", Rect(s.left + 16, s.bottom - 44, Width(s) - 32, 32), false, true, glyph::OpenNew, L"Windows settings", L"")) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:");
        }
        grid = {s.left + 10, s.top + 136, s.right - 10, s.bottom - 54};
    }

    float cw = (Width(grid) - kGap) / 2, ch = (Height(grid) - kGap) / 2; // four instruments
    Material(p, Rect(grid.left, grid.top, cw, ch), live);
    Position(p, Rect(grid.left + cw + kGap, grid.top, cw, ch), live);
    Screens(p, Rect(grid.left, grid.top + ch + kGap, cw, ch), live);
    Notices(p, Rect(grid.left + cw + kGap, grid.top + ch + kGap, cw, ch), live);

    if (ui.RClick(s)) m.CloseSettings(); // right click closes, like the other sheets
    End(p, frame);
}
