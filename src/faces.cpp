// The widget catalogue, and how each one draws itself in whatever rectangle it's given.
#include "faces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include "faces_state.h"
#include "feeds.h"
#include "glyphs.h"
#include "sports.h"

namespace faces {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kPad = 14; // a tile's margin
constexpr float kClockAspect = 2.7f; // four seven-segment digits and a colon, as wide as they are tall
constexpr float kBigH = 180; // this tall: a large tile, with its own layout
constexpr float kWideAt = 1.9f; // this much wider than tall: the words and the picture sit side by side
constexpr float kHeadH = 16, kHeadIcon = 13; // the small line a tile opens with
constexpr float kSparkEnd = 4; // room for the dot at a line's end
constexpr float kBadge = 34; // the soft square behind a launcher's icon
constexpr float kThreeFrom = 240; // narrower than this: two clock faces, not three
constexpr float kCellLeast = 38; // a forecast's narrowest column
constexpr float kWatchRoomy = 190; // this much room: a watchlist's prices fit beside its changes
constexpr uint32_t kUp = 0x4ADE80, kDown = 0xFF6B6B, kUpPale = 0x15803D, kDownPale = 0xDC2626; // the pale ones for a light tile

Color Good(const Theme& t) { return Color::Rgb(theme::Luma(t.text) < 0.4f ? kUpPale : kUp); }
Color Bad(const Theme& t) { return Color::Rgb(theme::Luma(t.text) < 0.4f ? kDownPale : kDown); }

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
    {"stock.us", L"US Markets", L"The S&P 500, Dow, Nasdaq, Russell 2000 and VIX, each with its day, and whether New York is open", kMoney, glyph::Up, 6, 2, 196, 280, nullptr, false},
    {"stock.amex", L"NYSE American", L"The NYSE American Composite and its day", kMoney, glyph::Up, 7, 1, 176, 256, nullptr, false},
    // sports
    {"sport.scores", L"Scores", L"Live scores from up to three leagues, else their latest results", kSports, glyph::Dashboard, 6, 1, 196, 280, nullptr, false},
    {"sport.fixtures", L"Fixtures", L"Upcoming games by day, in your time", kSports, glyph::Calendar, 6, 2, 196, 280, nullptr, false},
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

bool gSampling = false; // a preview in the gallery: what it would look like, with nothing fetched

feeds::Series Sample(const std::string& id) {
    feeds::Series s;
    constexpr int kPoints = 30;
    s.ok = true, s.value = 1284, s.total = 48210, s.week = 12840, s.change = 0.124, s.extra = 7, s.extra2 = 1250, s.text = L"Followers";
    for (int i = 0; i < kPoints; ++i) s.points.push_back(40 + 25 * std::sin(i * 0.55) + i * 1.6);
    if (id.rfind("quote:", 0) == 0) s.value = 212.48, s.change = 0.0087;
    if (id == "sky") s.points = {360, 1080}, s.value = 12, s.extra = 0.4;
    if (id == "thermal") s.value = 62;
    if (id == "rates") s.value = 0.92, s.text = L"1 USD in EUR";
    if (id == "metric") s.value = 68120, s.text = L"BTC-USD";
    if (id == "supabase") s.text = L"Healthy";
    if (id == "github.activity") {
        s.points.clear();
        for (int i = 0; i < 200; ++i) s.points.push_back((i * 7) % 5 == 0 ? 0 : (i * 13) % 9);
    }
    if (id.rfind("sport:", 0) == 0) { // made-up games: one on, one done, one next
        s.rows = {sports::Encode({sports::kLive, L"Arsenal", L"2", L"Chelsea", L"1", L"67'", 0, false}),
                  sports::Encode({sports::kFinal, L"Lakers", L"112", L"Celtics", L"108", L"Final", 0, false}),
                  sports::Encode({sports::kPre, L"Barcelona", L"", L"Real Madrid", L"", L"Sat 20:00", 0, false})};
        s.value = 1;
    }
    if (id.rfind("next:", 0) == 0) // made-up fixtures, two days of them
        s.rows = {sports::Encode({sports::kPre, L"Arsenal", L"", L"Chelsea", L"", L"Sat 15:00", 1, false}), sports::Encode({sports::kPre, L"Liverpool", L"", L"Spurs", L"", L"Sat 17:30", 2, false}),
                  sports::Encode({sports::kPre, L"Lakers", L"", L"Celtics", L"", L"Sun 01:30", 3, false})};
    return s;
}

feeds::Series Feed(const std::string& id) { return gSampling ? Sample(id) : feeds::Get(id); }

bool IsBig(float h) { return h >= kBigH; }
bool IsWide(float w, float h) { return !IsBig(h) && w >= kWideAt * h; }

float Fit(Canvas& c, const std::wstring& s, Weight weight, float most, float least, float room) { // as large as it can be in the room: whole sizes, so few cached fonts
    float at = c.Measure(s, weight, most);
    return at <= room || at <= 0 ? most : (std::max)(least, std::floor(most * room / at));
}

void FitText(Canvas& c, const std::wstring& s, const D2D1_RECT_F& r, Weight weight, float most, float least, Color ink, Align a = Align::Left) {
    c.Text(s, r, weight, Fit(c, s, weight, most, least, Width(r)), ink, a);
}

void Head(Canvas& c, const Theme& t, const D2D1_RECT_F& r, int g, const std::wstring& name, Color lit, float rightRoom = 0) { // the small line every tile opens with
    float x = r.left + kPad, y = r.top + kPad;
    c.Icon(g, {x + kHeadIcon / 2, y + kHeadH / 2}, kHeadIcon, lit, true);
    c.Text(name, Rect(x + kHeadIcon + 6, y, Width(r) - 2 * kPad - kHeadIcon - 6 - rightRoom, kHeadH), Weight::Medium, 11.5f, t.textDim);
}

void Spark(Canvas& c, const D2D1_RECT_F& r, const std::vector<double>& pts, Color ink) { // a line with its area fading out under it
    if (pts.size() < 2 || Width(r) < 8 || Height(r) < 8) return;
    double lo = pts[0], hi = pts[0];
    for (double p : pts) lo = (std::min)(lo, p), hi = (std::max)(hi, p);
    if (hi - lo < 1e-9) hi = lo + 1;
    float step = Width(r) / (float)(pts.size() - 1), rise = Height(r) - kSparkEnd;
    auto at = [&](size_t i) { return D2D1_POINT_2F{r.left + step * i, r.bottom - rise * (float)((pts[i] - lo) / (hi - lo))}; };
    ComPtr<ID2D1PathGeometry> line, area;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&line)) || FAILED(gfx::D2D()->CreatePathGeometry(&area))) return;
    if (SUCCEEDED(area->Open(&sink))) {
        sink->BeginFigure({r.left, r.bottom}, D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 0; i < pts.size(); ++i) sink->AddLine(at(i));
        sink->AddLine({r.right, r.bottom});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    sink.Reset();
    if (SUCCEEDED(line->Open(&sink))) {
        sink->BeginFigure(at(0), D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(at(i));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
    }
    c.FillGeoV(area.Get(), r, ink.Alpha(0.26f), ink.Alpha(0));
    c.StrokeGeo(line.Get(), ink, 1.7f);
    c.Dot(at(pts.size() - 1), 2.8f, ink);
}

void Lane(Canvas& c, const Theme& t, const D2D1_RECT_F& r, double v, Color ink) {
    c.Fill(r, Height(r) / 2, t.text.Alpha(0.12f));
    c.Fill(Rect(r.left, r.top, (std::max)(Height(r), Width(r) * (float)(std::clamp)(v, 0.0, 1.0)), Height(r)), Height(r) / 2, ink);
}

struct StatOpts {
    const std::vector<double>* pts = nullptr; // a line through its history
    Color ink{}; // the icon and the line; none: the accent
    Color subInk{}; // the sub line; none: dim
    double bar = -1; // how full, 0..1; below zero: no bar
};

bool Set(Color c) { return c.r + c.g + c.b > 0; }

void Stat(Canvas& c, const Theme& t, const D2D1_RECT_F& r, int g, const std::wstring& name, const std::wstring& main, const std::wstring& sub, const StatOpts& o = {}) { // the common tile: a small head, the number as large as it fits, a line under it
    float w = Width(r), h = Height(r), x = r.left + kPad, room = w - 2 * kPad, bottom = r.bottom - kPad;
    Color lit = Set(o.ink) ? o.ink : t.accentHi, soft = Set(o.subInk) ? o.subInk : t.textDim;
    if (IsBig(h)) {
        Head(c, t, r, g, name, lit);
        if (o.pts) {
            float y = r.top + kPad + kHeadH + 8;
            FitText(c, main, Rect(x, y, room, 58), Weight::Light, 46, 20, t.text);
            c.Text(sub, Rect(x, y + 58, room, 20), Weight::Regular, 13, soft);
            Spark(c, {x, y + 92, r.right - kPad, bottom}, *o.pts, lit);
            return;
        }
        float by = bottom; // nothing to fill the middle: it all sits at the foot
        if (o.bar >= 0) {
            Lane(c, t, Rect(x, by - 8, room, 8), o.bar, lit);
            by -= 16;
        }
        if (!sub.empty()) {
            c.Text(sub, Rect(x, by - 20, room, 20), Weight::Regular, 13, soft);
            by -= 22;
        }
        FitText(c, main, Rect(x, by - 62, room, 62), Weight::Light, 56, 20, t.text);
        return;
    }
    if (o.pts && IsWide(w, h)) { // the words on one side, the line on the other
        float words = room * 0.46f;
        Head(c, t, Rect(r.left, r.top, words + 2 * kPad, h), g, name, lit);
        c.Text(sub, Rect(x, bottom - 16, words, 16), Weight::Regular, 11.5f, soft);
        FitText(c, main, Rect(x, bottom - 56, words, 40), Weight::Light, 32, 15, t.text);
        Spark(c, {x + words + 14, r.top + kPad + 2, r.right - kPad, bottom}, *o.pts, lit);
        return;
    }
    if (o.pts) { // small: what the sub line says moves up beside the head, the line takes the bottom
        float side = sub.empty() ? 0 : c.Measure(sub, Weight::Medium, 11) + 6;
        Head(c, t, r, g, name, lit, side);
        c.Text(sub, Rect(x, r.top + kPad, room, kHeadH), Weight::Medium, 11, soft, Align::Right);
        float y = r.top + kPad + kHeadH + 2;
        FitText(c, main, Rect(x, y, room, 34), Weight::Light, 28, 13, t.text);
        Spark(c, {x, y + 36, r.right - kPad, bottom}, *o.pts, lit);
        return;
    }
    Head(c, t, r, g, name, lit);
    float by = bottom;
    if (o.bar >= 0) {
        Lane(c, t, Rect(x, by - 6, room, 6), o.bar, lit);
        by -= 12;
    }
    if (!sub.empty()) {
        c.Text(sub, Rect(x, by - 16, room, 16), Weight::Regular, 11.5f, soft);
        by -= 17;
    }
    FitText(c, main, Rect(x, by - 40, room, 40), Weight::Light, IsWide(w, h) ? 34.0f : 30.0f, 13, t.text);
}

void Rows(Canvas& c, const Theme& t, const D2D1_RECT_F& area, const std::vector<std::pair<std::wstring, std::wstring>>& rows, float size) { // label on the left, value on the right, evenly down the room
    if (rows.empty()) return;
    float rh = Height(area) / rows.size();
    for (size_t i = 0; i < rows.size(); ++i) {
        float y = area.top + rh * i, vw = c.Measure(rows[i].second, Weight::Medium, size) + 4;
        c.Text(rows[i].first, Rect(area.left, y, Width(area) - vw - 6, rh), Weight::Regular, size, t.textDim);
        c.Text(rows[i].second, Rect(area.left, y, Width(area), rh), Weight::Medium, size, t.text, Align::Right);
    }
}

void Launch(Canvas& c, const Theme& t, const D2D1_RECT_F& r, int g, const std::wstring& name, const std::wstring& about) { // a tile that opens something: a soft badge, its name, what it does
    float big = IsBig(Height(r)) ? 1.4f : 1.0f, s = kBadge * big, x = r.left + kPad, room = Width(r) - 2 * kPad, bottom = r.bottom - kPad;
    D2D1_RECT_F badge = Rect(x, r.top + kPad, s, s);
    c.Fill(badge, s * 0.3f, t.accent.Alpha(0.16f));
    c.Icon(g, {x + s / 2, r.top + kPad + s / 2}, s * 0.58f, t.accentHi, true);
    c.Text(about, Rect(x, bottom - 15 * big, room, 15 * big), Weight::Regular, 11 * big, t.textDim);
    FitText(c, name, Rect(x, bottom - 15 * big - 20 * big, room, 20 * big), Weight::Semi, 13.5f * big, 11, t.text);
}

void RingFace(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const std::wstring& name, double v, const std::wstring& inside, const std::wstring& sub) {
    float w = Width(r), h = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
    Color ink = v > 0.85 ? Bad(t) : t.accentHi;
    auto ring = [&](D2D1_POINT_2F o, float rad, float thick, float type) {
        c.Ring(o, rad, t.text.Alpha(0.12f), thick);
        c.Arc(o, rad, -kPi / 2, 2 * kPi * (float)(std::clamp)(v, 0.0, 1.0), ink, thick);
        FitText(c, inside, Rect(o.x - rad + thick, o.y - rad / 2, 2 * (rad - thick), rad), Weight::Medium, type, 10, t.text, Align::Center);
    };
    if (IsWide(w, h)) { // the ring on the left, its words beside it
        float rad = h / 2 - 15, tx = r.left + kPad + 2 * rad + 16;
        ring({r.left + kPad + rad + 1, mid}, rad, 7, 20);
        c.Text(name, Rect(tx, sub.empty() ? r.top : mid - 20, r.right - kPad - tx, sub.empty() ? h : 20), Weight::Semi, 14, t.text);
        if (!sub.empty()) c.Text(sub, Rect(tx, mid + 1, r.right - kPad - tx, 18), Weight::Regular, 12, t.textDim);
        return;
    }
    if (IsBig(h)) {
        float rad = (std::min)(w, h) * 0.3f;
        ring({cx, r.top + kPad + rad + 8}, rad, 11, 34);
        float y = r.top + kPad + 2 * rad + 26;
        c.Text(name, Rect(r.left, y, w, 22), Weight::Semi, 15, t.text, Align::Center);
        if (!sub.empty()) c.Text(sub, Rect(r.left, y + 22, w, 18), Weight::Regular, 12.5f, t.textDim, Align::Center);
        return;
    }
    float rad = (h - 46) / 2;
    ring({cx, r.top + 12 + rad}, rad, 7, 18);
    c.Text(name, Rect(r.left + 6, r.bottom - 28, w - 12, 18), Weight::Medium, 11.5f, t.textDim, Align::Center);
}

void Analog(Canvas& c, const Theme& t, D2D1_POINT_2F o, float rad, int style, int hh, int mm) { // 0 white, 1 black, 2 bare ticks
    Color face = style == 0 ? Color{0.96f, 0.96f, 0.95f, 1} : Color{0.07f, 0.07f, 0.08f, 1};
    Color ink = style == 0 ? Color{0.08f, 0.08f, 0.09f, 1} : style == 1 ? Color{0.95f, 0.95f, 0.95f, 1} : t.text;
    if (style != 2) c.Dot(o, rad, face);
    if (style == 1) c.Ring(o, rad, Color{1, 1, 1, 0.14f}, 1);
    for (int i = 0; i < 12; ++i) {
        if (rad < 22 && i % 3) continue; // too small for all twelve
        float a = i * kPi / 6, s = std::sin(a), k = std::cos(a), in = rad * (i % 3 ? 0.88f : 0.8f);
        c.Line({o.x + s * in, o.y - k * in}, {o.x + s * rad * 0.94f, o.y - k * rad * 0.94f}, ink.Alpha(i % 3 ? 0.4f : 0.9f), i % 3 ? 1.0f : 1.6f);
    }
    float ha = ((hh % 12) + mm / 60.0f) * kPi / 6, ma = mm * kPi / 30;
    c.Line(o, {o.x + std::sin(ha) * rad * 0.5f, o.y - std::cos(ha) * rad * 0.5f}, ink, (std::max)(2.0f, rad * 0.075f));
    c.Line(o, {o.x + std::sin(ma) * rad * 0.74f, o.y - std::cos(ma) * rad * 0.74f}, ink, (std::max)(1.5f, rad * 0.05f));
    c.Dot(o, (std::max)(2.2f, rad * 0.07f), t.accentHi);
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
    float h = (std::min)(Height(r), Width(r) / kClockAspect), dw = h * 0.52f, gap = dw * 0.26f, colon = dw * 0.34f, dot = (std::max)(1.5f, h * 0.045f);
    float x = (r.left + r.right) / 2 - (4 * dw + 3 * gap + colon) / 2, y = (r.top + r.bottom) / 2 - h / 2;
    Color on = t.accentHi, off = t.text.Alpha(0.07f);
    for (int i = 0; i < 4; ++i) {
        if (i == 2) {
            c.Dot({x + colon / 2 - gap / 2, y + h * 0.32f}, dot, on);
            c.Dot({x + colon / 2 - gap / 2, y + h * 0.68f}, dot, on);
            x += colon;
        }
        Segment(c, Rect(x, y, dw, h), d[i], on, off);
        x += dw + gap;
    }
}

Color Trend(const Theme& t, double change) { return change > 0.0005 ? Good(t) : change < -0.0005 ? Bad(t) : t.textDim; }

std::wstring Pct(double change) {
    wchar_t b[24];
    swprintf_s(b, L"%+.1f%%", change * 100);
    return b;
}

std::wstring NotYet(const feeds::Series& s) { // a feed with nothing yet, in as few words as fit a tile
    if (s.error.empty()) return L"Fetching…";
    return s.error.rfind(L"Connect", 0) == 0 || s.error.rfind(L"Enter", 0) == 0 ? L"Click to connect" : s.error;
}

void MoneyFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r) {
    const Meta& m = kAll[kind];
    feeds::Series s = Feed(m.feed);
    std::wstring sym = feeds::Setting("money.sym");
    if (sym.empty()) sym = L"$";
    bool chart = wcsstr(m.name, L"Chart") != nullptr;
    if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
    StatOpts o;
    if (chart) {
        o.pts = &s.points, o.ink = o.subInk = Trend(t, s.change);
        return Stat(c, t, r, m.glyph, m.name, sym + Short(s.week), Pct(s.change), o);
    }
    Stat(c, t, r, m.glyph, m.name, sym + Short(s.total), L"Last 30 days");
}

void VisitFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r, const wchar_t* unit) {
    const Meta& m = kAll[kind];
    feeds::Series s = Feed(m.feed);
    if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
    StatOpts o;
    o.pts = &s.points, o.ink = o.subInk = Trend(t, s.change);
    Stat(c, t, r, m.glyph, std::wstring(m.name) + L"  ·  " + unit, Short(s.week), Pct(s.change), o);
}

void CountFace(Canvas& c, const Theme& t, int kind, const D2D1_RECT_F& r, const std::wstring& words) {
    const Meta& m = kAll[kind];
    feeds::Series s = Feed(m.feed);
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

constexpr wchar_t kAmex[] = L"^XAX"; // the nyse american composite
const Market kUsIndex[] = {{L"S&P 500", L"^GSPC"}, {L"Dow Jones", L"^DJI"}, {L"Nasdaq", L"^IXIC"}, {L"Russell 2000", L"^RUT"}, {L"VIX", L"^VIX"}, {L"NYSE Composite", L"^NYA"},
                           {L"NYSE American", L"^XAX"}, {L"10-year Treasury", L"^TNX"}, {L"Gold", L"GC=F"}, {L"Crude oil", L"CL=F"}}; // the first says whether new york is open
constexpr size_t kUsCompact = 3, kUsLarge = 5; // a medium tile: the big three; a large one adds small companies and the fear gauge
constexpr size_t kUsMoreMax = 20; // tickers added in its panel

int scoresFrom = 0, scoresCount = 0, scoresRows = 0; // the scores tile scrolled this many games down; how many it had and showed last drawn

void DrawSports(Canvas& c, const Theme& t, const Meta& m, const D2D1_RECT_F& r) { // live games first, then the latest results, then what's next
    constexpr float kRowH = 24, kLiveW = 44;
    float x = r.left + kPad, room = Width(r) - 2 * kPad;
    std::vector<std::string> leagues = sports::Followed();
    std::vector<sports::Game> games;
    feeds::Series first;
    for (const std::string& l : leagues) {
        feeds::Series s = Feed(sports::FeedId(l));
        if (first.rows.empty() && first.error.empty()) first = s;
        for (const std::wstring& row : s.rows) games.push_back(sports::Decode(row));
    }
    games = sports::Order(std::move(games));
    bool live = !games.empty() && games.front().state == sports::kLive;
    const sports::League* one = leagues.size() == 1 ? sports::Find(leagues[0]) : nullptr;
    Head(c, t, r, m.glyph, one ? one->name : leagues.empty() ? m.name : L"Scores", live ? Bad(t) : t.accentHi, live ? kLiveW : 0);
    if (live) { // a small red mark: something's on now
        D2D1_RECT_F tag = Rect(r.right - kPad - kLiveW + 6, r.top + kPad, kLiveW - 6, kHeadH);
        c.Fill(tag, kHeadH / 2, Bad(t).Alpha(0.16f));
        c.Text(L"LIVE", tag, Weight::Semi, 10, Bad(t), Align::Center);
    }
    float y = r.top + kPad + kHeadH + 6;
    if (leagues.empty()) return (void)c.Text(L"Click to pick leagues", Rect(x, y, room, kRowH), Weight::Regular, 12, t.textDim);
    if (games.empty()) return (void)c.Text(first.ok ? std::wstring(L"Nothing played lately") : NotYet(first), Rect(x, y, room, kRowH), Weight::Regular, 12, t.textDim);
    int fits = (std::max)(1, (int)((r.bottom - kPad + 2 - y) / kRowH));
    if (!gSampling) scoresCount = (int)games.size(), scoresRows = fits; // the gallery's preview isn't the tile
    scoresFrom = gSampling ? 0 : (std::clamp)(scoresFrom, 0, (std::max)(0, scoresCount - fits)); // the wheel scrolled it this far down
    for (size_t i = (size_t)(gSampling ? 0 : scoresFrom); i < games.size(); ++i) {
        const sports::Game& g = games[i];
        if (y + kRowH > r.bottom - kPad + 2) break;
        float detailW = (std::min)(room * 0.3f, c.Measure(g.detail, Weight::Regular, 11) + 6);
        Color score = g.state == sports::kLive ? t.accentHi : t.text;
        c.Text(g.detail, Rect(x, y, room, kRowH), Weight::Regular, 11, g.state == sports::kLive ? Bad(t) : t.textDim, Align::Right);
        float body = room - detailW - 6;
        if (g.solo) {
            c.Text(g.b.empty() ? g.a : g.a + L"  ·  " + g.b, Rect(x, y, body, kRowH), Weight::Medium, 12, t.text);
        } else {
            std::wstring mid = g.state == sports::kPre ? L"v" : g.as + L" – " + g.bs;
            float midW = c.Measure(mid, Weight::Semi, 12.5f) + 12, side = (body - midW) / 2;
            c.Text(g.a, Rect(x, y, side, kRowH), Weight::Medium, 12, t.text, Align::Right);
            c.Text(mid, Rect(x + side, y, midW, kRowH), Weight::Semi, 12.5f, g.state == sports::kPre ? t.textLow : score, Align::Center);
            c.Text(g.b, Rect(x + side + midW, y, side, kRowH), Weight::Medium, 12, t.text);
        }
        y += kRowH;
    }
}

void DrawFixtures(Canvas& c, const Theme& t, const Meta& m, const D2D1_RECT_F& r) { // the coming games under their days, as a calendar's agenda
    constexpr float kRowH = 22, kDayH = 20;
    float x = r.left + kPad, room = Width(r) - 2 * kPad;
    constexpr size_t kRecent = 2; // a couple of results before what's on and what's next
    std::vector<sports::Game> games, live, done;
    feeds::Series first;
    auto seen = [](const std::vector<sports::Game>& in, const sports::Game& g) { return std::any_of(in.begin(), in.end(), [&](const sports::Game& o) { return o.a == g.a && o.b == g.b && o.when == g.when; }); };
    for (const std::string& l : sports::Followed())
        for (const std::string& id : {sports::FeedId(l), sports::NextId(l)}) {
            feeds::Series s = Feed(id);
            if (first.rows.empty() && first.error.empty()) first = s;
            for (const std::wstring& row : s.rows) {
                sports::Game g = sports::Decode(row);
                std::vector<sports::Game>& to = g.state == sports::kLive ? live : g.state == sports::kFinal ? done : games;
                if (!seen(to, g)) to.push_back(std::move(g));
            }
        }
    games = sports::Order(std::move(games)), live = sports::Order(std::move(live)), done = sports::Order(std::move(done));
    if (done.size() > kRecent) done.resize(kRecent);
    for (sports::Game& g : live) g.detail = L"Live  " + g.detail; // under their own headings: the day part says which
    for (sports::Game& g : done) g.detail = L"Results  " + g.detail;
    std::vector<sports::Game> all = live; // what's on, a couple of results, then the coming days
    all.insert(all.end(), done.begin(), done.end());
    all.insert(all.end(), games.begin(), games.end());
    games = std::move(all);
    Head(c, t, r, m.glyph, m.name, t.accentHi);
    float y = r.top + kPad + kHeadH + 4;
    if (games.empty()) return (void)c.Text(first.ok || gSampling ? std::wstring(L"Nothing lately, nothing this week") : NotYet(first), Rect(x, y + 2, room, kRowH), Weight::Regular, 12, t.textDim);
    std::wstring lastDay;
    for (const sports::Game& g : games) {
        size_t cut = g.detail.find(L' '); // "Sat 15:00": its day, then its time; "Live  67'" and "Results  FT" the same way
        std::wstring day = cut == std::wstring::npos ? g.detail : g.detail.substr(0, cut), time = cut == std::wstring::npos ? L"" : g.detail.substr(cut + 1);
        while (!time.empty() && time[0] == L' ') time.erase(0, 1);
        if (g.state != sports::kPre && !g.solo) time = g.as + L"–" + g.bs + L"  " + time; // a score where a time would be
        if (day != lastDay) {
            if (y + kDayH + kRowH > r.bottom - kPad + 2) break;
            c.Text(day, Rect(x, y + 2, room, kDayH - 2), Weight::Semi, 11, t.accentHi);
            y += kDayH, lastDay = day;
        }
        if (y + kRowH > r.bottom - kPad + 2) break;
        c.Text(time, Rect(x, y, room, kRowH), Weight::Medium, 11.5f, t.textDim, Align::Right);
        c.Text(g.solo ? g.a : g.a + L" v " + g.b, Rect(x, y, room - c.Measure(time, Weight::Medium, 11.5f) - 10, kRowH), Weight::Regular, 12, t.text);
        y += kRowH;
    }
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
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2, x = r.left + kPad, room = w - 2 * kPad;
    bool big = IsBig(rh), wide = IsWide(w, rh);
    if (key == "clock.digital") {
        std::wstring time = ClockText(st, h.h24);
        wchar_t sec[8];
        swprintf_s(sec, L":%02d", st.wSecond);
        float most = big ? 72.0f : wide ? 52.0f : 40.0f, secRoom = room * 0.28f;
        float size = Fit(c, time, Weight::Light, most, 18, room - secRoom), tiny = (std::max)(11.0f, std::floor(size * 0.42f));
        float tw = c.Measure(time, Weight::Light, size), sw = c.Measure(sec, Weight::Regular, tiny), left = cx - (tw + sw + 2) / 2, boxH = size * 1.3f, y = mid - boxH / 2 - rh * 0.09f;
        c.Text(time, Rect(left, y, tw + 2, boxH), Weight::Light, size, t.text);
        c.Text(sec, Rect(left + tw + 2, y + (size - tiny) * 0.42f, sw + 2, boxH), Weight::Regular, tiny, t.textDim);
        FitText(c, DateText(st, L"dddd d MMMM"), Rect(x, y + boxH - 2, room, 18), Weight::Regular, big ? 15.0f : 12.0f, 10, t.textDim, Align::Center);
    } else if (key == "clock.analog" || key == "clock.analogdark" || key == "clock.analogmin") {
        int style = key == "clock.analog" ? 0 : key == "clock.analogdark" ? 1 : 2;
        float rad = (std::min)(w, rh) / 2 - 10;
        if (!wide) return Analog(c, t, {cx, mid}, rad, style, st.wHour, st.wMinute);
        Analog(c, t, {r.left + kPad + rad, mid}, rad, style, st.wHour, st.wMinute); // room beside it: the time in words too
        float tx = r.left + kPad + 2 * rad + 16;
        FitText(c, ClockText(st, h.h24), Rect(tx, mid - 30, r.right - kPad - tx, 40), Weight::Light, 32, 16, t.text);
        c.Text(DateText(st, L"ddd d MMM"), Rect(tx, mid + 10, r.right - kPad - tx, 18), Weight::Regular, 12, t.textDim);
    } else if (key == "clock.segment") {
        Clock7(c, t, {x, r.top + kPad, r.right - kPad, r.bottom - kPad - 20}, st, h.h24);
        c.Text(DateText(st, L"dddd d MMM"), Rect(r.left, r.bottom - kPad - 16, w, 16), Weight::Regular, big ? 13.0f : 11.0f, t.textDim, Align::Center);
    } else if (key == "clock.worldfaces") {
        auto zones = state::Zones(w < kThreeFrom ? 2 : 3);
        float cw = room / (float)(std::max)((size_t)1, zones.size()), rad = (std::min)(cw / 2 - 6, (rh - 2 * kPad - 18) / 2);
        for (size_t i = 0; i < zones.size(); ++i) {
            SYSTEMTIME z = state::ZoneNow(zones[i]);
            float zx = x + cw * (i + 0.5f), top = mid - (2 * rad + 18) / 2;
            Analog(c, t, {zx, top + rad}, rad, 1, z.wHour, z.wMinute);
            c.Text(zones[i], Rect(zx - cw / 2, top + 2 * rad + 3, cw, 15), Weight::Medium, big ? 12.0f : 10.5f, t.textDim, Align::Center);
        }
    } else if (key == "clock.worldlist") {
        auto zones = state::Zones(3);
        float row = (rh - 2 * kPad + 4) / 3, y = mid - row * zones.size() / 2, size = big ? 16.0f : 12.5f;
        for (const auto& name : zones) {
            SYSTEMTIME z = state::ZoneNow(name);
            std::wstring when = ClockText(z, h.h24), off = wide || big ? state::Offset(name) : L"";
            float tw = c.Measure(when, Weight::Medium, size) + 4, ow = off.empty() ? 0 : c.Measure(off, Weight::Regular, size - 2) + 10;
            c.Text(name, Rect(x, y, room - tw - ow - 4, row), Weight::Regular, size, t.textDim);
            c.Text(when, Rect(x, y, room - ow, row), Weight::Medium, size, t.text, Align::Right);
            if (!off.empty()) c.Text(off, Rect(x, y, room, row), Weight::Regular, size - 2, t.textLow, Align::Right);
            y += row;
        }
    } else if (key == "clock.worldone") {
        std::wstring name = Ticker("zone.one", L"Tokyo");
        SYSTEMTIME z = state::ZoneNow(name);
        Stat(c, t, r, m.glyph, name, ClockText(z, h.h24), state::Offset(name));
    } else if (key == "clock.today") {
        c.Text(DateText(st, L"dddd"), Rect(x, r.top + kPad - 1, room, 18), Weight::Semi, big ? 15.0f : 12.5f, t.accentHi);
        FitText(c, DateText(st, L"d MMM"), Rect(x, r.top + kPad + 15, room, big ? 66.0f : 40.0f), Weight::Light, big ? 50.0f : 30.0f, 16, t.text);
        float by = r.bottom - kPad - 18, size = big ? 15.0f : 12.5f;
        c.Text(ClockText(st, h.h24), Rect(x, by, room, 18), Weight::Medium, size, t.textDim);
        if (h.wxValid) {
            std::wstring deg = Deg(h.tempC, h.fahrenheit);
            float dw = c.Measure(deg, Weight::Medium, size) + 2;
            c.Text(deg, Rect(x, by, room, 18), Weight::Medium, size, t.text, Align::Right);
            c.Icon(WxIcon(h.wxCode, h.wxDay), {r.right - kPad - dw - 11, by + 9}, size + 3, t.accentHi, true);
        }
    } else if (key == "clock.date") {
        Stat(c, t, r, m.glyph, DateText(st, L"dddd"), DateText(st, L"d MMMM"), std::to_wstring(st.wYear));
    } else if (key == "clock.daynum") {
        float k = big ? 1.9f : 1.0f;
        std::wstring day = DateText(st, L"ddd");
        CharUpperBuffW(day.data(), (DWORD)day.size());
        c.Text(day, Rect(r.left, r.top + kPad - 3, w, 16 * k), Weight::Semi, 11.5f * k, t.accentHi, Align::Center);
        c.Text(std::to_wstring(st.wDay), Rect(r.left, r.top + kPad + 8 * k, w, 58 * k), Weight::Light, 46 * k, t.text, Align::Center);
        c.Text(DateText(st, L"MMMM"), Rect(r.left, r.bottom - kPad - 12 * k, w, 15 * k), Weight::Regular, 10.5f * k, t.textDim, Align::Center);
    } else if (key == "clock.week") {
        int back = (st.wDayOfWeek + 6) % 7; // from monday
        float cw = room / 7, ph = (std::min)(rh - 2 * kPad, big ? 92.0f : 64.0f), py = mid - ph / 2, k = (std::clamp)(cw / 22, 0.95f, 1.5f);
        static const wchar_t* const kNames[7] = {L"M", L"T", L"W", L"T", L"F", L"S", L"S"};
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        for (int i = 0; i < 7; ++i) {
            ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
            u.QuadPart += (ULONGLONG)((LONGLONG)(i - back) * 864000000000LL);
            FILETIME f2 = {u.LowPart, u.HighPart};
            SYSTEMTIME d;
            FileTimeToSystemTime(&f2, &d);
            float dx = x + cw * i;
            bool today = i == back;
            if (today) c.Fill(Rect(dx + 1, py, cw - 2, ph), (std::min)(cw / 2 - 1, 11.0f), t.accent);
            c.Text(kNames[i], Rect(dx, mid - 17 * k, cw, 15 * k), Weight::Medium, 10 * k, today ? t.onAccent : t.textDim, Align::Center);
            c.Text(std::to_wstring(d.wDay), Rect(dx, mid + 1, cw, 17 * k), Weight::Semi, 12.5f * k, today ? t.onAccent : t.text, Align::Center);
        }
    } else if (key == "clock.wx") {
        constexpr float kGapBlocks = 6;
        float bw = (room - 2 * kGapBlocks) / 3, bh = (std::min)(rh - 2 * kPad, big ? 110.0f : 76.0f), top = mid - bh / 2;
        std::wstring parts[3] = {ClockText(st, h.h24), DateText(st, L"d MMM"), h.wxValid ? Deg(h.tempC, h.fahrenheit) : L"--"};
        const wchar_t* const labels[3] = {L"Now", L"Today", L"Outside"};
        Color inks[3] = {t.accent, t.text.Alpha(0.1f), t.text.Alpha(0.06f)};
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F b = Rect(x + i * (bw + kGapBlocks), top, bw, bh);
            c.Fill(b, 12, inks[i]);
            Color main = i == 0 ? t.onAccent : t.text;
            FitText(c, parts[i], Rect(b.left + 4, top + bh * 0.16f, bw - 8, bh * 0.46f), Weight::Medium, big ? 26.0f : 17.0f, 10, main, Align::Center);
            c.Text(labels[i], Rect(b.left, top + bh * 0.6f, bw, bh * 0.28f), Weight::Regular, big ? 12.0f : 10.0f, main.Alpha(0.72f), Align::Center);
        }
    } else if (key == "clock.flip") {
        wchar_t a[4], b[4];
        swprintf_s(a, L"%02d", h.h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12)), swprintf_s(b, L"%02d", st.wMinute);
        constexpr float kFlapGap = 6;
        float pw = (std::min)((room - kFlapGap) / 2, (rh - 2 * kPad) * 1.05f), ph = (std::min)(rh - 2 * kPad, pw * 1.15f);
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F p = Rect(cx - pw - kFlapGap / 2 + i * (pw + kFlapGap), mid - ph / 2, pw, ph);
            c.FillV(p, 12, t.text.Alpha(0.13f), t.text.Alpha(0.06f));
            FitText(c, i ? b : a, Inset(p, 3, 0), Weight::Medium, ph * 0.66f, 14, t.text, Align::Center);
            c.Line({p.left, mid}, {p.right, mid}, t.card.Alpha(0.9f), 1.5f, false);
        }
    }
}

