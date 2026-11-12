#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "anim.h"
#include "glyphs.h"
#include "history.h"
#include "page_sheet.h"
#include "pages.h"
#include "saver.h"

void TodoList(PageCtx& p, const D2D1_RECT_F& r); // page_lists.cpp
void HostList(PageCtx& p, const D2D1_RECT_F& r);
void TimerField(PageCtx& p, const D2D1_RECT_F& r);

namespace {

constexpr float kTimerW = 400; // the timer card; the quick tiles take the rest
constexpr size_t kClockChars = 7; // 9:59:59; ten hours and up take the smaller size, clear of the stop button
constexpr float kClockLong = 38;
constexpr float kQuickGap = 10;
constexpr float kStackedW = 140; // w::Tile puts the icon over the words below this width
constexpr float kPickMinW = 220, kPickerMaxW = 1000, kPickerPad = 18, kPickerHeadH = 30, kPickerDoneW = 84; // the tiles' editor: its gallery
constexpr float kRemoveSize = 24, kRemoveInset = 6, kTileDragStart = 6; // a tile being arranged

bool tileShow = false; // the tiles are in their editor: they look as they do, and don't do anything
int heldTile = -1; // the one being dragged
History tileHist; // the editor's undo: the tiles, in order
sheet::Ids heldTiles; // the set as it would land
D2D1_POINT_2F heldTileGrab{};

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

    Cell cells[3]{}; // on the stack: this runs every frame the page shows
    size_t count = 0;
    cells[count++] = {L"CPU", glyph::Cpu, (float)s.cpu, w::Pct((float)s.cpu), std::to_wstring(s.threads) + L" threads"};
    if (s.gpu >= 0) cells[count++] = {L"GPU", glyph::Gpu, (float)s.gpu, w::Pct((float)s.gpu), L"3D engine"}; // gpu gauge only if counter exists
    cells[count++] = {L"Memory", glyph::Ram, (float)s.mem, w::Pct((float)s.mem), fmt::Gb(s.memUsedGb) + L" of " + fmt::Gb(s.memTotalGb)};

