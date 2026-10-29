// Settings as a set of small visual instruments: a color orb, material swatches, a miniature monitor to dock on,
// miniature screens, a miniature island. You touch the thing itself instead of reading rows of options.
#include <cmath>
#include <cwchar>
#include <vector>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "version.h"

using namespace sheet;

namespace {

constexpr float kGap = 10;
constexpr int kPresets[] = {0x5B8CFF, 0x9B7BFF, 0xFF6FAE, 0xFF6B6B, 0xFF9F43, 0xF5C542, 0x3DDC97, 0x2EC4D6}; // curated accents
bool ringDrag = false; // dragging around the hue ring
int wheelPick = 0; // custom wheel: the handle being edited, 0 accent, 1 background
float settingsScroll = 0; // wanted scroll of the cards, eased by Scroll
constexpr float kMinRow = 196; // cards never squeeze below this: the page scrolls instead
constexpr float kLyricsRow = 112; // the lyrics card is a single switch, a shorter row
constexpr float kNoteLines = 44; // three short lines of a note beside a small picture
constexpr float kDndW = 44; // the do not disturb pill in the notifications card
constexpr float kDeskRowH = 30; // the island card's desktop switch
constexpr int kSwatchCols = 3; // material swatches per row
constexpr double kWheelHold = 0.4; // seconds the page keeps the wheel after it scrolled
constexpr float kSwatchGap = 10;
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
    return theme::FromCol(m.accentMode == 1 ? Color::Rgb((uint32_t)m.accentHex) : t.accent);
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
            m.hexTarget = 0;
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
    Theme tinted = Theme::From(Seed{cur.hue, cur.chroma, m.Look(), cur.rgb, m.CustomBg()}); // what the theme makes of it
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
    if (m.hexFocus && m.hexTarget == 0) {
        std::wstring shown = L"#" + m.hexEdit;
        c.Text(shown, label, Weight::Medium, 13, ink, Align::Center);
        if (std::fmod(ui.Now(), 1.0) < 0.55) { // caret blinks while typing
            float cx = o.x + shown.size() * 4.3f + 1;
            c.Line({cx, o.y - 8}, {cx, o.y + 8}, ink, 1.5f);
        }
        ui.KeepAlive();
    } else {
        c.Text(custom ? L"#" + HexText(m.accentHex) : std::wstring(m.accentMode == Model::kAccentWindows ? L"Windows" : L"Wallpaper"), label, Weight::Medium, custom ? 13.0f : 11.5f, ink, Align::Center);
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

// ---- custom look: a color wheel with two handles, accent and background, any color at all ----
struct Hsv {
    float h = 0, s = 0, v = 0;
};

Hsv ToHsv(Color c) {
    float mx = (std::max)(c.r, (std::max)(c.g, c.b)), mn = (std::min)(c.r, (std::min)(c.g, c.b)), d = mx - mn;
    Hsv o{0, mx > 0 ? d / mx : 0, mx};
    if (d > 1e-5f) {
        if (mx == c.r) o.h = 60 * std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) o.h = 60 * ((c.b - c.r) / d + 2);
        else o.h = 60 * ((c.r - c.g) / d + 4);
        if (o.h < 0) o.h += 360;
    }
    return o;
}

Color FromHsv(Hsv x) {
    float c = x.v * x.s, hp = std::fmod(x.h, 360.0f) / 60, k = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1)), m = x.v - c;
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = k; }
    else if (hp < 2) { r = k; g = c; }
    else if (hp < 3) { g = c; b = k; }
    else if (hp < 4) { g = k; b = c; }
    else if (hp < 5) { r = k; b = c; }
    else { r = c; b = k; }
    return {r + m, g + m, b + m, 1};
}

ID2D1Bitmap* WheelBmp(ID2D1RenderTarget* rt) { // hue around, saturation outward, at full brightness; made once per device
    static ComPtr<ID2D1Bitmap> bmp;
    static ComPtr<ID2D1Device> owner; // held, so a new device can't reuse its address
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Device> dev;
    if (SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc)))) dc->GetDevice(&dev);
    if (bmp && dev == owner) return bmp.Get();
    bmp.Reset();
    owner = dev;
    constexpr UINT kN = 192;
    std::vector<uint32_t> px(kN * kN);
    for (UINT y = 0; y < kN; ++y)
        for (UINT x = 0; x < kN; ++x) {
            float dx = (x + 0.5f) / kN * 2 - 1, dy = (y + 0.5f) / kN * 2 - 1, r = std::hypot(dx, dy);
            float a = anim::Clamp01((1 - r) * kN / 2); // a soft pixel at the rim
            if (a <= 0) continue;
            Color c = FromHsv({std::fmod(std::atan2(dy, dx) * 180 / kPi + 90 + 360, 360.0f), (std::min)(r, 1.0f), 1});
            auto ch = [a](float v) { return (uint32_t)(anim::Clamp01(v) * a * 255 + 0.5f); }; // premultiplied
            px[y * kN + x] = (uint32_t)(a * 255 + 0.5f) << 24 | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
        }
    rt->CreateBitmap(D2D1::SizeU(kN, kN), px.data(), kN * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return bmp.Get();
}

