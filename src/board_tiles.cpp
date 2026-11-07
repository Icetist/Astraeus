// The dashboard's tiles: each one reads the model and fills its own card, sized by the cells it covers.
#include "board.h"

#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr float kRadius = 20; // the cards' corners
constexpr float kPad = 18; // inside a card, times the tile's scale
constexpr float kBaseSide = 150; // a tile this many dips on its short side draws at scale 1
constexpr double kLyrWaitSec = 12;
constexpr float kHeadMax = 1.5f; // headers, knobs and lines grow with a tile only this far

float Scale(const D2D1_RECT_F& r) { return std::clamp((std::min)(Width(r), Height(r)) / kBaseSide, 0.6f, 2.4f); }

void Caption(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const std::wstring& text, float k) { // the little spaced label in a card's corner
    c.TextSp(text, Rect(r.left + kPad * k, r.top + 14 * k, Width(r) - 2 * kPad * k, 14 * k), Weight::Medium, 10 * k, t.textLow, 1.4f);
}

int64_t DaysFrom(int y, int m, int d) { // days since 1970-01-01, any calendar date
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int WeekDay(int y, int m, int d) { return (int)((DaysFrom(y, m, d) % 7 + 11) % 7); } // 0 sunday; 1970-01-01 was a thursday

int DaysIn(int y, int m) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : kDays[m - 1];
}

std::wstring DateText(int y, int m, int d, const wchar_t* pattern) { // in the user's language
    SYSTEMTIME st = {};
    st.wYear = (WORD)y, st.wMonth = (WORD)m, st.wDay = (WORD)d, st.wDayOfWeek = (WORD)WeekDay(y, m, d);
    wchar_t buf[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, pattern, buf, 64, nullptr);
    return buf;
}

int FirstDay() { // the week's first day in the user's settings, 0 sunday
    wchar_t v[4] = {};
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, v, 4)) return 1;
    return (v[0] - L'0' + 1) % 7; // windows counts from monday
}

bool Chevron(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F at, float s, bool back) { // a month back or forward
    D2D1_RECT_F hit = {at.x - s, at.y - s, at.x + s, at.y + s};
    Ui::Hit h = ui.Button(id, hit);
    ui.Name(back ? L"Previous month" : L"Next month");
    Canvas& c = ui.C();
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    if (hov > 0.01f) c.Dot(at, s, t.text.Alpha(0.08f * hov));
    float d = back ? 1.0f : -1.0f, a = s * 0.3f;
    Color ink = t.textDim.Mix(t.text, hov);
    c.Line({at.x + d * a * 0.5f, at.y - a}, {at.x - d * a * 0.5f, at.y}, ink, 1.8f);
    c.Line({at.x - d * a * 0.5f, at.y}, {at.x + d * a * 0.5f, at.y + a}, ink, 1.8f);
    return h.clicked;
}

}

void Board::DrawTile(const Model::BoardTile& tile, const D2D1_RECT_F& r, int index) {
    constexpr float kTileTextMax = 1.35f; // a tile's room is fixed: its words grow this far before they'd spill
    float k = Scale(r) * std::clamp(model.TextPct() / 100.0f, 1.0f, kTileTextMax);
    w::Card(ui, t, r, kRadius);
    canvas->PushClip(r);
    switch (tile.kind) {
    case Model::kBdClock: TileClock(r, k); break;
    case Model::kBdCalendar: TileCalendar(r, k); break;
    case Model::kBdWeather: TileWeather(r, k); break;
    case Model::kBdMedia: TileMedia(r, k); break;
    case Model::kBdLyrics: TileLyrics(r, k); break;
    case Model::kBdCpu:
    case Model::kBdGpu:
    case Model::kBdRam: TileGauge(r, k, tile.kind); break;
    case Model::kBdNet: TileNet(r, k); break;
    case Model::kBdDisk: TileDisk(r, k); break;
    case Model::kBdProcs: TileProcs(r, k); break;
    case Model::kBdVolume: TileVolume(r, k); break;
    case Model::kBdTodo: TileTodo(r, k); break;
    case Model::kBdShelf: TileShelf(r, k); break;
    case Model::kBdTimer: TileTimer(r, k); break;
    case Model::kBdServers: TileServers(r, k); break;
    case Model::kBdBattery: TileBattery(r, k); break;
    default: TileWork(r, k); break;
    }
    canvas->PopClip();
    (void)index;
}

void Board::TileClock(const D2D1_RECT_F& r, float k) { // the time big, the date under it
    Canvas& c = *canvas;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t time[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, 32);
    std::wstring date = DateText(st.wYear, st.wMonth, st.wDay, L"dddd, d MMMM");
    float big = (std::min)(Height(r) * 0.42f, Width(r) * 0.2f), sub = (std::max)(11.0f, big * 0.24f);
    float block = big * 1.15f + sub * 1.6f, y = (r.top + r.bottom - block) / 2;
    c.Text(time, Rect(r.left, y, Width(r), big * 1.2f), Weight::Light, big, t.text, Align::Center);
    c.Text(date, Rect(r.left + 8, y + big * 1.15f, Width(r) - 16, sub * 1.5f), Weight::Regular, sub, t.textDim, Align::Center);
    float day = (st.wHour * 3600 + st.wMinute * 60 + st.wSecond) / 86400.0f, lw = Width(r) * 0.4f, ly = r.bottom - 14 * k; // the day so far, a quiet line
    c.Line({(r.left + r.right - lw) / 2, ly}, {(r.left + r.right + lw) / 2, ly}, t.track, 3 * k);
    c.Line({(r.left + r.right - lw) / 2, ly}, {(r.left + r.right - lw) / 2 + lw * day, ly}, t.accent, 3 * k);
}

