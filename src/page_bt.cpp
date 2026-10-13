#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHero = 236; // left column width
constexpr float kRowH = 54, kRowGap = 6, kHeadH = 30, kNoteH = 40; // list item heights
const Color kDanger = Color::Rgb(0xFF8A80); // soft red for forget
float scrollTo = 0; // wheel target, eased into "bt.scroll"

int Key(const std::wstring& s) { // stable per-device ui id, survives re-sorting
    uint32_t h = 2166136261u;
    for (wchar_t ch : s) h = (h ^ ch) * 16777619u; // fnv-1a
    return static_cast<int>(h & 0x7fffffff);
}

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

void Spinner(Canvas& c, D2D1_POINT_2F o, float r, Color col, double now) {
    float a = static_cast<float>(std::fmod(now * 5.5, 2 * kPi)); // ~0.9 turns/s
    c.Arc(o, r, a, kPi * 1.3f, col, 2);
}

void Radar(Ui& ui, const Theme& t, D2D1_POINT_2F o, bool on, bool scanning) {
    Canvas& c = ui.C();
    float scan = ui.Smooth(Ui::Id("bt.radar"), scanning ? 1.0f : 0, 6);
    float lit = ui.Smooth(Ui::Id("bt.lit"), on ? 1.0f : 0, 10);
    c.Glow(o, 96, 80, t.accent.Alpha(0.05f + 0.13f * lit)); // soft halo behind the badge
    if (scan > 0.01f) {
        float ph = static_cast<float>(std::fmod(ui.Now() * 0.55, 1.0)); // ring cycle ~1.8s
        for (int i = 0; i < 3; ++i) {
            float f = std::fmod(ph + i / 3.0f, 1.0f); // 3 staggered rings expand and fade
            c.Ring(o, 34 + f * 32, t.accent.Alpha((1 - f) * 0.5f * scan), 1.5f);
        }
        ui.KeepAlive(); // ambient rate is plenty for rings
    }
    c.Dot(o, 32, t.track.Mix(t.accent.Alpha(0.24f), lit));
    c.Ring(o, 32, t.hairTop.Mix(t.accentHi.Alpha(0.45f), lit), 1);
    int g = scanning ? glyph::BtSearch : on ? glyph::Bluetooth : glyph::BluetoothOff;
    c.Icon(g, o, 30, t.textDim.Mix(t.accentHi, lit), on); // filled glyph when on
}

