// The widget catalogue, and how each one draws itself in whatever rectangle it's given.
#include "faces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "faces_state.h"
#include "feeds.h"
#include "glyphs.h"

namespace faces {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kPad = 9;
constexpr float kTinyW = 60; // narrower than this: an icon and a number
constexpr float kTallH = 72; // taller than this: a card with room to breathe
constexpr uint32_t kUp = 0x4ADE80, kDown = 0xFF6B6B;

constexpr int kClockG = glyph::Timer, kWebG = glyph::Web, kMoneyG = glyph::Bolt, kPeopleG = glyph::Phone;

const Meta kAll[] = {
    // clocks
    {"clock.digital", L"Clock", L"Large, readable time, its seconds and the date", kClocks, kClockG, 7, 1, 112, 176, nullptr, false},
    {"clock.analog", L"Analog Classic", L"A white face with proper hands", kClocks, kClockG, 7, 0, 44, 44, nullptr, false},
    {"clock.analogdark", L"Analog Classic Dark", L"The same classic face, in black", kClocks, kClockG, 7, 0, 44, 44, nullptr, false},
    {"clock.analogmin", L"Analog Minimal", L"Hands and hour ticks, nothing else", kClocks, kClockG, 7, 0, 44, 44, nullptr, false},
    {"clock.segment", L"Segment Clock", L"The time as a seven-segment display", kClocks, kClockG, 6, 1, 112, 160, nullptr, false},
    {"clock.worldfaces", L"World Clocks", L"Three small faces for the cities you work with", kClocks, kWebG, 6, 1, 132, 176, nullptr, false},
    {"clock.worldlist", L"World Clock", L"Three cities, each with its hour difference", kClocks, kWebG, 6, 1, 148, 200, nullptr, false},
    {"clock.worldone", L"World Time", L"One more city in big digits, with its offset", kClocks, kWebG, 7, 1, 120, 168, nullptr, false},
    {"clock.today", L"Today", L"Weekday, date, time and weather in one tile", kClocks, glyph::Calendar, 6, 1, 156, 216, nullptr, false},
    {"clock.date", L"Date", L"The full date, set large", kClocks, glyph::Calendar, 7, 1, 124, 184, nullptr, false},
    {"clock.daynum", L"Day Number", L"Today's date as a calendar page", kClocks, glyph::Calendar, 1, 0, 44, 44, nullptr, false},
    {"clock.week", L"7 Days", L"This week as a strip of days", kClocks, glyph::Calendar, 6, 1, 176, 240, nullptr, false},
    {"clock.wx", L"Clock & Weather", L"Time, date and temperature as three blocks", kClocks, kClockG, 6, 1, 168, 216, nullptr, false},
    {"clock.flip", L"Flip Clock", L"Split-flap digits that turn over each minute", kClocks, kClockG, 7, 1, 92, 160, nullptr, true},
    // weather
    {"wx.details", L"Weather Details", L"Wind, rain and the day's range", kWeather, glyph::Cloud, 6, 1, 160, 224, "sky", false},
    {"wx.forecast", L"Weather Forecast", L"The next four days", kWeather, glyph::Cloud, 6, 1, 176, 256, nullptr, false},
    {"wx.todayfc", L"Today & Forecast", L"Today next to the next three days", kWeather, glyph::Cloud, 6, 2, 196, 280, nullptr, false},
    {"wx.status", L"Weather Status", L"The conditions in words, with your city", kWeather, glyph::Cloud, 6, 1, 148, 216, nullptr, false},
    {"wx.hourly", L"Hourly", L"The next six hours", kWeather, glyph::Cloud, 6, 2, 196, 280, nullptr, false},
    {"wx.sun", L"Sunrise & Sunset", L"Where the sun is between the two", kWeather, glyph::ClearDay, 7, 1, 148, 216, "sky", false},
    // productivity
    {"time.timeline", L"Day Timeline", L"Your whole day on one line", kWork, glyph::Timer, 6, 2, 220, 320, "screen", false},
    {"time.screen", L"Screen Time", L"Today in front of the PC, by hour and by app", kWork, glyph::Pc, 7, 1, 148, 224, "screen", false},
    {"time.timer", L"Timer", L"A countdown that starts in one click", kWork, glyph::Timer, 7, 0, 112, 160, nullptr, false},
    {"time.stopwatch", L"Stopwatch", L"Start, pause and lap from here", kWork, glyph::Timer, 7, 0, 112, 160, nullptr, false},
    {"money.currency", L"Currency", L"Convert at current rates", kWork, kMoneyG, 7, 1, 148, 208, "rates", false},
    {"prog.day", L"Day Progress", L"How much of today is behind you", kWork, glyph::Timer, 7, 0, 112, 160, nullptr, false},
    {"prog.month", L"Month Progress", L"How much of the month is behind you", kWork, glyph::Calendar, 7, 0, 112, 160, nullptr, false},
    {"prog.year", L"Year Progress", L"The year as a grid of dots, filling up", kWork, glyph::Calendar, 6, 2, 160, 256, nullptr, false},
    {"list.water", L"Drink Water", L"Glasses today, against your goal", kWork, glyph::Coffee, 7, 0, 92, 176, nullptr, true},
    {"list.notes", L"Notes", L"A quick note, kept between restarts", kWork, glyph::Edit, 7, 1, 136, 196, nullptr, true},
    {"list.habits", L"Habits", L"Daily habits and their streaks", kWork, glyph::Check, 7, 1, 124, 196, nullptr, true},
    // system
    {"sys.cpu", L"CPU", L"Processor load as a ring", kSystem, glyph::Cpu, 7, 0, 104, 148, nullptr, false},
    {"sys.ram", L"RAM", L"Memory in use as a ring", kSystem, glyph::Ram, 7, 0, 104, 148, nullptr, false},
    {"sys.temp", L"Temperature", L"Thermal zone, with the load beside it", kSystem, glyph::Cpu, 7, 1, 124, 176, "thermal", false},
    {"sys.lang", L"Language", L"The keyboard layout; click to switch", kSystem, glyph::Keys, 7, 0, 112, 148, nullptr, false},
    {"sys.bright", L"Brightness", L"A slider for the display's brightness", kSystem, glyph::Bright, 7, 0, 124, 176, nullptr, false},
    {"sys.theme", L"Theme Toggle", L"Switch Windows between light and dark", kSystem, glyph::Bright, 7, 0, 112, 148, nullptr, false},
    {"sys.tray", L"Tray", L"Your tray icons, one click away", kSystem, glyph::Tools, 1, 0, 44, 44, nullptr, false},
    {"sys.storage", L"Storage", L"Free space on your drive", kSystem, glyph::Pc, 7, 1, 136, 196, nullptr, false},
    {"sys.drives", L"Drives", L"Your drives and their free space", kSystem, glyph::Pc, 7, 0, 136, 196, nullptr, true},
    // media
    {"media.compact", L"Now Playing (compact)", L"Title, artist and play or pause", kMedia, glyph::Music, 6, 1, 176, 236, nullptr, false},
    {"media.mini", L"Now Playing Mini", L"One round play button with the song's progress", kMedia, glyph::Music, 1, 0, 44, 44, nullptr, false},
    {"media.bar", L"Now Playing Bar", L"The song with a progress bar and skip", kMedia, glyph::Music, 6, 1, 196, 280, nullptr, false},
    // money
    {"money.total", L"Total Revenue", L"Every connected store's last 30 days", kMoney, kMoneyG, 7, 1, 148, 208, "total", false},
    {"money.stripe", L"Stripe", L"Net revenue over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "stripe", false},
    {"money.stripechart", L"Stripe Chart", L"Your Stripe revenue as a line", kMoney, kMoneyG, 6, 1, 176, 256, "stripe", false},
    {"money.polar", L"Polar", L"Net revenue over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "polar", false},
    {"money.polarchart", L"Polar Chart", L"Your Polar revenue as a 14-day line", kMoney, kMoneyG, 6, 1, 176, 256, "polar", false},
    {"money.dodo", L"Dodo Payments", L"Revenue over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "dodo", false},
    {"money.dodochart", L"Dodo Chart", L"Your Dodo Payments revenue as a line", kMoney, kMoneyG, 6, 1, 176, 256, "dodo", false},
    {"money.shopify", L"Shopify", L"Your store's sales over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "shopify", false},
    {"money.shopifychart", L"Shopify Chart", L"This week's sales and how they moved", kMoney, kMoneyG, 6, 1, 176, 256, "shopify", false},
    {"money.lemon", L"Lemon Squeezy", L"Revenue over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "lemon", false},
    {"money.lemonchart", L"Lemon Squeezy Chart", L"This week's revenue over a 14-day line", kMoney, kMoneyG, 6, 1, 176, 256, "lemon", false},
    {"money.paddle", L"Paddle", L"What you earned over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "paddle", false},
    {"money.paddlechart", L"Paddle Chart", L"This week's revenue over a 14-day line", kMoney, kMoneyG, 6, 1, 176, 256, "paddle", false},
    {"money.gumroad", L"Gumroad", L"Your sales over 30 days", kMoney, kMoneyG, 7, 1, 136, 196, "gumroad", false},
    {"money.gumroadchart", L"Gumroad Chart", L"This week's sales over a 14-day line", kMoney, kMoneyG, 6, 1, 176, 256, "gumroad", false},
    {"stock.one", L"Stock", L"One ticker's price and daily change", kMoney, glyph::Up, 7, 1, 136, 196, nullptr, false},
    {"stock.chart", L"Stock Chart", L"One ticker with its intraday line", kMoney, glyph::Up, 6, 1, 176, 256, nullptr, false},
    {"stock.watch", L"Stock Watchlist", L"Three tickers at a glance", kMoney, glyph::Up, 6, 2, 196, 300, nullptr, false},
    {"money.metric", L"Metric", L"A live number, such as BTC-USD", kMoney, kMoneyG, 7, 1, 136, 196, "metric", false},
    // online
    {"web.plausible", L"Plausible", L"Visitors this week, with the trend", kOnline, kWebG, 7, 1, 148, 224, "plausible", false},
    {"web.dub", L"Dub", L"Clicks on your links this week", kOnline, kWebG, 7, 1, 148, 224, "dub", false},
    {"web.datafast", L"DataFast Chart", L"Daily visitors over 14 days", kOnline, kWebG, 6, 1, 176, 256, "datafast", false},
    {"web.resend", L"Resend", L"Emails sent this week", kOnline, kWebG, 7, 1, 136, 196, "resend", false},
    {"web.linear", L"Linear", L"Issues assigned to you, and in progress", kOnline, glyph::Check, 7, 1, 136, 196, "linear", false},
    {"web.supabase", L"Supabase", L"API requests on your project, and its health", kOnline, kWebG, 7, 1, 148, 208, "supabase", false},
    {"web.gmail", L"Gmail", L"Unread mail in your inbox", kOnline, glyph::Sparkle, 7, 0, 112, 176, "gmail", false},
    {"web.github", L"GitHub", L"Unread notifications", kOnline, glyph::Sparkle, 7, 0, 112, 176, "github", false},
    {"web.githubover", L"GitHub Overview", L"Notifications, open pull requests and stars", kOnline, glyph::Sparkle, 6, 1, 176, 236, "github.overview", false},
    {"web.githubgrid", L"GitHub Activity", L"Your contribution grid and streak", kOnline, glyph::Check, 6, 2, 196, 320, "github.activity", false},
    {"web.leetcode", L"LeetCode Stats", L"Problems solved and your streak", kOnline, glyph::Check, 7, 1, 136, 196, "leetcode", false},
    {"social.youtube", L"YouTube", L"Subscribers on your channel", kOnline, kPeopleG, 7, 1, 136, 196, "youtube", false},
    {"social.x", L"X Profile", L"Followers on your account", kOnline, kPeopleG, 7, 1, 136, 196, "x", false},
    {"social.tiktok", L"TikTok", L"Followers on your profile", kOnline, kPeopleG, 7, 1, 136, 196, "tiktok", false},
    {"social.instagram", L"Instagram", L"Followers on your account", kOnline, kPeopleG, 7, 1, 136, 196, "instagram", false},
    // apps and files
    {"files.recent", L"Recently Opened", L"The files you used last", kFiles, glyph::Folder, 7, 0, 136, 196, nullptr, false},
    {"files.commands", L"Commands", L"Type a word to launch an app or an action", kFiles, glyph::Keys, 7, 0, 136, 196, nullptr, false},
    {"files.websearch", L"Web Search", L"Search the web, or type a folder path to open it", kFiles, kWebG, 7, 1, 156, 216, nullptr, false},
    {"files.shots", L"Recent Screenshots", L"Your newest screenshots", kFiles, glyph::Snip, 7, 0, 136, 196, nullptr, false},
    {"files.wallpaper", L"Wallpaper Gallery", L"Browse and set wallpapers", kFiles, glyph::Pc, 7, 0, 136, 196, nullptr, false},
    {"files.downloads", L"Downloads", L"The newest files, a click away", kFiles, glyph::Folder, 7, 0, 136, 196, nullptr, true},
    {"files.documents", L"Documents", L"Your documents folder's newest files", kFiles, glyph::Folder, 7, 0, 136, 196, nullptr, true},
    {"files.photo", L"Photo", L"A picture from your Pictures folder", kFiles, glyph::Pc, 7, 1, 116, 184, nullptr, true},
    {"files.emoji", L"Emoji", L"Windows' emoji panel, into what you type", kFiles, glyph::Edit, 1, 0, 44, 44, nullptr, true},
};

constexpr int kKinds = (int)(sizeof(kAll) / sizeof(kAll[0]));

std::wstring Short(double v) { // 950, 1.2k, 3.4M
    wchar_t b[32];
    double a = std::fabs(v);
    if (a >= 1e9) swprintf_s(b, L"%.1fB", v / 1e9);
    else if (a >= 1e6) swprintf_s(b, L"%.1fM", v / 1e6);
    else if (a >= 1e4) swprintf_s(b, L"%.1fk", v / 1e3);
    else if (a >= 100 || a == std::floor(a)) swprintf_s(b, L"%.0f", v);
    else swprintf_s(b, L"%.2f", v);
    return b;
}

std::wstring Deg(double c, bool f) { return std::to_wstring((int)std::lround(f ? c * 9 / 5 + 32 : c)) + L"°"; }

std::wstring ClockText(const SYSTEMTIME& st, bool h24) {
    wchar_t b[16];
    int hh = h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12);
    swprintf_s(b, h24 ? L"%02d:%02d" : L"%d:%02d", hh, st.wMinute);
    return b;
}

std::wstring DateText(const SYSTEMTIME& st, const wchar_t* format) {
    wchar_t b[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, format, b, 64, nullptr);
    return b;
}

double Frac(int kind) { // day, month, year
    SYSTEMTIME st;
    GetLocalTime(&st);
    double day = (st.wHour * 60 + st.wMinute) / 1440.0;
    if (kind == 0) return day;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (st.wYear % 4 == 0 && st.wYear % 100) || st.wYear % 400 == 0;
    if (kind == 1) return (st.wDay - 1 + day) / (kDays[st.wMonth - 1] + (st.wMonth == 2 && leap));
    int doy = st.wDay - 1;
    for (int m = 1; m < st.wMonth; ++m) doy += kDays[m - 1] + (m == 2 && leap);
    return (doy + day) / (leap ? 366 : 365);
}

void Spark(Canvas& c, const D2D1_RECT_F& r, const std::vector<double>& pts, Color ink) {
    if (pts.size() < 2) return;
    double lo = pts[0], hi = pts[0];
    for (double p : pts) lo = (std::min)(lo, p), hi = (std::max)(hi, p);
    if (hi - lo < 1e-9) hi = lo + 1;
    float step = Width(r) / (float)(pts.size() - 1);
    auto at = [&](size_t i) { return D2D1_POINT_2F{r.left + step * i, r.bottom - Height(r) * (float)((pts[i] - lo) / (hi - lo))}; };
    for (size_t i = 1; i < pts.size(); ++i) c.Line(at(i - 1), at(i), ink, 1.4f);
    c.Dot(at(pts.size() - 1), 2.2f, ink);
}

void Stat(Canvas& c, const Theme& t, const D2D1_RECT_F& r, int g, const std::wstring& name, const std::wstring& main, const std::wstring& sub, const std::vector<double>* pts = nullptr, Color ink = {}) { // the common face
    float w = Width(r), h = Height(r), mid = (r.top + r.bottom) / 2;
    Color lit = (ink.r + ink.g + ink.b) > 0 ? ink : t.accentHi;
    if (w < kTinyW) {
        c.Icon(g, {(r.left + r.right) / 2, mid - 4}, 17, lit);
        c.Text(main, Rect(r.left - 2, mid + 5, w + 4, 12), Weight::Semi, 9, t.text, Align::Center);
        return;
    }
    if (h < kTallH) {
        c.Icon(g, {r.left + kPad + 10, mid}, 19, lit, true);
        float tx = r.left + kPad + 26;
        float room = pts && w > 150 ? w * 0.48f : 0;
        float tw = r.right - kPad - tx - room;
        if (sub.empty()) {
            c.Text(main, Rect(tx, r.top, tw, h), Weight::Semi, 12.5f, t.text);
        } else {
            c.Text(main, Rect(tx, mid - 15, tw, 16), Weight::Semi, 12.5f, t.text);
            c.Text(sub, Rect(tx, mid + 1, tw, 14), Weight::Regular, 10.5f, t.textDim);
        }
        if (room > 0) Spark(c, {r.right - kPad - room + 6, r.top + 9, r.right - kPad - 2, r.bottom - 9}, *pts, lit);
        return;
    }
    c.Icon(g, {r.left + 22, r.top + 22}, 18, lit, true);
    c.Text(name, Rect(r.left + 40, r.top + 12, w - 50, 20), Weight::Medium, 12, t.textDim);
    c.Text(main, Rect(r.left + 16, r.top + 38, w - 32, 36), Weight::Light, 25, t.text);
    if (!sub.empty()) c.Text(sub, Rect(r.left + 16, r.top + 74, w - 32, 16), Weight::Regular, 11, t.textDim);
    if (pts && r.bottom - (r.top + 96) > 14) Spark(c, {r.left + 16, r.top + 96, r.right - 16, r.bottom - 12}, *pts, lit);
}

void Lane(Canvas& c, const Theme& t, const D2D1_RECT_F& r, double v, Color ink) {
    c.Fill(r, Height(r) / 2, t.text.Alpha(0.12f));
    c.Fill(Rect(r.left, r.top, (std::max)(Height(r), Width(r) * (float)(std::clamp)(v, 0.0, 1.0)), Height(r)), Height(r) / 2, ink);
}

void RingFace(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const std::wstring& name, double v, const std::wstring& inside, const std::wstring& sub) {
    float w = Width(r), h = Height(r), mid = (r.top + r.bottom) / 2;
    float rad = (std::min)(13.0f, h / 2 - 5);
    bool tall = h >= kTallH;
    D2D1_POINT_2F o = w < kTinyW ? D2D1_POINT_2F{(r.left + r.right) / 2, mid} : D2D1_POINT_2F{r.left + kPad + rad, mid};
    if (tall) rad = (std::min)(w, h) * 0.32f, o = {(r.left + r.right) / 2, r.top + h * 0.45f};
    Color hot = Color::Rgb(kDown);
    c.Ring(o, rad, t.text.Alpha(0.14f), tall ? 5.0f : 2.5f);
    c.Arc(o, rad, -kPi / 2, 2 * kPi * (float)(std::clamp)(v, 0.0, 1.0), v > 0.85 ? hot : t.accentHi, tall ? 5.0f : 2.5f);
    if (tall) {
        c.Text(inside, Rect(o.x - rad, o.y - 14, 2 * rad, 28), Weight::Medium, 18, t.text, Align::Center);
        c.Text(name, Rect(r.left, r.bottom - 26, w, 16), Weight::Medium, 11.5f, t.textDim, Align::Center);
        return;
    }
    c.Text(inside, Rect(o.x - 13, mid - 7, 26, 14), Weight::Semi, 9.5f, t.text, Align::Center);
    if (w >= kTinyW) {
        float tx = o.x + rad + 8;
        c.Text(name, Rect(tx, sub.empty() ? r.top : mid - 15, r.right - kPad - tx, sub.empty() ? h : 16), Weight::Semi, 12, t.text);
        if (!sub.empty()) c.Text(sub, Rect(tx, mid + 1, r.right - kPad - tx, 14), Weight::Regular, 10.5f, t.textDim);
    }
}

void Analog(Canvas& c, const Theme& t, D2D1_POINT_2F o, float rad, int style, int hh, int mm) { // 0 white, 1 black, 2 bare ticks
    Color face = style == 0 ? Color{0.96f, 0.96f, 0.95f, 1} : Color{0.07f, 0.07f, 0.08f, 1};
    Color ink = style == 0 ? Color{0.08f, 0.08f, 0.09f, 1} : style == 1 ? Color{0.95f, 0.95f, 0.95f, 1} : t.text;
    if (style != 2) c.Dot(o, rad, face);
    if (style == 1) c.Ring(o, rad, Color{1, 1, 1, 0.14f}, 1);
    for (int i = 0; i < 12; ++i) {
        if (rad < 16 && i % 3) continue; // too small for all twelve
        float a = i * kPi / 6, s = std::sin(a), k = std::cos(a), in = rad * (i % 3 ? 0.84f : 0.76f);
        c.Line({o.x + s * in, o.y - k * in}, {o.x + s * (rad - 2), o.y - k * (rad - 2)}, ink.Alpha(i % 3 ? 0.45f : 0.9f), i % 3 ? 1.0f : 1.4f);
    }
    float ha = ((hh % 12) + mm / 60.0f) * kPi / 6, ma = mm * kPi / 30;
    c.Line(o, {o.x + std::sin(ha) * rad * 0.5f, o.y - std::cos(ha) * rad * 0.5f}, ink, (std::max)(1.6f, rad * 0.09f));
    c.Line(o, {o.x + std::sin(ma) * rad * 0.76f, o.y - std::cos(ma) * rad * 0.76f}, ink, (std::max)(1.2f, rad * 0.06f));
    c.Dot(o, (std::max)(1.6f, rad * 0.07f), t.accent);
}

void Segment(Canvas& c, const D2D1_RECT_F& r, int digit, Color on, Color off) { // a b c d e f g, clockwise from the top, g across the middle
    static const uint8_t kMap[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    uint8_t m = kMap[digit % 10];
    float w = Width(r), h = Height(r), th = (std::max)(1.6f, w * 0.17f);
    auto seg = [&](int bit, float x, float y, float sw, float sh) { c.Fill(Rect(r.left + x, r.top + y, sw, sh), th / 2, (m >> bit) & 1 ? on : off); };
    seg(0, th * 0.7f, 0, w - 1.4f * th, th);
    seg(1, w - th, th * 0.7f, th, h / 2 - th);
    seg(2, w - th, h / 2 + th * 0.3f, th, h / 2 - th);
    seg(3, th * 0.7f, h - th, w - 1.4f * th, th);
    seg(4, 0, h / 2 + th * 0.3f, th, h / 2 - th);
    seg(5, 0, th * 0.7f, th, h / 2 - th);
    seg(6, th * 0.7f, h / 2 - th / 2, w - 1.4f * th, th);
}

void Clock7(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const SYSTEMTIME& st, bool h24) {
    int hh = h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12);
    int d[4] = {hh / 10, hh % 10, st.wMinute / 10, st.wMinute % 10};
    float h = (std::min)(Height(r) - 12, Width(r) * 0.42f), dw = h * 0.52f, gap = dw * 0.26f, colon = dw * 0.34f;
    float x = (r.left + r.right) / 2 - (4 * dw + 3 * gap + colon) / 2, y = (r.top + r.bottom) / 2 - h / 2;
    Color on = t.accentHi, off = t.text.Alpha(0.07f);
    for (int i = 0; i < 4; ++i) {
        if (i == 2) {
            c.Dot({x + colon / 2 - gap / 2, y + h * 0.32f}, 1.5f, on);
            c.Dot({x + colon / 2 - gap / 2, y + h * 0.68f}, 1.5f, on);
            x += colon;
        }
        Segment(c, Rect(x, y, dw, h), d[i], on, off);
        x += dw + gap;
    }
}

Color Trend(const Theme& t, double change) { return change > 0.0005 ? Color::Rgb(kUp) : change < -0.0005 ? Color::Rgb(kDown) : t.textDim; }

std::wstring Pct(double change) {
    wchar_t b[24];
    swprintf_s(b, L"%+.1f%%", change * 100);
    return b;
}

std::wstring NotYet(const feeds::Series& s) { return s.error.empty() ? std::wstring(L"Fetching…") : s.error; }

void MoneyFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r) {
    const Meta& m = kAll[kind];
    feeds::Series s = feeds::Get(m.feed);
    std::wstring sym = feeds::Setting("money.sym");
    if (sym.empty()) sym = L"$";
    bool chart = wcsstr(m.name, L"Chart") != nullptr;
    if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
    if (chart) return Stat(c, t, r, m.glyph, m.name, sym + Short(s.week) + L" this week", Pct(s.change), &s.points, Trend(t, s.change));
    Stat(c, t, r, m.glyph, m.name, sym + Short(s.total), L"Last 30 days");
}

void VisitFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r, const wchar_t* unit) {
    const Meta& m = kAll[kind];
    feeds::Series s = feeds::Get(m.feed);
    if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
    Stat(c, t, r, m.glyph, m.name, Short(s.week) + L" " + unit, Pct(s.change) + L" this week", &s.points, Trend(t, s.change));
}

void CountFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r, const std::wstring& words) {
    const Meta& m = kAll[kind];
    feeds::Series s = feeds::Get(m.feed);
    if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
    Stat(c, t, r, m.glyph, m.name, Short(s.value), words);
}

std::wstring Ticker(const char* key, const wchar_t* fallback) {
    std::wstring s = feeds::Setting(key);
    return s.empty() ? fallback : s;
}

std::string QuoteId(const std::wstring& ticker) {
    std::string id = "quote:";
    for (wchar_t ch : ticker) id += (char)ch;
    return id;
}

std::vector<std::wstring> Watch() {
    std::wstring all = Ticker("stock.watch", L"AAPL,MSFT,NVDA");
    std::vector<std::wstring> out;
    for (size_t a = 0; a < all.size() && out.size() < 3;) {
        size_t e = (std::min)(all.find(L',', a), all.size());
        std::wstring one = all.substr(a, e - a);
        if (!one.empty()) out.push_back(one);
        a = e + 1;
    }
    return out;
}

void DrawClock(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const Host& h, const SYSTEMTIME& st) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    auto twoLines = [&] {
        wchar_t a[4], b[4];
        swprintf_s(a, L"%02d", h.h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12)), swprintf_s(b, L"%02d", st.wMinute);
        c.Text(a, Rect(r.left, mid - 15, w, 15), Weight::Semi, 12.5f, t.text, Align::Center);
        c.Text(b, Rect(r.left, mid, w, 15), Weight::Semi, 12.5f, t.accentHi, Align::Center);
    };
    if (key == "clock.digital") {
        std::wstring time = ClockText(st, h.h24);
        if (tiny) {
            twoLines();
        } else if (tall) {
            wchar_t sec[8];
            swprintf_s(sec, L":%02d", st.wSecond);
            c.Text(time + sec, Rect(r.left + 12, mid - 30, w - 24, 44), Weight::Light, 32, t.text, Align::Center);
            c.Text(DateText(st, L"dddd d MMMM"), Rect(r.left + 12, mid + 14, w - 24, 18), Weight::Regular, 12, t.textDim, Align::Center);
        } else {
            c.Text(time, Rect(r.left + kPad, mid - 16, w - 2 * kPad, 20), Weight::Semi, 16, t.text);
            c.Text(DateText(st, L"ddd d MMM"), Rect(r.left + kPad, mid + 4, w - 2 * kPad, 13), Weight::Regular, 10, t.textDim);
        }
    } else if (key == "clock.analog" || key == "clock.analogdark" || key == "clock.analogmin") {
        int style = key == "clock.analog" ? 0 : key == "clock.analogdark" ? 1 : 2;
        Analog(c, t, {cx, mid}, (std::min)(w, rh) / 2 - 3, style, st.wHour, st.wMinute);
    } else if (key == "clock.segment") {
        Clock7(c, t, r, st, h.h24);
        if (tall) c.Text(DateText(st, L"dddd d MMM"), Rect(r.left, r.bottom - 26, w, 16), Weight::Regular, 11, t.textDim, Align::Center);
    } else if (key == "clock.worldfaces") {
        auto zones = state::Zones(3);
        float cw = w / (float)(std::max)((size_t)1, zones.size());
        for (size_t i = 0; i < zones.size(); ++i) {
            SYSTEMTIME z = state::ZoneNow(zones[i]);
            float zx = r.left + cw * (i + 0.5f), rad = (std::min)(cw / 2 - 4, tall ? rh * 0.3f : rh / 2 - 9);
            Analog(c, t, {zx, tall ? mid - 8 : mid - 5}, rad, 1, z.wHour, z.wMinute);
            c.Text(zones[i], Rect(zx - cw / 2, tall ? mid + rad : mid + rad - 4, cw, 12), Weight::Medium, 9, t.textDim, Align::Center);
        }
    } else if (key == "clock.worldlist") {
        auto zones = state::Zones(3);
        float row = (std::min)(rh / 3.2f, 26.0f), y = mid - row * zones.size() / 2;
        for (const auto& name : zones) {
            SYSTEMTIME z = state::ZoneNow(name);
            c.Text(name, Rect(r.left + kPad, y, w * 0.5f, row), Weight::Medium, tall ? 12.0f : 9.5f, t.text);
            c.Text(ClockText(z, h.h24) + L"  " + state::Offset(name), Rect(r.left, y, w - kPad, row), Weight::Regular, tall ? 11.5f : 9.5f, t.textDim, Align::Right);
            y += row;
        }
    } else if (key == "clock.worldone") {
        std::wstring name = Ticker("zone.one", L"Tokyo");
        SYSTEMTIME z = state::ZoneNow(name);
        Stat(c, t, r, m.glyph, name, ClockText(z, h.h24), tiny ? L"" : name + L"  " + state::Offset(name));
    } else if (key == "clock.today") {
        c.Text(DateText(st, L"ddd d MMM"), Rect(r.left + kPad, mid - 16, w * 0.58f, 16), Weight::Semi, 12, t.text);
        c.Text(ClockText(st, h.h24), Rect(r.left + kPad, mid + 1, w * 0.58f, 15), Weight::Regular, 11, t.textDim);
        if (h.wxValid) {
            c.Icon(WxIcon(h.wxCode, h.wxDay), {r.right - 44, mid}, 19, t.accentHi, true);
            c.Text(Deg(h.tempC, h.fahrenheit), Rect(r.right - 34, r.top, 30, rh), Weight::Semi, 12.5f, t.text);
        }
    } else if (key == "clock.date") {
        if (tiny) {
            c.Text(DateText(st, L"MMM"), Rect(r.left, mid - 15, w, 13), Weight::Medium, 9.5f, t.textDim, Align::Center);
            c.Text(std::to_wstring(st.wDay), Rect(r.left, mid - 3, w, 18), Weight::Semi, 14, t.text, Align::Center);
        } else if (tall) {
            c.Text(DateText(st, L"dddd"), Rect(r.left, mid - 34, w, 18), Weight::Regular, 13, t.textDim, Align::Center);
            c.Text(DateText(st, L"d MMMM"), Rect(r.left, mid - 14, w, 38), Weight::Light, 28, t.text, Align::Center);
            c.Text(std::to_wstring(st.wYear), Rect(r.left, mid + 24, w, 16), Weight::Regular, 11.5f, t.textDim, Align::Center);
        } else {
            c.Text(DateText(st, L"d MMMM"), Rect(r.left + kPad, mid - 15, w - 2 * kPad, 17), Weight::Semi, 13, t.text);
            c.Text(DateText(st, L"dddd"), Rect(r.left + kPad, mid + 2, w - 2 * kPad, 13), Weight::Regular, 10.5f, t.textDim);
        }
    } else if (key == "clock.daynum") {
        float s = (std::min)(w, rh) - 8;
        D2D1_RECT_F page = Rect(cx - s / 2, mid - s / 2, s, s);
        c.Fill(page, s * 0.2f, t.text.Alpha(0.1f));
        c.Fill({page.left, page.top, page.right, page.top + s * 0.3f}, s * 0.14f, t.accent);
        c.Text(std::to_wstring(st.wDay), Rect(page.left, page.top + s * 0.28f, s, s * 0.7f), Weight::Semi, s * 0.46f, t.text, Align::Center);
    } else if (key == "clock.week") {
        int back = (st.wDayOfWeek + 6) % 7; // from monday
        float cw = (w - 2 * kPad) / 7;
        static const wchar_t* const kNames[7] = {L"M", L"T", L"W", L"T", L"F", L"S", L"S"};
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        for (int i = 0; i < 7; ++i) {
            ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
            u.QuadPart += (ULONGLONG)((LONGLONG)(i - back) * 864000000000LL);
            FILETIME f2 = {u.LowPart, u.HighPart};
            SYSTEMTIME d;
            FileTimeToSystemTime(&f2, &d);
            float dx = r.left + kPad + cw * i;
            bool today = i == back;
            if (today) c.Fill(Rect(dx + 1, mid - 16, cw - 2, 32), 8, t.accent);
            c.Text(kNames[i], Rect(dx, mid - 15, cw, 13), Weight::Medium, 9, today ? t.onAccent : t.textDim, Align::Center);
            c.Text(std::to_wstring(d.wDay), Rect(dx, mid - 2, cw, 16), Weight::Semi, 11.5f, today ? t.onAccent : t.text, Align::Center);
        }
    } else if (key == "clock.wx") {
        float bw = (w - 2 * kPad - 8) / 3, half = (std::min)(rh / 2 - 5, 16.0f);
        std::wstring parts[3] = {ClockText(st, h.h24), DateText(st, L"d MMM"), h.wxValid ? Deg(h.tempC, h.fahrenheit) : L"--"};
        Color inks[3] = {t.accent, t.text.Alpha(0.12f), t.text.Alpha(0.06f)};
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F b = Rect(r.left + kPad + i * (bw + 4), mid - half, bw, 2 * half);
            c.Fill(b, 9, inks[i]);
            c.Text(parts[i], b, Weight::Semi, 11.5f, i == 0 ? t.onAccent : t.text, Align::Center);
        }
    } else if (key == "clock.flip") {
        if (tiny) return twoLines();
        wchar_t a[4], b[4];
        swprintf_s(a, L"%02d", h.h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12)), swprintf_s(b, L"%02d", st.wMinute);
        float ph = (std::min)(rh - 10, w * 0.42f), pw = ph * 1.05f;
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F p = Rect(cx - pw - 2 + i * (pw + 4), mid - ph / 2, pw, ph);
            c.FillV(p, ph * 0.22f, t.text.Alpha(0.12f), t.text.Alpha(0.06f));
            c.Line({p.left + 2, mid}, {p.right - 2, mid}, t.bg.Alpha(0.6f), 1, false);
            c.Text(i ? b : a, p, Weight::Semi, ph * 0.56f, t.text, Align::Center);
        }
    }
}