void Board::TileCalendar(const D2D1_RECT_F& r, float k) { // a month at a glance, today lit
    Canvas& c = *canvas;
    SYSTEMTIME now;
    GetLocalTime(&now);
    int y = now.wYear, m = now.wMonth + calMonth;
    while (m < 1) m += 12, --y;
    while (m > 12) m -= 12, ++y;
    float hk = (std::min)(k, kHeadMax), pad = kPad * hk, head = 30 * hk; // the header stays modest on a big tile: the days get the room
    D2D1_RECT_F in = Inset(r, pad, pad * 0.8f);
    float arrows = 64 * hk;
    c.Text(DateText(y, m, 1, L"MMMM yyyy"), Rect(in.left, in.top, Width(in) - arrows, head), Weight::Medium, (std::min)(15 * hk, (Width(in) - arrows) * 0.085f), t.text);
    if (Chevron(ui, t, Ui::Id("bd.cal.prev"), {in.right - 44 * hk, in.top + head / 2}, 13 * hk, true)) --calMonth;
    if (Chevron(ui, t, Ui::Id("bd.cal.next"), {in.right - 13 * hk, in.top + head / 2}, 13 * hk, false)) ++calMonth;
    if (calMonth != 0 && ui.Button(Ui::Id("bd.cal.today"), Rect(in.left, in.top, Width(in) - arrows, head)).clicked) calMonth = 0; // the title brings it home
    int first = FirstDay(), lead = (WeekDay(y, m, 1) - first + 7) % 7, days = DaysIn(y, m);
    int weeks = (lead + days + 6) / 7;
    float cw = Width(in) / 7, top = in.top + head + 6 * k, ch = (in.bottom - top) / (weeks + 1);
    float fs = std::clamp((std::min)(cw, ch) * 0.42f, 9.0f, 30.0f);
    for (int i = 0; i < 7; ++i) {
        int wd = (first + i) % 7, d = 1 + ((wd - WeekDay(2024, 1, 1) + 7) % 7); // a date in january 2024 on that weekday
        c.Text(fmt::Upper(DateText(2024, 1, d, L"ddd")).substr(0, 2), Rect(in.left + i * cw, top, cw, ch), Weight::Medium, fs * 0.8f,
               wd == 0 || wd == 6 ? t.textLow : t.textDim, Align::Center);
    }
    for (int d = 1; d <= days; ++d) {
        int cell = lead + d - 1, col = cell % 7, row = cell / 7 + 1;
        D2D1_RECT_F cr = Rect(in.left + col * cw, top + row * ch, cw, ch);
        bool today = calMonth == 0 && d == now.wDay;
        if (today) c.Dot({(cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2}, (std::min)(cw, ch) * 0.42f, t.accent);
        int wd = (first + col) % 7;
        c.Text(std::to_wstring(d), {cr.left, cr.top, cr.right, cr.bottom}, today ? Weight::Semi : Weight::Regular, fs,
               today ? t.onAccent : wd == 0 || wd == 6 ? t.textDim : t.text, Align::Center);
    }
    if (float n = ui.Wheel(r); n != 0) calMonth += n > 0 ? -1 : 1; // the wheel turns the months
}

void Board::TileWeather(const D2D1_RECT_F& r, float k) { // now, big; the next days under it when there's room
    Canvas& c = *canvas;
    const sys::Wx& wx = model.wx;
    float pad = kPad * k;
    if (!model.cfg.weather || !wx.valid) {
        c.Icon(glyph::Cloud, {(r.left + r.right) / 2, (r.top + r.bottom) / 2 - 12 * k}, 30 * k, t.textLow);
        c.Text(model.cfg.weather ? L"Fetching weather…" : L"Weather is off", Rect(r.left, (r.top + r.bottom) / 2 + 10 * k, Width(r), 20 * k), Weight::Light,
               12.5f * k, t.textDim, Align::Center);
        return;
    }
    bool fahr = model.cfg.fahr;
    bool row = wx.nDays > 0 && Height(r) > Width(r) * 0.62f; // tall enough for the coming days
    float topH = row ? Height(r) * 0.58f : Height(r);
    float big = std::clamp(topH * 0.34f, 18.0f, 110.0f);
    float cy = r.top + topH / 2;
    c.Icon(fmt::WxIcon(wx.code, wx.isDay), {r.left + pad + big * 0.55f, cy - big * 0.1f}, big * 1.05f, t.accentHi, true);
    float tx = r.left + pad + big * 1.25f;
    float split = (std::max)(r.left + Width(r) * 0.52f, tx + big * 2.4f); // the days start past the temperature, never over it
    bool side = wx.nDays > 0 && Width(r) > Height(r) * 1.7f && r.right - pad - split > 60 * k;
    float tw = (side ? split - pad * 0.5f : r.right - pad) - tx;
    c.Text(fmt::Temp(wx.tempC, fahr), Rect(tx, cy - big * 0.85f, tw, big * 1.15f), Weight::Light, big, t.text);
    c.Text(fmt::WxText(wx.code), Rect(tx, cy + big * 0.3f, tw, 18 * k), Weight::Regular, 12.5f * k, t.textDim);
    std::wstring sub = wx.place.empty() ? L"" : wx.place + L"  ·  ";
    sub += L"H " + fmt::Temp(wx.highC, fahr) + L"  L " + fmt::Temp(wx.lowC, fahr);
    c.Text(sub, Rect(tx, cy + big * 0.3f + 18 * k, tw, 16 * k), Weight::Light, 11 * k, t.textLow);
    if (!row && side) { // wide: the coming days down the right half
        float x0 = split, cwid = (r.right - pad - x0);
        int n = (std::min)(wx.nDays, (std::max)(1, (int)(cwid / (64 * k))));
        float dw = cwid / n;
        c.Line({x0 - pad * 0.5f, r.top + pad}, {x0 - pad * 0.5f, r.bottom - pad}, t.line, 1, false);
        for (int i = 0; i < n; ++i) {
            const sys::WxDay& d = wx.days[i];
            float x = x0 + i * dw, h = Height(r) - 2 * pad, y0 = r.top + pad;
            c.Text(DateText(d.y, d.m, d.d, L"ddd"), Rect(x, y0 + h * 0.06f, dw, h * 0.2f), Weight::Medium, 11 * k, t.textDim, Align::Center);
            c.Icon(fmt::WxIcon(d.code, true), {x + dw / 2, y0 + h * 0.48f}, (std::min)(h * 0.3f, 30 * k), t.accent, true);
            c.Text(fmt::Temp(d.hiC, fahr) + L" " + fmt::Temp(d.loC, fahr), Rect(x, y0 + h * 0.72f, dw, h * 0.22f), Weight::Regular, 11 * k, t.text, Align::Center);
        }
        return;
    }
    if (!row) return;
    int n = (std::min)(wx.nDays, (std::max)(1, (int)(Width(r) / (70 * k))));
    float dw = (Width(r) - 2 * pad) / n, y0 = r.top + topH;
    c.Line({r.left + pad, y0}, {r.right - pad, y0}, t.line, 1, false);
    for (int i = 0; i < n; ++i) {
        const sys::WxDay& d = wx.days[i];
        float x = r.left + pad + i * dw, h = r.bottom - y0;
        c.Text(DateText(d.y, d.m, d.d, L"ddd"), Rect(x, y0 + h * 0.10f, dw, h * 0.22f), Weight::Medium, 11 * k, t.textDim, Align::Center);
        c.Icon(fmt::WxIcon(d.code, true), {x + dw / 2, y0 + h * 0.48f}, (std::min)(h * 0.28f, 30 * k), t.accent, true);
        c.Text(fmt::Temp(d.hiC, fahr) + L" " + fmt::Temp(d.loC, fahr), Rect(x, y0 + h * 0.66f, dw, h * 0.24f), Weight::Regular, 11 * k, t.text, Align::Center);
    }
}

void Board::TileMedia(const D2D1_RECT_F& r, float k) { // the song: cover, words, where it is, the buttons
    Canvas& c = *canvas;
    const Theme& m = mt;
    const sys::Track& tr = model.track;
    bool has = model.hasTrack;
    float pad = kPad * k;
    bool tall = Height(r) > Width(r) * 1.05f;
    float side = tall ? (std::min)(Width(r) - 2 * pad, Height(r) * 0.48f) : Height(r) - 2 * pad;
    D2D1_RECT_F art = tall ? Rect((r.left + r.right - side) / 2, r.top + pad, side, side) : Rect(r.left + pad, r.top + pad, side, side);
    if (model.vizOn && has) { // the music's light, in the look picked in settings: bars across the tile behind the cover, or the glow rising from its floor
        D2D1_ROUNDED_RECT card = D2D1::RoundedRect(r, kRadius, kRadius);
        bool play = tr.playing;
        if (model.vizWave) {
            if (StepWave(wave, model, ui.Dt(), play)) {
                float mid = (art.top + art.bottom) / 2, hh = side * 0.45f;
                DrawWave(c, wave, {r.left + pad * 0.5f, mid - hh, r.right - pad * 0.5f, mid + hh}, m, 0.5f, &card);
            }
        } else if (StepSwell(swell, model, ui.Dt(), play, 5)) {
            DrawSwell(c, swell, 5, r, m, ui.Now(), &card);
        }
    }
    float glow = 0.22f + 0.45f * anim::Clamp01(std::sqrt(level) * model.VizGain());
    c.Glow({(art.left + art.right) / 2, (art.top + art.bottom) / 2 + side * 0.06f}, side * 0.85f, side * 0.75f, m.accent.Alpha(has && tr.playing ? glow : 0.15f));
    float rad = side * 0.07f;
    if (has && cover && model.showArt) c.Art(cover.Get(), coverBlur.Get(), art, rad, 1);
    else {
        c.FillV(art, rad, m.accentHi.Alpha(0.55f), m.accent.Alpha(0.3f));
        c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, side * 0.3f, m.onAccent.Alpha(0.8f), true);
    }
    D2D1_RECT_F col = tall ? D2D1_RECT_F{r.left + pad, art.bottom + 12 * k, r.right - pad, r.bottom - pad}
                           : D2D1_RECT_F{art.right + pad, r.top + pad, r.right - pad, r.bottom - pad};
    Align al = tall ? Align::Center : Align::Left;
    float ts = std::clamp(Height(col) * (tall ? 0.11f : 0.15f), 13.0f, 30.0f);
    std::wstring title = has && !tr.title.empty() ? tr.title : L"Nothing playing";
    std::wstring who = has ? tr.artist : L"Start music in any app";
    float y = col.top;
    if (has && !tr.app.empty()) {
        c.TextSp(fmt::Upper(tr.app), Rect(col.left, y, Width(col), ts * 0.7f), Weight::Medium, ts * 0.45f, m.accentHi, 1.6f, al);
        y += ts * 0.8f;
    }
    c.Text(title, Rect(col.left, y, Width(col), ts * 1.35f), Weight::Medium, ts, m.text, al);
    y += ts * 1.35f;
    c.Text(who, Rect(col.left, y, Width(col), ts * 1.1f), Weight::Light, ts * 0.72f, m.textDim, al);
    if (!has) return;
    float ctl = std::clamp(Height(col) * 0.26f, 38.0f, 64.0f); // the buttons, then the progress line over them
    k = (std::min)(k, kHeadMax);
    float cy = col.bottom - ctl / 2, cx = (col.left + col.right) / 2;
    if (w::IconBtn(ui, m, Ui::Id("bd.prev"), {cx - ctl * 1.25f, cy}, ctl * 0.8f, glyph::Prev, ctl * 0.55f, tr.canPrev)) model.media.Previous();
    if (w::PlayBtn(ui, m, Ui::Id("bd.play"), {cx, cy}, ctl, tr.playing, true)) model.media.TogglePlay();
    if (w::IconBtn(ui, m, Ui::Id("bd.next"), {cx + ctl * 1.25f, cy}, ctl * 0.8f, glyph::Next, ctl * 0.55f, tr.canNext)) model.media.Next();
    if (tr.duration <= 0) return;
    float py = cy - ctl / 2 - 16 * k;
    D2D1_RECT_F bar = {col.left, py - 8 * k, col.right, py + 8 * k};
    float v = (float)tr.Progress();
    uint32_t sid = Ui::Id("bd.seek");
    bool moved = tr.canSeek && ui.Slider(sid, bar, v, false);
    if ((ui.IsActive(sid) && ui.In().released) || (moved && !ui.IsActive(sid))) model.media.Seek(v); // let go, or the keyboard's step: it jumps there
    float done = anim::Clamp01(v);
    c.Line({col.left, py}, {col.right, py}, m.accent.Alpha(0.25f), 3.5f * k);
    c.Line({col.left, py}, {col.left + Width(col) * done, py}, m.accent, 3.5f * k);
    c.Dot({w::ThumbAt(col.left, col.right, 5 * k, done), py}, 5 * k, m.accentHi);
    if (py - 22 * k > y + ts) { // the times, when there's air for them
        c.Text(fmt::Clock(tr.Elapsed() / 1e7), Rect(col.left, py - 22 * k, 80 * k, 14 * k), Weight::Regular, 10.5f * k, m.textLow);
        c.Text(fmt::Clock(tr.duration / 1e7), Rect(col.right - 80 * k, py - 22 * k, 80 * k, 14 * k), Weight::Regular, 10.5f * k, m.textLow, Align::Right);
    }
}

