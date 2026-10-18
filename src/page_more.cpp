#include <algorithm>
#include <cmath>
#include <cwctype>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "pages.h"

namespace {

struct Cell {
    const wchar_t* name;
    int glyph;
    float value;
    std::wstring big, detail;
};

void GaugeCell(PageCtx& p, const D2D1_RECT_F& r, const Cell& cell, int index) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    const float radius = 50;
    D2D1_POINT_2F center = {(r.left + r.right) / 2, r.top + 96}; // gauge center near top of cell
    w::Gauge(p.ui, t, Ui::Id("sys.g", index), center, radius, 6, cell.value);
    c.Icon(cell.glyph, {center.x, center.y - 16}, 20, t.accent); // icon sits above value inside ring
    c.Text(cell.big, Rect(center.x - radius, center.y - 4, radius * 2, 28), Weight::Light, 22, t.text, Align::Center);
    c.Text(cell.name, Rect(r.left + 6, r.top + 170, Width(r) - 12, 20), Weight::Medium, 14, t.text, Align::Center); // label + detail below gauge
    c.Text(cell.detail, Rect(r.left + 6, r.top + 192, Width(r) - 12, 18), Weight::Light, 11.5f, t.textDim, Align::Center);
}

void Procs(PageCtx& p, const D2D1_RECT_F& r) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    w::Card(p.ui, t, r);
    c.TextSp(L"TOP PROCESSES", Rect(r.left + 18, r.top + 16, Width(r) - 36, 16), Weight::Medium, 10, t.textLow, 1.4f);
    if (w::IconBtn(p.ui, t, Ui::Id("sys.taskmgr"), {r.right - 26, r.top + 23}, 28, glyph::OpenNew, 15)) { // the whole list, in task manager
        p.wantClose = true;
        sys::OpenUri(L"taskmgr.exe");
    }
    float y = r.top + 42; // rows start under header
    const float rowH = 40;
    for (size_t i = 0; i < p.m.procs.size() && y + rowH <= r.bottom - 6; ++i, y += rowH) { // stop when next row would overflow card
        const sys::ProcRow& pr = p.m.procs[i];
        float x0 = r.left + 18, x1 = r.right - 18;
        c.Text(pr.name, Rect(x0, y, x1 - x0 - 70, 20), Weight::Medium, 13, t.text); // name leaves room for cpu % on right
        c.Text(w::Pct((float)pr.cpu), Rect(x1 - 70, y, 70, 20), Weight::Medium, 12.5f, t.text, Align::Right);
        c.Text(fmt::Gb(pr.memBytes / 1073741824.0), Rect(x1 - 90, y + 18, 90, 14), Weight::Light, 10.5f, t.textLow, Align::Right); // bytes -> gb
        float bar = p.ui.Soft(Ui::Id("sys.bar", (int)i), anim::Clamp01((float)pr.cpu)); // eased bar so values don't jump
        float by = y + 26; // bar under name line
        c.Line({x0, by}, {x1 - 100, by}, t.track, 3);
        if (bar > 0.003f) c.Line({x0, by}, {x0 + (x1 - 100 - x0) * bar, by}, t.accent, 3); // skip near-zero to avoid cap dot
    }
    if (p.m.procs.empty()) c.Text(L"Measuring…", Rect(r.left + 18, r.top + 46, 200, 20), Weight::Light, 12.5f, t.textDim); // worker hasn't sampled procs yet
}

void InfoRow(Canvas& c, const Theme& t, float x, float y, float wdt, const wchar_t* label, const std::wstring& value) {
    c.TextSp(label, Rect(x, y, wdt, 14), Weight::Medium, 9.5f, t.textLow, 1.4f);
    c.Text(value.empty() ? L"—" : value, Rect(x, y + 15, wdt, 20), Weight::Regular, 13.5f, t.text);
}

void Clip(const std::wstring& s) {
    if (s.empty() || !OpenClipboard(nullptr)) return; // bail if another app holds clipboard
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t); // include null terminator
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes); // clipboard needs movable global mem
    if (mem) {
        memcpy(GlobalLock(mem), s.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem); // on success clipboard owns mem
    }
    CloseClipboard();
}

double copiedAt = -10; // far past so check icon starts hidden