    float cw = (Width(left) - 20) / (float)count; // split card evenly among 2 or 3 gauges
    for (size_t i = 0; i < count; ++i)
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
    InfoRow(c, t, x, info.top + 184, wdt - 40, L"PUBLIC IP", !p.m.cfg.publicIp ? L"Off" : n.wanIp.empty() ? L"Looking up…" : n.wanIp); // narrower to fit copy button
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

void WorkTile(PageCtx& p, const D2D1_RECT_F& r) { // every workspace as a chip: the current one lit, busy ones with a dot; a tap switches
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const sys::WsState& s = m.wsNow;
    float hov = ui.Smooth(Ui::Id("tl.ws.hov"), Inside(r, ui.In().mouse) ? 1.0f : 0, 14);
    c.Fill(r, 18, t.cardHi.Alpha(0.35f).Mix(t.cardHi, 0.25f * hov));
    c.StrokeV(r, 18, t.hairTop, t.hairBot, 1);
    c.Icon(glyph::Workspaces, {r.left + 22, r.top + 22}, 18, t.textDim);
    sys::Tiler from = s.source == sys::WsState::kTiling ? sys::Tiler::kPipe : s.source == sys::WsState::kTilingWs ? sys::Tiler::kSocket : s.source == sys::WsState::kTilingCli ? sys::Tiler::kCli
                    : s.source == sys::WsState::kDesktops && !sys::TilesOnDesktops(m.tiler) ? sys::Tiler::kNone : m.tiler;
    std::wstring head = hov > 0.5f ? std::wstring(L"For ") + sys::TilerName(from) : L"Workspaces"; // the hover says whose workspaces these are
    c.Text(head, Rect(r.left + 40, r.top + 12, Width(r) - 50, 20), Weight::Medium, 12.5f, hov > 0.5f ? t.accentHi : t.text);
    if (s.list.empty()) {
        c.Text(L"Looking…", Rect(r.left + 16, r.top + 40, Width(r) - 32, 18), Weight::Light, 11.5f, t.textDim);
        return;
    }
    constexpr float kChip = 28, kChipGap = 6;
    int n = (int)s.list.size();
    float avail = Width(r) - 24, x = r.left + 12, y = r.top + 40;
    int perRow = (std::max)(1, (int)((avail + kChipGap) / (kChip + kChipGap)));
    for (int i = 0; i < n; ++i) {
        if (i > 0 && i % perRow == 0) x = r.left + 12, y += kChip + kChipGap;
        if (y + kChip > r.bottom - 6) break; // more than fit: the pill and the wheel still reach them
        D2D1_RECT_F chip = Rect(x, y, kChip, kChip);
        bool on = i == s.current;
        Ui::Hit h = ui.Button(Ui::Id("tl.ws", i), chip);
        float hov = ui.Smooth(Ui::Id("tl.ws", i + 100), h.hover ? 1.0f : 0, 16);
        c.Fill(chip, 9, on ? t.accent : t.track.Mix(t.cardHi, 0.5f * hov));
        std::wstring label = s.list[i].name.size() <= 2 ? s.list[i].name : std::to_wstring(i + 1); // long names show their number here, in full on the pill
        c.Text(label, chip, Weight::Semi, 11.5f, on ? t.onAccent : t.text, Align::Center);
        if (s.list[i].busy && !on) c.Dot({(chip.left + chip.right) / 2, chip.bottom - 4}, 1.8f, t.accentHi);
        if (h.clicked) m.ws.Focus(i);
        x += kChip + kChipGap;
    }
    if (tileShow) return;
    if (float notch = ui.Wheel(r)) m.ws.Step(notch > 0 ? -1 : 1); // the wheel walks through them
}

void Tile(PageCtx& p, int id, const D2D1_RECT_F& r) { // one quick tile: its state, the words that fit its width, what a tap does
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool narrow = Width(r) < kStackedW; // w::Tile stacks its icon over the words below this
    float room = narrow ? Width(r) - 24 : Width(r) - 56; // what's left beside or under the icon
    auto label = [&](const wchar_t* full, const wchar_t* brief) { return c.Measure(full, Weight::Medium, narrow ? 12.5f : 13) <= room ? full : brief; };
    auto detail = [&](const wchar_t* full, const wchar_t* brief) { return c.Measure(full, Weight::Light, narrow ? 10.5f : 11.5f) <= room ? full : brief; };
    uint32_t uid = Ui::Id("tl.tile", id);
    switch (id) {
    case Model::kTlSnip:
        if (w::Tile(ui, t, uid, r, glyph::Snip, label(L"Screenshot", L"Snip"), detail(L"Pick a region", L"A region"), false)) {
            p.wantClose = true; // hide drawer so it's not in the snip
            sys::OpenSnip();
        }
        break;
    case Model::kTlColor:
        if (w::Tile(ui, t, uid, r, glyph::Edit, L"Color", detail(L"Pick from screen", L"From screen"), false)) {
            p.wantClose = true; // out of the way of what you're picking from
            m.pickPending = true; // the picker starts once the panel is all the way up
        }
        break;
    case Model::kTlAwake:
        if (w::Tile(ui, t, uid, r, glyph::Coffee, L"Awake", m.awake ? detail(L"Screen stays on", L"Stays on") : L"Off", m.awake)) m.SetAwake(!m.awake);
        break;
    case Model::kTlAlerts:
        if (w::Tile(ui, t, uid, r, glyph::Bell, label(L"Notifications", L"Alerts"), detail(L"Open the list", L"Open"), false)) {
            p.wantClose = true;
            sys::OpenUri(L"ms-actioncenter:"); // windows' own notification center
        }
        break;
    case Model::kTlDnd:
        if (w::Tile(ui, t, uid, r, glyph::Dnd, label(L"Do not disturb", L"Quiet"), m.quiet ? L"On" : L"Off", m.quiet)) m.SetQuiet(!m.quiet); // halobar goes quiet with it
        break;
    case Model::kTlSaver: {
        static int saverHere = -1; // asked once: the state's shape doesn't change while running
        if (saverHere < 0) saverHere = sys::SaverAvailable() ? 1 : 0;
        bool saver = saverHere && sys::SaverOn();
        if (w::Tile(ui, t, uid, r, glyph::Saver, label(L"Energy saver", L"Saver"), saver ? L"On" : L"Off", saver, saverHere != 0) && !sys::SetSaver(!saver))
            p.wantClose = true; // older windows: its settings page opened instead
        break;
    }
    case Model::kTlAir: {
        bool hasW = m.radios.HasWifi(), hasB = m.radios.HasBt();
        bool air = (hasW || hasB) && !(hasW && m.radios.WifiOn()) && !(hasB && m.radios.BtOn()); // every radio off is airplane mode
        static bool wasW = true, wasB = true; // the radios that were on before airplane mode: only those come back
        if (w::Tile(ui, t, uid, r, air ? glyph::Airplane : glyph::AirplaneOff, L"Airplane", air ? L"On" : L"Off", air, hasW || hasB)) {
            if (!air) wasW = hasW && m.radios.WifiOn(), wasB = hasB && m.radios.BtOn();
            bool any = (hasW && wasW) || (hasB && wasB); // all were off when it began: off means every radio back
            if (hasW) m.radios.SetWifi(air && (wasW || !any)); // on: all of them down
            if (hasB) m.radios.SetBt(air && (wasB || !any));
        }
        break;
    }
    case Model::kTlWork:
        WorkTile(p, r);
        break;
    case Model::kTlLang: {
        std::wstring lang = sys::extras::Language();
        if (w::Tile(ui, t, uid, r, glyph::Translate, label(L"Language", L"Keys"), lang.empty() ? L"Switch" : lang, false)) sys::extras::NextLanguage(); // the app in front changes keyboard
        break;
    }
    case Model::kTlBin: {
        static sys::extras::BinInfo bin;
        static double at = -10;
        constexpr double kBinEvery = 5; // the bin is walked on disk: seconds between looks
        if (ui.Now() - at > kBinEvery) bin = sys::extras::Bin(), at = ui.Now();
        std::wstring size = bin.items == 0 ? std::wstring(L"Empty") : fmt::Gb(bin.bytes / (1024.0 * 1024 * 1024));
        if (w::Tile(ui, t, uid, r, glyph::Delete, label(L"Recycle bin", L"Bin"), size, false) && bin.items > 0) {
            sys::extras::EmptyBin(nullptr); // windows asks first
            at = -10;
        }
        break;
    }
    case Model::kTlUpdates: {
        const auto& u = m.updateNow;
        int waiting = (std::max)(0, u.system) + (std::max)(0, u.apps);
        std::wstring state = u.busy ? L"Checking…" : u.system < 0 && u.apps < 0 ? detail(L"Tap to check", L"Check") : waiting == 0 ? detail(L"Up to date", L"None") : std::to_wstring(waiting) + L" waiting";
        if (w::Tile(ui, t, uid, r, glyph::Update, L"Updates", state, waiting > 0)) {
            if (u.system > 0) p.wantClose = true, sys::OpenUri(L"ms-settings:windowsupdate");
            else if (u.apps > 0) p.wantClose = true, ShellExecuteW(nullptr, L"open", L"cmd.exe", L"/k winget upgrade --all", nullptr, SW_SHOWNORMAL); // asked for: shown in a console, nothing hidden
            else m.updates.Check(m.notify, WM_APP_UPDATES), m.updateNow.busy = true;
        }
        break;
    }
    default:
        if (w::Tile(ui, t, uid, r, glyph::Settings, L"Settings", L"Astraeus", false)) m.OpenSettings(); // windows settings is one tap away inside the sheet
        break;
    }
}

namespace {

D2D1_RECT_F TileSpot(const D2D1_RECT_F& quick, int n, int i) { // the i-th of n: half on top and the rest below
    int top = n / 2, bottom = n - top, count = i < top ? top : bottom, along = i < top ? i : i - top;
    float th = top ? (Height(quick) - kQuickGap) / 2 : Height(quick), tw = (Width(quick) - kQuickGap * (count - 1)) / count;
    return Rect(quick.left + along * (tw + kQuickGap), quick.top + (i < top || !top ? 0 : th + kQuickGap), tw, th);
}

sheet::Ids TilesShown(const Model& m) {
    sheet::Ids shown;
    for (int id : m.tiles)
        if (m.TileHere(id)) shown.push_back(id);
    return shown;
}

void QuickTiles(PageCtx& p, const D2D1_RECT_F& quick) { // the picked tiles; right-click to edit them
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    m.tileAreaW = Width(quick), m.tileAreaH = Height(quick);
    p.spotTiles = quick;
    if (ui.RClick(quick)) m.studioAsk = Model::kStudioTiles;
    sheet::Ids shown = TilesShown(m);
    int n = (int)shown.size();
    if (n == 0) { // every tile taken off: a way to bring them back
        Ui::Hit h = ui.Button(Ui::Id("tl.add"), quick);
        float hover = ui.Smooth(Ui::Id("tl.add", 1), h.hover ? 1.0f : 0, 16);
        c.Fill(quick, 18, t.track.Alpha(0.20f + 0.30f * hover));
        c.Stroke(quick, 18, t.accent.Alpha(0.30f + 0.40f * hover), 1);
        D2D1_POINT_2F o = {(quick.left + quick.right) / 2, (quick.top + quick.bottom) / 2 - 10};
        c.Icon(glyph::Add, o, 24, t.accentHi);
        c.Text(L"Add tiles", Rect(quick.left, o.y + 16, Width(quick), 18), Weight::Medium, 12.5f, t.textDim.Mix(t.text, hover), Align::Center);
        if (h.clicked) m.studioAsk = Model::kStudioTiles;
        return;
    }
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F r = TileSpot(quick, n, i);
        if (shown[(size_t)i] == Model::kTlSettings) p.spotSettings = r; // the tour's settings step points here
        Tile(p, shown[(size_t)i], r);
    }
}

void TileBoard(PageCtx& p, const D2D1_RECT_F& box) { // the tiles as the page lays them out: hover for the x, drag to move
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();
    sheet::Ids shown = TilesShown(m);
    const sheet::Ids& order = heldTile >= 0 ? heldTiles : shown;
    int n = (int)order.size(), removed = -1;
    if (n == 0) {
        float y = (box.top + box.bottom) / 2;
        c.Text(L"Your tiles go here", Rect(box.left, y - 22, Width(box), 24), Weight::Medium, 16, t.text, Align::Center);
        c.Text(L"Tap one below to add it", Rect(box.left, y + 4, Width(box), 18), Weight::Regular, 12, t.textDim, Align::Center);
        return;
    }
    tileShow = true;
    D2D1_RECT_F local = Rect(0, 0, Width(box), Height(box)); // the eased places don't count the rise it comes in on
    for (int i = 0; i < n; ++i) {
        int id = order[(size_t)i];
        D2D1_RECT_F want = TileSpot(local, n, i);
        float x = ui.Smooth(Ui::Id("tl.st.x", id), want.left, 18), y = ui.Smooth(Ui::Id("tl.st.y", id), want.top, 18);
        float wd = ui.Smooth(Ui::Id("tl.st.w", id), Width(want), 18), ht = ui.Smooth(Ui::Id("tl.st.h", id), Height(want), 18);
        if (id == heldTile) continue; // drawn last, on top of the rest
        D2D1_RECT_F r = Rect(box.left + x, box.top + y, wd, ht);
        bool over = heldTile < 0 && Inside(r, in.mouse);
        D2D1_POINT_2F corner = {r.right - kRemoveInset - kRemoveSize / 2, r.top + kRemoveInset + kRemoveSize / 2};
        Ui::Hit xh = over ? ui.Button(Ui::Id("tl.st.remove", id), Rect(corner.x - kRemoveSize / 2, corner.y - kRemoveSize / 2, kRemoveSize, kRemoveSize)) : Ui::Hit{}; // these first: the first button under a press takes it, so the tile's own never fires
        if (over) ui.Name(std::wstring(L"Remove ") + Model::TileName(id));
        uint32_t body = Ui::Id("tl.st.body", id);
        ui.Button(body, r);
        ui.NoStop();
        Tile(p, id, r);
        if (over) {
            c.Stroke(r, 18, t.accent.Alpha(0.7f), 1.5f);
            c.Dot(corner, kRemoveSize / 2, xh.hover ? sheet::kDanger : t.bg.Alpha(0.92f));
            c.Icon(glyph::Close, corner, 13, xh.hover ? Color{1, 1, 1, 1} : t.text);
        }
        if (xh.clicked) removed = id;
        if (heldTile < 0 && ui.IsActive(body) && in.down && std::hypot(in.mouse.x - in.pressPos.x, in.mouse.y - in.pressPos.y) > kTileDragStart) { // picked up
            heldTile = id;
            heldTiles = shown;
            heldTileGrab = {in.pressPos.x - r.left, in.pressPos.y - r.top};
        }
    }
    if (heldTile >= 0) { // it follows the mouse; the nearest place is the one it would land in
        ui.Wake();
        int best = 0;
        float bestD = 1e12f;
        for (int k = 0; k < n; ++k) {
            D2D1_RECT_F q = TileSpot(box, n, k);
            float dx = in.mouse.x - (q.left + q.right) / 2, dy = in.mouse.y - (q.top + q.bottom) / 2;
            if (dx * dx + dy * dy < bestD) bestD = dx * dx + dy * dy, best = k;
        }
        heldTiles.erase(std::find(heldTiles.begin(), heldTiles.end(), heldTile));
        heldTiles.insert(heldTiles.begin() + best, heldTile);
        D2D1_RECT_F spot = TileSpot(box, n, best);
        D2D1_RECT_F r = Rect(in.mouse.x - heldTileGrab.x, in.mouse.y - heldTileGrab.y, Width(spot), Height(spot));
        c.Glow({(r.left + r.right) / 2, (r.top + r.bottom) / 2 + 8}, Width(r) * 0.75f, Height(r) * 0.75f, Color{0, 0, 0, 0.45f});
        ui.Button(Ui::Id("tl.st.body", heldTile), r); // the press stays with it
        ui.NoStop();
        Tile(p, heldTile, r);
        c.Stroke(r, 18, t.accent, 1.8f);
        if (!in.down) { // let go: the order it landed in is kept
            heldTile = -1;
            if (!(heldTiles == shown)) sheet::KeepTiles(m, heldTiles);
        }
    }
    tileShow = false;
    if (removed >= 0) {
        shown.erase(std::find(shown.begin(), shown.end(), removed));
        sheet::KeepTiles(m, shown);
    }
}

void TilePicker(PageCtx& p, const D2D1_RECT_F& panel, const sheet::Ids& here, int cols) { // every tile this pc can use: a tap puts it on or takes it off
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F a = Inset(panel, kPickerPad, kPickerPad);
    float resetX = a.right - kPickerDoneW - 8 - history::kResetW, histX = resetX - 8 - history::kW;
    c.Text(L"Tools tiles", Rect(a.left + 2, a.top, histX - a.left - 10, kPickerHeadH), Weight::Semi, 15, t.text);
    if (sheet::Act(ui, t, Ui::Id("tl.st.done"), Rect(a.right - kPickerDoneW, a.top, kPickerDoneW, kPickerHeadH), L"Done", true, false, true)) p.wantClose = true;
    auto load = [&](const History::Snap& s) {
        std::vector<int> v;
        for (float id : s) v.push_back((int)id);
        m.SetTiles(v);
    };
    if (sheet::Act(ui, t, Ui::Id("tl.st.reset"), Rect(resetX, a.top, history::kResetW, kPickerHeadH), L"Reset", false, false, m.tiles != Model::DefaultTiles())) m.SetTiles(Model::DefaultTiles()); // undo brings it back
    int go = history::Buttons(ui, t, "tl.st.hist", histX, a.top + kPickerHeadH / 2, tileHist, true);
    if (go < 0) load(tileHist.Undo());
    else if (go > 0) load(tileHist.Redo());
    float top = a.top + kPickerHeadH + kPickerPad / 2, cw = (Width(a) - (cols - 1) * sheet::kPickGap) / cols;
    sheet::Ids shown = TilesShown(m);
    int toggle = -1;
    for (int i = 0; i < (int)here.size(); ++i) {
        int id = here[(size_t)i];
        D2D1_RECT_F card = Rect(a.left + (i % cols) * (cw + sheet::kPickGap), top + (i / cols) * (sheet::kPickH + sheet::kPickGap), cw, sheet::kPickH);
        bool on = std::find(shown.begin(), shown.end(), id) != shown.end();
        if (sheet::PickCard(ui, t, Ui::Id("tl.st.pick", id), card, Model::TileGlyph(id), Model::TileName(id), Model::TileAbout(id), on)) toggle = id;
    }
    if (toggle >= 0) {
        auto at = std::find(shown.begin(), shown.end(), toggle);
        if (at != shown.end()) shown.erase(at);
        else shown.push_back(toggle);
        sheet::KeepTiles(m, shown);
    }
}

}