void Board::TileLyrics(const D2D1_RECT_F& r, float k) { // the fullscreen player's words, in a card
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    D2D1_RECT_F lr = {r.left + 20, r.top + 10, r.right - 20, r.bottom - 10};
    if (lyrics.Has()) {
        double at = tr.Elapsed() / 1e7 - model.lyricOffsetMs / 1000.0;
        double jump = lyrics.Draw(ctx.Get(), c, ui, lr, at, 1, mt, model.hasTrack && tr.playing);
        if (jump >= 0 && tr.canSeek && tr.duration > 0) model.media.Seek(jump * 1e7 / tr.duration);
        return;
    }
    bool waiting = !lyrKey.empty() && !lyrDoc && ui.Now() - lyrKeyAt < kLyrWaitSec;
    if (waiting) {
        lyrics.Waiting(c, lr, 1, mt, ui.Now());
        return;
    }
    const wchar_t* why = !model.hasTrack ? L"Nothing playing" : !model.fullLyrics ? L"Lyrics are off" : !tr.music ? L"Lyrics show for songs from music apps"
                                                                                                                              : L"No synced lyrics for this song";
    c.Icon(glyph::Lyrics, {(r.left + r.right) / 2, (r.top + r.bottom) / 2 - 14 * k}, 28 * k, t.textLow);
    c.Text(why, Rect(r.left + 12, (r.top + r.bottom) / 2 + 8 * k, Width(r) - 24, 20 * k), Weight::Light, 12.5f * k, t.textDim, Align::Center);
}