void DrawWeather(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const Host& h, const SYSTEMTIME& st) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2, x = r.left + kPad, room = w - 2 * kPad;
    bool big = IsBig(rh);
    if (!h.wxValid && key != "wx.sun") return Stat(c, t, r, m.glyph, m.name, L"--", L"Finding the forecast");
    if (key == "wx.status") {
        float is = big ? 56.0f : 30.0f;
        Stat(c, t, r, WxIcon(h.wxCode, h.wxDay), h.place.empty() ? std::wstring(m.name) : h.place, Deg(h.tempC, h.fahrenheit), WxText(h.wxCode));
        c.Icon(WxIcon(h.wxCode, h.wxDay), {r.right - kPad - is / 2, big ? r.top + kPad + kHeadH + 8 + 29 : r.bottom - kPad - 28}, is, t.accentHi, true);
        return;
    }
    if (key == "wx.details") {
        feeds::Series s = Feed("sky");
        wchar_t wind[32], rain[32];
        swprintf_s(wind, L"%.0f km/h", s.value);
        swprintf_s(rain, L"%.1f mm", s.extra);
        Head(c, t, r, WxIcon(h.wxCode, h.wxDay), m.name, t.accentHi);
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        rows.push_back({L"Now", Deg(h.tempC, h.fahrenheit)});
        rows.push_back({L"Range", Deg(h.lowC, h.fahrenheit) + L" – " + Deg(h.highC, h.fahrenheit)});
        if (s.ok) rows.push_back({L"Wind", wind}), rows.push_back({L"Rain", rain});
        if (!big && rows.size() > 3) rows.pop_back();
        Rows(c, t, {x, r.top + kPad + kHeadH + 2, r.right - kPad, r.bottom - kPad + 2}, rows, big ? 15.0f : 12);
        return;
    }
    if (key == "wx.sun") {
        feeds::Series s = Feed("sky");
        if (!s.ok || s.points.size() < 2 || s.points[0] < 0) return Stat(c, t, r, glyph::ClearDay, m.name, L"--", NotYet(s));
        auto hm = [](double mins) {
            wchar_t b[12];
            swprintf_s(b, L"%02d:%02d", (int)mins / 60, (int)mins % 60);
            return std::wstring(b);
        };
        double nowMin = st.wHour * 60.0 + st.wMinute, k = (nowMin - s.points[0]) / (std::max)(1.0, s.points[1] - s.points[0]);
        float labelH = big ? 20.0f : 16.0f, base = r.bottom - kPad - labelH - 2, rad = (std::min)(room * 0.42f, base - r.top - kPad - 4);
        D2D1_POINT_2F o = {cx, base};
        c.Line({x, base}, {r.right - kPad, base}, t.text.Alpha(0.14f), 1.2f);
        c.Arc(o, rad, -kPi, kPi, t.text.Alpha(0.16f), 2.5f);
        float past = (float)(std::clamp)(k, 0.0, 1.0), ang = -kPi + kPi * past;
        if (past > 0) c.Arc(o, rad, -kPi, kPi * past, t.accentHi, 2.5f);
        if (k >= 0 && k <= 1) c.Dot({o.x + std::cos(ang) * rad, o.y + std::sin(ang) * rad}, big ? 8.0f : 5.5f, t.accentHi);
        float size = big ? 13.0f : 11.0f;
        c.Text(hm(s.points[0]), Rect(x, r.bottom - kPad - labelH, room, labelH), Weight::Medium, size, t.textDim);
        c.Text(hm(s.points[1]), Rect(x, r.bottom - kPad - labelH, room, labelH), Weight::Medium, size, t.textDim, Align::Right);
        return;
    }
    bool hours = key == "wx.hourly", withToday = key == "wx.todayfc"; // rows of hours or days
    int have = hours ? h.nHours : h.nDays, most = (std::max)(2, (int)(room / kCellLeast));
    int n = (std::min)((std::min)(have, hours ? 6 : withToday ? 3 : 4), most - (withToday ? 1 : 0));
    int cells = n + (withToday ? 1 : 0);
    if (cells <= 0) return;
    float cw = room / cells, k = big ? 1.5f : 1.0f;
    for (int i = 0; i < cells; ++i) {
        float dx = x + cw * (i + 0.5f);
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
        c.Text(top, Rect(dx - cw / 2, mid - 37 * k, cw, 16 * k), Weight::Medium, 11 * k, t.textDim, Align::Center);
        c.Icon(icon, {dx, mid}, 24 * k, t.accentHi, true);
        c.Text(val, Rect(dx - cw / 2, mid + 19 * k, cw, 18 * k), Weight::Semi, 13 * k, t.text, Align::Center);
    }
}