int& Pick(Model& m, int which) { return which == 0 ? m.accentHex : m.customBg; }

Hsv handles[2]; // each handle's place on the wheel, kept apart from its hex so black doesn't lose its hue
int handleFor[2] = {-1, -1}; // the hex each place belongs to

Hsv Handle(Model& m, int which) {
    int hex = Pick(m, which) & 0xFFFFFF;
    if (handleFor[which] != hex) { // changed elsewhere (typed, or reset): find it on the wheel
        Hsv fresh = ToHsv(Color::Rgb((uint32_t)hex));
        if (fresh.v < 0.01f || fresh.s < 0.01f) fresh.h = handles[which].h; // black or grey has no hue: keep where it was
        if (fresh.v < 0.01f) fresh.s = handles[which].s;
        handles[which] = fresh;
        handleFor[which] = hex;
    }
    return handles[which];
}

void Store(Model& m, int which, Hsv x, bool done) { // live while dragging, saved on release
    int hex = ToHex(FromHsv(x));
    handles[which] = x;
    handleFor[which] = hex;
    if (which == 0) {
        m.accentHex = hex;
        m.accentMode = 1;
        if (done) m.SetAccent(1, hex);
    } else {
        m.customBg = hex;
        if (done) m.SetPref(m.customBg, L"CustomBg", hex);
    }
}

void Wheel(PageCtx& p, D2D1_POINT_2F o, float R, bool live) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    Hsv sel = Handle(m, wheelPick);
    ID2D1RenderTarget* rt = c.Target();
    c.Glow(o, R * 1.4f, R * 1.4f, Color::Rgb((uint32_t)m.accentHex).Alpha(0.18f));
    if (ID2D1Bitmap* bmp = WheelBmp(rt)) rt->DrawBitmap(bmp, {o.x - R, o.y - R, o.x + R, o.y + R}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (sel.v < 0.999f) c.Dot(o, R, Color{0, 0, 0, 1 - sel.v}); // the wheel at the chosen handle's brightness
    c.Ring(o, R, t.hairTop, 1);

    uint32_t id = Ui::Id("set.wheel");
    if (live) ui.Button(id, {o.x - R - 8, o.y - R - 8, o.x + R + 8, o.y + R + 8});
    auto spot = [&](int which) {
        Hsv x = Handle(m, which);
        float a = (x.h - 90) * kPi / 180;
        return D2D1_POINT_2F{o.x + std::cos(a) * x.s * R, o.y + std::sin(a) * x.s * R};
    };
    D2D1_POINT_2F ms = ui.In().mouse;
    if (ui.IsActive(id) && ui.In().pressed) { // grab the handle under the press, or move the selected one here
        for (int w = 0; w < 2; ++w) {
            D2D1_POINT_2F q = spot(w);
            if (std::hypot(ms.x - q.x, ms.y - q.y) < 12) wheelPick = w;
        }
        if (m.hexFocus) m.CommitHex();
    }
    if (ui.IsActive(id) && (ui.In().down || ui.In().released)) {
        float dx = ms.x - o.x, dy = ms.y - o.y;
        Hsv x = Handle(m, wheelPick);
        x.h = std::fmod(std::atan2(dy, dx) * 180 / kPi + 90 + 360, 360.0f);
        x.s = (std::min)(1.0f, std::hypot(dx, dy) / R);
        Store(m, wheelPick, x, ui.In().released);
    }
    for (int k = 0; k < 2; ++k) { // the other handle first, so the one being edited sits on top
        int w = k == 0 ? 1 - wheelPick : wheelPick;
        D2D1_POINT_2F q = spot(w);
        float big = w == wheelPick ? 1.0f : 0.0f;
        Color col = Color::Rgb((uint32_t)(Pick(m, w) & 0xFFFFFF));
        c.Dot({q.x, q.y + 1.5f}, 9 + 2 * big, Color{0, 0, 0, 0.35f}); // lifts off the wheel
        c.Dot(q, 9 + 2 * big, Color{1, 1, 1, 1});
        c.Dot(q, 6.5f + 2 * big, col);
    }
}

void Shade(PageCtx& p, const D2D1_RECT_F& r, bool live) { // brightness of the selected handle: black to its full color
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    Model& m = p.m;
    Hsv x = Handle(m, wheelPick);
    float rad = Height(r) / 2;
    c.FillH(r, rad, Color{0, 0, 0, 1}, FromHsv({x.h, x.s, 1}));
    c.Stroke(r, rad, p.shell.hairTop, 1);
    uint32_t id = Ui::Id("set.shade");
    float v = x.v;
    D2D1_RECT_F track = {r.left + rad, r.top - 4, r.right - rad, r.bottom + 4};
    if (live && ui.Slider(id, track, v, false)) {
        x.v = v;
        Store(m, wheelPick, x, false);
    }
    if (ui.IsActive(id) && ui.In().released) Store(m, wheelPick, Handle(m, wheelPick), true);
    D2D1_POINT_2F k = {track.left + Width(track) * x.v, (r.top + r.bottom) / 2};
    c.Dot(k, rad + 2, Color{1, 1, 1, 1});
    c.Dot(k, rad - 1, FromHsv(x));
}