void Board::TileGauge(const D2D1_RECT_F& r, float k, int kind) { // a ring, its number, what it is, and the last minute under it when there's room
    Canvas& c = *canvas;
    const sys::Stats& s = model.stats;
    float v = 0;
    std::wstring name, detail;
    int g = glyph::Cpu;
    const Hist* hist = nullptr;
    if (kind == Model::kBdCpu) v = (float)s.cpu, name = L"CPU", detail = s.threads ? std::to_wstring(s.threads) + L" threads" : L"", hist = &model.cpuHist;
    else if (kind == Model::kBdRam) v = (float)s.mem, name = L"Memory", detail = fmt::Gb(s.memUsedGb) + L" of " + fmt::Gb(s.memTotalGb), g = glyph::Ram, hist = &model.ramHist;
    else v = (float)(std::max)(0.0, s.gpu), name = L"GPU", detail = s.gpu < 0 ? L"No counter on this PC" : L"3D engine", g = glyph::Gpu, hist = &gpuHist;
    float pad = kPad * k;
    bool wide = Width(r) > Height(r) * 1.4f;
    float spark = !wide && Height(r) > Width(r) * 1.25f ? Height(r) * 0.28f : 0; // tall: the minute's graph along the bottom
    float area = Height(r) - spark;
    float rad = wide ? Height(r) * 0.32f : (std::min)(Width(r) * 0.28f, area * 0.27f);
    D2D1_POINT_2F o = wide ? D2D1_POINT_2F{r.left + pad + rad + 4, (r.top + r.bottom) / 2} : D2D1_POINT_2F{(r.left + r.right) / 2, r.top + pad + rad + 6 * k};
    w::Gauge(ui, t, Ui::Id("bd.gauge", kind), o, rad, (std::max)(4.0f, rad * 0.12f), v);
    c.Icon(g, {o.x, o.y - rad * 0.32f}, rad * 0.36f, t.accent);
    c.Text(w::Pct(v), Rect(o.x - rad, o.y - rad * 0.08f, rad * 2, rad * 0.6f), Weight::Light, rad * 0.42f, t.text, Align::Center);
    D2D1_RECT_F words = wide ? D2D1_RECT_F{o.x + rad + pad, r.top, r.right - pad, r.bottom} : D2D1_RECT_F{r.left + 6, o.y + rad + 8 * k, r.right - 6, r.bottom - spark};
    float ny = wide ? (r.top + r.bottom) / 2 - 18 * k : words.top;
    Align al = wide ? Align::Left : Align::Center;
    c.Text(name, Rect(words.left, ny, Width(words), 20 * k), Weight::Medium, 14 * k, t.text, al);
    c.Text(detail, Rect(words.left, ny + 20 * k, Width(words), 16 * k), Weight::Light, 11 * k, t.textDim, al);
    if (spark > 0 && hist) {
        D2D1_RECT_F sr = {r.left + 1, r.bottom - spark, r.right - 1, r.bottom - 1};
        D2D1_ROUNDED_RECT in = {r, kRadius, kRadius};
        w::Spark(c, sr, hist->Values(), hist->Version(), 1.0f, t.accent, false, &in);
    }
}