void DrawWork(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const SYSTEMTIME& st) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, x = r.left + kPad, room = w - 2 * kPad;
    bool big = IsBig(rh), wide = IsWide(w, rh);
    if (key == "time.timeline") {
        feeds::Screen s = feeds::ScreenTime();
        feeds::Series sky = Feed("sky");
        Head(c, t, r, m.glyph, Hours(s.today) + L" today", t.accentHi);
        float laneH = big ? 22.0f : 12.0f;
        D2D1_RECT_F lane = Rect(x, mid - laneH / 2 + 5, room, laneH);
        c.Fill(lane, laneH / 2, t.text.Alpha(0.08f));
        float hw = room / 24;
        for (int i = 0; i < 24; ++i)
            if (s.hours[i] > 0) c.Fill(Rect(lane.left + hw * i, lane.top, hw, laneH), 2, t.accent.Alpha(0.25f + 0.75f * (std::min)(1.0f, s.hours[i] / 3600.0f)));
        float nowX = lane.left + room * (st.wHour * 60 + st.wMinute) / 1440;
        c.Line({nowX, lane.top - 4}, {nowX, lane.bottom + 4}, t.text, 1.6f);
        if (sky.ok && sky.points.size() >= 2 && sky.points[0] >= 0)
            for (double mins : sky.points) c.Dot({lane.left + room * (float)(mins / 1440), lane.top - 7}, 2.5f, Color::Rgb(0xFACC15));
        static const wchar_t* const kMarks[] = {L"00", L"06", L"12", L"18"};
        for (int i = 0; i < 4; ++i) c.Text(kMarks[i], Rect(lane.left + room * i / 4, lane.bottom + 6, 30, 14), Weight::Regular, 10.5f, t.textLow);
    } else if (key == "time.screen") {
        feeds::Screen s = feeds::ScreenTime();
        std::wstring top = s.apps.empty() ? L"Nothing yet today" : s.apps[0].app + L"  " + Hours(s.apps[0].seconds);
        std::vector<double> pts(s.hours, s.hours + 24);
        StatOpts o;
        if (big || wide) o.pts = &pts;
        Stat(c, t, r, m.glyph, m.name, Hours(s.today), top, o);
    } else if (key == "time.timer") {
        double left = state::TimerLeft();
        bool on = state::TimerRunning();
        wchar_t b[16];
        swprintf_s(b, L"%d:%02d", (int)left / 60, (int)left % 60);
        std::wstring sub = on ? L"Click to pause" : left > 0 && left < state::TimerLength() ? L"Paused" : L"Click to start";
        StatOpts o;
        o.ink = on ? t.accentHi : t.textDim;
        Stat(c, t, r, m.glyph, m.name, b, sub, o);
    } else if (key == "time.stopwatch") {
        double v = state::WatchSeconds();
        wchar_t b[20];
        swprintf_s(b, L"%d:%02d", (int)v / 60, (int)v % 60);
        StatOpts o;
        o.ink = state::WatchRunning() ? t.accentHi : t.textDim;
        Stat(c, t, r, m.glyph, m.name, b, state::WatchRunning() ? L"Running" : v > 0 ? L"Paused" : L"Click to start", o);
    } else if (key == "money.currency") {
        feeds::Series s = Feed("rates");
        wchar_t b[32];
        swprintf_s(b, L"%.4g", s.value);
        Stat(c, t, r, m.glyph, m.name, s.ok ? std::wstring(b) : L"--", s.ok ? s.text : NotYet(s));
    } else if (key == "prog.day" || key == "prog.month") {
        double v = Frac(key == "prog.day" ? 0 : 1);
        RingFace(c, t, r, key == "prog.day" ? L"Today" : DateText(st, L"MMMM"), v, std::to_wstring((int)(v * 100)) + L"%", L"");
    } else if (key == "prog.year") {
        double v = Frac(2);
        constexpr int kDays = 365;
        wchar_t pct[12];
        swprintf_s(pct, L"%d%%", (int)(v * 100));
        Head(c, t, r, m.glyph, std::to_wstring(st.wYear), t.accentHi);
        c.Text(pct, Rect(x, r.top + kPad, room, kHeadH), Weight::Medium, 11.5f, t.text, Align::Right);
        D2D1_RECT_F dots = {x, r.top + kPad + kHeadH + 8, r.right - kPad, r.bottom - kPad};
        float dw = Width(dots), dh = Height(dots);
        int cols = (std::max)(7, (int)std::ceil(std::sqrt(kDays * dw / dh))), rows = (kDays + cols - 1) / cols, lit = (int)(v * kDays); // a dot a day, as square as the room allows
        float sx = dw / cols, sy = dh / rows, rad = (std::min)(sx, sy) * 0.33f;
        for (int i = 0; i < kDays; ++i) c.Dot({dots.left + sx * (i % cols + 0.5f), dots.top + sy * (i / cols + 0.5f)}, rad, i < lit ? t.accentHi : t.text.Alpha(0.12f));
    } else if (key == "list.water") {
        int have = state::Water(), goal = state::WaterGoal();
        StatOpts o;
        o.bar = goal > 0 ? (double)have / goal : 0;
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(have) + L" of " + std::to_wstring(goal), L"Glasses today", o);
    } else if (key == "list.notes") {
        Head(c, t, r, m.glyph, m.name, t.accentHi);
        std::wstring note = feeds::Setting("notes");
        float y = r.top + kPad + kHeadH + 6, line = big ? 22.0f : 18.0f;
        if (note.empty()) return c.Text(L"Click to write", Rect(x, y, room, line), Weight::Regular, big ? 15.0f : 13.0f, t.textLow);
        for (size_t a = 0; a < note.size() && y + line <= r.bottom - kPad + 4;) {
            size_t e = (std::min)(note.find(L'\n', a), note.size());
            c.Text(note.substr(a, e - a), Rect(x, y, room, line), Weight::Regular, big ? 15.0f : 13.0f, t.text);
            y += line, a = e + 1;
        }
    } else if (key == "list.habits") {
        auto hb = state::Habits();
        int today = state::Today(), done = 0;
        for (const auto& one : hb) done += one.last == today;
        StatOpts o;
        o.bar = hb.empty() ? 0 : (double)done / hb.size();
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(done) + L" of " + std::to_wstring(hb.size()), L"Today", o);
    }
}

