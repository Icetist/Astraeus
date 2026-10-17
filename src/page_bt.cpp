#include <algorithm>
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

constexpr float kInner = 0.42f, kMiddle = 0.74f, kOuter = 1.0f; // orbits: connected close in, paired further out, new finds at the edge
constexpr float kTurn[3] = {2 * kPi / 80, -2 * kPi / 120, 2 * kPi / 160}; // each orbit's slow drift, radians a second; the middle one runs the other way
float orbitTime = 0; // how far the orbits have drifted, in seconds of motion

int IconOf(int kind) {
    switch (kind) {
    case sys::kBtAudio: return glyph::Headphones;
    case sys::kBtKeys: return glyph::Keys;
    case sys::kBtMouse: return glyph::Mouse;
    case sys::kBtPhone: return glyph::Phone;
    case sys::kBtPc: return glyph::Pc;
    case sys::kBtPad: return glyph::Pad;
    case sys::kBtWatch: return glyph::Watch;
    default: return glyph::Devices;
    }
}

const wchar_t* KindName(int kind) {
    switch (kind) {
    case sys::kBtAudio: return L"Audio";
    case sys::kBtKeys: return L"Keyboard";
    case sys::kBtMouse: return L"Mouse";
    case sys::kBtPhone: return L"Phone";
    case sys::kBtPc: return L"Computer";
    case sys::kBtPad: return L"Controller";
    case sys::kBtWatch: return L"Wearable";
    default: return L"Device";
    }
}

bool Tab(Ui& ui, const Theme& t, const char* idName, const D2D1_RECT_F& r, const wchar_t* label, bool on) { // list header tab
    uint32_t id = Ui::Id(idName);
    Ui::Hit h = ui.Button(id, r);
    float lit = ui.Smooth(id, on ? 1.0f : 0, 14);
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    Canvas& c = ui.C();
    c.TextSp(label, {r.left + 4, r.top + 8, r.right, r.top + 22}, Weight::Medium, 10, t.textLow.Mix(t.text, 0.5f * hover).Mix(t.accent, lit), 1.4f);
    return h.clicked;
}

float TabW(const wchar_t* label) { // a tab label's drawn width, letter spacing included
    size_t n = std::wcslen(label);
    float w = n * 7.0f;
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(label, (UINT32)n, gfx::TextFmt(Weight::Medium, 10, Align::Left), 400, 20, &lay)) && SUCCEEDED(lay->GetMetrics(&tm)))
        w = tm.widthIncludingTrailingWhitespace;
    return w + 1.4f * (n - 1);
}

void TabLine(Ui& ui, const Theme& t, float top, float left, float width) { // one slim marker that glides to the active tab, as wide as its word
    float l = ui.Smooth(Ui::Id("bt.tabline", 0), left, 18), w = ui.Smooth(Ui::Id("bt.tabline", 1), width, 18);
    ui.C().Fill(Rect(l, top, w, 2), 1, t.accent);
}

void Badge(Ui& ui, const Theme& t, D2D1_POINT_2F o, float r, const sys::BtDev& d, bool busy, float lit) {
    Canvas& c = ui.C();
    c.Dot(o, r, t.track.Mix(t.accent.Alpha(0.28f), lit));
    float ring = r + 4.5f;
    if (d.battery >= 0) { // the charge, drawn around the device itself
        Color col = d.battery <= 20 ? kDanger : t.accent;
        c.Ring(o, ring, t.track, 2.5f);
        if (d.battery > 0) c.Arc(o, ring, -kPi / 2, d.battery / 100.0f * 2 * kPi, col, 2.5f);
    }
    if (busy) { // connecting or pairing: a sweep runs around the icon
        float a = (float)std::fmod(ui.Now() * 1.4, 1.0) * 2 * kPi;
        c.Arc(o, ring, a - kPi / 2, kPi * 0.55f, t.accentHi, 2.5f);
        ui.KeepAlive();
    }
    c.Icon(IconOf(d.kind), o, r * 1.12f, t.textDim.Mix(t.accentHi, lit), d.on);
}

std::wstring Charge(const sys::BtDev& d) { return d.battery >= 0 ? std::to_wstring(d.battery) + L"%" : L""; }

bool Busy(const sys::BtDev& d, uint64_t nowMs) { return d.pairing || (d.until > nowMs && d.on != d.want); } // pending until state flips or deadline