void Board::TileNet(const D2D1_RECT_F& r, float k) { // up and down now, and the minute behind them
    Canvas& c = *canvas;
    const sys::Net& n = model.net;
    float pad = kPad * k;
    bool wifi = !n.ssid.empty();
    Caption(c, t, r, fmt::Upper(wifi ? n.ssid : n.online ? std::wstring(L"Ethernet") : std::wstring(L"Offline")), k);
    float y = r.top + 36 * k, half = (Width(r) - 2 * pad) / 2, big = std::clamp(Height(r) * 0.1f, 14.0f, 34.0f);
    c.Icon(glyph::Down, {r.left + pad + 10 * k, y + big * 0.6f}, 18 * k, t.accent);
    c.Text(fmt::Rate(n.downBps), Rect(r.left + pad + 24 * k, y, half - 24 * k, big * 1.3f), Weight::Light, big, t.text);
    c.Icon(glyph::Up, {r.left + pad + half + 10 * k, y + big * 0.6f}, 18 * k, t.accentHi);
    c.Text(fmt::Rate(n.upBps), Rect(r.left + pad + half + 24 * k, y, half - 24 * k, big * 1.3f), Weight::Light, big, t.text);
    float top = y + big * 1.5f + 8 * k, bottom = r.bottom - 1, mid = (top + bottom) / 2;
    if (bottom - top < 30) return;
    float peak = (std::max)((std::max)(model.upHist.Max(), model.downHist.Max()), 64.0f * 1024);
    D2D1_ROUNDED_RECT in = {r, kRadius, kRadius};
    w::Spark(c, {r.left + 1, top, r.right - 1, mid - 1}, model.downHist.Values(), model.downHist.Version(), peak, t.accent, false, &in);
    w::Spark(c, {r.left + 1, mid + 1, r.right - 1, bottom}, model.upHist.Values(), model.upHist.Version(), peak, t.accentHi, true, &in);
    c.Line({r.left + pad, mid}, {r.right - pad, mid}, t.hairTop, 1, false);
}