void DrawSystem(Canvas& c, const Theme& t, const std::string& key, const Meta& m, const D2D1_RECT_F& r, const Host& h) {
    if (key == "sys.cpu") {
        RingFace(c, t, r, L"CPU", h.cpu, std::to_wstring((int)std::lround(h.cpu * 100)) + L"%", L"");
    } else if (key == "sys.ram") {
        wchar_t b[32] = {};
        if (h.memTotalGb > 0) swprintf_s(b, L"%.1f of %.0f GB", h.memUsedGb, h.memTotalGb);
        RingFace(c, t, r, L"RAM", h.mem, std::to_wstring((int)std::lround(h.mem * 100)) + L"%", b);
    } else if (key == "sys.temp") {
        feeds::Series s = Feed("thermal");
        wchar_t b[48];
        swprintf_s(b, L"CPU %d%%  ·  RAM %d%%", (int)std::lround(h.cpu * 100), (int)std::lround(h.mem * 100));
        StatOpts o;
        o.ink = s.ok && s.value > 85 ? Bad(t) : t.accentHi;
        Stat(c, t, r, m.glyph, m.name, s.ok ? std::to_wstring((int)std::lround(s.value)) + L"°" : L"--", s.ok ? std::wstring(b) : s.error.empty() ? L"Reading…" : s.error, o);
    } else if (key == "sys.lang") {
        Stat(c, t, r, m.glyph, m.name, state::Language(), L"Click to switch");
    } else if (key == "sys.bright") {
        if (h.bright < 0) return Stat(c, t, r, m.glyph, m.name, L"--", L"Not on this display");
        StatOpts o;
        o.bar = h.bright / 100.0;
        Stat(c, t, r, m.glyph, m.name, std::to_wstring(h.bright) + L"%", L"", o);
    } else if (key == "sys.theme") {
        Stat(c, t, r, m.glyph, L"Windows", state::LightTheme() ? L"Light" : L"Dark", L"Click to switch");
    } else if (key == "sys.storage" || key == "sys.drives") {
        ULARGE_INTEGER freeBytes{}, total{};
        GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &total, nullptr);
        double used = total.QuadPart ? 1 - (double)freeBytes.QuadPart / (double)total.QuadPart : 0;
        wchar_t main[32], sub[32];
        swprintf_s(main, L"%.0f GB", freeBytes.QuadPart / 1073741824.0);
        swprintf_s(sub, L"Free of %.0f GB", total.QuadPart / 1073741824.0);
        StatOpts o;
        o.bar = used, o.ink = used > 0.9 ? Bad(t) : t.accentHi;
        Stat(c, t, r, m.glyph, m.name, main, sub, o);
    }
}

