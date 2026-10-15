#include <cwchar>

#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

float scrollTo = 0; // wheel target, eased into "wifi.scroll"

std::wstring Status(const sys::WifiNet& n) {
    if (n.pending) return n.on ? L"Disconnecting…" : L"Connecting…";
    if (n.on) return L"Connected";
    std::wstring s = !n.profile.empty() ? L"Saved" : n.secured ? L"Secured" : L"Open";
    if (!n.profile.empty() && n.secured) s += L"  ·  Secured";
    return s;
}

bool Row(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, const sys::WifiNet& n) { // true = acted, caller re-snaps after the loop
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool live = Inside(view, ui.In().mouse); // parts scrolled under the edges stay inert
    bool hov = live && Inside(r, ui.In().mouse);
    uint32_t id = Ui::Id("wifi.row", Key(n.ssid));
    float hover = ui.Smooth(id, hov ? 1.0f : 0, 16);
    float lit = ui.Smooth(id + 1, n.on ? 1.0f : 0, 10);
    float cy = (r.top + r.bottom) / 2;

    c.Fill(r, 16, t.cardHi.Alpha(0.35f + 0.65f * hover)); // row lifts on hover
    if (lit > 0.01f) c.FillH(r, 16, t.accent.Alpha(0.16f * lit), t.accent.Alpha(0.02f * lit)); // connected wash
    c.StrokeV(r, 16, t.hairTop.Mix(t.accentHi.Alpha(0.5f), lit), t.hairBot, 1);
    D2D1_POINT_2F ic = {r.left + 28, cy};
    c.Dot(ic, 17, t.track.Mix(t.accent.Alpha(0.28f), lit));
    Bars(c, t, ic.x - 12, ic.y + 9, n.signal, t.accentHi.Mix(t.textDim, 1 - lit)); // signal meter inside the badge

    bool needsPassword = !n.on && n.profile.empty() && n.secured;
    const wchar_t* label = n.on ? L"Disconnect" : needsPassword ? L"Join" : L"Connect";
    float aw = n.on ? 96.0f : 82.0f; // fits the longer label
    D2D1_RECT_F ar = {r.right - 12 - aw, cy - 15, r.right - 12, cy + 15};
    bool acted = Act(ui, t, id + 2, ar, label, !n.on, n.pending, live);
    if (acted) {
        if (n.on) {
            p.m.wifi.Disconnect();
        } else if (needsPassword) { // the panel can't take typing without stealing focus, windows' flyout asks
            sys::OpenUri(L"ms-availablenetworks:");
            p.wantClose = true;
        } else {
            p.m.wifi.Connect(n);
        }
    }

    float tx = r.left + 56, textR = ar.left - 10;
    c.Text(n.ssid, {tx, cy - 19, textR, cy + 1}, Weight::Medium, 13.5f, t.text); // ellipsis if too long
    Color sc = n.pending ? t.accentHi : n.on ? t.accent : t.textLow;
    if (n.secured && !n.on && !n.pending) { // padlock before secured networks
        c.Icon(glyph::Lock, {tx + 5, cy + 10}, 12, t.textLow);
        tx += 14;
    }
    if (n.on && !n.pending) { // live dot before "connected"
        c.Dot({tx + 3, cy + 10}, 3, t.accent);
        tx += 11;
    }
    c.Text(Status(n), {tx, cy + 1, textR, cy + 19}, Weight::Light, 11.5f, sc);
    if (n.pending) ui.KeepAlive(); // spinner + deadline expiry need frames
    return acted;
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
    Frame frame = Begin(p, k);

    bool on = m.radios.WifiOn();
    const sys::WifiNet* cur = nullptr;
    for (const auto& n : m.wifiNets)
        if (n.on) cur = &n;
    bool usable = on && m.wifiState == sys::WifiState::Ok;

    Radar(ui, t, "wifi.radar", frame.badge, on && cur, usable && m.wifiScan, glyph::Wifi, glyph::WifiOff, glyph::Wifi);
    c.Text(L"Wi-Fi", frame.title, Weight::Medium, frame.titleSize, t.text, frame.align);
    std::wstring sub = !on ? L"Turned off" : cur ? cur->ssid : L"Not connected";
    c.Text(sub, frame.sub, Weight::Light, 12, t.textDim, frame.align);
    if (w::Switch(ui, t, Ui::Id("wifi.power"), frame.sw, on, m.radios.HasWifi())) m.radios.SetWifi(!on);
    if (ScanBtn(ui, t, "wifi.scan", frame.pill, usable && m.wifiScan, on, glyph::Restart, L"Scan again",
                L"Scanning…") &&
        !m.wifiScan) {
        if (m.wifiState == sys::WifiState::Ok) m.wifi.Scan();
        else m.ReopenWifi(); // adapter came back or location was allowed since the sheet opened
        m.OnWifi();
    }

    D2D1_RECT_F list = frame.list;
    c.TextSp(L"NETWORKS", Rect(list.left + 4, list.top + 8, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    if (w::IconBtn(ui, t, Ui::Id("wifi.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseWifi();
    if (w::IconBtn(ui, t, Ui::Id("wifi.more"), {list.right - 48, list.top + 14}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // hand off to windows settings
        sys::OpenUri(L"ms-settings:network-wifi");
    }
    D2D1_RECT_F view = {list.left, list.top + 34, list.right, list.bottom}; // scrollable area under header

    if (!on) {
        Empty(c, t, view, glyph::WifiOff, L"Wi-Fi is off", L"Turn it on to see networks");
    } else if (m.wifiState == sys::WifiState::NoAdapter) {
        Empty(c, t, view, glyph::WifiOff, L"No Wi-Fi adapter", L"This PC has no wireless card, or it's disabled");
    } else if (m.wifiState == sys::WifiState::Denied) {
        Empty(c, t, view, glyph::Lock, L"Location access is off", L"Windows only lists Wi-Fi networks for apps when it's on");
        float cx = (view.left + view.right) / 2, by = (view.top + view.bottom) / 2 + 52;
        if (Act(ui, t, Ui::Id("wifi.loc"), {cx - 70, by - 15, cx + 70, by + 15}, L"Open settings", true, false, true)) {
            p.wantClose = true;
            sys::OpenUri(L"ms-settings:privacy-location");
        }
    } else if (m.wifiNets.empty()) {
        c.Text(m.wifiScan ? L"Looking for networks nearby…" : L"No networks found. Try scanning again.",
               Inset(D2D1_RECT_F{view.left, view.top, view.right, view.top + kNoteH}, 6, 0), Weight::Light, 12, t.textDim);
    } else {
        float total = m.wifiNets.size() * (kRowH + kRowGap); // content height for scroll clamp
        float scroll = Scroll(ui, "wifi.scroll", scrollTo, view, total);
        c.PushClip(view);
        float y = view.top - scroll;
        bool acted = false;
        for (const auto& n : m.wifiNets) {
            if (y + kRowH > view.top && y < view.bottom) acted = Row(p, {view.left, y, view.right, y + kRowH}, view, n) || acted; // skip offscreen rows
            y += kRowH + kRowGap;
        }
        c.PopClip();
        if (acted) m.OnWifi(); // after the loop: re-snapping replaces the list it walks
        Fades(c, t, view, scroll, total);
    }

    if (ui.RClick(s)) m.CloseWifi(); // right click again closes
    End(p, frame);
}