bool AddBtn(Ui& ui, const Theme& t, const D2D1_RECT_F& r, bool scanning, bool enabled) {
    uint32_t id = Ui::Id("bt.add");
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
    const wchar_t* label = scanning ? L"Stop scanning" : L"Add device";
    float est = std::wcslen(label) * 7.0f; // rough text width, keeps icon+label centered
    float x0 = (b.left + b.right) / 2 - (est + 26) / 2, cy = (b.top + b.bottom) / 2;
    if (scanning) Spinner(c, {x0 + 8, cy}, 7, ink, ui.Now());
    else c.Icon(glyph::Add, {x0 + 8, cy}, 20, ink);
    c.Text(label, Rect(x0 + 24, b.top, est + 12, Height(b)), Weight::Medium, 13, ink);
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

void Row(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, const sys::BtDev& d, uint64_t nowMs) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool live = Inside(view, ui.In().mouse); // parts scrolled under the edges stay inert
    bool hov = live && Inside(r, ui.In().mouse); // plain test, inner buttons own the clicks
    uint32_t id = Ui::Id("bt.row", Key(d.id));
    float hover = ui.Smooth(id, hov ? 1.0f : 0, 16);
    float lit = ui.Smooth(id + 1, d.on ? 1.0f : 0, 10);
    bool busy = d.pairing || (d.until > nowMs && d.on != d.want); // pending until state flips or deadline
    float cy = (r.top + r.bottom) / 2;

    c.Fill(r, 16, t.cardHi.Alpha(0.35f + 0.65f * hover)); // row lifts on hover
    if (lit > 0.01f) c.FillH(r, 16, t.accent.Alpha(0.16f * lit), t.accent.Alpha(0.02f * lit)); // connected wash
    c.StrokeV(r, 16, t.hairTop.Mix(t.accentHi.Alpha(0.5f), lit), t.hairBot, 1);
    D2D1_POINT_2F ic = {r.left + 28, cy};
    c.Dot(ic, 17, t.track.Mix(t.accent.Alpha(0.28f), lit));
    c.Icon(IconOf(d.kind), ic, 19, t.textDim.Mix(t.accentHi, lit), d.on);

    const wchar_t* label = !d.paired ? L"Pair" : d.on ? L"Disconnect" : L"Connect";
    float aw = d.on ? 96.0f : 82.0f; // fits the longer label
    D2D1_RECT_F ar = {r.right - 12 - aw, cy - 15, r.right - 12, cy + 15};
    if (Act(ui, t, id + 2, ar, label, !d.on, busy, live)) {
        if (d.paired) p.m.bt.Connect(d.id, !d.on); // toggle link
        else p.m.bt.Pair(d.id);
        p.m.OnBt(); // show the spinner this frame
    }

    float textR = ar.left - 10;
    if (d.paired) { // forget: press and hold so it can't happen by accident
        D2D1_POINT_2F fc = {ar.left - 24, cy};
        D2D1_RECT_F fr = {fc.x - 15, fc.y - 15, fc.x + 15, fc.y + 15};
        float prog = 0;
        bool fire = live && ui.Hold(id + 4, fr, 0.8f, prog);
        float show = ui.Smooth(id + 6, hov || prog > 0 ? 1.0f : 0, 14); // only on row hover
        if (show > 0.01f) {
            bool fh = live && Inside(fr, ui.In().mouse);
            if (fh) c.Dot(fc, 15, kDanger.Alpha(0.12f * show));
            c.Icon(glyph::Delete, fc, 18, (fh ? kDanger : t.textLow).Alpha(show));
            if (prog > 0) c.Arc(fc, 14, -kPi / 2, prog * 2 * kPi, kDanger, 2); // hold progress from 12 o'clock
        }
        if (fire) p.m.bt.Forget(d.id); // unpair, row vanishes via watcher
        textR = fc.x - 20;
    }

    float tx = r.left + 56;
    c.Text(d.name, {tx, cy - 19, textR, cy + 1}, Weight::Medium, 13.5f, t.text); // ellipsis if too long
    std::wstring st = d.paired ? L"Not connected" : std::wstring(KindName(d.kind)) + L"  ·  new";
    Color sc = t.textLow;
    if (d.pairing) st = L"Pairing…", sc = t.accentHi;
    else if (busy) st = d.want ? L"Connecting…" : L"Disconnecting…", sc = t.accentHi;
    else if (d.on) st = L"Connected", sc = t.accent;
    if (d.on && !busy) { // live dot before "connected"
        c.Dot({tx + 3, cy + 10}, 3, t.accent);
        tx += 11;
    }
    c.Text(st, {tx, cy + 1, textR, cy + 19}, Weight::Light, 11.5f, sc);
    if (busy) ui.KeepAlive(); // spinner + deadline expiry need frames
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

}