void DrawMediaFace(Canvas& c, const Theme& t, const std::string& key, const D2D1_RECT_F& r, const Host& h) {
    float w = Width(r), rh = Height(r), mid = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2, x = r.left + kPad, room = w - 2 * kPad;
    bool big = IsBig(rh);
    std::wstring title = h.hasTrack ? h.title : L"Nothing playing";
    if (key == "media.mini") {
        float rad = (std::min)(w, rh) / 2 - 12;
        c.Ring({cx, mid}, rad, t.text.Alpha(0.12f), big ? 9.0f : 6.0f);
        if (h.hasTrack) c.Arc({cx, mid}, rad, -kPi / 2, 2 * kPi * (float)h.progress, t.accentHi, big ? 9.0f : 6.0f);
        c.Icon(h.playing ? glyph::Pause : glyph::Play, {cx, mid}, rad * 0.95f, h.hasTrack ? t.text : t.textLow, true);
        return;
    }
    bool bar = key == "media.bar";
    float k = big ? 1.4f : 1.0f, ctl = 30 * k, bottom = r.bottom - kPad;
    Head(c, t, r, glyph::Music, L"Now playing", h.hasTrack ? t.accentHi : t.textLow);
    c.Icon(bar ? glyph::Next : h.playing ? glyph::Pause : glyph::Play, {r.right - kPad - ctl / 2, bar ? r.top + kPad + kHeadH / 2 : bottom - ctl / 2}, ctl * (bar ? 0.72f : 1.0f), h.hasTrack ? t.text : t.textLow, true);
    float tw = bar ? room : room - ctl - 6, by = bottom;
    if (bar) {
        Lane(c, t, Rect(x, by - 5 * k, room, 5 * k), h.hasTrack ? h.progress : 0, t.accentHi);
        by -= 13 * k;
    }
    if (h.hasTrack) {
        c.Text(h.artist, Rect(x, by - 16 * k, tw, 16 * k), Weight::Regular, 11.5f * k, t.textDim);
        by -= 17 * k;
    }
    FitText(c, title, Rect(x, by - 21 * k, tw, 21 * k), Weight::Semi, 14.5f * k, 12, h.hasTrack ? t.text : t.textDim);
}