void Chip(PageCtx& p, const D2D1_RECT_F& r, int which, const wchar_t* name, bool live) { // a handle's color and hex: tap to edit it, tap again to type
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    uint32_t id = Ui::Id("set.chip", which);
    Ui::Hit h = live ? ui.Button(id, r) : Ui::Hit{};
    bool on = wheelPick == which, typing = m.hexFocus && m.hexTarget == which;
    float rad = Height(r) / 2;
    c.Fill(r, rad, on ? t.cardHi.Mix(t.track, 0.6f) : t.cardHi.Alpha(0.6f));
    c.Stroke(r, rad, on || typing ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, on ? 1.5f : 1);
    D2D1_POINT_2F dot = {r.left + rad, (r.top + r.bottom) / 2};
    c.Dot(dot, rad - 5, Color::Rgb((uint32_t)(Pick(m, which) & 0xFFFFFF)));
    c.Ring(dot, rad - 5, t.hairTop, 1);
    D2D1_RECT_F label = {dot.x + rad - 1, r.top, r.right - 6, r.bottom};
    std::wstring hex = L"#" + (typing ? m.hexEdit : HexText(Pick(m, which)));
    c.Text(typing || on ? hex : std::wstring(name), label, Weight::Medium, 11, typing || on ? t.text : t.textDim);
    if (typing) {
        if (std::fmod(ui.Now(), 1.0) < 0.55) { // caret while typing
            float cx = label.left + hex.size() * 6.6f + 1;
            c.Line({cx, dot.y - 7}, {cx, dot.y + 7}, t.text, 1.5f);
        }
        ui.KeepAlive();
        if (ui.In().pressed && !Inside(r, ui.In().mouse)) m.CommitHex(); // clicked away: keep it if it's a color
    }
    if (h.clicked) {
        if (!on) wheelPick = which; // first tap picks the handle
        else if (!typing) { // second tap types its hex
            m.hexFocus = true;
            m.hexTarget = which;
            m.hexEdit = HexText(Pick(m, which));
        }
    }
}

void Lyrics(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a tiny fullscreen player with lyrics beside the cover: tap to turn them on or off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"FULLSCREEN LYRICS");
    Ui::Hit h = live ? ui.Button(Ui::Id("set.lyrics"), in) : Ui::Hit{};
    float on = ui.Smooth(Ui::Id("set.lyrics", 1), m.fullLyrics ? 1.0f : 0, 12);
    float mh = (std::min)(Height(in) - 6, 64.0f), mw = mh / 0.6f;
    D2D1_RECT_F scr = Rect(in.left + 4, in.top + 1, mw, mh);
    c.FillV(scr, 7, t.track.Alpha(0.9f), t.track.Alpha(0.5f));
    c.Stroke(scr, 7, t.hairTop, 1);
    float side = mh * 0.46f, ax = scr.left + mw * (0.5f - 0.27f * on) - side / 2; // the cover slides aside when the lyrics come in
    c.Fill(Rect(ax, scr.top + mh * 0.18f, side, side), 3, t.accent.Alpha(0.85f));
    c.Fill(Rect(ax, scr.top + mh * 0.18f + side + 4, side, 2.5f), 1.2f, t.text.Alpha(0.6f));
    for (int i = 0; i < 4; ++i) { // lines of lyrics, the sung one bright
        float lw = mw * (i == 1 ? 0.36f : 0.28f - 0.03f * (i % 2)), y = scr.top + mh * (0.22f + 0.17f * i);
        c.Fill(Rect(scr.left + mw * 0.52f, y, lw, 3), 1.5f, t.text.Alpha((i == 1 ? 0.9f : 0.35f) * on));
    }
    if (h.hover) c.Stroke(Inset(in, -4, -4), 12, t.accent.Alpha(0.35f), 1.5f);
    float x0 = scr.right + 16;
    c.Text(m.fullLyrics ? L"On" : L"Off", Rect(x0, in.top + 6, in.right - x0, 20), Weight::Medium, 14, m.fullLyrics ? t.accentHi : t.textDim);
    c.Text(m.fullLyrics ? L"Synced lyrics beside\nthe cover, for songs\nfrom music apps" : L"The fullscreen player\nshows just the song", // a narrow column: whole lines, nothing cut
           Rect(x0, in.top + 26, in.right - x0, kNoteLines), Weight::Light, 10.5f, t.textLow);
    if (h.clicked) m.SetPref(m.fullLyrics, L"FullLyrics", m.fullLyrics ? 0 : 1);
}