void DrawWeather(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const Host& h, const SYSTEMTIME& st) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    if (!h.wxValid && key != "wx.sun") return Stat(c, t, r, m.glyph, m.name, L"--", tiny ? L"" : L"Finding the forecast");
    if (key == "wx.status") return Stat(c, t, r, WxIcon(h.wxCode, h.wxDay), m.name, WxText(h.wxCode), tiny ? L"" : h.place);
    if (key == "wx.details") {
        feeds::Series s = feeds::Get("sky");
        wchar_t a[48], b[64];
        swprintf_s(a, L"Wind %.0f km/h", s.value);
        swprintf_s(b, L"Rain %.1f mm  ·  %ls–%ls", s.extra, Deg(h.lowC, h.fahrenheit).c_str(), Deg(h.highC, h.fahrenheit).c_str());
        return Stat(c, t, r, WxIcon(h.wxCode, h.wxDay), m.name, s.ok ? std::wstring(a) : Deg(h.tempC, h.fahrenheit), tiny ? L"" : s.ok ? std::wstring(b) : NotYet(s));
    }
    if (key == "wx.sun") {
        feeds::Series s = feeds::Get("sky");
        if (!s.ok || s.points.size() < 2 || s.points[0] < 0) return Stat(c, t, r, glyph::ClearDay, m.name, L"--", tiny ? L"" : NotYet(s));
        auto hm = [](double mins) {
            wchar_t b[12];
            swprintf_s(b, L"%02d:%02d", (int)mins / 60, (int)mins % 60);
            return std::wstring(b);
        };
        double nowMin = st.wHour * 60.0 + st.wMinute, k = (nowMin - s.points[0]) / (std::max)(1.0, s.points[1] - s.points[0]);
        if (!tall) {
            float lx = tiny ? r.left + 4 : r.left + kPad, lw = tiny ? w - 8 : w - 2 * kPad;
            Lane(c, t, Rect(lx, mid + (tiny ? -2 : 2), lw, 4), k, t.accentHi);
            if (!tiny) {
                c.Text(hm(s.points[0]), Rect(lx, mid - 14, lw, 13), Weight::Medium, 10, t.textDim);
                c.Text(hm(s.points[1]), Rect(lx, mid - 14, lw, 13), Weight::Medium, 10, t.textDim, Align::Right);
            }
            return;
        }
        float rad = (std::min)(w * 0.36f, rh * 0.5f);
        D2D1_POINT_2F o = {cx, r.top + rh * 0.68f};
        c.Arc(o, rad, -kPi, kPi, t.text.Alpha(0.16f), 2);
        float ang = -kPi + kPi * (float)(std::clamp)(k, 0.0, 1.0);
        c.Dot({o.x + std::cos(ang) * rad, o.y + std::sin(ang) * rad}, 5, t.accentHi);
        c.Text(hm(s.points[0]), Rect(r.left + 12, r.bottom - 26, 70, 16), Weight::Medium, 11, t.textDim);
        c.Text(hm(s.points[1]), Rect(r.right - 82, r.bottom - 26, 70, 16), Weight::Medium, 11, t.textDim, Align::Right);
        return;
    }
    bool hours = key == "wx.hourly", withToday = key == "wx.todayfc"; // rows of hours or days
    int n = hours ? (std::min)(h.nHours, 6) : (std::min)(h.nDays, withToday ? 3 : 4);
    int cells = n + (withToday ? 1 : 0);
    if (cells <= 0) return;
    float cw = (w - 2 * kPad) / cells;
    for (int i = 0; i < cells; ++i) {
        float dx = r.left + kPad + cw * (i + 0.5f);
        std::wstring top, val;
        int icon;
        if (withToday && i == 0) {
            top = L"Now", val = Deg(h.tempC, h.fahrenheit), icon = WxIcon(h.wxCode, h.wxDay);
        } else if (hours) {
            wchar_t lab[8];
            swprintf_s(lab, L"%02d", h.hours[i].hour);
            top = lab, val = Deg(h.hours[i].tempC, h.fahrenheit), icon = WxIcon(h.hours[i].code, h.hours[i].day);
        } else {
            const Host::Day& d = h.days[i - (withToday ? 1 : 0)];
            SYSTEMTIME ds{(WORD)d.y, (WORD)d.m, 0, (WORD)d.d};
            top = DateText(ds, L"ddd"), val = Deg(d.hiC, h.fahrenheit), icon = WxIcon(d.code, true);
        }
        if (tall) {
            c.Text(top, Rect(dx - cw / 2, mid - 34, cw, 14), Weight::Medium, 10.5f, t.textDim, Align::Center);
            c.Icon(icon, {dx, mid}, 22, t.accentHi, true);
            c.Text(val, Rect(dx - cw / 2, mid + 18, cw, 16), Weight::Semi, 12, t.text, Align::Center);
        } else {
            c.Text(top, Rect(dx - cw / 2, mid - 17, cw, 11), Weight::Medium, 8.5f, t.textDim, Align::Center);
            c.Icon(icon, {dx, mid - 1}, 13, t.accentHi, true);
            c.Text(val, Rect(dx - cw / 2, mid + 6, cw, 12), Weight::Semi, 9.5f, t.text, Align::Center);
        }
    }
}

