// Wi-Fi sheet: the panel is one big wifi fan. Your connection is its source at the bottom, the networks nearby ride its arcs,
// strong ones close to the source and weak ones out on the far waves.
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <vector>

#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

constexpr int kArcs = 4; // signal bands, inner = strongest
constexpr float kMaxSpan = 2.1f; // widest an arc opens, about 120 degrees
constexpr float kTagH = 26, kTagGap = 12;

const wchar_t* Quality(int signal) { return signal >= 75 ? L"Excellent" : signal >= 50 ? L"Good" : signal >= 25 ? L"Fair" : L"Weak"; }
int Level(int signal) { return (std::max)(1, (std::min)(kArcs, (signal * kArcs + 99) / 100)); } // arcs lit for a 0-100 signal
int Band(int signal) { return kArcs - Level(signal); } // which arc a network rides, 0 = inner
std::wstring BandText(int band) { return band == 2 ? L"2.4 GHz" : band == 5 ? L"5 GHz" : band == 6 ? L"6 GHz" : L""; }
float Ease(float x) { x = (std::clamp)(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); }

float TextW(const std::wstring& s) { // tag widths, measured once per name
    static std::map<std::wstring, float> cache;
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    float w = s.size() * 6.8f;
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), gfx::TextFmt(Weight::Medium, 12, Align::Left), 600, 30, &lay)) &&
        SUCCEEDED(lay->GetMetrics(&tm)))
        w = tm.widthIncludingTrailingWhitespace;
    if (cache.size() > 200) cache.clear(); // never grows without bound
    return cache[s] = (std::min)(w, 150.0f);
}

void Act(PageCtx& p, const sys::WifiNet& n) { // connect, disconnect, or hand a password over to windows
    if (n.on) {
        p.m.wifi.Disconnect();
    } else if (n.profile.empty() && n.secured) {
        if (p.m.wifi.CanJoin(n)) {
            if (p.m.passFor != n.raw) p.m.AskPass(n); // a password is all it needs: typed right here; clicking it again keeps what's typed
        } else { // enterprise logins and the like: windows' own flyout asks
            sys::OpenUri(L"ms-availablenetworks:");
            p.wantClose = true;
        }
    } else {
        p.m.wifi.Connect(n);
    }
}

std::wstring Details(const sys::WifiNet& n) { // band, generation, speed
    std::wstring s = Quality(n.signal);
    if (n.band) s += L"  ·  " + BandText(n.band);
    if (n.gen) s += L"  ·  Wi-Fi " + std::to_wstring(n.gen) + (n.gen == 6 && n.band == 6 ? L"E" : L"");
    if (n.rate) s += L"  ·  " + std::to_wstring(n.rate) + L" Mbps";
    return s;
}

struct Field { // the fan's geometry for this frame
    D2D1_POINT_2F src;
    float radius[kArcs];
    float span[kArcs];
};

Field Lay(const D2D1_RECT_F& s, bool tall) {
    Field f;
    f.src = {(s.left + s.right) / 2, tall ? s.bottom - 132 : s.bottom - 30};
    float rMax = f.src.y - s.top - (tall ? 100 : 26), half = Width(s) / 2 - (tall ? 40 : 70);
    for (int k = 0; k < kArcs; ++k) {
        static const float kAt[kArcs] = {0.28f, 0.48f, 0.73f, 1.0f}; // the gaps widen outward, like a real fan's waves
        f.radius[k] = rMax * kAt[k];
        f.span[k] = (std::min)(kMaxSpan, 2 * std::asin((std::min)(1.0f, half / f.radius[k]))); // outer arcs narrow to fit a tall sheet
    }
    return f;
}

D2D1_POINT_2F On(const Field& f, int k, float a) { return {f.src.x + f.radius[k] * std::cos(a), f.src.y + f.radius[k] * std::sin(a)}; }

