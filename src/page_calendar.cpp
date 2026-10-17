// A month at a glance, opened from the weather card: today big on the left with the weather, the month beside it.
#include <algorithm>
#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "theme.h"

using namespace sheet;

namespace {

int viewYear = 0, viewMonth = 0; // the month on show, 1..12
int pickY = 0, pickM = 0, pickD = 0; // the day tapped, 0 = today

bool Leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int DaysIn(int y, int m) { return m == 2 ? (Leap(y) ? 29 : 28) : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31; }

int Weekday(int y, int m, int d) { // 0 sunday .. 6 saturday
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

long DayIndex(int y, int m, int d) { // days since a fixed point, for "in 3 days"
    if (m < 3) y -= 1, m += 12;
    return 365L * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d;
}

std::wstring Format(int y, int m, int d, const wchar_t* pattern) { // the user's own words for dates
    SYSTEMTIME st = {};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = (WORD)d;
    st.wDayOfWeek = (WORD)Weekday(y, m, d);
    wchar_t buf[96] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, pattern, buf, 96, nullptr);
    return buf;
}

int NthWeekday(int y, int m, int wd, int n) { // the nth given weekday of a month, e.g. the second sunday of may
    int first = 1 + (wd - Weekday(y, m, 1) + 7) % 7;
    return first + (n - 1) * 7;
}

void Easter(int y, int& m, int& d) { // anonymous gregorian computus
    int a = y % 19, b = y / 100, c = y % 100, e = b / 4, f = b % 4, g = (b + 8) / 25, h = (b - g + 1) / 3;
    int i = (19 * a + b - e - h + 15) % 30, k = c / 4, l = c % 4, n = (32 + 2 * f + 2 * k - i - l) % 7, q = (a + 11 * i + 22 * n) / 451;
    m = (i + n - 7 * q + 114) / 31;
    d = (i + n - 7 * q + 114) % 31 + 1;
}

struct Feast { const wchar_t* name; const wchar_t* greet; };

const Feast* Holiday(int y, int m, int d) { // days the world marks; the moon-led ones are a table and can differ by a day where you are
    static const Feast kNewYear{L"New Year's Day", L"Happy New Year!"}, kValentine{L"Valentine's Day", L"Happy Valentine's Day!"},
        kLanguage{L"Mother Language Day", L"Celebrate every mother tongue!"}, kWomen{L"Women's Day", L"Happy Women's Day!"},
        kPatrick{L"St Patrick's Day", L"Happy St Patrick's Day!"}, kEarth{L"Earth Day", L"Happy Earth Day!"},
        kWorkers{L"Workers' Day", L"Happy Workers' Day!"}, kHalloween{L"Halloween", L"Happy Halloween!"},
        kXmasEve{L"Christmas Eve", L"Merry Christmas Eve!"}, kXmas{L"Christmas Day", L"Merry Christmas!"},
        kNyEve{L"New Year's Eve", L"See you next year!"}, kEaster{L"Easter Sunday", L"Happy Easter!"},
        kGoodFriday{L"Good Friday", L"A blessed Good Friday"}, kMother{L"Mother's Day", L"Happy Mother's Day!"},
        kFather{L"Father's Day", L"Happy Father's Day!"}, kLunar{L"Lunar New Year", L"Happy Lunar New Year!"},
        kFitr{L"Eid al-Fitr", L"Eid Mubarak!"}, kAdha{L"Eid al-Adha", L"Eid Mubarak!"}, kDiwali{L"Diwali", L"Happy Diwali!"};
    struct Fixed { int m, d; const Feast* f; };
    static const Fixed kFixed[] = {{1, 1, &kNewYear}, {2, 14, &kValentine}, {2, 21, &kLanguage}, {3, 8, &kWomen}, {3, 17, &kPatrick},
                                   {4, 22, &kEarth}, {5, 1, &kWorkers}, {10, 31, &kHalloween}, {12, 24, &kXmasEve}, {12, 25, &kXmas},
                                   {12, 31, &kNyEve}};
    for (const Fixed& f : kFixed)
        if (f.m == m && f.d == d) return f.f;
    int em, ed;
    Easter(y, em, ed);
    long easter = DayIndex(y, em, ed), day = DayIndex(y, m, d);
    if (day == easter) return &kEaster;
    if (day == easter - 2) return &kGoodFriday;
    if (m == 5 && d == NthWeekday(y, 5, 0, 2)) return &kMother;
    if (m == 6 && d == NthWeekday(y, 6, 0, 3)) return &kFather;
    struct Moon { int y, m, d; const Feast* f; };
    static const Moon kMoon[] = {
        {2025, 1, 29, &kLunar}, {2026, 2, 17, &kLunar}, {2027, 2, 6, &kLunar}, {2028, 1, 26, &kLunar}, {2029, 2, 13, &kLunar}, {2030, 2, 3, &kLunar},
        {2025, 3, 31, &kFitr}, {2026, 3, 20, &kFitr}, {2027, 3, 10, &kFitr}, {2028, 2, 27, &kFitr}, {2029, 2, 14, &kFitr}, {2030, 2, 4, &kFitr},
        {2025, 6, 7, &kAdha}, {2026, 5, 27, &kAdha}, {2027, 5, 17, &kAdha}, {2028, 5, 5, &kAdha}, {2029, 4, 24, &kAdha}, {2030, 4, 13, &kAdha},
        {2025, 10, 20, &kDiwali}, {2026, 11, 8, &kDiwali}, {2027, 10, 29, &kDiwali}, {2028, 10, 17, &kDiwali}, {2029, 11, 5, &kDiwali}, {2030, 10, 26, &kDiwali}};
    for (const Moon& x : kMoon)
        if (x.y == y && x.m == m && x.d == d) return x.f;
    return nullptr;
}

int FirstDay() { // the locale's first day of the week, 0 sunday .. 6 saturday
    wchar_t v[4] = {};
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, v, 4)) return 1;
    return (_wtoi(v) + 1) % 7; // windows counts from monday
}