void Forget(PageCtx& p, uint32_t id, D2D1_POINT_2F fc, bool shown, bool live, const sys::BtDev& d) { // press and hold, so it can't happen by accident
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    D2D1_RECT_F fr = {fc.x - 15, fc.y - 15, fc.x + 15, fc.y + 15};
    float prog = 0;
    bool fire = live && ui.Hold(id + 4, fr, 0.8f, prog);
    float show = ui.Smooth(id + 6, shown || prog > 0 ? 1.0f : 0, 14);
    if (show > 0.01f) {
        bool fh = live && Inside(fr, ui.In().mouse);
        if (fh) c.Dot(fc, 15, kDanger.Alpha(0.12f * show));
        c.Icon(glyph::Delete, fc, 18, (fh ? kDanger : p.shell.textLow).Alpha(show));
        if (prog > 0) c.Arc(fc, 14, -kPi / 2, prog * 2 * kPi, kDanger, 2); // hold progress from 12 o'clock
    }
    if (fire) p.m.bt.Forget(d.id); // unpair, it vanishes via the watcher
}

const wchar_t* Doing(const sys::BtDev& d) { // what's underway, for the line under its name
    if (d.pairing) return L"Pairing\u2026";
    return d.want ? L"Connecting\u2026" : L"Disconnecting\u2026";
}

void Bubble(PageCtx& p, D2D1_POINT_2F at, float r, const sys::BtDev& d, float appear, bool live, uint64_t nowMs, bool above) { // a device in orbit: click it to connect, disconnect or pair; above: its words go over it, away from the middle
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    uint32_t id = Ui::Id("bt.orb", Key(d.id));
    bool busy = Busy(d, nowMs);
    D2D1_RECT_F hitR = {at.x - r - 6, at.y - r - 6, at.x + r + 6, at.y + r + 6};
    bool nearby = live && Inside({hitR.left, hitR.top - 8, hitR.right + 36, hitR.bottom}, ui.In().mouse);
    if (d.paired) Forget(p, id, {at.x + r + 18, at.y - r + 2}, nearby, live, d); // beside the bubble, never under its click
    Ui::Hit h = live && !busy ? ui.Button(id, hitR) : Ui::Hit{};
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    float lit = ui.Smooth(id + 2, d.on ? 1.0f : 0, 10);
    bool layer = appear < 0.995f;
    if (layer) {
        c.Target()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), appear), nullptr);
        ui.KeepAlive();
    }
    if (hover > 0.01f) c.Glow(at, r * 2.2f, r * 2.2f, t.accent.Alpha(0.16f * hover));
    Badge(ui, t, at, r * (1 + 0.08f * hover), d, busy, (std::max)(lit, 0.35f * hover));
    float nameY = above ? at.y - r - 40 : at.y + r + 8, lineY = above ? at.y - r - 24 : at.y + r + 24; // the words sit on the side away from this pc
    c.Text(d.name, Rect(at.x - 64, nameY, 128, 16), Weight::Medium, 11.5f, t.textDim.Mix(t.text, (std::max)(lit, hover)), Align::Center);
    std::wstring line = busy ? std::wstring(Doing(d))
                      : hover > 0.5f ? std::wstring(!d.paired ? L"Pair" : d.on ? L"Disconnect" : L"Connect")
                                     : d.battery >= 0 ? Charge(d) : std::wstring(KindName(d.kind));
    Color ink = busy || hover > 0.5f ? t.accentHi : d.battery >= 0 && d.battery <= 20 ? kDanger : t.textLow;
    c.Text(line, Rect(at.x - 64, lineY, 128, 14), Weight::Light, 10.5f, ink, Align::Center);
    if (layer) c.Target()->PopLayer();
    if (h.clicked) { // the device itself is the button
        if (d.paired) p.m.bt.Connect(d.id, !d.on);
        else p.m.bt.Pair(d.id);
        p.m.OnBt(); // the sweep starts this frame
    }
}