void Motion(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a tiny spring: bouncy when full, a smooth arrival when calm
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"MOTION");
    Ui::Hit h = live ? ui.Button(Ui::Id("set.motion"), in) : Ui::Hit{};
    float calm = ui.Smooth(Ui::Id("set.motion", 1), m.reduceMotion ? 1.0f : 0, 12);
    float mh = (std::min)(Height(in) - 6, 64.0f), mw = mh / 0.6f;
    D2D1_RECT_F scr = Rect(in.left + 4, in.top + 1, mw, mh);
    c.Fill(scr, 7, t.track.Alpha(0.7f));
    c.Stroke(scr, 7, t.hairTop, 1);
    D2D1_POINT_2F last{};
    for (int i = 0; i <= 24; ++i) { // where the panel's edge goes over time
        float x = i / 24.0f, ease = 1 - std::exp(-5 * x), bounce = 0.38f * std::sin(11 * x) * std::exp(-3.5f * x) * (1 - calm);
        D2D1_POINT_2F pt = {scr.left + 6 + x * (mw - 12), scr.bottom - 8 - (ease + bounce) * (mh - 22)};
        if (i) c.Line(last, pt, t.accentHi, 2);
        last = pt;
    }
    if (h.hover) c.Stroke(Inset(in, -4, -4), 12, t.accent.Alpha(0.35f), 1.5f);
    float x0 = scr.right + 16;
    c.Text(m.reduceMotion ? L"Reduced" : L"Full", Rect(x0, in.top + 6, in.right - x0, 20), Weight::Medium, 14, m.reduceMotion ? t.accentHi : t.textDim);
    c.Text(m.reduceMotion ? L"No bounce or drifting,\npages fade in place\nof sliding" : L"Springy drops,\nsliding pages,\ndrifting orbits",
           Rect(x0, in.top + 26, in.right - x0, kNoteLines), Weight::Light, 10.5f, t.textLow);
    if (h.clicked) m.SetPref(m.reduceMotion, L"ReduceMotion", m.reduceMotion ? 0 : 1);
}

void Swatch(Canvas& c, const Theme& look, D2D1_RECT_F q, bool glassy, float hue, float sw, float sh) { // one material, drawn in itself
    if (glassy) { // glass: color under frost, a light streak, a bright rim, all inside its rounded corners
        ComPtr<ID2D1RoundedRectangleGeometry> round;
        if (FAILED(gfx::D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(q, 12, 12), &round))) return;
        c.Target()->PushLayer(D2D1::LayerParameters(q, round.Get()), nullptr);
        c.Dot({q.left + sw * 0.3f, q.top + sh * 0.7f}, sh * 0.55f, look.accent.Alpha(0.65f));
        c.Dot({q.right - sw * 0.25f, q.top + sh * 0.25f}, sh * 0.45f, theme::Hsl(hue + 60, 0.7f, 0.6f, 0.55f));
        c.FillV(q, 12, Color{1, 1, 1, 0.22f}, Color{1, 1, 1, 0.06f});
        c.Line({q.left + sw * 0.15f, q.bottom + 6}, {q.left + sw * 0.75f, q.top - 6}, Color{1, 1, 1, 0.22f}, 9);
        c.Target()->PopLayer();
        c.Stroke(q, 12, Color{1, 1, 1, 0.40f}, 1.2f);
        return;
    }
    c.FillV(q, 12, look.cardHi, look.bg.Alpha(1));
    float u = (std::min)(1.0f, sh / 60); // the little page inside scales with the swatch
    c.Fill(Rect(q.left + 10, q.top + 10 * u, sw * 0.5f, 5), 2.5f, look.text.Alpha(0.85f)); // a line of "text"
    c.Fill(Rect(q.left + 10, q.top + 20 * u, sw * 0.32f, 4), 2, look.textDim.Alpha(0.8f));
    c.Fill(Rect(q.left + 10, q.bottom - 12 * u - 4, sw - 20, 5), 2.5f, look.accent); // and an accent bar
    c.StrokeV(q, 12, look.hairTop, look.hairBot, 1);
}