void DrawWork(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const SYSTEMTIME& st) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    if (key == "time.timeline") {
        feeds::Screen s = feeds::ScreenTime();
        feeds::Series sky = feeds::Get("sky");
        D2D1_RECT_F lane = Rect(r.left + kPad, mid - 3, w - 2 * kPad, 8);
        c.Fill(lane, 4, t.text.Alpha(0.08f));
        float hw = Width(lane) / 24;
        for (int i = 0; i < 24; ++i)
            if (s.hours[i] > 0) c.Fill(Rect(lane.left + hw * i, lane.top, hw, 8), 2, t.accent.Alpha(0.25f + 0.75f * (std::min)(1.0f, s.hours[i] / 3600.0f)));
        float nowX = lane.left + Width(lane) * (st.wHour * 60 + st.wMinute) / 1440;
        c.Line({nowX, lane.top - 4}, {nowX, lane.bottom + 4}, t.text, 1.2f);
        if (sky.ok && sky.points.size() >= 2 && sky.points[0] >= 0)
            for (double mins : sky.points) c.Dot({lane.left + Width(lane) * (float)(mins / 1440), lane.top - 5}, 2, Color::Rgb(0xFACC15));
        c.Text(Hours(s.today) + L" today", Rect(lane.left, r.top + 2, Width(lane), 13), Weight::Semi, 10, t.textDim);
    } else if (key == "time.screen") {
        feeds::Screen s = feeds::ScreenTime();
        std::wstring top = s.apps.empty() ? L"Nothing yet today" : s.apps[0].app + (tall ? L"  " + Hours(s.apps[0].seconds) : L"");
        std::vector<double> pts(s.hours, s.hours + 24);
        Stat(c, t, r, m.glyph, m.name, Hours(s.today), tiny ? L"" : top, tall ? &pts : nullptr);
    } else if (key == "time.timer") {
        double left = state::TimerLeft();
        bool on = state::TimerRunning();
        wchar_t b[16];
        swprintf_s(b, L"%d:%02d", (int)left / 60, (int)left % 60);
        std::wstring sub = on ? L"Click to pause" : left > 0 && left < state::TimerLength() ? L"Paused" : L"Click to start";
        Stat(c, t, r, m.glyph, m.name, b, tiny ? L"" : sub, nullptr, on ? t.accentHi : t.textDim);
    } else if (key == "time.stopwatch") {
        double v = state::WatchSeconds();
        wchar_t b[20];
        swprintf_s(b, L"%d:%02d", (int)v / 60, (int)v % 60);
        std::wstring sub = state::WatchRunning() ? L"Running" : v > 0 ? L"Paused" : L"Click to start";
        Stat(c, t, r, m.glyph, m.name, b, tiny ? L"" : sub, nullptr, state::WatchRunning() ? t.accentHi : t.textDim);
    } else if (key == "money.currency") {
        feeds::Series s = feeds::Get("rates");
        wchar_t b[32];
        swprintf_s(b, L"%.4g", s.value);
        Stat(c, t, r, m.glyph, m.name, s.ok ? std::wstring(b) : L"--", tiny ? L"" : s.ok ? s.text : NotYet(s));
    } else if (key == "prog.day" || key == "prog.month") {
        double v = Frac(key == "prog.day" ? 0 : 1);
        RingFace(c, t, r, m.name, v, std::to_wstring((int)(v * 100)), L"");
    } else if (key == "prog.year") {
        double v = Frac(2);
        int cols = (std::max)(12, (int)(w / 6)), rows = (std::clamp)((int)(rh / 6) - 1, 3, 26);
        float sx = (w - 2 * kPad) / cols, sy = (rh - 8) / rows;
        int lit = (int)(v * cols * rows);
        for (int i = 0; i < cols * rows; ++i) c.Dot({r.left + kPad + sx * (i % cols + 0.5f), r.top + 4 + sy * (i / cols + 0.5f)}, (std::min)(sx, sy) * 0.3f, i < lit ? t.accentHi : t.text.Alpha(0.1f));
    } else if (key == "list.water") {
        int have = state::Water(), goal = state::WaterGoal();
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(have) + L"/" + std::to_wstring(goal), tiny ? L"" : L"Glasses today", nullptr, have >= goal ? t.accentHi : t.textDim);
    } else if (key == "list.notes") {
        std::wstring note = feeds::Setting("notes");
        std::wstring first = note.substr(0, note.find(L'\n'));
        Stat(c, t, r, m.glyph, m.name, first.empty() ? L"Notes" : first, tiny || !first.empty() ? L"" : L"Click to write", nullptr, t.textDim);
    } else if (key == "list.habits") {
        auto hb = state::Habits();
        int today = state::Today(), done = 0;
        for (const auto& x : hb) done += x.last == today;
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(done) + L" of " + std::to_wstring(hb.size()), tiny ? L"" : L"Today", nullptr, done && done == (int)hb.size() ? t.accentHi : t.textDim);
    }
}