void DrawBt(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;

    ComPtr<ID2D1DeviceContext> dc;
    bool layered = k < 0.999f && SUCCEEDED(c.Target()->QueryInterface(IID_PPV_ARGS(&dc))); // layer only while animating
    D2D1_MATRIX_3X2_F base;
    c.Target()->GetTransform(&base);
    c.Target()->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - k) * 16) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base)); // rise in
    if (layered) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), k), nullptr);

    c.Fill(Inset(s, -10, -10), 30, Color{0, 0, 0, 0.5f}); // dim what's behind
    c.Fill(s, 24, t.card); // sheet body
    c.PushClip(s);
    c.Glow({s.left + kHero / 2, s.top + 84}, 200, 160, t.accent.Alpha(0.09f)); // tint the hero side
    c.PopClip();
    c.FillV({s.left, s.top, s.right, s.top + 80}, 24, Color{1, 1, 1, 0.035f}, Color{1, 1, 1, 0}); // top sheen
    c.StrokeV(s, 24, t.hairTop, t.hairBot, 1);
    c.Line({s.left + kHero, s.top + 22}, {s.left + kHero, s.bottom - 22}, Color{1, 1, 1, 0.06f}, 1, false);

    bool on = m.radios.BtOn();
    int paired = 0, conn = 0;
    for (const auto& d : m.btDevs)
        if (d.paired) ++paired, conn += d.on ? 1 : 0;

    float hx = s.left + kHero / 2; // hero column center
    Radar(ui, t, {hx, s.top + 84}, on, on && m.btScan);
    c.Text(L"Bluetooth", Rect(s.left + 16, s.top + 134, kHero - 32, 28), Weight::Medium, 21, t.text, Align::Center);
    std::wstring sub = !on ? L"Turned off" : paired == 0 ? L"No devices yet"
                                                          : std::to_wstring(conn) + L" connected  ·  " + std::to_wstring(paired) + L" paired";
    c.Text(sub, Rect(s.left + 16, s.top + 162, kHero - 32, 18), Weight::Light, 12, t.textDim, Align::Center);
    if (w::Switch(ui, t, Ui::Id("bt.power"), {hx, s.top + 204}, on, m.radios.HasBt())) {
        if (on) m.bt.Scan(false); // no point scanning with the radio off
        m.radios.SetBt(!on);
    }
    if (AddBtn(ui, t, Rect(s.left + 28, s.bottom - 62, kHero - 56, 38), m.btScan, on)) {
        m.bt.Scan(!m.btScan);
        m.OnBt();
    }

    D2D1_RECT_F list = {s.left + kHero + 18, s.top + 16, s.right - 16, s.bottom - 14};
    c.TextSp(L"DEVICES", Rect(list.left + 4, list.top + 8, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    if (w::IconBtn(ui, t, Ui::Id("bt.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseBt();
    if (w::IconBtn(ui, t, Ui::Id("bt.more"), {list.right - 48, list.top + 14}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // hand off to windows settings
        sys::OpenUri(L"ms-settings:bluetooth");
    }
    D2D1_RECT_F view = {list.left, list.top + 34, list.right, list.bottom}; // scrollable area under header

    if (!on) {
        Empty(c, t, view, glyph::BluetoothOff, L"Bluetooth is off", L"Turn it on to see your devices");
    } else {
        struct Item { int type; const sys::BtDev* d; const wchar_t* text; }; // 0 head, 1 device, 2 note
        std::vector<Item> items;
        bool anyNew = false;
        for (const auto& d : m.btDevs)
            if (d.paired) items.push_back({1, &d, nullptr});
            else anyNew = true;
        if (paired == 0) items.push_back({2, nullptr, L"No paired devices yet. Tap Add device to find one."});
        if (m.btScan || anyNew) {
            items.push_back({0, nullptr, L"AVAILABLE"});
            for (const auto& d : m.btDevs)
                if (!d.paired) items.push_back({1, &d, nullptr});
            if (!anyNew) items.push_back({2, nullptr, L"Looking for devices nearby. Put yours in pairing mode."});
        }
        float total = 0; // content height for scroll clamp
        for (const Item& it : items) total += it.type == 1 ? kRowH + kRowGap : it.type == 0 ? kHeadH : kNoteH;
        float maxS = (std::max)(0.0f, total - Height(view));
        float notches = ui.Wheel(view);
        scrollTo = (std::clamp)(scrollTo - notches * 60, 0.0f, maxS); // 60 dips per notch
        float scroll = ui.Smooth(Ui::Id("bt.scroll"), scrollTo, 18);

        uint64_t nowMs = GetTickCount64(); // for pending op deadlines
        c.PushClip(view);
        float y = view.top - scroll; // first item position
        for (const Item& it : items) {
            float h = it.type == 1 ? kRowH + kRowGap : it.type == 0 ? kHeadH : kNoteH;
            if (y + h > view.top && y < view.bottom) { // skip offscreen items entirely
                D2D1_RECT_F r = {view.left, y, view.right, y + h};
                if (it.type == 1) Row(p, {r.left, r.top, r.right, r.top + kRowH}, view, *it.d, nowMs);
                else if (it.type == 0) Head(ui, t, r, it.text, m.btScan);
                else c.Text(it.text, Inset(r, 6, 0), Weight::Light, 12, t.textDim);
            }
            y += h;
        }
        c.PopClip();
        if (scroll > 1) c.FillV({view.left, view.top, view.right, view.top + 18}, 0, t.card, t.card.Alpha(0)); // fade hints more above
        if (scroll < maxS - 1) c.FillV({view.left, view.bottom - 18, view.right, view.bottom}, 0, t.card.Alpha(0), t.card);
    }

    if (ui.RClick(s)) m.CloseBt(); // right click again closes
    if (layered) dc->PopLayer();
    c.Target()->SetTransform(base);
}