void Material(PageCtx& p, const D2D1_RECT_F& r, bool live) { // five swatches drawn in the actual materials, halobar's in whatever it wears
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"MATERIAL");
    bool glass = m.look == 2;
    Seed s = Base(m, t);
    constexpr float extra = kSlideH + 6; // room for the blur slider, or the opacity slider on the solid looks
    float sw = (Width(in) - 2 * kSwatchGap) / kSwatchCols, sh = (std::min)((Height(in) - extra - 2 * 18 - 6) / 2, sw * 0.62f); // three, then the rest, a label under each
    static const wchar_t* const kNames[] = {L"Dark", L"Light", L"Glass", L"Custom", L"HaloBar"};
    int looks = m.halo.FollowsUs() && !m.Following() ? Model::kLookHalo : Model::kLookHalo + 1; // halobar following us: no swatch to follow it back, unless already picked
    for (int i = 0; i < looks; ++i) {
        int row = i / kSwatchCols, col = i % kSwatchCols, inRow = (std::min)(kSwatchCols, looks - row * kSwatchCols);
        float x0 = in.left + (kSwatchCols - inRow) * (sw + kSwatchGap) / 2; // a shorter second row sits centered
        D2D1_RECT_F q = Rect(x0 + col * (sw + kSwatchGap), in.top + 2 + row * (sh + 24), sw, sh);
        uint32_t id = Ui::Id("set.mat", i);
        Ui::Hit h = live ? ui.Button(id, q) : Ui::Hit{};
        float on = ui.Smooth(id + 10, m.look == i ? 1.0f : 0, 14), hov = ui.Smooth(id + 20, h.hover ? 1.0f : 0, 16);
        q = Inset(q, -2 * on, -2 * on); // the chosen one lifts
        int mat = i, bg = m.customBg;
        Seed seed = s;
        if (i == Model::kLookHalo) { // what following halobar would look like right now
            mat = m.halo.look >= 0 ? m.halo.look : 0; // it follows us back: dark
            if (m.halo.look == 3) bg = m.halo.customBg;
            if (m.halo.accent == 1) seed = theme::FromCol(Color::Rgb((uint32_t)m.halo.accentHex));
        }
        Theme look = Theme::From(Seed{seed.hue, seed.chroma, mat, seed.rgb, bg});
        if (on > 0.01f) c.Glow({(q.left + q.right) / 2, (q.top + q.bottom) / 2}, sw, sh, t.accent.Alpha(0.20f * on));
        Swatch(c, look, q, mat == 2, seed.hue, sw, sh);
        if (on > 0.01f || hov > 0.01f) c.Stroke(Inset(q, -3, -3), 14, t.accent.Alpha((std::max)(on, 0.5f * hov)), 2);
        c.Text(kNames[i], Rect(q.left - 4, q.bottom + 3, Width(q) + 8, 14), Weight::Medium, 11, t.textDim.Mix(t.text, on), Align::Center);
        if (h.clicked && m.look != i) {
            if (i == 3 && m.accentMode != 1) m.SetAccent(1, ToHex(t.accent)); // the wheel starts from the accent you're looking at
            m.SetLook(i); // drawer crossfades, glass swaps its pipeline
        }
    }
    if (m.Following()) { // halobar's look: its blur and opacity are tuned there, and shared, so no slider here that does nothing
        c.Text(m.Look() == 2 ? L"Blur follows HaloBar" : L"Opacity follows HaloBar", Rect(in.left, in.bottom - 30, Width(in), 16), Weight::Medium, 11, t.textLow, Align::Center);
        return;
    }
    { // glass: how frosted, between clear glass and windows' full backdrop blur; the solid looks: how much desktop shows through
        int& field = glass ? m.glassBlur : m.bgOpacity;
        int lo = glass ? 0 : Model::kMinOpacity;
        float v = (field - lo) / (float)(100 - lo);
        bool done = false;
        if (Knob(ui, t, Ui::Id(glass ? "set.blur" : "set.opacity"), {in.left, in.bottom - kSlideH, in.right, in.bottom}, glass ? L"Blur" : L"Opacity",
                 std::to_wstring(field) + L"%", v, live, done))
            field = lo + (int)(v * (100 - lo) + 0.5f); // live preview
        if (done) m.SetPref(field, glass ? L"GlassBlur" : L"BgOpacity", field);
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
        bool taken = m.haloEdge == e; // halobar's dock is there: not offered
        Ui::Hit h = live && !taken ? ui.Button(id, hot) : Ui::Hit{};
        float on = ui.Smooth(id + 10, m.Edge() == e ? 1.0f : 0, 14), hov = h.hover ? 1.0f : 0;
        Color col = taken ? t.textDim.Alpha(0.18f) : t.accent.Alpha(0.25f + 0.75f * on).Mix(t.text.Alpha(0.5f), hov * (1 - on));
        float d = 4 + (e == 0 ? mh * 0.30f : mw * 0.22f) * on; // the docked edge grows into a little panel
        D2D1_RECT_F bar = e == 0 ? D2D1_RECT_F{scr.left + mw * 0.3f, scr.top + 3, scr.right - mw * 0.3f, scr.top + 3 + d}
                        : e == 1 ? D2D1_RECT_F{scr.left + 3, scr.top + mh * 0.22f, scr.left + 3 + d, scr.bottom - mh * 0.22f}
                                 : D2D1_RECT_F{scr.right - 3 - d, scr.top + mh * 0.22f, scr.right - 3, scr.bottom - mh * 0.22f};
        c.Fill(bar, 3, col);
        if (h.clicked && m.edge != e) m.SetPref(m.edge, L"Edge", e); // panel retracts and re-docks
    }
    static const wchar_t* const kNames[] = {L"Top edge", L"Left edge", L"Right edge"};
    c.Text(m.edge != m.Edge() ? std::wstring(kNames[m.Edge()]) + L" · HaloBar " + (m.edge == 0 ? L"top" : m.edge == 1 ? L"left" : L"right") : std::wstring(kNames[m.Edge()]), Rect(in.left, in.bottom - 14, Width(in), 14), Weight::Medium, 11, t.textDim, Align::Center);
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
        D2D1_RECT_F dock = m.Edge() == 0 ? D2D1_RECT_F{mx - Width(q) * 0.18f, q.top + 3, mx + Width(q) * 0.18f, q.top + 6}
                         : m.Edge() == 1 ? D2D1_RECT_F{q.left + 3, my - Height(q) * 0.2f, q.left + 6, my + Height(q) * 0.2f}
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
    std::wstring label = list.size() <= 1 ? L"One screen" : m.screens ? L"Every screen · stays put while open" : L"Screen " + std::to_wstring(shown); // open, it never jumps to the screen the mouse moves to
    c.Text(label, Rect(in.left, in.bottom - 16, Width(in) - 48, 14), Weight::Medium, 11, t.textDim);
}

