#include <cmath>

#include "glyphs.h"
#include "pages.h"

namespace {

void Player(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell; // the home card wears the accent; album colors are for the media page and fullscreen
    const sys::Track& tr = p.m.track;
    bool has = p.m.hasTrack;
    w::Card(ui, t, r);
    if (p.coverBlur) { // blurred art as card backdrop
        c.Image(p.coverBlur, r, 22, 0.55f * p.artFade);
        c.FillH(r, 22, t.veil.Alpha(0.20f), t.veil.Alpha(0.68f)); // wash toward bottom for text contrast
        c.StrokeV(r, 22, t.hairTop, t.hairBot, 1);
    } else {
        c.FillH(r, 22, t.accent.Alpha(0.10f), Color{0, 0, 0, 0}); // no art: faint accent wash
    }
    VizGlow(p, r, 1.15f, t); // glow scaled up a bit for small card

    const float pad = Height(r) < 160 ? 16.0f : 20.0f; // sidebar card is shorter
    float side = Height(r) - 2 * pad; // square art fills card height
    float k = (std::min)(1.0f, (Height(r) - 2 * pad) / 126); // text and transport rows squeeze with the card
    D2D1_RECT_F art = Rect(r.left + pad, r.top + pad, side, side);
    c.Fill(art, 16, t.cardHi);
    if (p.cover) c.Art(p.cover, p.coverBlur, art, 16, p.artFade);
    else c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, 40, t.textLow); // placeholder note, centered

    float tx = art.right + 20, tw = r.right - pad - tx; // text column right of art
    float ty = r.top + pad + 4 * k;
    c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(tx, ty, tw, 16), Weight::Medium, 10.5f, t.accent, 1.4f);
    c.Text(has && !tr.title.empty() ? tr.title : L"Nothing playing", Rect(tx, ty + 21 * k, tw, 24), Weight::Medium, 17, t.text);
    c.Text(has ? tr.artist : L"Play something in any app", Rect(tx, ty + 21 * k + 25, tw, 20), Weight::Light, 13, t.textDim);

    float cy = (std::min)(r.top + 117, r.bottom - pad - 22); // transport row under text, clear of the progress line
    bool slim = Height(r) < 160; // sidebar card: a lighter transport row
    float bs = slim ? 0.78f : 1.0f, step = 54 * bs;
    const Theme ct = theme::Lift(t); // the play button leans toward the ink, so it stands clear of the glow behind it
    if (w::IconBtn(ui, ct, Ui::Id("ov.prev"), {tx + 16 * bs, cy}, 38 * bs, glyph::Prev, 24 * bs, has && tr.canPrev)) p.m.media.Previous();
    if (w::PlayBtn(ui, ct, Ui::Id("ov.play"), {tx + 16 * bs + step, cy}, 44 * bs, has && tr.playing, has)) p.m.media.TogglePlay();
    if (w::IconBtn(ui, ct, Ui::Id("ov.next"), {tx + 16 * bs + 2 * step, cy}, 38 * bs, glyph::Next, 24 * bs, has && tr.canNext)) p.m.media.Next();

    if (!has || tr.duration > 0) { // app reports no length: no bar, a stuck empty one would mislead
        float y = r.bottom - pad + 4, x1 = r.right - pad; // progress line hugs card bottom
        static float preview = -1; // drag preview, committed on release like the media page
        uint32_t id = Ui::Id("ov.seek");
        float f = preview >= 0 ? preview : has ? (float)tr.Progress() : 0;
        if (has && tr.canSeek && ui.Slider(id, {tx, y - 9, x1, y + 9}, f, false)) preview = f; // a tall grab zone around the thin line
        if (ui.IsActive(id) && ui.In().released) {
            p.m.media.Seek(f);
            preview = -1;
        }
        if (!ui.IsActive(id)) preview = -1;
        float grow = ui.Smooth(id + 1, ui.IsActive(id) || (has && tr.canSeek && Inside({tx, y - 9, x1, y + 9}, ui.In().mouse)) ? 1.0f : 0, 16);
        float thick = 3 + 2 * grow; // line thickens under the mouse, a knob shows where it is
        c.Line({tx, y}, {x1, y}, t.track, thick);
        if (f > 0.002f) c.Line({tx, y}, {tx + (x1 - tx) * f, y}, t.accent, thick); // skip near-zero, avoids stray cap dot
        if (grow > 0.01f) c.Dot({tx + (x1 - tx) * f, y}, 5.5f * grow, t.accentHi);
    }
    Ui::Hit card = ui.Button(Ui::Id("ov.card"), r); // last, so the buttons and the bar above keep their own clicks
    float lift = ui.Smooth(Ui::Id("ov.card", 1), card.hover ? 1.0f : 0, 14);
    if (lift > 0.01f) c.Stroke(r, 22, t.accent.Alpha(0.30f * lift), 1.5f); // the whole card is a way to the media page
    if (card.clicked) p.wantPage = 1; // the media page
}