void Board::TileDisk(const D2D1_RECT_F& r, float k) { // how full the windows drive is
    Canvas& c = *canvas;
    float rad = (std::min)(Width(r) * 0.3f, Height(r) * 0.24f);
    D2D1_POINT_2F o = {(r.left + r.right) / 2, r.top + Height(r) * 0.40f};
    float used = (std::max)(0.0f, diskUsed);
    w::Gauge(ui, t, Ui::Id("bd.disk"), o, rad, (std::max)(4.0f, rad * 0.12f), used);
    c.Icon(glyph::Disk, {o.x, o.y - rad * 0.3f}, rad * 0.36f, t.accent);
    c.Text(w::Pct(used), Rect(o.x - rad, o.y - rad * 0.06f, rad * 2, rad * 0.6f), Weight::Light, rad * 0.42f, t.text, Align::Center);
    wchar_t drive[4] = L"C:";
    if (wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH)) drive[0] = win[0];
    c.Text(std::wstring(L"Disk ") + drive, Rect(r.left + 6, o.y + rad + 10 * k, Width(r) - 12, 20 * k), Weight::Medium, 14 * k, t.text, Align::Center);
    c.Text(diskUsed < 0 ? L"Reading…" : fmt::Gb(diskFree) + L" free", Rect(r.left + 6, o.y + rad + 30 * k, Width(r) - 12, 16 * k), Weight::Light, 11 * k, t.textDim,
           Align::Center);
}

void Board::TileProcs(const D2D1_RECT_F& r, float k) { // the busiest apps right now
    Canvas& c = *canvas;
    Caption(c, t, r, L"TOP PROCESSES", k);
    float pad = kPad * k, y = r.top + 38 * k, rowH = 36 * k;
    for (size_t i = 0; i < model.procs.size() && y + rowH <= r.bottom - 6; ++i, y += rowH) {
        const sys::ProcRow& p = model.procs[i];
        float x0 = r.left + pad, x1 = r.right - pad;
        c.Text(p.name, Rect(x0, y, x1 - x0 - 60 * k, 18 * k), Weight::Medium, 12.5f * k, t.text);
        c.Text(w::Pct((float)p.cpu), Rect(x1 - 60 * k, y, 60 * k, 18 * k), Weight::Medium, 12 * k, t.text, Align::Right);
        float bar = ui.Soft(Ui::Id("bd.proc", (int)i), anim::Clamp01((float)p.cpu)), by = y + 24 * k;
        c.Line({x0, by}, {x1, by}, t.track, 3 * k);
        if (bar > 0.003f) c.Line({x0, by}, {x0 + (x1 - x0) * bar, by}, t.accent, 3 * k);
    }
    if (model.procs.empty()) c.Text(L"Measuring…", Rect(r.left + pad, r.top + 40 * k, Width(r) - 2 * pad, 18 * k), Weight::Light, 12 * k, t.textDim);
}

void Board::TileVolume(const D2D1_RECT_F& r, float k) { // the speakers' level: drag, scroll, or tap the icon to mute
    Canvas& c = *canvas;
    Caption(c, t, r, L"VOLUME", k);
    float pad = kPad * k, ph = std::clamp(Height(r) * 0.32f, 30.0f, 56.0f);
    D2D1_RECT_F pill = Rect(r.left + pad, (r.top + r.bottom) / 2 - ph / 2 + 8 * k, Width(r) - 2 * pad, ph);
    float v = model.vol;
    bool tap = false;
    if (w::Pill(ui, t, Ui::Id("bd.vol"), pill, model.muted ? glyph::VolumeOff : glyph::VolumeUp, v, &tap, model.muted)) model.SetVol(v);
    if (tap) model.ToggleMute();
}

void Board::TileTodo(const D2D1_RECT_F& r, float k) { // the tools page's list: tick things off here, add them there
    Canvas& c = *canvas;
    int left = (int)std::count_if(model.todos.begin(), model.todos.end(), [](const Model::Todo& x) { return !x.done; });
    Caption(c, t, r, L"TO-DO  ·  " + std::to_wstring(left) + L" LEFT", k);
    float pad = kPad * k, y = r.top + 38 * k, rowH = 30 * k;
    if (model.todos.empty()) {
        c.Text(L"Add to-dos on the Tools page", Rect(r.left + pad, y + 4 * k, Width(r) - 2 * pad, 18 * k), Weight::Light, 12 * k, t.textDim);
        return;
    }
    for (size_t i = 0; i < model.todos.size() && y + rowH <= r.bottom - 6; ++i, y += rowH) {
        auto& todo = model.todos[i];
        D2D1_RECT_F row = Rect(r.left + pad - 6, y, Width(r) - 2 * pad + 12, rowH);
        Ui::Hit h = ui.Button(Ui::Id("bd.todo", (int)i), row);
        if (h.hover) c.Fill(row, 8, t.text.Alpha(0.05f));
        D2D1_POINT_2F box = {row.left + 16 * k, (row.top + row.bottom) / 2};
        c.Icon(todo.done ? glyph::BoxOn : glyph::Box, box, 18 * k, todo.done ? t.accentHi : t.textDim, todo.done);
        c.Text(todo.text, {row.left + 32 * k, row.top, row.right - 6, row.bottom}, Weight::Regular, 12.5f * k, todo.done ? t.textLow : t.text);
        if (h.clicked) todo.done = !todo.done, model.SaveTodos();
    }
}