void Notices(PageCtx& p, const D2D1_RECT_F& r, bool live) { // a little screen with a notification dropping in as a pill
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"NOTIFICATIONS");
    D2D1_RECT_F dnd = {in.right - kDndW, in.bottom - 18, in.right, in.bottom}; // do not disturb, beside the label like the screens card's "all"
    Ui::Hit dh = live ? ui.Button(Ui::Id("set.dnd"), dnd) : Ui::Hit{}; // first, so a press on it isn't the card's
    Ui::Hit h = live ? ui.Button(Ui::Id("set.notice"), in) : Ui::Hit{};
    h.hover = h.hover && !dh.hover;
    float on = ui.Smooth(Ui::Id("set.notice", 1), m.PillsWanted() ? 1.0f : 0, 12); // halobar's handed-over pills show here too
    float quiet = ui.Smooth(Ui::Id("set.dnd", 1), m.quiet ? 1.0f : 0, 14);
    float mw = (std::min)(Width(in) - 40, (Height(in) - 34) / 0.6f), mh = mw * 0.6f;
    D2D1_RECT_F scr = Rect((in.left + in.right - mw) / 2, in.top + 4, mw, mh);
    Monitor(c, t, scr);
    float pw = mw * (0.30f + 0.22f * on), ph = 7 + 7 * on; // the pill grows when it carries a notification
    D2D1_RECT_F pill = Rect((scr.left + scr.right - pw) / 2, scr.top + 3, pw, ph);
    c.Fill(pill, ph / 2, Color{0.02f, 0.02f, 0.03f, 0.95f});
    if (on > 0.05f) {
        c.Dot({pill.left + ph / 2 + 1, (pill.top + pill.bottom) / 2}, ph * 0.26f, t.accent.Alpha(on));
        c.Fill(Rect(pill.left + ph + 2, pill.top + ph * 0.38f, pw * 0.5f, ph * 0.24f), ph * 0.12f, Color{1, 1, 1, 0.75f * on});
    }
    if (h.hover) c.Stroke(Inset(in, -4, -4), 12, t.accent.Alpha(0.35f), 1.5f);
    const wchar_t* label = m.quiet ? L"Do not disturb" : m.notifyPills ? L"Shown as pills" : m.haloPills ? L"Pills for HaloBar" : L"Off"; // halobar hands its pills over while we run
    c.Text(label, Rect(in.left, in.bottom - 16, Width(in) - kDndW - 4, 14), Weight::Medium, 11, m.PillsWanted() ? t.accentHi : t.textDim);
    c.Fill(dnd, 9, t.track.Alpha(0.8f).Mix(t.accent, quiet));
    c.Icon(glyph::Dnd, {(dnd.left + dnd.right) / 2, (dnd.top + dnd.bottom) / 2}, 14, t.textDim.Mix(t.onAccent, quiet).Mix(t.text, dh.hover ? 0.4f * (1 - quiet) : 0), quiet > 0.5f);
    if (dh.clicked) m.SetQuiet(!m.quiet); // shared: halobar goes quiet too
    if (h.clicked) m.SetNotifyPills(m.notifyPills == 0);
}

constexpr float kAlwaysAt = 0.96f; // the last stretch of the island slider means always

float StayToSlider(int secs) { // finer at the short end: 4 s and 7 s are easy to hit, minutes are a longer reach
    if (secs < 0) return 1.0f;
    return kAlwaysAt * std::pow(secs / (float)Model::kIslandMaxSecs, 1 / 2.2f);
}

int SliderToStay(float v) {
    if (v >= kAlwaysAt + 0.01f) return -1;
    return (int)std::lround(Model::kIslandMaxSecs * std::pow(anim::Clamp01(v / kAlwaysAt), 2.2f));
}

std::wstring StayText(int secs) {
    if (secs < 0) return L"Always";
    if (secs == 0) return L"Brief";
    if (secs < 60) return std::to_wstring(secs) + L" s";
    return std::to_wstring(secs / 60) + L" min" + (secs % 60 ? L" " + std::to_wstring(secs % 60) + L" s" : L"");
}