void Orbit(PageCtx& p, const D2D1_RECT_F& v) { // this pc in the middle, its devices around it: connected close, paired further, new finds at the edge
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    uint64_t nowMs = GetTickCount64();
    bool live = Inside(v, ui.In().mouse);
    D2D1_POINT_2F o = {(v.left + v.right) / 2, (v.top + v.bottom) / 2 - 8};
    float ax = Width(v) / 2 - 64, ay = Height(v) / 2 - 44; // room for a bubble and its name at the edge

    std::vector<const sys::BtDev*> rings[3]; // connected, paired, found
    for (const auto& d : m.btDevs) rings[d.paired ? (d.on ? 0 : 1) : 2].push_back(&d);
    const float frac[3] = {kInner, kMiddle, kOuter};
    bool outer = m.btScan || !rings[2].empty();

    ComPtr<ID2D1SolidColorBrush> guide; // faint orbits, ellipses so they use the panel's width
    if (SUCCEEDED(c.Target()->CreateSolidColorBrush(t.text.Alpha(0.07f).D2D(), &guide)))
        for (int i = 0; i < (outer ? 3 : 2); ++i) {
            float rx = ax * frac[i], ry = ay * frac[i];
            if (i == 0) rx = (std::max)(rx, 30.0f + 22 + 70), ry = (std::max)(ry, 30.0f + 22 + 14); // same clearance the devices keep
            c.Target()->DrawEllipse(D2D1::Ellipse(o, rx, ry), guide.Get(), 1);
        }

    if (!p.m.reduceMotion) orbitTime += ui.Dt(); // always turning, slow enough that a bubble is easy to click; calm: still
    ui.KeepAlive(); // the drift and the core: the ambient rate is plenty, and only while this sheet is open

    struct Place { const sys::BtDev* d; D2D1_POINT_2F at; float r, appear; };
    std::vector<Place> placed;
    for (int ring = 0; ring < 3; ++ring) {
        size_t n = rings[ring].size();
        for (size_t i = 0; i < n; ++i) {
            const sys::BtDev& d = *rings[ring][i];
            float ang = (float)i / n * 2 * kPi + (ring == 1 ? kPi / n : ring == 2 ? 0.35f : 0) + orbitTime * kTurn[ring]; // rings offset so they don't line up, each drifting round
            float rx = ax * frac[ring], ry = ay * frac[ring];
            if (ring == 0) { // the inner orbit keeps clear of this pc: its disc, a bubble and its words
                rx = (std::max)(rx, 30.0f + 22 + 70);
                ry = (std::max)(ry, 30.0f + 22 + 14);
            }
            D2D1_POINT_2F want = {o.x + std::cos(ang) * rx, o.y + std::sin(ang) * ry};
            int key = Key(d.id);
            float push = d.paired ? 0.0f : 40.0f; // new finds drift in from beyond the edge
            D2D1_POINT_2F at = {ui.SmoothFrom(Ui::Id("bt.ox", key), want.x, want.x + std::cos(ang) * push, 7),
                                ui.SmoothFrom(Ui::Id("bt.oy", key), want.y, want.y + std::sin(ang) * push, 7)}; // glides to a new orbit when it connects
            float appear = d.paired ? 1.0f : ui.SmoothFrom(Ui::Id("bt.oa", key), 1.0f, 0.0f, 6);
            placed.push_back({&d, at, ring == 0 ? 22.0f : 18.0f, appear});
        }
    }

    for (const Place& pl : placed) { // links to this pc: steady when connected, a pulse running along it while connecting
        bool busy = Busy(*pl.d, nowMs);
        if (!pl.d->on && !busy) continue;
        c.Line(o, pl.at, t.accent.Alpha(pl.d->on ? 0.35f : 0.15f), 2);
        if (busy) {
            float f = (float)std::fmod(ui.Now() * 1.1, 1.0);
            if (!pl.d->want) f = 1 - f; // disconnecting: the pulse runs home
            c.Dot({o.x + (pl.at.x - o.x) * f, o.y + (pl.at.y - o.y) * f}, 3.5f, t.accentHi);
            ui.KeepAlive();
        }
    }

    double now = ui.Now(); // the core: a breathing glow, arcs turning round it, ripples running out while it searches
    bool linked = !rings[0].empty(), scan = m.btScan;
    float breath = 0.5f + 0.5f * (float)std::sin(now * 1.6);
    c.Glow(o, 78 + 14 * breath, 68 + 12 * breath, t.accent.Alpha((linked ? 0.22f : 0.14f) + 0.07f * breath));
    if (scan && guide) // ripples from the core out to the edge of the orbit
        for (int k = 0; k < 2; ++k) {
            float f = (float)std::fmod(now * 0.35 + k * 0.5, 1.0);
            guide->SetColor(t.accent.Alpha(0.28f * (1 - f)).D2D());
            c.Target()->DrawEllipse(D2D1::Ellipse(o, 32 + (ax - 32) * f, 32 + (ay - 32) * f), guide.Get(), 1.5f);
        }
    float spin = (float)std::fmod(now * (scan ? 1.6 : 0.55), 2 * kPi), bright = linked ? 1.0f : 0.65f;
    c.Arc(o, 39, spin, 1.25f, t.accentHi.Alpha(0.60f * bright), 2);
    c.Arc(o, 39, spin + kPi, 0.55f, t.accent.Alpha(0.40f * bright), 2);
    c.Arc(o, 46, -spin * 0.7f, 0.95f, t.accent.Alpha(0.28f * bright), 1.5f);
    c.Arc(o, 46, -spin * 0.7f + kPi * 0.9f, 0.35f, t.accentHi.Alpha(0.22f * bright), 1.5f);
    c.FillV(Rect(o.x - 30, o.y - 30, 60, 60), 30, t.accentHi, t.accent); // a glossy disc in the accent
    c.Ring(o, 30, Color{1, 1, 1, 0.20f}, 1);
    c.Dot({o.x - 9, o.y - 11}, 10, Color{1, 1, 1, 0.16f}); // a highlight, light catching the top
    c.Icon(glyph::Bluetooth, o, 28, t.onAccent, true);

    for (const Place& pl : placed) Bubble(p, pl.at, pl.r, *pl.d, pl.appear, live, nowMs, pl.at.y < o.y - 2);

    if (placed.empty()) // nothing around yet: a word under this pc
        c.Text(m.btScan ? L"Looking for devices nearby. Put yours in pairing mode." : L"No devices yet. Add device to pair one.",
               Rect(v.left, o.y + 44, Width(v), 18), Weight::Light, 12, t.textDim, Align::Center);
    else if (m.btScan && rings[2].empty())
        c.Text(L"Looking for devices nearby\u2026", Rect(v.left, v.bottom - 20, Width(v), 16), Weight::Light, 11, t.textLow, Align::Center);
}

}