void Board::TileShelf(const D2D1_RECT_F& r, float k) { // the shelf's newest cards: a tap puts one on the clipboard
    Canvas& c = *canvas;
    const auto& items = model.shelf.Items();
    Caption(c, t, r, L"SHELF  ·  " + std::to_wstring(items.size()), k);
    float pad = kPad * k, y = r.top + 38 * k, rowH = 32 * k;
    if (items.empty()) {
        c.Text(L"Drop files or text on the island to keep them here", Rect(r.left + pad, y + 4 * k, Width(r) - 2 * pad, 18 * k), Weight::Light, 12 * k, t.textDim);
        return;
    }
    static const int kKindGlyph[] = {glyph::Note, glyph::Web, glyph::Image, glyph::File, glyph::Tune, glyph::Note}; // text, link, image, file, color, code
    for (size_t i = 0; i < items.size() && y + rowH <= r.bottom - 6; ++i, y += rowH) {
        const auto& it = items[i];
        D2D1_RECT_F row = Rect(r.left + pad - 6, y, Width(r) - 2 * pad + 12, rowH);
        Ui::Hit h = ui.Button(Ui::Id("bd.shelf", (int)i), row);
        if (h.hover) c.Fill(row, 8, t.text.Alpha(0.05f));
        bool copied = it.id == copiedId && ui.Now() - copiedAt < 1.4;
        int g = copied ? glyph::Check : kKindGlyph[(std::min)((size_t)it.kind, std::size(kKindGlyph) - 1)];
        c.Icon(g, {row.left + 16 * k, (row.top + row.bottom) / 2}, 17 * k, copied ? t.accentHi : t.textDim, copied);
        c.Text(it.Name(), {row.left + 34 * k, row.top, row.right - 6, row.bottom}, Weight::Regular, 12.5f * k, t.text);
        if (h.clicked && model.shelf.Copy(it.id, sys::Shelf::kCopy, {})) copiedId = it.id, copiedAt = ui.Now();
        if (copied) ui.KeepAlive();
    }
}

void Board::TileTimer(const D2D1_RECT_F& r, float k) { // a running timer, stopwatch or focus session; or a quick start
    Canvas& c = *canvas;
    int act = model.Activity();
    float pad = kPad * k;
    if (act == Model::kActNone) {
        Caption(c, t, r, L"TIMER", k);
        c.Text(L"Nothing running", Rect(r.left + pad, r.top + 40 * k, Width(r) - 2 * pad, 20 * k), Weight::Light, 13 * k, t.textDim);
        const int mins[] = {5, 15, 25};
        float gap = 8 * k, cw = (Width(r) - 2 * pad - 2 * gap) / 3, ch = std::clamp(Height(r) * 0.22f, 28.0f, 46.0f), y = r.bottom - pad - ch;
        for (int i = 0; i < 3; ++i)
            if (w::Chip(ui, t, Ui::Id("bd.timer", i), Rect(r.left + pad + i * (cw + gap), y, cw, ch), std::to_wstring(mins[i]) + L" min", false))
                model.StartTimer(mins[i] * 60);
        return;
    }
    Caption(c, t, r, act == Model::kActTimer ? L"TIMER" : act == Model::kActFocus ? (model.focusBreak ? L"BREAK" : L"FOCUS") : L"STOPWATCH", k);
    float rad = (std::min)(Width(r) * 0.3f, Height(r) * 0.3f);
    D2D1_POINT_2F o = {(r.left + r.right) / 2, r.top + Height(r) * 0.5f};
    if (act != Model::kActWatch) w::Gauge(ui, t, Ui::Id("bd.timer.ring"), o, rad, (std::max)(4.0f, rad * 0.1f), 1 - model.ActivityFrac());
    else c.Ring(o, rad, t.accent.Alpha(0.6f), (std::max)(4.0f, rad * 0.1f));
    c.Text(model.ActivityText(), Rect(o.x - rad, o.y - rad * 0.3f, rad * 2, rad * 0.6f), Weight::Light, rad * 0.42f, t.text, Align::Center);
    if (w::IconBtn(ui, t, Ui::Id("bd.timer.stop"), {r.right - pad - 10 * k, r.top + 22 * k}, 28 * k, glyph::Stop, 16 * k)) {
        if (act == Model::kActTimer) model.StopTimer();
        else if (act == Model::kActFocus) model.StopFocus();
        else model.SwToggle();
    }
}