void Island(PageCtx& p, const D2D1_RECT_F& r, bool live) { // the pill over a maximized window: how long it stays once the mouse leaves
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"ISLAND");
    float mh = (std::min)(Height(in) - 6, Width(in) * 0.26f), mw = mh / 0.6f; // a little screen with an app filling it
    D2D1_RECT_F scr = Rect(in.left + 4, (in.top + in.bottom - mh) / 2 - 3, mw, mh);
    Monitor(c, t, scr);
    c.FillV(Inset(scr, 3, 3), 4, t.cardHi.Mix(t.track, 0.5f), t.card); // the maximized window
    float always = ui.Smooth(Ui::Id("set.stay", 9), m.IslandAlways() ? 1.0f : 0, 12);
    float pw = mw * 0.40f, ph = (std::max)(5.0f, mh * 0.10f);
    D2D1_RECT_F pill = Rect((scr.left + scr.right - pw) / 2, scr.top + 3, pw, ph);
    c.Fill(pill, ph / 2, Color{0.02f, 0.02f, 0.03f, 0.95f});
    c.Dot({pill.right - ph * 0.9f, (pill.top + pill.bottom) / 2}, ph * 0.22f, t.accent.Alpha(0.5f + 0.5f * always)); // its little visualizer, lit when it stays

    float x0 = scr.right + 18, rw = in.right - x0 - 2; // slider, note, then the desktop switch, as one block in the middle
    float y = (in.top + in.bottom) / 2 - (kSlideH + 8 + 30 + 6 + kDeskRowH) / 2;
    float v = StayToSlider(m.islandSecs);
    bool done = false;
    if (Knob(ui, t, Ui::Id("set.stay"), Rect(x0, y, rw, kSlideH), L"Stays", StayText(m.islandSecs), v, live, done, 0.04f))
        m.islandSecs = SliderToStay(v); // any time from a moment to five minutes, or always at the end
    if (done) m.SetPref(m.islandSecs, L"IslandSecs", m.islandSecs);
    y += kSlideH + 8;
    c.Text(m.IslandAlways() ? L"Up whenever music plays" : L"Over maximized windows,\nafter you move away", Rect(x0, y, rw, 30), Weight::Light, 10.5f, t.textLow);
    D2D1_RECT_F desk = Rect(x0, y + 36, rw, kDeskRowH); // off: on the desktop a hover drops the whole panel, as before
    Ui::Hit h = live ? ui.Button(Ui::Id("set.desk", 1), {desk.left, desk.top, desk.right - 52, desk.bottom}) : Ui::Hit{}; // the words flip it too
    c.Text(L"Pill on the desktop", {desk.left, desk.top, desk.right - 56, desk.bottom}, Weight::Regular, 11.5f, h.hover ? t.text : t.text.Mix(t.textDim, 0.25f));
    if ((w::Switch(ui, t, Ui::Id("set.desk"), {desk.right - 24, (desk.top + desk.bottom) / 2}, m.pillDesk != 0, live) || h.clicked) && live)
        m.SetPref(m.pillDesk, L"PillDesktop", m.pillDesk ? 0 : 1); // shown while no app is under it, even with stays short of always
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
    static const wchar_t* const kSources[] = {L"Wallpaper", L"Windows", L"Custom"};
    static const int kSourceMode[] = {0, Model::kAccentWindows, 1}; // segment to accent mode
    int segNow = m.accentMode == 1 ? 2 : m.accentMode == Model::kAccentWindows ? 1 : 0;

    D2D1_RECT_F grid;
    if (!frame.tall) { // hero column: the color orb
        float hx = s.left + kHero / 2;
        c.TextSp(std::wstring(L"ASTRAEUS  ·  ") + APP_VER_W, Rect(s.left + 10, s.top + 16, kHero - 20, 12), Weight::Medium, 9.5f, t.textLow, 1.4f,
                 Align::Center);
        if (m.look == 3) { // custom: any accent and any background, straight off the wheel
            Wheel(p, {hx, s.top + 98}, 62, live);
            Chip(p, Rect(hx - 96, s.top + 176, 94, 26), 0, L"Accent", live);
            Chip(p, Rect(hx + 2, s.top + 176, 94, 26), 1, L"Background", live);
            Shade(p, Rect(hx - 90, s.top + 218, 180, 12), live);
        } else {
            Orb(p, {hx, s.top + 98}, 56, live);
            int v = Segmented(ui, t, "set.accent", Rect(hx - 100, s.top + 172, 200, 24), kSources, 3, segNow, live);
            if (v >= 0) m.SetAccent(kSourceMode[v], m.accentHex);
            Presets(p, hx - 84, s.top + 214, 24, live);
        }
        if (ScanBtn(ui, t, "set.win", Rect(s.left + 28, s.bottom - 44, kHero - 56, 32), false, true, glyph::OpenNew, L"Windows settings", L"")) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:");
        }
        D2D1_RECT_F list = frame.list;
        c.TextSp(L"SETTINGS", Rect(list.left + 4, list.top + 8, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
        if (w::IconBtn(ui, t, Ui::Id("set.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseSettings();
        grid = {list.left, list.top + 34, list.right, list.bottom};
    } else { // sidebar header: orb on the left, source switch and presets beside it
        float x0 = s.left + 130, w0 = (std::min)(Width(s) - 176, 190.0f);
        if (m.look == 3) {
            Wheel(p, {s.left + 66, s.top + 66}, 46, live);
            Chip(p, Rect(x0, s.top + 26, (w0 - 6) / 2, 26), 0, L"Accent", live);
            Chip(p, Rect(x0 + (w0 + 6) / 2, s.top + 26, (w0 - 6) / 2, 26), 1, L"Background", live);
            Shade(p, Rect(x0 + 4, s.top + 72, w0 - 8, 12), live);
            c.Text(L"Tap a color twice to type a hex", Rect(x0, s.top + 96, Width(s) - 146, 16), Weight::Light, 10.5f, t.textLow);
        } else {
            Orb(p, {s.left + 66, s.top + 66}, 44, live);
            int v = Segmented(ui, t, "set.accent", Rect(x0, s.top + 28, (std::min)(Width(s) - 176, 200.0f), 24), kSources, 3, segNow, live);
            if (v >= 0) m.SetAccent(kSourceMode[v], m.accentHex);
            Presets(p, x0 + 10, s.top + 76, 22, live);
            c.Text(L"Tap the orb to type a hex", Rect(x0, s.top + 96, Width(s) - 146, 16), Weight::Light, 10.5f, t.textLow);
        }
        if (w::IconBtn(ui, t, Ui::Id("set.close"), {s.right - 22, s.top + 22}, 30, glyph::Close, 18)) m.CloseSettings();
        if (ScanBtn(ui, t, "set.win", Rect(s.left + 16, s.bottom - 44, Width(s) - 32, 32), false, true, glyph::OpenNew, L"Windows settings", L"")) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:");
        }
        grid = {s.left + 10, s.top + 136, s.right - 10, s.bottom - 54};
    }

    int rows = frame.tall ? 3 : 2;
    float ch = (std::max)(kMinRow, (Height(grid) - rows * kGap - kLyricsRow) / rows); // short page: cards keep their size and the page scrolls
    static float prefsH = 0; // the newer cards' height, as they measured last frame
    float content = rows * (ch + kGap) + kLyricsRow + (frame.tall ? kLyricsRow + kGap : 0) + kGap + prefsH; // the sidebar stacks lyrics and motion
    static double scrolledAt = -10; // a scroll in flight keeps the wheel even as a slider passes under the mouse
    bool pageWheel = !OverSlider(ui) || ui.Now() - scrolledAt < kWheelHold;
    if (pageWheel && ui.In().wheel != 0 && Inside(grid, ui.In().mouse)) scrolledAt = ui.Now();
    float scroll = Scroll(ui, "set.scroll", settingsScroll, grid, content, pageWheel);
    D2D1_RECT_F view = grid;
    live = live && (Inside(view, ui.In().mouse) || ui.Captured()); // cards scrolled out of view can't be hit through the header; a slider being dragged keeps following past it
    c.PushClip(view);
    grid.top -= scroll;
    if (!frame.tall) { // five instruments: three across, then notifications and a wide island
        float cw = (Width(grid) - 2 * kGap) / 3;
        Material(p, Rect(grid.left, grid.top, cw, ch), live);
        Position(p, Rect(grid.left + cw + kGap, grid.top, cw, ch), live);
        Screens(p, Rect(grid.left + 2 * (cw + kGap), grid.top, cw, ch), live);
        Notices(p, Rect(grid.left, grid.top + ch + kGap, cw, ch), live);
        Island(p, Rect(grid.left + cw + kGap, grid.top + ch + kGap, 2 * cw + kGap, ch), live);
        float half = (Width(grid) - kGap) / 2;
        Lyrics(p, Rect(grid.left, grid.top + 2 * (ch + kGap), half, kLyricsRow), live);
        Motion(p, Rect(grid.left + half + kGap, grid.top + 2 * (ch + kGap), half, kLyricsRow), live);
        prefsH = DrawPrefs(p, {grid.left, grid.top + 2 * (ch + kGap) + kLyricsRow + kGap, grid.right, grid.bottom}, live, false);
    } else { // sidebar: two by two, the island across the bottom
        float cw = (Width(grid) - kGap) / 2;
        Material(p, Rect(grid.left, grid.top, cw, ch), live);
        Position(p, Rect(grid.left + cw + kGap, grid.top, cw, ch), live);
        Screens(p, Rect(grid.left, grid.top + ch + kGap, cw, ch), live);
        Notices(p, Rect(grid.left + cw + kGap, grid.top + ch + kGap, cw, ch), live);
        Island(p, Rect(grid.left, grid.top + 2 * (ch + kGap), Width(grid), ch), live);
        Lyrics(p, Rect(grid.left, grid.top + 3 * (ch + kGap), Width(grid), kLyricsRow), live);
        Motion(p, Rect(grid.left, grid.top + 3 * (ch + kGap) + kLyricsRow + kGap, Width(grid), kLyricsRow), live);
        prefsH = DrawPrefs(p, {grid.left, grid.top + 3 * (ch + kGap) + 2 * (kLyricsRow + kGap), grid.right, grid.bottom}, live, true);
    }
    c.PopClip();
    Fades(c, t, view, scroll, content);

    if (ui.RClick(s)) m.CloseSettings(); // right click closes, like the other sheets
    End(p, frame);
}