void DrawSystem(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const Host& h) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    if (key == "sys.cpu") {
        RingFace(c, t, r, L"Processor", h.cpu, std::to_wstring((int)std::lround(h.cpu * 100)), L"");
    } else if (key == "sys.ram") {
        wchar_t b[32] = {};
        if (h.memTotalGb > 0) swprintf_s(b, L"%.1f of %.0f GB", h.memUsedGb, h.memTotalGb);
        RingFace(c, t, r, L"Memory", h.mem, std::to_wstring((int)std::lround(h.mem * 100)), b);
    } else if (key == "sys.temp") {
        feeds::Series s = feeds::Get("thermal");
        wchar_t b[48];
        swprintf_s(b, L"CPU %d%%  ·  RAM %d%%", (int)std::lround(h.cpu * 100), (int)std::lround(h.mem * 100));
        std::wstring why = s.error.empty() ? L"Reading…" : s.error;
        Stat(c, t, r, m.glyph, m.name, s.ok ? std::to_wstring((int)std::lround(s.value)) + L"°" : L"--", tiny ? L"" : s.ok ? std::wstring(b) : why, nullptr, s.ok && s.value > 85 ? Color::Rgb(kDown) : t.accentHi);
    } else if (key == "sys.lang") {
        Stat(c, t, r, m.glyph, m.name, state::Language(), tiny ? L"" : L"Click to switch");
    } else if (key == "sys.bright") {
        if (h.bright < 0) return Stat(c, t, r, m.glyph, m.name, L"--", tiny ? L"" : L"Not on this display");
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(h.bright) + L"%", tiny ? L"" : L"Click for the slider");
    } else if (key == "sys.theme") {
        Stat(c, t, r, m.glyph, m.name, state::LightTheme() ? L"Light" : L"Dark", tiny ? L"" : L"Click to switch");
    } else if (key == "sys.tray") {
        c.Icon(m.glyph, {cx, mid}, 19, t.textDim);
    } else if (key == "sys.storage" || key == "sys.drives") {
        ULARGE_INTEGER freeBytes{}, total{};
        GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &total, nullptr);
        double used = total.QuadPart ? 1 - (double)freeBytes.QuadPart / (double)total.QuadPart : 0;
        wchar_t b[32];
        swprintf_s(b, L"%.0f GB free", freeBytes.QuadPart / 1073741824.0);
        if (tiny) return RingFace(c, t, r, m.name, used, std::to_wstring((int)(used * 100)), L"");
        Stat(c, t, r, m.glyph, m.name, b, L"");
        Color ink = used > 0.9 ? Color::Rgb(kDown) : t.accentHi;
        if (tall) Lane(c, t, Rect(r.left + 16, r.bottom - 22, w - 32, 6), used, ink);
        else Lane(c, t, Rect(r.left + kPad + 26, r.bottom - 10, w - 2 * kPad - 26, 3), used, ink);
        (void)rh;
    }
}