void DrawTileStudio(PageCtx& p) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    const Input& in = p.ui.In();
    tileHist.Sync(m.studioRun, History::Snap(m.tiles.begin(), m.tiles.end()), !in.down && heldTile < 0); // a drag counts once it lands
    sheet::Ids here;
    for (int i = 0; i < Model::kToolTiles; ++i)
        if (m.TileHere(i)) here.push_back(i);
    float panelW = (std::min)(kPickerMaxW, Width(a) - 2 * sheet::kStudioEdge), inner = panelW - 2 * kPickerPad;
    int cols = (std::max)(2, (int)((inner + sheet::kPickGap) / (kPickMinW + sheet::kPickGap))), rows = ((int)here.size() + cols - 1) / cols;
    float panelH = 2 * kPickerPad + kPickerHeadH + kPickerPad / 2 + rows * sheet::kPickH + (rows - 1) * sheet::kPickGap;
    float boardW = (std::min)(m.tileAreaW + 2 * sheet::kBoardPad, Width(a) - 2 * sheet::kStudioEdge);
    float boardH = (std::min)(m.tileAreaH + 2 * sheet::kBoardPad, Height(a) - 2 * sheet::kStudioEdge - sheet::kPanelGap - panelH);
    float mid = (a.left + a.right) / 2, top = a.top + (Height(a) - boardH - sheet::kPanelGap - panelH) / 2; // the two together, in the middle of the screen
    D2D1_RECT_F board = Rect(mid - boardW / 2, top, boardW, boardH), panel = Rect(mid - panelW / 2, board.bottom + sheet::kPanelGap, panelW, panelH);
    sheet::Surface(c, t, board, t.bg.Alpha(sheet::kBoardFill));
    TileBoard(p, Inset(board, sheet::kBoardPad, sheet::kBoardPad));
    sheet::Surface(c, t, panel, t.cardHi);
    TilePicker(p, panel, here, cols);
    bool onSheet = Inside(board, in.pressPos) || Inside(panel, in.pressPos);
    if (heldTile < 0 && in.released && !onSheet) p.wantClose = true; // a click away, as in the long pill's editor
    if (in.rclick && !Inside(board, in.mouse) && !Inside(panel, in.mouse)) p.wantClose = true;
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
    D2D1_RECT_F timer = tall ? Rect(a.left, power.bottom + 12, Width(a), 190) : Rect(a.left, power.bottom + 12, kTimerW, a.bottom - power.bottom - 12);
    w::Card(ui, t, timer);
    static const wchar_t* kTabs[Model::kToolTabs] = {L"TIMER", L"STOPWATCH", L"FOCUS", L"ALARM", L"TO-DO", L"SERVERS"};
    float tx = timer.left + 20, tabW = 0, tabGap = 14;
    for (const wchar_t* tab : kTabs) tabW += 13 + 7.4f * std::wcslen(tab);
    if (tabW + tabGap * (Model::kToolTabs - 1) > Width(timer) - 40) tabGap = (std::max)(4.0f, (Width(timer) - 40 - tabW) / (Model::kToolTabs - 1)); // a narrow sidebar: the tabs close up
    for (int i = 0; i < Model::kToolTabs; ++i) { // the tools in one card, a slim marker under the one showing
        float tw = 13 + 7.4f * std::wcslen(kTabs[i]);
        Ui::Hit th = ui.Button(Ui::Id("tl.tab", i), Rect(tx - 4, timer.top + 8, tw + 8, 28));
        float on = ui.Smooth(Ui::Id("tl.tab", 10 + i), m.toolTab == i ? 1.0f : 0, 14);
        c.TextSp(kTabs[i], Rect(tx, timer.top + 16, tw, 14), Weight::Medium, 10, t.textLow.Mix(t.text, th.hover ? 0.5f : 0).Mix(t.accent, on), 1.4f);
        if (on > 0.02f) c.Fill(Rect(tx, timer.top + 34, (tw - 13) * on, 2), 1, t.accent);
        if (th.clicked) m.toolTab = i, m.field = Model::kFieldNone;
        tx += tw + tabGap;
    }
    D2D1_RECT_F listArea = {timer.left + 20, timer.top + 48, timer.right - 20, timer.bottom - 14};
    if (m.toolTab == Model::kTabTodo) {
        TodoList(p, listArea);
    } else if (m.toolTab == Model::kTabHosts) {
        HostList(p, listArea);
    } else if (m.toolTab == Model::kTabWatch) {
        Stopwatch(p, timer, tall);
    } else if (m.toolTab == Model::kTabFocus) {
        Focus(p, timer, tall);
    } else if (m.toolTab == Model::kTabAlarm) {
        c.Text(L"Alarms", listArea, Weight::Light, 12, t.textDim, Align::Center);
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
        float fieldY = timer.top + (tall ? 132 : 92), noteY = fieldY + 38;
        TimerField(p, Rect(timer.left + 20, fieldY, Width(timer) - 40, 32));
        if (noteY + 18 <= timer.bottom - 8) // the sidebar's shorter card goes without it
            c.Text(L"Ends with a notification, even if the dropdown is closed.", Rect(timer.left + 20, noteY, Width(timer) - 40, 18), Weight::Light, 12, t.textDim); // timer runs in model, survives close
    } else {
        if (m.field == Model::kFieldTimer) m.field = Model::kFieldNone, m.fieldEdit.clear(); // started another way: its field is gone, so it can't keep the keyboard
        D2D1_POINT_2F rc = {timer.left + 72, (timer.top + timer.bottom) / 2 + 6}; // ring left, countdown right
        w::Gauge(ui, t, Ui::Id("tl.ring"), rc, 40, 5, (float)m.TimerFrac());
        c.Icon(glyph::Timer, rc, 22, t.accent);
        std::wstring left = fmt::Clock(std::ceil(m.TimerLeft())); // ceil so 0:00 shows only at end
        c.Text(left, Rect(timer.left + 134, rc.y - 34, 220, 64), Weight::Thin, left.size() > kClockChars ? kClockLong : 50, t.text);
        if (w::IconBtn(ui, t, Ui::Id("tl.stop"), {timer.right - 44, rc.y}, 44, glyph::Stop, 26, true, true)) m.StopTimer();
    }

    D2D1_RECT_F quick = tall ? D2D1_RECT_F{a.left, timer.bottom + 12, a.right, a.bottom} : D2D1_RECT_F{timer.right + 12, timer.top, a.right, a.bottom};
    QuickTiles(p, quick);
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