void Chevron(Canvas& c, D2D1_POINT_2F o, bool right, Color ink) {
    float s = right ? 1.0f : -1.0f;
    c.Line({o.x - 3 * s, o.y - 6}, {o.x + 3 * s, o.y}, ink, 2);
    c.Line({o.x + 3 * s, o.y}, {o.x - 3 * s, o.y + 6}, ink, 2);
}

bool Arrow(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F o, bool right) {
    D2D1_RECT_F r = {o.x - 16, o.y - 16, o.x + 16, o.y + 16};
    Ui::Hit h = ui.Button(id, r);
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 16);
    if (hover > 0.01f) ui.C().Dot(o, 16, t.text.Alpha(0.08f * hover));
    Chevron(ui.C(), o, right, t.textDim.Mix(t.text, hover));
    return h.clicked;
}

void Hero(PageCtx& p, const Frame& f, const SYSTEMTIME& now) { // today, huge, inside a ring that fills as the month goes by
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    float R = f.tall ? 40.0f : 62.0f, ring = R + 10;
    D2D1_POINT_2F o = f.tall ? f.badge : D2D1_POINT_2F{f.badge.x, f.badge.y + 22};
    float today = now.wDay - 1 + (now.wHour * 60 + now.wMinute) / 1440.0f; // days gone, with the hours of this one
    int doy = 0;
    for (int mo = 1; mo < now.wMonth; ++mo) doy += DaysIn(now.wYear, mo);
    float month = today / DaysIn(now.wYear, now.wMonth), year = (doy + today) / (Leap(now.wYear) ? 366.0f : 365.0f); // how much is behind us
    Ui::Hit h = p.ui.Button(Ui::Id("cal.flip"), {o.x - ring, o.y - ring, o.x + ring, o.y + ring}); // hover: the date turns over to the year
    float flip = p.ui.Smooth(Ui::Id("cal.flip", 1), h.hover ? 1.0f : 0, 7), turn = flip * flip * (3 - 2 * flip);
    bool back = turn > 0.5f;
    float sx = (std::max)(0.02f, std::fabs(std::cos(turn * kPi))); // squeezes to an edge, then opens on the other face
    float in = p.ui.SmoothFrom(Ui::Id(back ? "cal.ringy" : "cal.ring"), back ? year : month, 0.0f, 4); // each face's ring sweeps up when it shows
    D2D1_MATRIX_3X2_F was;
    c.Target()->GetTransform(&was);
    c.Target()->SetTransform(D2D1::Matrix3x2F::Scale(sx, 1, o) * was);
    c.Ring(o, ring, t.track, 6);
    c.Arc(o, ring, -kPi / 2, (std::max)(0.02f, in) * 2 * kPi, back ? t.accentHi : t.accent, 6);
    float tip = -kPi / 2 + in * 2 * kPi;
    c.Dot({o.x + std::cos(tip) * ring, o.y + std::sin(tip) * ring}, 6, back ? t.accent : t.accentHi); // where we are on the ring
    if (!back) {
        c.Text(std::to_wstring(now.wDay), Rect(o.x - R, o.y - R, 2 * R, 2 * R), Weight::Semi, R * 1.15f, t.text, Align::Center);
    } else { // the year: how much of it is gone
        c.Text(std::to_wstring((int)(year * 100)) + L"%", Rect(o.x - R, o.y - R * 0.95f, 2 * R, 1.5f * R), Weight::Semi, R * 0.72f, t.text, Align::Center);
        c.TextSp(L"OF " + std::to_wstring(now.wYear), Rect(o.x - R, o.y + R * 0.38f, 2 * R, 16), Weight::Medium, 10, t.accentHi, 2.0f, Align::Center);
    }
    c.Target()->SetTransform(was);
    if (flip > 0.01f && flip < 0.99f) p.ui.Wake();
    if (!f.tall) {
        c.TextSp(fmt::Upper(Format(now.wYear, now.wMonth, now.wDay, L"dddd")), Rect(f.title.left, o.y + ring + 18, Width(f.title), 16), Weight::Medium, 12, t.accentHi, 3.0f, Align::Center);
        c.Text(Format(now.wYear, now.wMonth, now.wDay, L"MMMM yyyy"), Rect(f.title.left, o.y + ring + 38, Width(f.title), 18), Weight::Light, 12.5f, t.textDim, Align::Center);
    } else {
        c.TextSp(fmt::Upper(Format(now.wYear, now.wMonth, now.wDay, L"dddd")), f.title, Weight::Medium, 13, t.accentHi, 2.5f, f.align);
        c.Text(Format(now.wYear, now.wMonth, now.wDay, L"MMMM yyyy"), f.sub, Weight::Light, 12, t.textDim, f.align);
    }
    const sys::Wx& wx = p.m.wx;
    if (!p.m.cfg.weather || !wx.valid) return;
    D2D1_RECT_F card = f.pill; // the weather where the sheets keep their main button; a click shows the days ahead
    Ui::Hit wh = wx.nDays > 0 ? p.ui.Button(Ui::Id("cal.wx"), card) : Ui::Hit{};
    if (wh.clicked) p.m.calForecast = !p.m.calForecast;
    float sel = p.ui.Smooth(Ui::Id("cal.wx", 1), p.m.calForecast ? 1.0f : wh.hover ? 0.5f : 0, 14);
    c.Fill(card, Height(card) / 2, t.accent.Alpha(0.16f + 0.16f * sel));
    c.Stroke(card, Height(card) / 2, t.accentHi.Alpha(0.35f + 0.35f * sel), 1);
    float cy = (card.top + card.bottom) / 2;
    c.Icon(fmt::WxIcon(wx.code, wx.isDay), {card.left + 24, cy}, 22, t.accentHi, true);
    c.Text(fmt::Temp(wx.tempC, p.m.cfg.fahr), Rect(card.left + 44, card.top, 56, Height(card)), Weight::Semi, 15, t.text);
    c.Text(fmt::WxText(wx.code), Rect(card.left + 94, card.top, Width(card) - 106, Height(card)), Weight::Light, 12, t.textDim);
}

}