bool Tag(PageCtx& p, const D2D1_POINT_2F& at, const sys::WifiNet& n, float appear, float wave, const D2D1_POINT_2F& src, const sys::WifiNet** hovered) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool saved = !n.profile.empty(), joining = p.m.passFocus && p.m.passFor == n.raw;
    float w = TextW(n.ssid) + 40 + (saved ? 22 : 0);
    D2D1_RECT_F r = Rect(at.x - w / 2, at.y - kTagH / 2 + (1 - appear) * 10, w, kTagH);
    uint32_t id = Ui::Id("wifi.tag", Key(n.ssid));
    D2D1_RECT_F bin = Rect(r.right - 26, r.top, 26, kTagH); // saved: hold the bin to forget it
    D2D1_RECT_F body = saved ? D2D1_RECT_F{r.left, r.top, bin.left, r.bottom} : r;
    Ui::Hit h = appear > 0.9f && !n.pending ? ui.Button(id, body) : Ui::Hit{};
    bool over = appear > 0.9f && Inside(r, ui.In().mouse);
    float hover = ui.Smooth(id, h.hover || joining ? 1.0f : 0, 16);
    if (over) *hovered = &n;
    float a = appear;
    if (hover > 0.01f) { // a beam from the source to the network under the mouse
        c.Line(src, {at.x, r.bottom}, t.accentHi.Alpha(0.45f * hover * a), 1.5f);
    }
    float lit = (std::max)(hover, wave); // the scan wave lights tags as it passes
    c.Fill(r, kTagH / 2, t.veil.Alpha(0.88f * a)); // solid, so the arcs behind never cut through the name
    c.Fill(r, kTagH / 2, t.cardHi.Mix(t.accent.Alpha(0.9f), hover).Alpha(a));
    c.Stroke(r, kTagH / 2, (n.profile.empty() ? t.hairTop : t.accentHi.Alpha(0.7f)).Mix(t.accentHi, lit).Alpha(a), n.profile.empty() ? 1 : 1.4f);
    D2D1_POINT_2F ic = {r.left + 15, (r.top + r.bottom) / 2};
    Color ink = t.text.Mix(t.onAccent, hover).Alpha(a);
    if (n.pending) {
        Spinner(c, ic, 6, t.accentHi, ui.Now());
        ui.KeepAlive();
    } else if (n.secured) {
        c.Icon(glyph::Lock, ic, 13, t.textDim.Mix(t.onAccent, hover).Alpha(a));
    } else {
        c.Dot(ic, 3, t.accentHi.Mix(t.onAccent, hover).Alpha(a)); // open network
    }
    c.Text(n.ssid, {r.left + 27, r.top, body.right - 12 + (saved ? 10 : 0), r.bottom}, Weight::Medium, 12, ink);
    if (saved) {
        float prog = 0;
        bool fire = appear > 0.9f && ui.Hold(id + 4, bin, 0.8f, prog);
        bool bh = Inside(bin, ui.In().mouse);
        D2D1_POINT_2F bc = {(bin.left + bin.right) / 2 - 2, (bin.top + bin.bottom) / 2};
        float show = ui.Smooth(id + 6, over || prog > 0 ? 1.0f : 0.35f, 14);
        c.Icon(glyph::Delete, bc, 14, (bh ? kDanger : t.textDim.Mix(t.onAccent, hover)).Alpha(show * a));
        if (prog > 0) c.Arc(bc, 10, -kPi / 2, prog * 2 * kPi, kDanger, 1.8f); // hold progress
        if (fire) {
            p.m.wifi.Forget(n);
            return true;
        }
    }
    if (!h.clicked) return false;
    Act(p, n);
    return true;
}