void DrawBt(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    Frame frame = Begin(p, k);

    bool on = m.radios.BtOn();
    int paired = 0, conn = 0;
    for (const auto& d : m.btDevs)
        if (d.paired) ++paired, conn += d.on ? 1 : 0;

    Radar(ui, t, "bt.radar", frame.badge, on, on && m.btScan, glyph::Bluetooth, glyph::BluetoothOff, glyph::BtSearch);
    c.Text(L"Bluetooth", frame.title, Weight::Medium, frame.titleSize, t.text, frame.align);
    std::wstring sub = !on ? L"Turned off" : paired == 0 ? L"No devices yet"
                                                          : std::to_wstring(conn) + L" connected  ·  " + std::to_wstring(paired) + L" paired";
    c.Text(sub, frame.sub, Weight::Light, 12, t.textDim, frame.align);
    if (w::Switch(ui, t, Ui::Id("bt.power"), frame.sw, on, m.radios.HasBt())) {
        if (on) m.bt.Scan(false); // no point scanning with the radio off
        m.radios.SetBt(!on);
    }
    D2D1_RECT_F pill = frame.pill;
    if (m.shareTab) {
        bool sharing = m.share.Active();
        if (ScanBtn(ui, t, "bt.stopshare", pill, false, sharing, glyph::Stop, sharing ? L"Stop sharing" : L"Not sharing", L"")) {
            m.share.StopAll(); // thread ends, every extra device is released
            m.OnShare();
        }
    } else if (ScanBtn(ui, t, "bt.add", pill, m.btScan, on, glyph::Add, L"Add device", L"Stop scanning")) {
        m.bt.Scan(!m.btScan);
        m.OnBt();
    }

    D2D1_RECT_F list = frame.list;
    if (Tab(ui, t, "bt.tab.dev", {list.left, list.top, list.left + 80, list.top + 30}, L"DEVICES", !m.shareTab)) m.shareTab = false;
    if (Tab(ui, t, "bt.tab.share", {list.left + 88, list.top, list.left + 214, list.top + 30}, L"SHARED AUDIO", m.shareTab) && !m.shareTab) {
        m.shareTab = true;
        m.OnShare();
    }
    static const float kDevW = TabW(L"DEVICES"), kShareW = TabW(L"SHARED AUDIO"); // measured once
    TabLine(ui, t, list.top + 25, m.shareTab ? list.left + 92 : list.left + 4, m.shareTab ? kShareW : kDevW);
    if (w::IconBtn(ui, t, Ui::Id("bt.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseBt();
    if (w::IconBtn(ui, t, Ui::Id("bt.more"), {list.right - 48, list.top + 14}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // hand off to windows settings
        sys::OpenUri(L"ms-settings:bluetooth");
    }
    D2D1_RECT_F view = {list.left, list.top + 34, list.right, list.bottom}; // scrollable area under header

    if (m.shareTab) {
        DrawShare(p, view); // plays what the main output plays on more devices
    } else if (!on) {
        Empty(c, t, view, glyph::BluetoothOff, L"Bluetooth is off", L"Turn it on to see your devices");
    } else {
        Orbit(p, view); // devices around this pc, touched directly
    }

    if (ui.RClick(s)) m.CloseBt(); // right click again closes
    End(p, frame);
}