void DrawMediaFace(Canvas& c, const Theme& t, const std::string& key, const D2D1_RECT_F& r, const Host& h) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    std::wstring title = h.hasTrack ? h.title : L"Nothing playing";
    if (key == "media.mini") {
        float rad = (std::min)(w, rh) / 2 - 4;
        c.Ring({cx, mid}, rad, t.text.Alpha(0.14f), 2.5f);
        if (h.hasTrack) c.Arc({cx, mid}, rad, -kPi / 2, 2 * kPi * (float)h.progress, t.accentHi, 2.5f);
        c.Icon(h.playing ? glyph::Pause : glyph::Play, {cx, mid}, rad * 1.1f, t.text, true);
    } else if (key == "media.compact") {
        c.Icon(h.playing ? glyph::Pause : glyph::Play, {r.left + kPad + 11, mid}, 22, h.hasTrack ? t.accentHi : t.textLow, true);
        float tx = r.left + kPad + 28, tw = r.right - kPad - tx;
        c.Text(title, Rect(tx, mid - 15, tw, 16), Weight::Semi, 12, t.text);
        if (h.hasTrack) c.Text(h.artist, Rect(tx, mid + 1, tw, 14), Weight::Regular, 10.5f, t.textDim);
    } else {
        float tx = r.left + kPad, tw = w - 2 * kPad - 26;
        c.Text(title, Rect(tx, mid - 16, tw, 16), Weight::Semi, 12, t.text);
        Lane(c, t, Rect(tx, mid + 6, tw, 4), h.hasTrack ? h.progress : 0, t.accentHi);
        c.Icon(glyph::Next, {r.right - kPad - 10, mid}, 20, h.hasTrack ? t.text : t.textLow, true);
    }
}