bool PowerBtn(PageCtx& p, D2D1_POINT_2F c, int glyph, const wchar_t* label, const wchar_t* hint, int index, bool hold) {
    Ui& ui = p.ui;
    Canvas& cv = ui.C();
    const Theme& t = p.shell;
    uint32_t id = Ui::Id("tl.pw", index);
    D2D1_RECT_F hit = {c.x - 44, c.y - 30, c.x + 44, c.y + 52}; // hit box covers circle + label
    bool fire = false;
    float progress = 0;
    Ui::Hit h;
    if (hold) {
        fire = ui.Hold(id, hit, 0.9f, progress); // hold 0.9s to fire, progress fills ring
        h.hover = Inside(hit, ui.In().mouse);
    } else {
        h = ui.Button(id, hit);
        fire = h.clicked;
    }
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    cv.Dot(c, 27, t.cardHi.Mix(t.track, hover));
    cv.Ring(c, 27, t.hairTop.Mix(t.accent.Alpha(0.5f), hover), 1);
    if (progress > 0) cv.Arc(c, 27, -3.14159265f / 2, progress * 2 * 3.14159265f, t.accent, 3); // progress arc from 12 o'clock
    cv.Icon(glyph, c, 24, hover > 0.5f ? t.accentHi : t.text);
    const wchar_t* text = progress > 0 ? L"Keep holding…" : (hover > 0.5f ? hint : label); // label swaps to hint on hover
    cv.Text(text, Rect(c.x - 74, c.y + 34, 148, 18), Weight::Medium, 12, hover > 0.5f ? t.text : t.textDim, Align::Center);
    return fire;
}

}

void DrawSys(PageCtx& p) {
    const D2D1_RECT_F& a = p.area;
    const sys::Stats& s = p.m.stats;
    bool tall = Height(a) > Width(a); // sidebar: gauges on top, processes below
    D2D1_RECT_F left = tall ? Rect(a.left, a.top, Width(a), 232) : Rect(a.left, a.top, 480, Height(a)); // fixed gauge card, procs take rest
    w::Card(p.ui, p.shell, left);

    std::vector<Cell> cells;
    cells.push_back({L"CPU", glyph::Cpu, (float)s.cpu, w::Pct((float)s.cpu), std::to_wstring(s.threads) + L" threads"});
    if (s.gpu >= 0) cells.push_back({L"GPU", glyph::Gpu, (float)s.gpu, w::Pct((float)s.gpu), L"3D engine"}); // gpu gauge only if counter exists
    cells.push_back({L"Memory", glyph::Ram, (float)s.mem, w::Pct((float)s.mem), fmt::Gb(s.memUsedGb) + L" of " + fmt::Gb(s.memTotalGb)});

    float cw = (Width(left) - 20) / (float)cells.size(); // split card evenly among 2 or 3 gauges
    for (size_t i = 0; i < cells.size(); ++i)
        GaugeCell(p, Rect(left.left + 10 + cw * i, left.top + 10, cw, Height(left) - 20), cells[i], (int)i);

    Procs(p, tall ? D2D1_RECT_F{a.left, left.bottom + 12, a.right, a.bottom} : D2D1_RECT_F{left.right + 12, a.top, a.right, a.bottom});
}