void WxCard(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    const sys::Wx& wx = p.m.wx;
    w::Card(ui, t, r);
    Ui::Hit open = ui.Button(Ui::Id("ov.wx"), r); // the card opens the calendar
    float lift = ui.Smooth(Ui::Id("ov.wx", 1), open.hover ? 1.0f : 0, 14);
    if (lift > 0.01f) c.Stroke(r, 22, t.accent.Alpha(0.30f * lift), 1.5f);
    if (open.clicked) p.m.OpenCal();
    if (!p.m.cfg.weather || !wx.valid) { // no weather yet: show date instead
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t day[64] = {}, date[64] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd", day, 64, nullptr); // locale-aware day name
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMMM", date, 64, nullptr);
        c.TextSp(fmt::Upper(day), Rect(r.left + 18, r.top + 20, 160, 16), Weight::Medium, 10.5f, t.accent, 1.4f);
        bool compact = Height(r) < 140; // sidebar tile: smaller date, no status line
        float room = Width(r) - 28, size = (std::min)(compact ? 28.0f : 34.0f, room / ((float)wcslen(date) * 0.50f)); // thin digits ~half an em wide: long dates shrink to fit
        c.Text(date, Rect(r.left + 16, r.top + 44, room + 8, 50), Weight::Thin, (std::max)(size, 16.0f), t.text);
        if (!compact)
            c.Text(p.m.cfg.weather ? L"Fetching weather…" : L"Weather is off", Rect(r.left + 18, r.top + 118, 160, 18), Weight::Light, 12, t.textDim);
        return;
    }
    c.Icon(fmt::WxIcon(wx.code, wx.isDay), {r.left + 32, r.top + 34}, 30, t.accent, true); // day/night icon variant
    bool clock = Height(r) >= 120 && Width(r) >= 170; // room for the stacked clock in the card's empty right strip
    if (clock) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t fmt24[4] = {};
        bool h24 = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, fmt24, 4) > 0 && fmt24[0] == L'1'; // the user's 12/24 hour choice
        int hour = h24 ? st.wHour : (st.wHour + 11) % 12 + 1;
        wchar_t hh[4], mm[4];
        swprintf_s(hh, L"%02d", hour);
        swprintf_s(mm, L"%02d", st.wMinute);
        float fs = (std::min)(Height(r) * 0.36f, 58.0f), lh = fs * 1.3f, cw = fs * 1.6f, right = r.right - 14; // two lines as one block
        float mid = (r.top + r.bottom) / 2, step = fs * 0.47f; // lines hug the card's middle, not its edges
        c.Text(hh, Rect(right - cw, mid - step - lh / 2, cw, lh), Weight::Thin, fs, t.text, Align::Right);
        c.Text(mm, Rect(right - cw, mid + step - lh / 2, cw, lh), Weight::Thin, fs, t.accent, Align::Right);
    }
    float textW = clock ? Width(r) - (std::min)(Height(r) * 0.36f, 58.0f) * 1.25f - 26 : 162; // weather keeps left of the clock
    c.Text(fmt::Temp(wx.tempC, p.m.cfg.fahr), Rect(r.left + 14, r.top + 52, textW + 8, 62), Weight::Thin, 52, t.text);
    c.Text(fmt::WxText(wx.code), Rect(r.left + 18, r.top + 110, textW, 18), Weight::Regular, 13, t.text);
    std::wstring sub = clock ? wx.place : wx.place + L"  ·  " + fmt::Temp(wx.highC, p.m.cfg.fahr) + L" / " + fmt::Temp(wx.lowC, p.m.cfg.fahr); // place · hi / lo
    c.Text(sub, Rect(r.left + 18, r.top + 129, textW, 16), Weight::Light, 11.5f, t.textDim);
}