void DrawOnline(Canvas& c, const Theme& t, int kind, const std::string& key, const Meta& m, const D2D1_RECT_F& r) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    feeds::Series s = m.feed ? feeds::Get(m.feed) : feeds::Series{};
    if (key == "money.metric") {
        wchar_t b[32];
        swprintf_s(b, s.value >= 100 ? L"%.0f" : L"%.2f", s.value);
        Stat(c, t, r, m.glyph, s.ok ? s.text : m.name, s.ok ? std::wstring(b) : L"--", tiny ? L"" : s.ok ? s.text : NotYet(s));
    } else if (key == "stock.one" || key == "stock.chart") {
        std::wstring tick = Ticker("stock.id", L"AAPL");
        feeds::Series q = feeds::Get(QuoteId(tick));
        wchar_t b[32];
        swprintf_s(b, L"%.2f", q.value);
        if (!q.ok) return Stat(c, t, r, m.glyph, tick, tick, tiny ? L"" : NotYet(q));
        Stat(c, t, r, q.change >= 0 ? glyph::Up : glyph::Down, tick, tick + L"  " + b, tiny ? L"" : Pct(q.change), key == "stock.chart" ? &q.points : nullptr, Trend(t, q.change));
    } else if (key == "stock.watch") {
        auto list = Watch();
        float cw = (w - 2 * kPad) / (float)(std::max)((size_t)1, list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            feeds::Series q = feeds::Get(QuoteId(list[i]));
            float x = r.left + kPad + cw * i;
            wchar_t b[32];
            swprintf_s(b, L"%.2f", q.value);
            c.Text(list[i], Rect(x, mid - 17, cw, 12), Weight::Semi, 10, t.text);
            c.Text(q.ok ? std::wstring(b) : L"--", Rect(x, mid - 5, cw, 12), Weight::Regular, 10, t.textDim);
            if (q.ok) c.Text(Pct(q.change), Rect(x, mid + 6, cw, 12), Weight::Medium, 9.5f, Trend(t, q.change));
        }
    } else if (key == "web.plausible" || key == "web.datafast") {
        VisitFace(c, t, kind, r, L"visitors");
    } else if (key == "web.dub") {
        VisitFace(c, t, kind, r, L"clicks");
    } else if (key == "web.resend") {
        if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", tiny ? L"" : NotYet(s));
        Stat(c, t, r, m.glyph, m.name, Short(s.week) + L" sent", tiny ? L"" : L"This week");
    } else if (key == "web.linear") {
        CountFace(c, t, kind, r, tiny ? L"" : Short(s.extra) + L" started");
    } else if (key == "web.supabase") {
        CountFace(c, t, kind, r, tiny ? L"" : s.text);
    } else if (key == "web.gmail") {
        CountFace(c, t, kind, r, tiny ? L"" : L"Unread");
    } else if (key == "web.github") {
        CountFace(c, t, kind, r, tiny ? L"" : L"Notifications");
    } else if (key == "web.githubover") {
        CountFace(c, t, kind, r, Short(s.extra) + L" PRs  ·  " + Short(s.extra2) + L" stars");
    } else if (key == "web.githubgrid") {
        if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", tiny ? L"" : NotYet(s));
        int rows = 7, cols = (std::max)(1, (int)((w - 2 * kPad) / (tall ? 8.0f : 4.6f)));
        float cell = (std::min)((w - 2 * kPad) / cols, (rh - 8) / rows);
        size_t show = (std::min)((size_t)(cols * rows), s.points.size()), from = s.points.size() - show;
        double top = 1;
        for (size_t i = from; i < s.points.size(); ++i) top = (std::max)(top, s.points[i]);
        for (size_t i = 0; i < show; ++i) {
            double v = s.points[from + i];
            c.Fill(Rect(r.left + kPad + cell * (float)(i / rows), r.top + 4 + cell * (float)(i % rows), cell - 1, cell - 1), 1, v > 0 ? t.accent.Alpha(0.3f + 0.7f * (float)(v / top)) : t.text.Alpha(0.07f));
        }
    } else if (key == "web.leetcode") {
        CountFace(c, t, kind, r, tiny ? L"" : L"Solved  ·  " + Short(s.extra) + L" day streak");
    } else { // the follower counts
        CountFace(c, t, kind, r, tiny ? L"" : s.text);
    }
}

}