void DrawNet(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    const sys::Net& n = p.m.net;
    const D2D1_RECT_F& a = p.area;

    bool tall = Height(a) > Width(a); // sidebar: info on top, graph below
    D2D1_RECT_F info = tall ? Rect(a.left, a.top, Width(a), 290) : Rect(a.left, a.top, 316, Height(a));
    w::Card(ui, t, info);
    bool wifi = !n.ssid.empty(); // ssid present = wifi, else wired
    c.Icon(wifi ? glyph::Wifi : glyph::Lan, {info.left + 32, info.top + 36}, 26, t.accent, true);
    c.Text(wifi ? n.ssid : (n.online ? L"Ethernet" : L"Offline"), Rect(info.left + 18, info.top + 58, 280, 34), Weight::Light, 25, t.text);
    if (p.m.radios.HasWifi() && // opens the wi-fi picker sheet, also on right click of the home tile
        sheet::Act(ui, t, Ui::Id("net.networks"), Rect(info.right - 112, info.top + 21, 94, 30), L"Networks", false, false, true))
        p.m.OpenWifi();
    if (wifi) {
        sheet::Bars(c, t, info.left + 20, info.top + 112, n.signal, t.accent);
        c.Text(std::to_wstring(n.signal) + L"% signal", Rect(info.left + 56, info.top + 96, 200, 18), Weight::Regular, 12.5f, t.textDim);
    }
    float x = info.left + 20, wdt = 276;
    InfoRow(c, t, x, info.top + 134, wdt, L"LOCAL IP", n.lanIp);
    InfoRow(c, t, x, info.top + 184, wdt - 40, L"PUBLIC IP", n.wanIp.empty() ? L"Looking up…" : n.wanIp); // narrower to fit copy button
    bool copied = ui.Now() - copiedAt < 1.6; // check icon shows 1.6s after copy
    if (!n.wanIp.empty() &&
        w::IconBtn(ui, t, Ui::Id("net.copy"), {info.right - 34, info.top + 206}, 34, copied ? glyph::Check : glyph::Copy, 18, true, copied)) {
        Clip(n.wanIp);
        copiedAt = ui.Now();
    }
    if (copied) ui.KeepAlive(); // repaint until check reverts
    InfoRow(c, t, x, info.top + 234, wdt, L"ADAPTER", n.adapter);

    D2D1_RECT_F graph = tall ? D2D1_RECT_F{a.left, info.bottom + 12, a.right, a.bottom} : D2D1_RECT_F{info.right + 12, a.top, a.right, a.bottom};
    w::Card(ui, t, graph);
    c.TextSp(L"THROUGHPUT", Rect(graph.left + 20, graph.top + 18, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    c.Icon(glyph::Up, {graph.left + 30, graph.top + 56}, 20, t.accentHi);
    c.Text(fmt::Rate(n.upBps), Rect(graph.left + 46, graph.top + 42, 170, 30), Weight::Light, 22, t.text);
    c.Icon(glyph::Down, {graph.left + 250, graph.top + 56}, 20, t.accent);
    c.Text(fmt::Rate(n.downBps), Rect(graph.left + 266, graph.top + 42, 170, 30), Weight::Light, 22, t.text);
    float pm = p.m.pingMs; // latency to the internet, measured only while this page shows
    D2D1_RECT_F lat = Rect(graph.right - 170, graph.top + 16, 150, 70);
    c.TextSp(L"LATENCY", Rect(lat.left, lat.top + 2, Width(lat), 14), Weight::Medium, 10, t.textLow, 1.4f, Align::Right);
    std::wstring ms = pm >= 0 ? std::to_wstring((int)std::lround(pm)) + L" ms" : pm == -1 ? L"No reply" : L"\u2026";
    c.Text(ms, Rect(lat.left, lat.top + 26, Width(lat), 30), Weight::Light, 22, pm >= 0 ? t.text : t.textDim, Align::Right);
    if (pm >= 0) c.Text(L"\u00B1 " + std::to_wstring((int)std::lround(p.m.jitterMs)) + L" ms jitter", Rect(lat.left, lat.top + 54, Width(lat), 16), Weight::Light, 11, t.textDim, Align::Right);

    float top = graph.top + 92, bottom = graph.bottom - 16, mid = (top + bottom) / 2; // graph split: up on top, down mirrored below
    float peak = (std::max)((std::max)(p.m.upHist.Max(), p.m.downHist.Max()), 64.0f * 1024); // shared scale, 64 KB/s floor
    D2D1_RECT_F upR = {graph.left + 1, top, graph.right - 1, mid - 1};
    D2D1_RECT_F downR = {graph.left + 1, mid + 1, graph.right - 1, bottom};
    c.PushClip(Inset(graph, 1, 1));
    w::Spark(c, upR, p.m.upHist.Values(), p.m.upHist.Version(), peak, t.accentHi);
    w::Spark(c, downR, p.m.downHist.Values(), p.m.downHist.Version(), peak, t.accent, true); // true = draw downward
    c.PopClip();
    c.Line({graph.left + 20, mid}, {graph.right - 20, mid}, t.hairTop, 1, false);
    c.Text(L"60 s", Rect(graph.right - 60, bottom - 16, 44, 14), Weight::Regular, 10, t.textLow, Align::Right);
}

std::wstring Watch(ULONGLONG ms) { // 1:02.4, or 1:02:03.4 past an hour
    ULONGLONG s = ms / 1000, tenth = ms / 100 % 10;
    wchar_t buf[32];
    if (s >= 3600) swprintf_s(buf, L"%llu:%02llu:%02llu.%llu", s / 3600, s / 60 % 60, s % 60, tenth);
    else swprintf_s(buf, L"%llu:%02llu.%llu", s / 60, s % 60, tenth);
    return buf;
}

void Stopwatch(PageCtx& p, const D2D1_RECT_F& r, bool tall) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    float cy = r.top + 74;
    c.Text(Watch(m.SwMs()), Rect(r.left + 20, cy - 28, Width(r) - 40, 60), Weight::Thin, 46, t.text);
    if (m.swOn) ui.KeepAlive(); // tenths tick only while it runs and you can see it
    float bx = r.left + 20, by = r.bottom - 52;
    if (sheet::Act(ui, t, Ui::Id("tl.sw.go"), Rect(bx, by, 96, 34), m.swOn ? L"Pause" : m.SwMs() ? L"Resume" : L"Start", true, false, true)) m.SwToggle();
    if (sheet::Act(ui, t, Ui::Id("tl.sw.lap"), Rect(bx + 104, by, 76, 34), L"Lap", false, false, m.swOn)) m.SwLap();
    if (sheet::Act(ui, t, Ui::Id("tl.sw.reset"), Rect(bx + 188, by, 76, 34), L"Reset", false, false, !m.swOn && m.SwMs() > 0)) m.SwReset();
    float lx = tall ? r.left + 20 : r.right - 150, ly = tall ? cy + 34 : r.top + 52;
    for (size_t i = 0; i < m.laps.size() && i < (tall ? 2u : 4u); ++i) { // the latest laps, newest first
        ULONGLONG prev = i + 1 < m.laps.size() ? m.laps[i + 1] : 0;
        c.Text(L"Lap " + std::to_wstring(m.laps.size() - i), Rect(lx, ly + i * 20, 60, 18), Weight::Regular, 11.5f, t.textDim);
        c.Text(Watch(m.laps[i] - prev), Rect(lx + 56, ly + i * 20, 80, 18), Weight::Medium, 11.5f, t.text);
    }
}

void Focus(PageCtx& p, const D2D1_RECT_F& r, bool tall) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    if (!m.focusOn) { // pick a rhythm
        struct Plan { int focus, rest; } plans[] = {{25, 5}, {50, 10}, {90, 15}};
        float cw = (Width(r) - 40 - 16) / 3;
        for (int i = 0; i < 3; ++i) {
            std::wstring label = std::to_wstring(plans[i].focus) + L" / " + std::to_wstring(plans[i].rest);
            if (w::Chip(ui, t, Ui::Id("tl.focus", i), Rect(r.left + 20 + i * (cw + 8), r.top + 52, cw, 32), label, false)) m.StartFocus(plans[i].focus, plans[i].rest);
        }
        c.Text(L"Minutes of focus, then a break, round after round.", Rect(r.left + 20, r.top + (tall ? 100 : 100), Width(r) - 40, 18), Weight::Light, 12, t.textDim);
        return;
    }
    D2D1_POINT_2F rc = {r.left + 72, (r.top + r.bottom) / 2 + 6};
    w::Gauge(ui, t, Ui::Id("tl.focus.ring"), rc, 40, 5, (float)m.FocusFrac());
    c.Icon(m.focusBreak ? glyph::Coffee : glyph::Timer, rc, 22, t.accent);
    c.TextSp(m.focusBreak ? std::wstring(L"BREAK") : L"FOCUS  \u00B7  ROUND " + std::to_wstring(m.focusRound), Rect(r.left + 134, rc.y - 40, 240, 14), Weight::Medium, 10,
             m.focusBreak ? t.textDim : t.accentHi, 1.4f);
    c.Text(fmt::Clock(std::ceil(m.FocusLeft())), Rect(r.left + 134, rc.y - 26, 220, 64), Weight::Thin, 50, t.text);
    if (w::IconBtn(ui, t, Ui::Id("tl.focus.skip"), {r.right - 92, rc.y}, 44, glyph::Next, 24)) m.SkipFocus();
    if (w::IconBtn(ui, t, Ui::Id("tl.focus.stop"), {r.right - 44, rc.y}, 44, glyph::Stop, 26, true, true)) m.StopFocus();
}