void Board::TileServers(const D2D1_RECT_F& r, float k) { // the tools page's servers: up, down, how fast
    Canvas& c = *canvas;
    Caption(c, t, r, L"SERVERS", k);
    float pad = kPad * k, y = r.top + 38 * k, rowH = 30 * k;
    if (model.hosts.empty()) {
        c.Text(L"Add servers on the Tools page", Rect(r.left + pad, y + 4 * k, Width(r) - 2 * pad, 18 * k), Weight::Light, 12 * k, t.textDim);
        return;
    }
    const Color up = Color{0.36f, 0.84f, 0.56f, 1}, down = Color{1.0f, 0.45f, 0.45f, 1};
    for (size_t i = 0; i < model.hosts.size() && y + rowH <= r.bottom - 6; ++i, y += rowH) {
        const sys::extras::HostHealth* h = i < model.hostNow.size() && model.hostNow[i].host == model.hosts[i] ? &model.hostNow[i] : nullptr;
        bool known = h && h->known;
        float cy = y + rowH / 2;
        c.Dot({r.left + pad + 5 * k, cy}, 5 * k, !known ? t.textLow : h->up ? up : down);
        c.Text(model.hosts[i], Rect(r.left + pad + 18 * k, y, Width(r) - 2 * pad - 90 * k, rowH), Weight::Regular, 12.5f * k, t.text);
        std::wstring state = !known ? L"Checking…" : !h->up ? L"Down" : h->ms >= 0 ? std::to_wstring(h->ms) + L" ms" : L"Up";
        c.Text(state, Rect(r.right - pad - 80 * k, y, 80 * k, rowH), Weight::Medium, 11.5f * k, !known ? t.textLow : h->up ? t.textDim : down, Align::Right);
    }
}

void Board::TileBattery(const D2D1_RECT_F& r, float k) { // the laptop's charge
    Canvas& c = *canvas;
    float rad = (std::min)(Width(r) * 0.3f, Height(r) * 0.26f);
    D2D1_POINT_2F o = {(r.left + r.right) / 2, r.top + Height(r) * 0.42f};
    float v = model.batPct >= 0 ? model.batPct / 100.0f : 0;
    w::Gauge(ui, t, Ui::Id("bd.bat"), o, rad, (std::max)(4.0f, rad * 0.12f), v);
    c.Icon(model.charging ? glyph::BatteryCharging : glyph::Battery, {o.x, o.y - rad * 0.3f}, rad * 0.36f, t.accent);
    c.Text(model.batPct >= 0 ? std::to_wstring(model.batPct) + L"%" : L"—", Rect(o.x - rad, o.y - rad * 0.06f, rad * 2, rad * 0.6f), Weight::Light, rad * 0.42f,
           t.text, Align::Center);
    c.Text(model.charging ? L"Charging" : L"On battery", Rect(r.left + 6, o.y + rad + 10 * k, Width(r) - 12, 20 * k), Weight::Medium, 14 * k, t.text, Align::Center);
}

void Board::TileWork(const D2D1_RECT_F& r, float k) { // every workspace as a chip, the current one lit; a tap switches, the wheel steps
    Canvas& c = *canvas;
    const sys::WsState& ws = model.wsNow;
    Caption(c, t, r, L"WORKSPACES", k);
    float pad = kPad * k;
    if (ws.list.empty()) {
        c.Text(L"Looking for workspaces…", Rect(r.left + pad, r.top + 40 * k, Width(r) - 2 * pad, 18 * k), Weight::Light, 12 * k, t.textDim);
        return;
    }
    int n = (int)ws.list.size();
    float gap = 8 * k, cw = (std::min)(110 * k, (Width(r) - 2 * pad - gap * (n - 1)) / n), ch = std::clamp(Height(r) * 0.36f, 28.0f, 70.0f);
    float x = (r.left + r.right - (cw * n + gap * (n - 1))) / 2, y = (r.top + r.bottom) / 2 - ch / 2 + 8 * k;
    for (int i = 0; i < n; ++i, x += cw + gap) {
        D2D1_RECT_F chip = Rect(x, y, cw, ch);
        Ui::Hit h = ui.Button(Ui::Id("bd.ws", i), chip);
        bool on = i == ws.current;
        c.Fill(chip, ch * 0.3f, on ? t.accent : t.track.Mix(t.cardHi, h.hover ? 0.6f : 0));
        std::wstring name = ws.list[i].name.empty() ? std::to_wstring(i + 1) : ws.list[i].name;
        c.Text(name, chip, Weight::Medium, (std::min)(ch * 0.4f, 16 * k), on ? t.onAccent : t.text, Align::Center);
        if (ws.list[i].busy && !on) c.Dot({(chip.left + chip.right) / 2, chip.bottom - 6 * k}, 2 * k, t.accent);
        if (h.clicked && !on) model.ws.Focus(i);
    }
    if (float w = ui.Wheel(r); w != 0) model.ws.Step(w > 0 ? -1 : 1);
}