void DrawOnline(Canvas& c, const Theme& t, int kind, const std::string& key, const Meta& m, const D2D1_RECT_F& r) {
    float w = Width(r), rh = Height(r), x = r.left + kPad, room = w - 2 * kPad;
    bool big = IsBig(rh);
    feeds::Series s = m.feed ? Feed(m.feed) : feeds::Series{};
    if (key == "money.metric") {
        wchar_t b[32];
        swprintf_s(b, s.value >= 100 ? L"%.0f" : L"%.2f", s.value);
        Stat(c, t, r, m.glyph, s.ok ? s.text : m.name, s.ok ? std::wstring(b) : L"--", s.ok ? L"" : NotYet(s));
    } else if (key == "stock.one" || key == "stock.chart") {
        std::wstring tick = Ticker("stock.id", L"AAPL");
        feeds::Series q = Feed(QuoteId(tick));
        wchar_t b[32];
        swprintf_s(b, L"%.2f", q.value);
        if (!q.ok) return Stat(c, t, r, m.glyph, tick, L"--", NotYet(q));
        StatOpts o;
        o.ink = o.subInk = Trend(t, q.change);
        if (key == "stock.chart") o.pts = &q.points;
        Stat(c, t, r, q.change >= 0 ? glyph::Up : glyph::Down, tick, b, Pct(q.change), o);
    } else if (key == "stock.amex") {
        feeds::Series q = Feed(QuoteId(kAmex));
        wchar_t b[32];
        swprintf_s(b, L"%.2f", q.value);
        if (!q.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(q));
        StatOpts o;
        o.ink = o.subInk = Trend(t, q.change), o.pts = &q.points;
        Stat(c, t, r, q.change >= 0 ? glyph::Up : glyph::Down, m.name, b, Pct(q.change), o);
    } else if (key == "stock.us") { // the three that say how america's market is doing, and whether it's trading
        feeds::Series lead = Feed(QuoteId(kUsIndex[0].tick));
        feeds::Phase phase = gSampling ? feeds::kOpen : feeds::MarketPhase(lead);
        const wchar_t* says = feeds::PhaseName(phase);
        float chip = c.Measure(says, Weight::Medium, 11) + 18;
        Head(c, t, r, m.glyph, m.name, t.accentHi, chip + 8);
        Color dot = phase == feeds::kOpen ? Good(t) : phase == feeds::kClosed ? t.textLow : t.accentHi;
        D2D1_RECT_F pill = Rect(r.right - kPad - chip, r.top + kPad - 1, chip, kHeadH + 2);
        c.Fill(pill, Height(pill) / 2, dot.Alpha(0.14f));
        c.Dot({pill.left + 8, (pill.top + pill.bottom) / 2}, 3, dot);
        c.Text(says, Rect(pill.left + 14, pill.top, chip - 16, Height(pill)), Weight::Medium, 11, t.text);
        size_t shown = big ? kUsLarge : kUsCompact;
        float y = r.top + kPad + kHeadH + 8, row = (r.bottom - kPad - y) / (float)shown, size = big ? 14.5f : 12.5f;
        bool roomy = room >= kWatchRoomy, lines = big || IsWide(Width(r), Height(r)); // room for each index's own day
        for (size_t i = 0; i < shown; ++i) {
            const Market& idx = kUsIndex[i];
            feeds::Series q = Feed(QuoteId(idx.tick));
            float mid = y + row / 2;
            c.Text(idx.label, Rect(x, y, room * 0.34f, row), Weight::Semi, size, t.text);
            if (q.ok) {
                wchar_t b[32];
                swprintf_s(b, q.value >= 10000 ? L"%.0f" : L"%.2f", q.value);
                Color ink = Trend(t, q.change);
                std::wstring pct = Pct(q.change);
                float pctW = c.Measure(pct, Weight::Semi, size - 1.5f) + 14;
                D2D1_RECT_F tag = Rect(r.right - kPad - pctW, mid - 10, pctW, 20); // the change on a soft chip of its colour
                c.Fill(tag, 10, ink.Alpha(0.14f));
                c.Text(pct, tag, Weight::Semi, size - 1.5f, ink, Align::Center);
                float valueW = roomy ? c.Measure(b, Weight::Medium, size) + 10 : 0;
                if (roomy) c.Text(b, Rect(tag.left - valueW - 4, y, valueW, row), Weight::Medium, size, t.text, Align::Right);
                if (lines) Spark(c, {x + room * 0.35f, mid - row * 0.32f, tag.left - valueW - 14, mid + row * 0.32f}, q.points, ink);
            } else {
                c.Text(q.error.empty() ? L"…" : L"--", Rect(x, y, room, row), Weight::Regular, size, t.textLow, Align::Right);
            }
            if (i + 1 < shown) c.Line({x, y + row}, {r.right - kPad, y + row}, t.text.Alpha(0.05f), 1);
            y += row;
        }
    } else if (key == "stock.watch") {
        auto list = Watch();
        float row = (rh - 2 * kPad) / (float)(std::max)((size_t)1, list.size()), y = r.top + kPad, size = big ? 16.0f : 12.5f;
        bool roomy = room >= kWatchRoomy;
        for (const auto& one : list) {
            feeds::Series q = Feed(QuoteId(one));
            wchar_t b[32];
            swprintf_s(b, L"%.2f", q.value);
            c.Text(one, Rect(x, y, room * 0.4f, row), Weight::Semi, size, t.text);
            if (q.ok) {
                c.Text(Pct(q.change), Rect(x, y, room, row), Weight::Medium, size - 1, Trend(t, q.change), Align::Right);
                if (roomy) c.Text(b, Rect(x, y, room * 0.68f, row), Weight::Regular, size, t.textDim, Align::Right);
            } else {
                c.Text(L"--", Rect(x, y, room, row), Weight::Regular, size, t.textLow, Align::Right);
            }
            y += row;
        }
    } else if (key == "web.plausible" || key == "web.datafast") {
        VisitFace(c, t, kind, r, L"visitors");
    } else if (key == "web.dub") {
        VisitFace(c, t, kind, r, L"clicks");
    } else if (key == "web.resend") {
        if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
        Stat(c, t, r, m.glyph, m.name, Short(s.week), L"Sent this week");
    } else if (key == "web.linear") {
        CountFace(c, t, kind, r, Short(s.extra) + L" started");
    } else if (key == "web.supabase") {
        CountFace(c, t, kind, r, s.text);
    } else if (key == "web.gmail") {
        CountFace(c, t, kind, r, L"Unread");
    } else if (key == "web.github") {
        CountFace(c, t, kind, r, L"Notifications");
    } else if (key == "web.githubover") {
        CountFace(c, t, kind, r, Short(s.extra) + L" PRs  ·  " + Short(s.extra2) + L" stars");
    } else if (key == "web.githubgrid") {
        if (!s.ok) return Stat(c, t, r, m.glyph, m.name, L"--", NotYet(s));
        constexpr int kWeekDays = 7;
        float cell = (rh - 2 * kPad) / kWeekDays;
        int cols = (std::max)(1, (int)(room / cell));
        size_t show = (std::min)((size_t)(cols * kWeekDays), s.points.size()), from = s.points.size() - show;
        double top = 1;
        for (size_t i = from; i < s.points.size(); ++i) top = (std::max)(top, s.points[i]);
        float left = x + (room - cols * cell) / 2;
        for (size_t i = 0; i < show; ++i) {
            double v = s.points[from + i];
            c.Fill(Rect(left + cell * (float)(i / kWeekDays), r.top + kPad + cell * (float)(i % kWeekDays), cell - 2, cell - 2), cell * 0.22f, v > 0 ? t.accent.Alpha(0.3f + 0.7f * (float)(v / top)) : t.text.Alpha(0.08f));
        }
    } else if (key == "web.leetcode") {
        CountFace(c, t, kind, r, L"Solved  ·  " + Short(s.extra) + L" day streak");
    } else { // the follower counts
        CountFace(c, t, kind, r, s.text.empty() ? L"Followers" : s.text);
    }
}

}