void Toggles(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    const Theme& t = p.shell;
    Model& m = p.m;
    float tw = (Width(r) - 12) / 2, th = (Height(r) - 12) / 2; // 2x2 toggle grid, 12px gaps
    D2D1_RECT_F cells[4] = {Rect(r.left, r.top, tw, th), Rect(r.left + tw + 12, r.top, tw, th),
                            Rect(r.left, r.top + th + 12, tw, th), Rect(r.left + tw + 12, r.top + th + 12, tw, th)};

    bool wifi = m.radios.WifiOn();
    std::wstring wifiTxt = !m.radios.HasWifi() ? L"Unavailable" : !wifi ? L"Off" : m.net.ssid.empty() ? L"On" : m.net.ssid; // ssid when connected
    if (w::Tile(ui, t, Ui::Id("tg.wifi"), cells[0], wifi ? glyph::Wifi : glyph::WifiOff, L"Wi-Fi", wifiTxt, wifi, m.radios.HasWifi()))
        m.radios.SetWifi(!wifi);
    if (m.radios.HasWifi() && ui.RClick(cells[0])) m.OpenWifi(); // right click: network picker

    bool bt = m.radios.BtOn();
    if (w::Tile(ui, t, Ui::Id("tg.bt"), cells[1], bt ? glyph::Bluetooth : glyph::BluetoothOff, L"Bluetooth",
                m.radios.HasBt() ? (m.share.Active() ? L"Sharing audio" : bt ? L"On" : L"Off") : L"Unavailable", bt, m.radios.HasBt()))
        m.radios.SetBt(!bt);
    if (m.radios.HasBt() && ui.RClick(cells[1])) m.OpenBt(); // right click: device menu

    if (m.portable) { // laptop: power mode tile
        static const wchar_t* kModes[] = {L"Efficiency", L"Balanced", L"Performance"};
        static const int kModeIcons[] = {glyph::Eco, glyph::Balance, glyph::Bolt};
        int mode = m.pwrMode;
        std::wstring bat = m.batPct >= 0 ? std::to_wstring(m.batPct) + L"%" : L"Power"; // the battery is the title on a laptop; desktops never get this tile
        std::wstring sub = m.charging ? L"Charging" : mode >= 0 ? kModes[mode] : L"Unavailable"; // the icon still shows the mode
        if (w::Tile(ui, t, Ui::Id("tg.power"), cells[2], mode >= 0 ? kModeIcons[mode] : glyph::Bolt, bat.c_str(), sub, mode == 2, mode >= 0)) // lit only in perf
            m.CyclePower();
    } else { // desktop: output device tile
        const sys::OutDev* current = nullptr;
        for (const auto& d : m.outs)
            if (d.id == m.outId) current = &d;
        bool phones = current && current->headphones;
        if (w::Tile(ui, t, Ui::Id("tg.output"), cells[2], phones ? glyph::Headphones : glyph::Speaker, L"Output",
                    current ? current->name : L"No device", false, m.outs.size() > 1)) // cycling needs 2+ devices
            m.CycleOut();
    }

    bool live = m.mic.Ready() && !m.micMuted; // lit when mic open
    if (w::Tile(ui, t, Ui::Id("tg.mic"), cells[3], live ? glyph::Mic : glyph::MicOff, L"Mic",
                !m.mic.Ready() ? L"None" : live ? L"Live" : L"Muted", live, m.mic.Ready()))
        m.ToggleMic();
}

void Sliders(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    const Theme& t = p.shell;
    Model& m = p.m;
    w::Card(ui, t, r);
    int rows = 1 + (m.mic.Ready() ? 1 : 0) + (m.brightPct >= 0 ? 1 : 0); // volume always, mic/brightness if present
    const float h = 30, gap = 8;
    float y = r.top + (Height(r) - (rows * h + (rows - 1) * gap)) / 2; // center the stack vertically
    float x = r.left + 16, wdt = Width(r) - 32;
    bool icon = false;

    float v = m.vol;
    if (w::Pill(ui, t, Ui::Id("sl.vol"), Rect(x, y, wdt, h), m.muted || v < 0.005f ? glyph::VolumeOff : glyph::VolumeUp, v, &icon, m.muted)) // ~0 shows muted icon
        m.SetVol(v);
    if (icon || ui.RClick(Rect(x, y, wdt, h))) m.OpenMixer(); // the speaker opens every app's own volume
    y += h + gap;

    if (m.mic.Ready()) {
        float mv = m.micVol;
        if (w::Pill(ui, t, Ui::Id("sl.mic"), Rect(x, y, wdt, h), m.micMuted ? glyph::MicOff : glyph::Mic, mv, nullptr, m.micMuted)) // muted: greyed, a drag unmutes; the mic tile mutes
            m.SetMic(mv);
        y += h + gap;
    }
    if (m.brightPct >= 0) {
        float bv = m.brightPct / 100.0f;
        if (w::Pill(ui, t, Ui::Id("sl.light"), Rect(x, y, wdt, h), glyph::Bright, bv, nullptr)) m.SetBright(bv);
    }
}