bool Password(PageCtx& p, const D2D1_RECT_F& r, const sys::WifiNet& n) { // join a new network: its password, typed in the panel
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    c.TextSp(m.passWrong ? L"THAT PASSWORD DIDN'T WORK" : L"JOIN", Rect(r.left, r.top + 2, 300, 12), Weight::Medium, 9, m.passWrong ? kDanger : t.textLow, 1.4f);
    c.Text(n.ssid, Rect(r.left, r.top + 16, Width(r), 26), Weight::Semi, 19, t.text);
    D2D1_RECT_F field = Rect(r.left, r.bottom - 32, 214, 32);
    if (ui.Button(Ui::Id("wifi.pass"), field).clicked) m.passFocus = true;
    c.Fill(field, 16, t.veil.Alpha(0.55f));
    c.Stroke(field, 16, (m.passWrong ? kDanger : t.accentHi).Alpha(0.8f), 1.3f);
    D2D1_RECT_F text = {field.left + 14, field.top, field.right - 48, field.bottom};
    if (m.passEdit.empty()) {
        c.Text(L"Password", text, Weight::Light, 12.5f, t.textLow);
    } else { // drawn and measured outside every cache, so the typed text is kept nowhere but the field
        std::wstring shown = m.passShow ? m.passEdit : std::wstring(m.passEdit.size(), L'•');
        float tw = c.TextOnce(shown, text, Weight::Medium, 13, t.text);
        SecureZeroMemory(shown.data(), shown.size() * sizeof(wchar_t));
        float caret = text.left + (std::min)(tw, Width(text) - 2); // a steady caret, no blinking frames
        c.Line({caret + 2, field.top + 9}, {caret + 2, field.bottom - 9}, t.accentHi, 1.5f, false);
    }
    D2D1_RECT_F eye = Rect(field.right - 46, field.top + 6, 38, 20);
    if (ui.Button(Ui::Id("wifi.pass.show"), eye).clicked) m.passShow = !m.passShow;
    c.Text(m.passShow ? L"Hide" : L"Show", eye, Weight::Medium, 11, t.textDim, Align::Center);
    bool ready = m.passEdit.size() >= 8; // wpa needs at least 8
    bool go = sheet::Act(ui, t, Ui::Id("wifi.join"), Rect(field.right + 8, field.top, 64, 32), L"Join", ready, false, ready);
    if (w::IconBtn(ui, t, Ui::Id("wifi.pass.x"), {field.right + 92, field.top + 16}, 28, glyph::Close, 16)) {
        m.CancelPass();
        return false;
    }
    if (!go) return false;
    m.SubmitPass();
    return true;
}

}