int Count() { return kKinds; }
const Meta& Info(int kind) { return kAll[(std::clamp)(kind, 0, kKinds - 1)]; }

int Find(const char* key) {
    for (int i = 0; i < kKinds; ++i)
        if (!strcmp(kAll[i].key, key)) return i;
    return -1;
}

const wchar_t* CatName(int cat) {
    static const wchar_t* const kNames[kCats] = {L"Clocks", L"Weather", L"Productivity", L"System", L"Media", L"Money", L"Online", L"Apps & Files"};
    return kNames[(std::clamp)(cat, 0, kCats - 1)];
}

int WxIcon(int code, bool day) {
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
    return L"Showers";
}

std::wstring Hours(int seconds) {
    int m = seconds / 60;
    wchar_t b[24];
    if (m >= 60) swprintf_s(b, L"%dh %dm", m / 60, m % 60);
    else swprintf_s(b, L"%dm", m);
    return b;
}

void Draw(Canvas& c, const Theme& t, int kind, int, const D2D1_RECT_F& r, const Host& h) { // the rectangle says how much room there is
    if (kind < 0 || kind >= kKinds) return;
    const Meta& m = kAll[kind];
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    bool tiny = w < kTinyW, tall = rh >= kTallH;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string key = m.key;
    if (key.rfind("clock.", 0) == 0) return DrawClock(c, t, key, m, r, h, st);
    if (key.rfind("wx.", 0) == 0) return DrawWeather(c, t, key, m, r, h, st);
    if (key.rfind("time.", 0) == 0 || key.rfind("prog.", 0) == 0 || key.rfind("list.", 0) == 0 || key == "money.currency") return DrawWork(c, t, key, m, r, st);
    if (key.rfind("sys.", 0) == 0) return DrawSystem(c, t, key, m, r, h);
    if (key.rfind("media.", 0) == 0) return DrawMediaFace(c, t, key, r, h);
    if (key.rfind("money.", 0) == 0 && key != "money.metric") return MoneyFace(c, t, kind, r);
    if (key == "money.metric" || key.rfind("stock.", 0) == 0 || key.rfind("web.", 0) == 0 || key.rfind("social.", 0) == 0) return DrawOnline(c, t, kind, key, m, r);
    if (key == "files.websearch") {
        D2D1_RECT_F f = Inset(r, 5, (std::max)(5.0f, rh / 2 - 16));
        if (tiny) return c.Icon(m.glyph, {cx, mid}, 19, t.textDim);
        c.Fill(f, Height(f) / 2, t.text.Alpha(0.07f));
        c.Icon(m.glyph, {f.left + 15, mid}, 15, t.textDim);
        c.Text(L"Search the web", Rect(f.left + 28, f.top, Width(f) - 32, Height(f)), Weight::Regular, 12, t.textLow);
        return;
    }
    if (key == "files.photo") return state::DrawPhoto(c, t, r);
    if (tiny) return c.Icon(m.glyph, {cx, mid}, 19, t.textDim);
    Stat(c, t, r, m.glyph, m.name, m.name, tall ? m.about : L"", nullptr, t.textDim);
}