void Stat(PageCtx& p, const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value,
              const Hist& h, float maxV, int page) { // page: where a click on the card goes
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    w::Card(ui, t, r, 20);
    uint32_t id = Ui::Id("ov.stat", page * 8 + (int)label[0]);
    Ui::Hit hit = ui.Button(id, r);
    float lift = ui.Smooth(id + 1, hit.hover ? 1.0f : 0, 14);
    if (lift > 0.01f) c.Stroke(r, 20, t.accent.Alpha(0.30f * lift), 1.5f); // the card is a way to its page
    if (hit.clicked) p.wantPage = page;
    c.TextSp(label, Rect(r.left + 14, r.top + 14, Width(r) - 28, 14), Weight::Medium, 10, t.textLow, 1.4f);
    c.Text(value, Rect(r.left + 13, r.top + 30, Width(r) - 26, 30), Weight::Light, 22, t.text);
    D2D1_RECT_F chart = {r.left + 1, (std::max)(r.bottom - 58, r.top + 62), r.right - 1, r.bottom - 10}; // sparkline in the bottom 48px, never over the value
    ComPtr<ID2D1RoundedRectangleGeometry> card; // inside the card's rounded corners, not its square bounds
    if (FAILED(gfx::D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(Inset(r, 1, 1), 19, 19), &card))) return;
    c.Target()->PushLayer(D2D1::LayerParameters(chart, card.Get()), nullptr); // bounded to the chart, so only its pixels pay for the mask
    w::Spark(c, chart, h.Values(), h.Version(), maxV, t.accent);
    c.Target()->PopLayer();
}

}

void DrawHome(PageCtx& p) {
    const D2D1_RECT_F& a = p.area;
    float x = a.left, y = a.top;
    if (Height(a) > Width(a)) { // sidebar: one column, most-used first
        const float g = 12, wd = Width(a), half = (wd - g) / 2;
        float row = (Height(a) - 4 * g) / 5.45f; // five rows, the player taller so its controls clear the text
        Model& m = p.m;
        Player(p, Rect(x, y, wd, row * 1.45f));
        y += row * 1.45f + g;
        Toggles(p, Rect(x, y, wd, row));
        y += row + g;
        Sliders(p, Rect(x, y, wd, row));
        y += row + g;
        WxCard(p, Rect(x, y, half, row));
        Stat(p, Rect(x + half + g, y, half, row), L"CPU", w::Pct(m.cpuHist.Last()), m.cpuHist, 1, 2); // the system page, with the processes
        y += row + g;
        float netMax = (std::max)(m.downHist.Max(), 64.0f * 1024);
        Stat(p, Rect(x, y, half, a.bottom - y), L"MEMORY", w::Pct(m.ramHist.Last()), m.ramHist, 1, 2);
        Stat(p, Rect(x + half + g, y, half, a.bottom - y), L"DOWNLOAD", fmt::Rate(m.downHist.Last()), m.downHist, netMax, 3); // the network page
        return;
    }
    const float topRow = 166, gap = 12; // fixed top row, bottom row takes rest
    Player(p, Rect(x, y, 388, topRow)); // columns at x 0 / 400 / 600
    WxCard(p, Rect(x + 400, y, 188, topRow));
    Toggles(p, Rect(x + 600, y, Width(a) - 600, topRow));
    Sliders(p, Rect(x, y + topRow + gap, 388, Height(a) - topRow - gap));

    float sw = (Width(a) - 400 - 24) / 3, sy = y + topRow + gap, sh = Height(a) - topRow - gap; // 3 stat cards share right side
    Model& m = p.m;
    Stat(p, Rect(x + 400, sy, sw, sh), L"CPU", w::Pct(m.cpuHist.Last()), m.cpuHist, 1, 2); // the system page, with the processes
    Stat(p, Rect(x + 412 + sw, sy, sw, sh), L"MEMORY", w::Pct(m.ramHist.Last()), m.ramHist, 1, 2);
    float netMax = (std::max)(m.downHist.Max(), 64.0f * 1024); // 64 KB/s floor keeps idle graph flat
    Stat(p, Rect(x + 424 + 2 * sw, sy, sw, sh), L"DOWNLOAD", fmt::Rate(m.downHist.Last()), m.downHist, netMax, 3); // the network page
}