void DrawTools(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;

    D2D1_RECT_F power = Rect(a.left, a.top, Width(a), 132); // power row across full width
    w::Card(ui, t, power);
    struct Action { int glyph; const wchar_t* label; const wchar_t* hint; bool hold; void (*run)(); }; // hold = destructive, needs press-and-hold
    const Action actions[] = {{glyph::Lock, L"Lock", L"Click to lock", false, sys::power::Lock},
                              {glyph::Sleep, L"Sleep", L"Click to sleep", false, sys::power::Sleep},
                              {glyph::SignOut, L"Sign out", L"Hold to sign out", true, sys::power::SignOut},
                              {glyph::Restart, L"Restart", L"Hold to restart", true, sys::power::Restart},
                              {glyph::Power, L"Shut down", L"Hold to shut down", true, sys::power::Shutdown}};
    float step = Width(power) / 5; // 5 equal columns
    for (int i = 0; i < 5; ++i) {
        D2D1_POINT_2F center = {power.left + step * (i + 0.5f), power.top + 50};
        if (PowerBtn(p, center, actions[i].glyph, actions[i].label, actions[i].hint, i, actions[i].hold)) {
            p.wantClose = true; // close drawer before acting
            actions[i].run();
        }
    }

    bool tall = Height(a) > Width(a); // sidebar: timer and tiles stack under the power row
    D2D1_RECT_F timer = tall ? Rect(a.left, power.bottom + 12, Width(a), 190) : Rect(a.left, power.bottom + 12, 470, a.bottom - power.bottom - 12);
    w::Card(ui, t, timer);
    static const wchar_t* kTabs[] = {L"TIMER", L"STOPWATCH", L"FOCUS"};
    float tx = timer.left + 20;
    for (int i = 0; i < 3; ++i) { // three tools in one card, a slim marker under the one showing
        float tw = 13 + 7.4f * std::wcslen(kTabs[i]);
        Ui::Hit th = ui.Button(Ui::Id("tl.tab", i), Rect(tx - 4, timer.top + 8, tw + 8, 28));
        float on = ui.Smooth(Ui::Id("tl.tab", 10 + i), m.toolTab == i ? 1.0f : 0, 14);
        c.TextSp(kTabs[i], Rect(tx, timer.top + 16, tw, 14), Weight::Medium, 10, t.textLow.Mix(t.text, th.hover ? 0.5f : 0).Mix(t.accent, on), 1.4f);
        if (on > 0.02f) c.Fill(Rect(tx, timer.top + 34, (tw - 13) * on, 2), 1, t.accent);
        if (th.clicked) m.toolTab = i;
        tx += tw + 14;
    }
    if (m.toolTab == 1) {
        Stopwatch(p, timer, tall);
    } else if (m.toolTab == 2) {
        Focus(p, timer, tall);
    } else if (!m.timerOn) {
        const int minutes[] = {1, 5, 10, 15, 25, 45, 60};
        int perRow = tall ? 4 : 7; // sidebar: two rows so the labels fit
        float cx = timer.left + 20, cw = (Width(timer) - 40 - (perRow - 1) * 8) / perRow; // 8px gaps
        for (int i = 0; i < 7; ++i) {
            std::wstring label = std::to_wstring(minutes[i]) + (minutes[i] == 60 ? L"h" : L"m");
            if (minutes[i] == 60) label = L"1h"; // 60 shows as 1h not 60m
            if (w::Chip(ui, t, Ui::Id("tl.min", i), Rect(cx + (i % perRow) * (cw + 8), timer.top + 52 + (i / perRow) * 40, cw, 32), label, false))
                m.StartTimer(minutes[i] * 60);
        }
        c.Text(L"Ends with a notification, even if the dropdown is closed.", Rect(timer.left + 20, timer.top + (tall ? 140 : 100), Width(timer) - 40, 18), // timer runs in model, survives close
               Weight::Light, 12, t.textDim);
    } else {
        D2D1_POINT_2F rc = {timer.left + 72, (timer.top + timer.bottom) / 2 + 6}; // ring left, countdown right
        w::Gauge(ui, t, Ui::Id("tl.ring"), rc, 40, 5, (float)m.TimerFrac());
        c.Icon(glyph::Timer, rc, 22, t.accent);
        c.Text(fmt::Clock(std::ceil(m.TimerLeft())), Rect(timer.left + 134, rc.y - 34, 220, 64), Weight::Thin, 50, t.text); // ceil so 0:00 shows only at end
        if (w::IconBtn(ui, t, Ui::Id("tl.stop"), {timer.right - 44, rc.y}, 44, glyph::Stop, 26, true, true)) m.StopTimer();
    }

    D2D1_RECT_F quick = tall ? D2D1_RECT_F{a.left, timer.bottom + 12, a.right, a.bottom} : D2D1_RECT_F{timer.right + 12, timer.top, a.right, a.bottom};
    float tw = (Width(quick) - 10) / 2, th = (Height(quick) - 10) / 2; // 2x2 quick grid, 10px gaps
    // two by two: screenshot and color on top; awake and settings below on laptops, settings across on desktops (task manager lives on the system page)
    if (w::Tile(ui, t, Ui::Id("tl.snip"), Rect(quick.left, quick.top, tw, th), glyph::Snip, L"Screenshot", L"Pick a region", false)) {
        p.wantClose = true; // hide drawer so it's not in the snip
        sys::OpenSnip();
    }
    if (w::Tile(ui, t, Ui::Id("tl.pick"), Rect(quick.left + tw + 10, quick.top, tw, th), glyph::Edit, L"Color", L"Pick from screen", false)) {
        p.wantClose = true; // out of the way of what you're picking from
        m.pickPending = true; // the picker starts once the panel is all the way up
    }
    float sx = quick.left, sw = Width(quick);
    if (m.portable) {
        if (w::Tile(ui, t, Ui::Id("tl.awake"), Rect(quick.left, quick.top + th + 10, tw, th), glyph::Coffee, L"Awake", m.awake ? L"Screen stays on" : L"Off", m.awake))
            m.SetAwake(!m.awake);
        sx += tw + 10;
        sw = tw;
    }
    if (w::Tile(ui, t, Ui::Id("tl.settings"), Rect(sx, quick.top + th + 10, sw, th), glyph::Settings, L"Settings", L"Astraeus Island", false))
        m.OpenSettings(); // windows settings is one tap away inside the sheet
}