std::wstring HourText(int hour) { // "3 PM" or "15", as this pc tells time
    SYSTEMTIME st{};
    st.wYear = 2024, st.wMonth = 1, st.wDay = 1, st.wHour = (WORD)hour;
    wchar_t buf[32];
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOMINUTESORSECONDS, &st, nullptr, buf, 32)) return std::to_wstring(hour);
    return buf;
}

void Forecast(PageCtx& p, const D2D1_RECT_F& list) { // the hours and days ahead, where the month was
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const sys::Wx& wx = m.wx;
    bool fahr = m.cfg.fahr;
    c.Text(L"Forecast", Rect(list.left + 2, list.top - 6, 200, 40), Weight::Semi, 26, t.text);
    c.Text(wx.place, Rect(list.left + 136, list.top + 4, Width(list) - 300, 26), Weight::Thin, 20, t.accentHi);
    if (w::IconBtn(ui, t, Ui::Id("cal.close"), {list.right - 14, list.top + 15}, 30, glyph::Close, 18)) m.CloseCal();
    D2D1_RECT_F back = Rect(list.right - 120, list.top + 3, 86, 24); // back to the month
    Ui::Hit bh = ui.Button(Ui::Id("cal.month.back"), back);
    c.Fill(back, 12, t.accent.Alpha(0.16f + (bh.hover ? 0.10f : 0.0f)));
    c.Text(L"Calendar", back, Weight::Medium, 11.5f, t.accentHi, Align::Center);
    if (bh.clicked) m.calForecast = false;

    float top = list.top + 46;
    c.TextSp(L"NEXT HOURS", Rect(list.left + 4, top, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    float cw = Width(list) / (std::max)(1, wx.nHours);
    for (int i = 0; i < wx.nHours; ++i) {
        const sys::WxHour& h = wx.hours[i];
        float x = list.left + i * cw;
        c.Text(HourText(h.hour), Rect(x, top + 20, cw, 16), Weight::Regular, 11, t.textDim, Align::Center);
        c.Icon(fmt::WxIcon(h.code, h.day), {x + cw / 2, top + 50}, 20, t.accentHi, true);
        c.Text(fmt::Temp(h.tempC, fahr), Rect(x, top + 66, cw, 18), Weight::Semi, 13, t.text, Align::Center);
    }

    top += 104;
    c.Line({list.left + 4, top - 10}, {list.right - 4, top - 10}, t.line, 1, false);
    c.TextSp(L"NEXT DAYS", Rect(list.left + 4, top, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    float dw = Width(list) / (std::max)(1, wx.nDays);
    for (int i = 0; i < wx.nDays; ++i) {
        const sys::WxDay& d = wx.days[i];
        float x = list.left + i * dw;
        c.Text(Format(d.y, d.m, d.d, L"ddd"), Rect(x, top + 20, dw, 16), Weight::Medium, 11.5f, t.textDim, Align::Center);
        c.Icon(fmt::WxIcon(d.code, true), {x + dw / 2, top + 50}, 20, t.accentHi, true);
        c.Text(fmt::Temp(d.hiC, fahr), Rect(x, top + 68, dw, 18), Weight::Semi, 13, t.text, Align::Center);
        c.Text(fmt::Temp(d.loC, fahr), Rect(x, top + 86, dw, 16), Weight::Light, 11.5f, t.textDim, Align::Center);
    }
}

void DrawCalendar(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (m.calFresh) { // each time it opens: this month, today picked
        viewYear = now.wYear;
        viewMonth = now.wMonth;
        pickY = pickM = pickD = 0;
        m.calFresh = false;
    }
    Frame frame = Begin(p, k);
    Hero(p, frame, now);
    if (m.calForecast && m.wx.valid && m.wx.nDays > 0) { // the weather pill swapped the month for the days ahead
        Forecast(p, frame.list);
        if (ui.RClick(s)) m.CloseCal();
        End(p, frame);
        return;
    }

    D2D1_RECT_F list = frame.list;
    std::wstring month = Format(viewYear, viewMonth, 1, L"MMMM");
    float monthW = month.size() * 15.0f; // measured, so the year never runs into a long name
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(month.c_str(), (UINT32)month.size(), gfx::TextFmt(Weight::Semi, 26, Align::Left), 1000, 40, &lay)) && SUCCEEDED(lay->GetMetrics(&tm)))
        monthW = tm.widthIncludingTrailingWhitespace;
    c.Text(month, Rect(list.left + 2, list.top - 6, monthW + 4, 40), Weight::Semi, 26, t.text);
    c.Text(std::to_wstring(viewYear), Rect(list.left + 2 + monthW + 10, list.top + 4, 80, 26), Weight::Thin, 20, t.accentHi);
    float ax = list.right - 104;
    if (Arrow(ui, t, Ui::Id("cal.prev"), {ax, list.top + 15}, false) && --viewMonth < 1) viewMonth = 12, --viewYear;
    if (Arrow(ui, t, Ui::Id("cal.next"), {ax + 34, list.top + 15}, true) && ++viewMonth > 12) viewMonth = 1, ++viewYear;
    if (w::IconBtn(ui, t, Ui::Id("cal.close"), {list.right - 14, list.top + 15}, 30, glyph::Close, 18)) m.CloseCal();
    bool away = viewYear != now.wYear || viewMonth != now.wMonth;
    float back = ui.Smooth(Ui::Id("cal.back"), away ? 1.0f : 0, 14);
    if (back > 0.01f) { // somewhere else in time: a way home
        D2D1_RECT_F r = Rect(ax - 86, list.top + 3, 66, 24);
        Ui::Hit h = away ? ui.Button(Ui::Id("cal.today"), r) : Ui::Hit{};
        c.Fill(r, 12, t.accent.Alpha((0.16f + (h.hover ? 0.10f : 0.0f)) * back));
        c.Text(L"Today", r, Weight::Medium, 11.5f, t.accentHi.Alpha(back), Align::Center);
        if (h.clicked) viewYear = now.wYear, viewMonth = now.wMonth, pickY = pickM = pickD = 0;
    }

    D2D1_RECT_F grid = {list.left, list.top + 42, list.right, list.bottom - 34};
    int first = FirstDay();
    float cw = Width(grid) / 7, rh = (Height(grid) - 22) / 6;
    for (int i = 0; i < 7; ++i) { // weekends sit in their own quiet columns
        int wd = (first + i) % 7;
        if (wd == 0 || wd == 6) c.Fill(Rect(grid.left + i * cw + 3, grid.top - 4, cw - 6, Height(grid) + 6), 14, t.text.Alpha(0.035f));
    }
    if (viewYear == now.wYear && viewMonth == now.wMonth) { // this week as a band
        int lead0 = (Weekday(viewYear, viewMonth, 1) - first + 7) % 7, row = (now.wDay - 1 + lead0) / 7;
        float top = grid.top + 22 + row * rh;
        c.Fill(Rect(grid.left, top + 2, Width(grid), rh - 4), (rh - 4) / 2, t.accent.Alpha(0.09f));
    }
    for (int i = 0; i < 7; ++i) { // weekday initials, in the locale's order
        int wd = (first + i) % 7;
        int d = 1 + ((wd - Weekday(2024, 1, 1) + 7) % 7); // a date in jan 2024 that falls on that weekday
        bool weekend = wd == 0 || wd == 6;
        c.Text(fmt::Upper(Format(2024, 1, d, L"ddd")).substr(0, 2), Rect(grid.left + i * cw, grid.top, cw, 16), Weight::Medium, 10, weekend ? t.textLow : t.textDim, Align::Center);
    }
    int lead = (Weekday(viewYear, viewMonth, 1) - first + 7) % 7, days = DaysIn(viewYear, viewMonth);
    int py = viewMonth == 1 ? viewYear - 1 : viewYear, pm = viewMonth == 1 ? 12 : viewMonth - 1, prevDays = DaysIn(py, pm);
    float in = ui.SmoothFrom(Ui::Id("cal.month", viewYear * 12 + viewMonth), 1.0f, 0.0f, 10); // a new month fades in
    const Feast* greet = nullptr; // the holiday under the mouse, greeted above its day
    D2D1_POINT_2F greetAt{};
    for (int cell = 0; cell < 42; ++cell) {
        int d = cell - lead + 1, y = viewYear, mo = viewMonth;
        bool outside = d < 1 || d > days;
        if (d < 1) d += prevDays, mo = pm, y = py;
        else if (d > days) d -= days, mo = viewMonth == 12 ? 1 : viewMonth + 1, y = viewMonth == 12 ? viewYear + 1 : viewYear;
        D2D1_POINT_2F o = {grid.left + (cell % 7 + 0.5f) * cw, grid.top + 22 + (cell / 7 + 0.5f) * rh};
        float rad = (std::min)(cw, rh) * 0.40f;
        uint32_t id = Ui::Id("cal.day", cell);
        Ui::Hit h = ui.Button(id, {o.x - rad, o.y - rad, o.x + rad, o.y + rad});
        float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 18);
        bool today = y == now.wYear && mo == now.wMonth && d == now.wDay;
        bool picked = pickD && y == pickY && mo == pickM && d == pickD;
        if (today) {
            c.Dot(o, rad * 1.08f, t.accent.Alpha(in)); // flat accent, no glow
        } else if (hover > 0.01f) {
            c.Dot(o, rad, t.text.Alpha(0.08f * hover * in));
        }
        if (picked && !today) c.Ring(o, rad, t.accentHi.Alpha(in), 1.5f);
        int wd = Weekday(y, mo, d);
        bool past = DayIndex(y, mo, d) < DayIndex(now.wYear, now.wMonth, now.wDay);
        const Feast* feast = Holiday(y, mo, d);
        Color ink = today ? t.onAccent : outside ? t.textLow.Alpha(0.45f) : past ? t.textLow : (wd == 0 || wd == 6) ? t.textDim : t.text; // what's gone fades back
        if (feast) { // a holiday: an accent-tinted disc with a fine rim, its number in the accent and no ornament
            float lit = (outside ? 0.35f : past ? 0.55f : 1.0f) * in;
            if (!today) {
                c.Dot(o, rad, t.accent.Alpha((0.20f + 0.10f * hover) * lit)); // a flat tint
                if (!picked) c.Ring(o, rad, t.accentHi.Alpha((0.40f + 0.30f * hover) * lit), 1);
                ink = t.accentHi.Alpha(outside ? 0.5f : past ? 0.7f : 1.0f);
            }
            if (today) c.Ring(o, rad * 1.34f, t.accentHi.Alpha(0.7f * lit), 1.2f); // a holiday that is today: a fine ring around its circle
            if (h.hover && !outside) greet = feast, greetAt = {o.x, o.y - rad};
        }
        c.Text(std::to_wstring(d), Rect(o.x - rad, o.y - rad, 2 * rad, 2 * rad), today || feast ? Weight::Semi : past ? Weight::Light : Weight::Medium, today ? 15.0f : 13.5f, ink.Alpha(in), Align::Center);
        if (h.clicked) {
            pickY = y, pickM = mo, pickD = d;
            if (outside) viewYear = y, viewMonth = mo; // a day from the next or last month takes you there
        }
    }
    if (in < 0.999f) ui.Wake();
    static const Feast* shown = nullptr; // keeps the last greeting while it fades out
    static D2D1_POINT_2F shownAt{};
    if (greet) shown = greet, shownAt = greetAt;
    float ga = ui.Smooth(Ui::Id("cal.greet"), greet ? 1.0f : 0, 18);
    if (shown && ga > 0.01f) {
        std::wstring hi = shown->greet;
        float gw = 42, gh = 26, lift = (1 - ga) * 6;
        ComPtr<IDWriteTextLayout> gl;
        DWRITE_TEXT_METRICS gm{};
        if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(hi.c_str(), (UINT32)hi.size(), gfx::TextFmt(Weight::Medium, 12, Align::Left), 1000, 30, &gl)) && SUCCEEDED(gl->GetMetrics(&gm)))
            gw += gm.widthIncludingTrailingWhitespace;
        float gx = std::clamp(shownAt.x - gw / 2, grid.left, grid.right - gw);
        D2D1_RECT_F r = Rect(gx, shownAt.y - gh - 6 + lift, gw, gh);
        c.Fill(Inset(r, -1, -2), gh / 2 + 2, Color{0, 0, 0, 0.22f * ga}); // a quiet tag, like the rail's: soft shadow, card body, hairline
        c.Fill(r, gh / 2, t.veil.Alpha(0.94f * ga)); // solid: under glass the days behind would read through
        c.Fill(r, gh / 2, t.cardHi.Alpha(ga));
        c.StrokeV(r, gh / 2, t.hairTop.Alpha(ga), t.hairBot.Alpha(ga), 1);
        c.Icon(glyph::Sparkle, {r.left + 16, (r.top + r.bottom) / 2}, 12, t.accentHi.Alpha(ga), true);
        c.Text(hi, Rect(r.left + 26, r.top, gw - 36, gh), Weight::Medium, 12, t.text.Alpha(ga), Align::Center);
    }

    int sy = pickD ? pickY : now.wYear, sm = pickD ? pickM : now.wMonth, sd = pickD ? pickD : now.wDay; // the line under the month
    long diff = DayIndex(sy, sm, sd) - DayIndex(now.wYear, now.wMonth, now.wDay);
    std::wstring when = diff == 0 ? L"Today" : diff == 1 ? L"Tomorrow" : diff == -1 ? L"Yesterday"
                      : diff > 0 ? L"In " + std::to_wstring(diff) + L" days" : std::to_wstring(-diff) + L" days ago";
    const Feast* feast = Holiday(sy, sm, sd);
    std::wstring day = Format(sy, sm, sd, L"dddd, d MMMM");
    c.Text(day, Rect(list.left + 4, list.bottom - 28, Width(list) * 0.6f, 20), Weight::Medium, 13, t.text);
    c.Text(feast ? std::wstring(feast->name) : when, Rect(list.left + Width(list) * 0.4f, list.bottom - 28, Width(list) * 0.6f - 4, 20), Weight::Semi, 13, t.accentHi, Align::Right); // a holiday names itself

    if (ui.RClick(s)) m.CloseCal(); // right click closes, like the other sheets
    End(p, frame);
}