void Sampling(bool on) { gSampling = on; }

bool Wheel(int kind, float notches) {
    if (kind < 0 || kind >= kKinds || strcmp(kAll[kind].key, "sport.scores") != 0) return false;
    int most = (std::max)(0, scoresCount - scoresRows);
    if (most == 0) return false; // all of it shows: the board scrolls instead
    scoresFrom = (std::clamp)(scoresFrom - (int)std::lround(notches), 0, most); // zero notches only asks; over it, the wheel is its own even at an end
    return true;
}

const std::vector<Market>& UsMarkets() {
    static const std::vector<Market> all(std::begin(kUsIndex), std::end(kUsIndex));
    return all;
}

std::vector<std::wstring> UsMoreTickers() {
    std::wstring all = feeds::Setting("us.more");
    std::vector<std::wstring> out;
    for (size_t a = 0; a < all.size() && out.size() < kUsMoreMax;) {
        size_t e = (std::min)(all.find(L',', a), all.size());
        std::wstring one;
        for (wchar_t ch : all.substr(a, e - a))
            if (ch != L' ') one += (wchar_t)towupper(ch); // tickers are upper case, whatever was typed
        if (!one.empty() && std::find(out.begin(), out.end(), one) == out.end()) out.push_back(one);
        a = e + 1;
    }
    return out;
}

int Count() { return kKinds; }
const Meta& Info(int kind) { return kAll[(std::clamp)(kind, 0, kKinds - 1)]; }

int Find(const char* key) {
    for (int i = 0; i < kKinds; ++i)
        if (!strcmp(kAll[i].key, key)) return i;
    return -1;
}

const wchar_t* CatName(int cat) {
    static const wchar_t* const kNames[kCats] = {L"Clocks", L"Weather", L"Productivity", L"System", L"Media", L"Money", L"Online", L"Apps & Files", L"Sports"};
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
    if (key == "sport.scores") return DrawSports(c, t, m, r);
    if (key == "sport.fixtures") return DrawFixtures(c, t, m, r);
    if (key == "files.websearch") {
        float mid = (r.top + r.bottom) / 2, fh = IsBig(Height(r)) ? 52.0f : 38.0f;
        D2D1_RECT_F f = {r.left + kPad, mid - fh / 2, r.right - kPad, mid + fh / 2};
        c.Fill(f, fh / 2, t.text.Alpha(0.08f));
        c.Icon(m.glyph, {f.left + fh / 2, mid}, fh * 0.44f, t.textDim);
        const wchar_t* says = c.Measure(L"Search the web", Weight::Regular, fh * 0.34f) <= Width(f) - fh ? L"Search the web" : L"Search";
        c.Text(says, Rect(f.left + fh * 0.9f, f.top, Width(f) - fh, fh), Weight::Regular, fh * 0.34f, t.textDim);
        return;
    }
    if (key == "files.photo") return state::DrawPhoto(c, t, r);
    Launch(c, t, r, m.glyph, m.name, m.about);
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
        if (key == "stock.us") { // its panel lists every one, and what's been added
            for (const auto& idx : kUsIndex) add(QuoteId(idx.tick));
            for (const std::wstring& one : UsMoreTickers()) add(QuoteId(one));
        }
        if (key == "stock.amex") add(QuoteId(kAmex));
        if (key == "sport.scores")
            for (const std::string& l : sports::Followed()) {
                add(sports::FeedId(l));
                if (l != "chess") add(sports::NextId(l)); // its panel shows the week either side too
            }
        if (key == "sport.fixtures")
            for (const std::string& l : sports::Followed()) {
                add(sports::FeedId(l)); // what's on now
                if (l != "chess") add(sports::NextId(l));
            }
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