namespace fmt {

std::wstring Rate(double bps) {
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    int u = 0;
    while (bps >= 1000 && u < 3) { // 1024 steps, switch unit at 1000 so max 3 digits
        bps /= 1024;
        ++u;
    }
    wchar_t buf[32];
    swprintf_s(buf, bps < 10 && u > 0 ? L"%.1f %s" : L"%.0f %s", bps, units[u]); // one decimal only under 10
    return buf;
}

std::wstring Clock(double seconds) {
    long long s = (long long)(seconds < 0 ? 0 : seconds); // clamp negatives
    wchar_t buf[32];
    if (s >= 3600) swprintf_s(buf, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60); // h:mm:ss over an hour
    else swprintf_s(buf, L"%lld:%02lld", s / 60, s % 60);
    return buf;
}

std::wstring Upper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) { return (wchar_t)std::towupper(ch); });
    return s;
}

std::wstring Temp(double c, bool fahr) {
    double v = fahr ? c * 9 / 5 + 32 : c; // c -> f
    return std::to_wstring((int)std::lround(v)) + L"°";
}

std::wstring Gb(double gb) {
    wchar_t buf[32];
    if (gb < 1) swprintf_s(buf, L"%.0f MB", gb * 1024); // under 1 gb show mb
    else swprintf_s(buf, gb >= 100 ? L"%.0f GB" : L"%.1f GB", gb);
    return buf;
}

int WxIcon(int code, bool day) { // wmo weather codes
    if (code == 0) return day ? glyph::ClearDay : glyph::ClearNight;
    if (code <= 2) return day ? glyph::PartlyDay : glyph::PartlyNight;
    if (code == 3) return glyph::Cloud;
    if (code == 45 || code == 48) return glyph::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return glyph::Snow;
    if (code >= 95) return glyph::Storm;
    return glyph::Rain;
}

const wchar_t* WxText(int code) {
    if (code == 0) return L"Clear";
    if (code <= 2) return L"Partly cloudy";
    if (code == 3) return L"Overcast";
    if (code == 45 || code == 48) return L"Fog";
    if (code >= 51 && code <= 57) return L"Drizzle";
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return L"Rain";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return L"Snow";
    if (code >= 95) return L"Thunderstorm";
    return L"Unsettled";
}

}