void DrawWifi(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    bool pending = false;
    for (const auto& n : m.wifiNets) pending = pending || n.pending;
    if (m.wifiScan || pending) m.OnWifi(); // pick up scan/op timeouts that no event reports
    Frame frame = Begin(p, k, false);
    bool tall = frame.tall;

    bool on = m.radios.WifiOn();
    bool usable = on && m.wifiState == sys::WifiState::Ok;
    bool scanning = usable && m.wifiScan;
    const sys::WifiNet* cur = nullptr;
    std::vector<const sys::WifiNet*> nearby; // strongest first
    for (const auto& n : m.wifiNets) {
        if (n.on) cur = &n;
        else nearby.push_back(&n);
    }
    std::stable_sort(nearby.begin(), nearby.end(), [](auto* a, auto* b) { return a->signal > b->signal; });

    static double seen = 0; // a fresh opening replays the intro
    static int session = 0;
    if (ui.Now() - seen > 0.3) ++session;
    seen = ui.Now();
    float intro = m.reduceMotion ? 1.0f : ui.SmoothFrom(Ui::Id("wifi.intro", session), 1.0f, 0.0f, 3.2f);
    if (intro < 0.999f) ui.Wake();
    float scan = ui.Smooth(Ui::Id("wifi.scanning"), scanning ? 1.0f : 0, 6);
    float wave = -1; // the scan pulse's radius as a share of the field
    if (scan > 0.01f && !m.reduceMotion) {
        wave = static_cast<float>(std::fmod(ui.Now() * 0.55, 1.0)) * 1.25f;
        ui.KeepAlive(); // only while scanning
    }

    // the field: arcs lit out to your signal, a pulse running out while it scans
    Field f = Lay(s, tall);
    int litArcs = cur ? Level(cur->signal) : 0;
    float rMax = f.radius[kArcs - 1];
    c.PushClip(s);
    for (int a = 0; a < kArcs; ++a) {
        float g = Ease(intro * 1.7f - a * 0.18f); // arcs sweep open from the middle, inner first
        float sp = f.span[a] * g, from = -kPi / 2 - sp / 2;
        bool lit = a < litArcs;
        Color col = lit ? t.accentHi.Mix(t.accent, (float)a / (kArcs - 1)) : on ? t.track.Mix(t.text, 0.06f) : t.track;
        if (wave >= 0) {
            float d = (f.radius[a] - wave * rMax) / (rMax * 0.12f);
            col = col.Mix(t.accentHi, 0.6f * scan * std::exp(-d * d));
        }
        c.Arc(f.src, f.radius[a], from, sp, col, lit ? 3.5f : 1.5f);
    }
    if (wave >= 0 && wave < 1.2f) c.Arc(f.src, wave * rMax, -kPi / 2 - kMaxSpan / 2, kMaxSpan, t.accentHi.Alpha((1.2f - wave) * 0.5f * scan), 2);
    c.PopClip();

    // the source: your connection
    float beat = cur && cur->pending ? 0.5f + 0.5f * std::sin(static_cast<float>(ui.Now()) * 6) : 0;
    if (cur) {
        c.Dot(f.src, 15, t.accent); // flat, like the arcs
        c.Ring(f.src, 21 + 3 * beat, t.accentHi.Alpha(0.35f), 1.2f);
        if (cur->pending) ui.KeepAlive();
    } else {
        c.Ring(f.src, 12, on ? t.textDim : t.textLow, 2);
    }

    // networks ride their band's arc; a full arc spills the weaker ones outward
    std::vector<std::vector<const sys::WifiNet*>> rows(kArcs);
    int hidden = 0;
    if (usable) {
        for (const auto* n : nearby) {
            int a = Band(n->signal);
            while (a < kArcs) {
                float room = f.radius[a] * f.span[a], used = 0;
                for (const auto* o : rows[a]) used += TextW(o->ssid) + 40 + kTagGap;
                if (used + TextW(n->ssid) + 40 <= room) break;
                ++a;
            }
            if (a < kArcs) rows[a].push_back(n);
            else ++hidden;
        }
    }
    const sys::WifiNet* hovered = nullptr;
    bool acted = false;
    int order = 0;
    for (int a = 0; a < kArcs; ++a) {
        auto& row = rows[a];
        std::stable_sort(row.begin(), row.end(), [](auto* x, auto* y) { return x->ssid < y->ssid; }); // steady places, they don't shuffle as signals wobble
        int n = (int)row.size();
        for (int j = 0; j < n; ++j) {
            float ang = -kPi / 2 - f.span[a] / 2 + f.span[a] * (j + 0.5f) / n;
            float appear = Ease(intro * 2.2f - 0.35f - order++ * 0.07f);
            float lit = wave >= 0 ? scan * std::exp(-std::pow((f.radius[a] - wave * rMax) / (rMax * 0.12f), 2.0f)) : 0;
            if (appear > 0.01f) acted = Tag(p, On(f, a, ang), *row[j], appear, lit, f.src, &hovered) || acted;
        }
    }

    // header: title, and what's under the mouse or how many are around
    c.Text(L"Wi-Fi", Rect(s.left + 24, s.top + 12, 160, 28), Weight::Semi, 21, t.text);
    float hy = s.top + 30;
    if (w::IconBtn(ui, t, Ui::Id("wifi.close"), {s.right - 30, hy}, 30, glyph::Close, 18)) m.CloseWifi();
    if (w::IconBtn(ui, t, Ui::Id("wifi.more"), {s.right - 64, hy}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // hand off to windows settings
        sys::OpenUri(L"ms-settings:network-wifi");
    }
    D2D1_POINT_2F sw = {s.right - 112, hy};
    if (w::Switch(ui, t, Ui::Id("wifi.power"), sw, on, m.radios.HasWifi())) m.radios.SetWifi(!on);
    std::wstring sub;
    Color subInk = t.textDim;
    if (hovered) {
        bool join = hovered->profile.empty() && hovered->secured;
        sub = std::to_wstring(hovered->signal) + L"%  ·  " + Details(*hovered) + (hovered->profile.empty() ? L"" : L"  ·  Saved") +
              (join ? L"  ·  click to join" : L"  ·  click to connect");
        subInk = t.accentHi;
    } else {
        sub = !on ? L"Turned off" : !usable ? L"Unavailable" : scanning ? L"Listening for networks…"
            : std::to_wstring(nearby.size()) + (nearby.size() == 1 ? L" network nearby" : L" networks nearby");
        if (hidden) sub += L"  ·  " + std::to_wstring(hidden) + L" too faint to show";
    }
    c.Text(sub, {s.left + 24, s.top + 40, sw.x - 34, s.top + 56}, Weight::Light, 12, subInk);

    // scan, bottom right (under the header on a tall sheet)
    D2D1_RECT_F scanR = tall ? Rect(s.right - 24 - 112, s.top + 62, 112, 30) : Rect(s.right - 24 - 120, s.bottom - 48, 120, 32);
    if (ScanBtn(ui, t, "wifi.scan", scanR, scanning, on, glyph::Restart, L"Scan", L"Scanning…") && !m.wifiScan) {
        if (m.wifiState == sys::WifiState::Ok) m.wifi.Scan();
        else m.ReopenWifi(); // adapter came back or location was allowed since the sheet opened
        m.OnWifi();
    }

    // the connection, bottom left; while joining a new network, its password field
    float bx = s.left + 24, bb = s.bottom;
    std::string bad = m.wifi.WrongKey();
    if (!bad.empty() && !m.passFocus) { // the key didn't work: ask again
        for (const auto& n : m.wifiNets)
            if (n.raw == bad) m.AskPass(n), m.passWrong = true;
        m.wifi.ForgetWrongKey();
    }
    const sys::WifiNet* joining = nullptr;
    for (const auto& n : m.wifiNets)
        if (m.passFocus && n.raw == m.passFor) joining = &n;
    if (m.passFocus && !joining) m.CancelPass(); // it went out of range
    if (joining) {
        acted = Password(p, {bx, bb - 112, bx + 330, bb - 12}, *joining) || acted;
    } else {
    c.TextSp(cur ? L"CONNECTED" : L"WI-FI", Rect(bx, bb - 110, 200, 12), Weight::Medium, 9, t.textLow, 1.4f);
    c.Text(!on ? L"Turned off" : cur ? cur->ssid : L"Not connected", Rect(bx, bb - 96, tall ? Width(s) - 48 : 230, 26), Weight::Semi, 19, t.text);
    std::wstring line = !on ? L"Switch it on to see networks" : !cur ? L"Pick a network on the waves" : cur->pending ? L"Disconnecting…" : Details(*cur);
    c.Text(line, Rect(bx, bb - 68, tall ? Width(s) - 48 : 240, 18), Weight::Medium, 11.5f, cur ? t.accentHi : t.textDim);
    if (cur && sheet::Act(ui, t, Ui::Id("wifi.off"), Rect(bx, bb - 44, 100, 28), L"Disconnect", false, cur->pending, true)) {
        Act(p, *cur);
        acted = true;
    }
    if (cur && !cur->profile.empty()) { // forget it: press and hold
        D2D1_RECT_F fr = Rect(bx + 108, bb - 44, 84, 28);
        uint32_t fid = Ui::Id("wifi.forget");
        float prog = 0;
        bool fire = ui.Hold(fid, fr, 0.8f, prog);
        bool fh = Inside(fr, ui.In().mouse);
        c.Fill(fr, 14, kDanger.Alpha(fh ? 0.16f : 0.0f));
        c.Stroke(fr, 14, (fh ? kDanger : t.hairTop).Alpha(fh ? 0.7f : 1.0f), 1);
        if (prog > 0) c.Fill(Rect(fr.left, fr.top, Width(fr) * prog, Height(fr)), 14, kDanger.Alpha(0.30f)); // fills while held
        c.Text(prog > 0 ? L"Hold…" : L"Forget", fr, Weight::Medium, 12, fh ? kDanger : t.textDim, Align::Center);
        if (fire) {
            m.wifi.Forget(*cur);
            acted = true;
        }
    }
    }

    // states where there is nothing to ride the waves
    D2D1_RECT_F note = {f.src.x - 150, f.src.y - rMax * 0.75f - 20, f.src.x + 150, f.src.y - rMax * 0.75f + 20};
    if (on && m.wifiState == sys::WifiState::NoAdapter) {
        c.Text(L"No Wi-Fi adapter on this PC", note, Weight::Medium, 13, t.textDim, Align::Center);
    } else if (on && m.wifiState == sys::WifiState::Denied) {
        c.Text(L"Windows only lists networks with location access on", note, Weight::Medium, 12, t.textDim, Align::Center);
        if (sheet::Act(ui, t, Ui::Id("wifi.loc"), Rect(f.src.x - 70, note.bottom + 6, 140, 30), L"Open settings", true, false, true)) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:privacy-location");
        }
    } else if (usable && nearby.empty() && !scanning) {
        c.Text(L"Nothing else nearby. Try scanning again.", note, Weight::Light, 12, t.textDim, Align::Center);
    }
    if (acted) m.OnWifi(); // after drawing: re-snapping replaces the list the tags walk

    if (ui.RClick(s)) m.CloseWifi(); // right click again closes
    End(p, frame);
}