std::vector<std::string> Feeds(const std::vector<int>& kinds) {
    std::vector<std::string> out;
    auto add = [&](const std::string& id) {
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (int k : kinds) {
        if (k < 0 || k >= kKinds) continue;
        const Meta& m = kAll[k];
        std::string key = m.key;
        if (m.feed) add(m.feed);
        if (key == "time.timeline") add("sky");
        if (key == "money.total")
            for (const char* id : {"stripe", "polar", "dodo", "shopify", "lemon", "paddle", "gumroad"}) {
                std::string base = std::string(id) + ".key";
                if (!feeds::Setting(base.c_str()).empty()) add(id); // only the stores connected
            }
        if (key == "stock.one" || key == "stock.chart") add(QuoteId(Ticker("stock.id", L"AAPL")));
        if (key == "stock.watch")
            for (const auto& one : Watch()) add(QuoteId(one));
    }
    return out;
}

int NextMs(const std::vector<int>& kinds, const Host& h) {
    int best = 0;
    auto soon = [&](int ms) { best = best ? (std::min)(best, ms) : ms; };
    SYSTEMTIME st;
    GetLocalTime(&st);
    int toMinute = (60 - st.wSecond) * 1000 - st.wMilliseconds + 20;
    for (int k : kinds) {
        if (k < 0 || k >= kKinds) continue;
        std::string key = kAll[k].key;
        if (key.rfind("clock.", 0) == 0 || key.rfind("prog.", 0) == 0) soon(toMinute);
        if (key == "clock.digital" && h.cards) soon(1000 - st.wMilliseconds + 20);
        if (key == "time.stopwatch" && state::WatchRunning()) soon(1000);
        if (key == "time.timer" && state::TimerRunning()) soon(1000);
        if ((key == "media.mini" || key == "media.bar") && h.playing) soon(5000);
    }
    return best;
}

}
