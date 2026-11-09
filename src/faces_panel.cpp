// What rises behind a face: more detail, a list to pick from, and the few fields it needs set up.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>

#include <map>
#include <string>
#include <vector>

#include "faces.h"
#include "faces_state.h"
#include "feeds.h"
#include "glyphs.h"
#include "sports.h"

namespace faces {
namespace {

constexpr float kHead = 26, kRow = 28, kFieldH = 46, kNoteH = 20;
constexpr int kMaxRows = 8;
constexpr size_t kMaxTyped = 4000;

struct Field {
    const char* key = nullptr;
    const wchar_t* label = nullptr;
    bool secret = false;
    const wchar_t* hint = L"";
};

struct Setup {
    const char* prefix; // meta keys starting with this share these
    const char* feed;
    Field fields[3];
    const wchar_t* note;
};

// a key is only ever a read-only one you made yourself; what each needs, in the words its maker uses
const Setup kSetups[] = {
    {"money.stripe", "stripe", {{"stripe.key", L"Restricted API key", true, L"rk_live_…"}}, L"Dashboard, Developers, API keys: read access to balance"},
    {"money.polar", "polar", {{"polar.key", L"Access token", true, L"polar_oat_…"}}, L"Settings, Developers: metrics read"},
    {"money.dodo", "dodo", {{"dodo.key", L"API key", true, L""}}, L"Developer, API keys"},
    {"money.shopify", "shopify", {{"shopify.id", L"Store domain", false, L"yours.myshopify.com"}, {"shopify.key", L"Admin API token", true, L"shpat_…"}}, L"A custom app with read_orders"},
    {"money.lemon", "lemon", {{"lemon.key", L"API key", true, L""}}, L"Settings, API"},
    {"money.paddle", "paddle", {{"paddle.key", L"API key", true, L"pdl_live_…"}}, L"Developer tools, Authentication: transactions read"},
    {"money.gumroad", "gumroad", {{"gumroad.key", L"Access token", true, L""}}, L"Settings, Advanced, Applications"},
    {"money.total", nullptr, {{"money.sym", L"Currency symbol", false, L"$"}}, L"Adds up every store connected in its own tile"},
    {"web.plausible", "plausible", {{"plausible.id", L"Site", false, L"yoursite.com"}, {"plausible.key", L"API key", true, L""}}, L"Account settings, API keys"},
    {"web.dub", "dub", {{"dub.key", L"API key", true, L"dub_…"}}, L"Workspace settings, API keys"},
    {"web.datafast", "datafast", {{"datafast.key", L"API key", true, L"df_…"}}, L"Website settings, API"},
    {"web.resend", "resend", {{"resend.key", L"API key", true, L"re_…"}}, L"API keys"},
    {"web.linear", "linear", {{"linear.key", L"Personal API key", true, L"lin_api_…"}}, L"Settings, API"},
    {"web.supabase", "supabase", {{"supabase.id", L"Project ref", false, L""}, {"supabase.key", L"Access token", true, L"sbp_…"}}, L"Account, Access tokens"},
    {"web.gmail", "gmail", {{"gmail.id", L"Address", false, L""}, {"gmail.key", L"App password", true, L""}}, L"Google account, Security, App passwords"},
    {"web.githubover", "github.overview", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Notifications and metadata, read only"},
    {"web.githubgrid", "github.activity", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Read only"},
    {"web.github", "github", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Notifications, read only"},
    {"web.leetcode", "leetcode", {{"leetcode.id", L"Username", false, L""}}, L"Your public profile: no key needed"},
    {"social.youtube", "youtube", {{"youtube.id", L"Channel ID", false, L"UC…"}, {"youtube.key", L"API key", true, L""}}, L"Google Cloud, YouTube Data API v3"},
    {"social.x", "x", {{"x.id", L"Username", false, L""}, {"x.key", L"Bearer token", true, L""}}, L"X developer portal"},
    {"social.tiktok", "tiktok", {{"tiktok.key", L"Access token", true, L""}}, L"TikTok for Developers: user.info.stats"},
    {"social.instagram", "instagram", {{"instagram.key", L"Access token", true, L""}}, L"Meta for Developers: Instagram API"},
    {"stock.one", nullptr, {{"stock.id", L"Ticker", false, L"AAPL"}}, L"Public quotes, delayed"},
    {"stock.chart", nullptr, {{"stock.id", L"Ticker", false, L"AAPL"}}, L"Public quotes, delayed"},
    {"stock.watch", nullptr, {{"stock.watch", L"Three tickers", false, L"AAPL,MSFT,NVDA"}}, L"Public quotes, delayed"},
    {"stock.us", nullptr, {{"us.more", L"Your tickers", false, L"AAPL,TSLA,BTC-USD"}}, L"Any symbol Yahoo Finance knows, comma separated; delayed"},
    {"money.metric", "metric", {{"metric.id", L"Pair", false, L"BTC-USD"}}, L"A public price"},
    {"money.currency", "rates", {{"rates.from", L"From", false, L"USD"}, {"rates.to", L"To", false, L"EUR"}}, L"Public daily rates"},
    {"clock.world", nullptr, {{"zones", L"Cities", false, L"London,New York,Tokyo"}}, L"Up to three of the world's big cities"},
    {"clock.worldone", nullptr, {{"zone.one", L"City", false, L"Tokyo"}}, L"One of the world's big cities"},
    {"time.timer", nullptr, {{"timer.min", L"Minutes", false, L"5"}}, nullptr},
    {"list.water", nullptr, {{"water.goal", L"Glasses a day", false, L"8"}}, nullptr},
};

const Setup* SetupFor(const char* key) {
    size_t best = 0;
    const Setup* found = nullptr;
    for (const Setup& s : kSetups) {
        size_t n = strlen(s.prefix);
        if (!strncmp(key, s.prefix, n) && n > best) best = n, found = &s;
    }
    return found;
}

int FieldCount(const Setup* s) {
    int n = 0;
    while (s && n < 3 && s->fields[n].key) ++n;
    return n;
}

struct RowItem {
    std::wstring name, path;
};
std::vector<RowItem> rows; // what the panel lists, read once as it opens
int editing = -1;
std::wstring typed, draft;
int panelKind = -1;
void (*resyncHost)(void*) = nullptr; // what the host gave the panel last: a new ticker is fetched from now on
void* resyncCtx = nullptr;

bool Is(int kind, const char* key) { return !strcmp(Info(kind).key, key); }

void Wipe(std::wstring& s) { // a key leaves no copy behind
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}

void ListFolder(const std::wstring& dir, bool imagesOnly) { // the newest few
    struct Found {
        std::wstring name;
        FILETIME at;
    };
    std::vector<Found> all;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY))) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if (imagesOnly && !(ext && (!_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".webp")))) continue;
        all.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    size_t keep = (std::min)((size_t)kMaxRows, all.size());
    std::partial_sort(all.begin(), all.begin() + (ptrdiff_t)keep, all.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.at, &b.at) > 0; });
    for (size_t i = 0; i < keep; ++i) {
        std::wstring shown = all[i].name;
        if (size_t dot = shown.rfind(L".lnk"); dot != std::wstring::npos && dot + 4 == shown.size()) shown.resize(dot); // a recent item's shortcut: its name
        rows.push_back({shown, dir + L"\\" + all[i].name});
    }
}

std::wstring Known(const KNOWNFOLDERID& id, const wchar_t* tail = L"") {
    PWSTR dir = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) out = std::wstring(dir) + tail;
    CoTaskMemFree(dir);
    return out;
}

void Open(const std::wstring& what) { ShellExecuteW(nullptr, L"open", what.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void SetWallpaper(const std::wstring& path) { SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }

void ListWallpapers() {
    ListFolder(Known(FOLDERID_Pictures, L"\\Wallpapers"), true);
    if (rows.empty()) ListFolder(Known(FOLDERID_Pictures), true);
}

void NextWallpaper() {
    rows.clear();
    ListWallpapers();
    if (rows.empty()) return;
    int at = (_wtoi(feeds::Setting("wallpaper.at").c_str()) + 1) % (int)rows.size();
    feeds::SetSetting("wallpaper.at", std::to_wstring(at));
    SetWallpaper(rows[(size_t)at].path);
}

void Save(const Setup* s) {
    if (editing < 0 || !s || editing >= FieldCount(s)) return;
    const Field& f = s->fields[editing];
    if (f.secret) {
        if (!typed.empty()) feeds::SetSecret(f.key, typed); // left empty: the saved key stays
    } else {
        feeds::SetSetting(f.key, typed);
    }
    Wipe(typed);
    if (resyncHost) resyncHost(resyncCtx); // a new ticker or league is wanted from now: the host asks for it
    if (s->feed) feeds::Refresh(s->feed);
    for (const std::string& id : Feeds({panelKind})) feeds::Refresh(id); // a new ticker is its own feed
    editing = -1;
}

// ---- the richer panels: a quote, a market, the forecast, the games ----
constexpr float kQuoteH = 246, kMarketRowH = 56, kForecastH = 292, kSportsH = 318;
constexpr uint32_t kUp = 0x4ADE80, kDown = 0xFF6B6B, kUpPale = 0x15803D, kDownPale = 0xDC2626;
constexpr wchar_t kAmexTick[] = L"^XAX";
float marketScroll = 0; // the markets list, scrolled
int sportTab = 0, sportPick = 0; // the games or the leagues; the sport whose leagues are listed
constexpr float kEndRoom = 28; // a scrolled list goes this far past its last item: it comes clear of the card's rounded foot
float gameScroll = 0, leagueScroll = 0, leagueH = 0; // the games and the leagues, scrolled; how tall the leagues ran last frame
sports::Game openGame; // the game opened, kept fresh from the lists while it's in them
float matchScroll = 0, matchH = 0; // its detail, scrolled; how tall it ran last frame

bool Pale(const Theme& t) { return (t.text.r + t.text.g + t.text.b) < 1.2f; } // dark ink: a light look
Color TrendInk(const Theme& t, double change) { return change > 0.0005 ? Color::Rgb(Pale(t) ? kUpPale : kUp) : change < -0.0005 ? Color::Rgb(Pale(t) ? kDownPale : kDown) : t.textDim; }

std::string QuoteOf(const std::wstring& tick) {
    std::string id = "quote:";
    for (wchar_t ch : tick) id += (char)ch;
    return id;
}

std::wstring Num2(double v) {
    wchar_t b[32];
    swprintf_s(b, v >= 10000 ? L"%.0f" : L"%.2f", v);
    return b;
}

std::wstring Big(double v) { // 3.4B shares
    wchar_t b[24];
    if (v >= 1e9) swprintf_s(b, L"%.1fB", v / 1e9);
    else if (v >= 1e6) swprintf_s(b, L"%.1fM", v / 1e6);
    else swprintf_s(b, L"%.0f", v);
    return b;
}

void Chart(Canvas& c, const D2D1_RECT_F& r, const std::vector<double>& pts, Color ink, double base) { // the day's line, its area fading under it, the previous close dotted across
    if (pts.size() < 2 || Width(r) < 8) return;
    double lo = pts[0], hi = pts[0];
    for (double p : pts) lo = (std::min)(lo, p), hi = (std::max)(hi, p);
    if (base > 0) lo = (std::min)(lo, base), hi = (std::max)(hi, base);
    if (hi - lo < 1e-9) hi = lo + 1;
    float step = Width(r) / (float)(pts.size() - 1);
    auto at = [&](size_t i) { return D2D1_POINT_2F{r.left + step * i, r.bottom - Height(r) * (float)((pts[i] - lo) / (hi - lo))}; };
    ComPtr<ID2D1PathGeometry> area;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(gfx::D2D()->CreatePathGeometry(&area)) && SUCCEEDED(area->Open(&sink))) {
        sink->BeginFigure({r.left, r.bottom}, D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 0; i < pts.size(); ++i) sink->AddLine(at(i));
        sink->AddLine({at(pts.size() - 1).x, r.bottom});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        c.FillGeoV(area.Get(), r, ink.Alpha(0.24f), ink.Alpha(0));
    }
    if (base > 0) { // the previous close: where up and down are measured from
        float by = r.bottom - Height(r) * (float)((base - lo) / (hi - lo));
        for (float x = r.left; x < r.right; x += 8) c.Line({x, by}, {(std::min)(x + 4, r.right), by}, ink.Alpha(0.35f), 1, false);
    }
    for (size_t i = 1; i < pts.size(); ++i) c.Line(at(i - 1), at(i), ink, 1.8f);
    c.Dot(at(pts.size() - 1), 3, ink);
}

void QuoteDetail(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float& y, const std::wstring& tick, const wchar_t* label) {
    float top = y;
    feeds::Series q = feeds::Get(QuoteOf(tick));
    std::wstring name = !q.name.empty() ? q.name : label ? label : tick;
    if (!q.ok) {
        c.Text(name, Rect(r.left, y, Width(r), 18), Weight::Semi, 15, t.text);
        c.Text(q.error.empty() ? L"Fetching…" : q.error, Rect(r.left, y + 24, Width(r), 18), Weight::Regular, 12, t.textDim);
        y = top + kQuoteH;
        return;
    }
    feeds::Phase phase = feeds::MarketPhase(q);
    c.Text(name + L"  ·  " + tick, Rect(r.left, y, Width(r) - 110, 18), Weight::Medium, 12.5f, t.textDim);
    c.Text(feeds::PhaseName(phase), Rect(r.left, y, Width(r), 18), Weight::Medium, 12, phase == feeds::kOpen ? TrendInk(t, 1) : t.textDim, Align::Right);
    y += 22;
    Color ink = TrendInk(t, q.change);
    double prev = q.more.size() > feeds::kPrevClose ? q.more[feeds::kPrevClose] : 0;
    wchar_t move[48];
    swprintf_s(move, L"%+.2f   %+.2f%%", prev > 0 ? q.value - prev : 0, q.change * 100);
    c.Text(Num2(q.value), Rect(r.left, y, Width(r) * 0.5f, 40), Weight::Light, 34, t.text);
    c.Text(move, Rect(r.left + Width(r) * 0.5f, y + 10, Width(r) * 0.5f, 24), Weight::Medium, 14, ink, Align::Right);
    y += 46;
    Chart(c, Rect(r.left, y, Width(r), 96), q.points, ink, prev);
    y += 108;
    if (q.more.size() >= feeds::kQuoteMore) {
        struct Cell {
            const wchar_t* label;
            std::wstring value;
        } cells[] = {{L"Previous close", Num2(prev)}, {L"Day's range", Num2(q.more[feeds::kDayLow]) + L" – " + Num2(q.more[feeds::kDayHigh])},
                     {L"52-week range", Num2(q.more[feeds::kYearLow]) + L" – " + Num2(q.more[feeds::kYearHigh])}, {L"Volume", q.more[feeds::kVolume] > 0 ? Big(q.more[feeds::kVolume]) : L"—"}};
        float cw = Width(r) / 4;
        for (int i = 0; i < 4; ++i) {
            c.Text(cells[i].label, Rect(r.left + cw * i, y, cw - 8, 14), Weight::Regular, 10.5f, t.textLow);
            c.Text(cells[i].value, Rect(r.left + cw * i, y + 15, cw - 8, 18), Weight::Medium, 12, t.text);
        }
    }
    y = top + kQuoteH;
}

void MarketRows(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float& y, const std::vector<std::pair<std::wstring, std::wstring>>& list) { // label and ticker: each with its day as a small line
    for (const auto& [label, tick] : list) {
        if (y + kMarketRowH < r.top || y > r.bottom) { // scrolled out of the view: not drawn
            y += kMarketRowH;
            continue;
        }
        feeds::Series q = feeds::Get(QuoteOf(tick));
        c.Text(label, Rect(r.left, y + 8, Width(r) * 0.3f, 20), Weight::Semi, 14, t.text);
        c.Text(q.ok && !q.name.empty() && q.name != label ? q.name : tick, Rect(r.left, y + 28, Width(r) * 0.3f, 16), Weight::Regular, 11, t.textDim);
        if (q.ok) {
            Color ink = TrendInk(t, q.change);
            Chart(c, Rect(r.left + Width(r) * 0.33f, y + 10, Width(r) * 0.3f, kMarketRowH - 20), q.points, ink, q.more.size() > feeds::kPrevClose ? q.more[feeds::kPrevClose] : 0);
            c.Text(Num2(q.value), Rect(r.left, y + 8, Width(r), 20), Weight::Medium, 14, t.text, Align::Right);
            wchar_t b[24];
            swprintf_s(b, L"%+.2f%%", q.change * 100);
            c.Text(b, Rect(r.left, y + 28, Width(r), 16), Weight::Medium, 12, ink, Align::Right);
        } else {
            c.Text(q.error.empty() ? L"Fetching…" : q.error, Rect(r.left, y + 16, Width(r), 18), Weight::Regular, 12, t.textDim, Align::Right);
        }
        c.Line({r.left, y + kMarketRowH - 1}, {r.right, y + kMarketRowH - 1}, t.text.Alpha(0.06f), 1);
        y += kMarketRowH;
    }
}

std::wstring Deg(double c, bool f) { return std::to_wstring((int)std::lround(f ? c * 9 / 5 + 32 : c)) + L"°"; }

void Forecast(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float& y, const Host& h) { // now, the coming hours, the coming days
    float top = y;
    if (!h.wxValid) {
        c.Text(L"Finding the forecast…", Rect(r.left, y, Width(r), 20), Weight::Regular, 12.5f, t.textDim);
        y = top + kForecastH;
        return;
    }
    c.Icon(WxIcon(h.wxCode, h.wxDay), {r.left + 24, y + 26}, 42, t.accentHi, true);
    c.Text(Deg(h.tempC, h.fahrenheit), Rect(r.left + 58, y, 120, 50), Weight::Light, 40, t.text);
    c.Text(WxText(h.wxCode), Rect(r.left + 170, y + 6, Width(r) - 170, 20), Weight::Medium, 14, t.text);
    c.Text(h.place + L"  ·  H " + Deg(h.highC, h.fahrenheit) + L"  L " + Deg(h.lowC, h.fahrenheit), Rect(r.left + 170, y + 28, Width(r) - 170, 18), Weight::Regular, 12, t.textDim);
    y += 62;
    int n = (std::min)(h.nHours, 8);
    float cw = Width(r) / (float)(std::max)(1, n);
    for (int i = 0; i < n; ++i) { // the next hours
        float hx = r.left + cw * i + cw / 2;
        wchar_t lab[8];
        swprintf_s(lab, L"%02d:00", h.hours[i].hour);
        c.Text(lab, Rect(hx - 30, y, 60, 14), Weight::Regular, 10.5f, t.textDim, Align::Center);
        c.Icon(WxIcon(h.hours[i].code, h.hours[i].day), {hx, y + 28}, 20, t.text, true);
        c.Text(Deg(h.hours[i].tempC, h.fahrenheit), Rect(hx - 30, y + 42, 60, 16), Weight::Medium, 12, t.text, Align::Center);
    }
    y += 70;
    double lo = 1e9, hi = -1e9; // the days' bars share one scale
    for (int i = 0; i < h.nDays; ++i) lo = (std::min)(lo, h.days[i].loC), hi = (std::max)(hi, h.days[i].hiC);
    for (int i = 0; i < h.nDays && i < 5; ++i) {
        const Host::Day& d = h.days[i];
        SYSTEMTIME st{(WORD)d.y, (WORD)d.m, 0, (WORD)d.d};
        wchar_t day[16] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd", day, 16, nullptr);
        float ry = y + i * 30;
        c.Text(i == 0 ? L"Today" : day, Rect(r.left, ry, 110, 26), Weight::Medium, 12.5f, t.text);
        c.Icon(WxIcon(d.code, true), {r.left + 128, ry + 13}, 18, t.textDim, true);
        c.Text(WxText(d.code), Rect(r.left + 146, ry, 120, 26), Weight::Regular, 11.5f, t.textDim);
        c.Text(Deg(d.loC, h.fahrenheit), Rect(r.left + 270, ry, 40, 26), Weight::Regular, 12, t.textDim, Align::Right);
        D2D1_RECT_F lane = Rect(r.left + 320, ry + 11, Width(r) - 370, 5);
        c.Fill(lane, 2.5f, t.text.Alpha(0.1f));
        float k = Width(lane) / (float)(std::max)(1.0, hi - lo);
        c.Fill(Rect(lane.left + (float)(d.loC - lo) * k, lane.top, (std::max)(6.0f, (float)(d.hiC - d.loC) * k), 5), 2.5f, t.accentHi);
        c.Text(Deg(d.hiC, h.fahrenheit), Rect(r.right - 44, ry, 44, 26), Weight::Medium, 12, t.text, Align::Right);
    }
    y = top + kForecastH;
}

void CloseMatch(const Host& h) {
    OpenMatch().clear();
    if (h.resync) h.resync(h.ctx); // its feed is let go
}

void OpenGame(const sports::Game& g, const Host& h) {
    std::string id = sports::MatchId(g);
    if (id.empty()) return;
    openGame = g;
    OpenMatch() = id;
    matchScroll = 0;
    if (h.resync) h.resync(h.ctx); // its feed is wanted from now
    feeds::Refresh(id);
}

void Match(Ui& ui, const Theme& t, const D2D1_RECT_F& r, float y, float bottom, const Host& h) { // one game in full: its header, what's happened, its numbers, its lists
    Canvas& c = ui.C();
    constexpr float kBackW = 104, kTopH = 26, kHeadH = 58, kTitleH = 22, kEventH = 22, kWhenW = 46, kMarkW = 16, kStatH = 30, kBarH = 4, kRowH = 22, kPosW = 40, kGap = 12;
    constexpr size_t kRankChars = 4; // "12", "FP1", "Qual": a short first column
    const sports::Game& g = openGame;
    feeds::Series s = feeds::Get(OpenMatch());
    sports::Detail d = sports::DecodeDetail(s.rows);
    bool live = g.state == sports::kLive;
    Color red = Color::Rgb(kDown);
    if (w::Chip(ui, t, Ui::Id("fc.sp.back"), Rect(r.left, y, kBackW, kTopH), L"‹  All games", false)) {
        CloseMatch(h);
        return;
    }
    std::wstring status = d.status.empty() ? g.detail : d.status;
    c.Text(status, Rect(r.left + kBackW + 10, y, Width(r) - kBackW - 10, kTopH), Weight::Medium, 11.5f, live ? red : t.textDim, Align::Right);
    y += kTopH + 8;
    D2D1_RECT_F head = Rect(r.left, y, Width(r), kHeadH);
    c.Fill(head, 14, live ? red.Alpha(0.07f) : t.text.Alpha(0.04f));
    if (g.solo) {
        c.Text(g.a, Rect(head.left + 14, head.top + 9, Width(head) - 28, 20), Weight::Semi, 14, t.text);
        c.Text(g.b, Rect(head.left + 14, head.top + 31, Width(head) - 28, 18), Weight::Regular, 11.5f, t.textDim);
    } else { // the two sides across the score, as a scoreline reads
        std::wstring score = g.state == sports::kPre ? L"v" : g.as + L"  –  " + g.bs;
        float sw = c.Measure(score, Weight::Semi, 22) + 24, mid = (head.left + head.right) / 2;
        c.Text(score, Rect(mid - sw / 2, head.top, sw, kHeadH), Weight::Semi, 22, live ? red : t.text, Align::Center);
        c.Text(g.a, Rect(head.left + 12, head.top, mid - sw / 2 - head.left - 16, kHeadH), Weight::Semi, 13, t.text, Align::Right);
        c.Text(g.b, Rect(mid + sw / 2 + 4, head.top, head.right - 12 - mid - sw / 2 - 4, kHeadH), Weight::Semi, 13, t.text);
    }
    if (live) ui.KeepAlive();
    y += kHeadH + kGap;
    D2D1_RECT_F view = {r.left - 4, y, r.right + 4, bottom};
    if (!s.ok || (d.events.empty() && d.stats.empty() && d.blocks.empty())) {
        c.Text(!s.ok ? (s.error.empty() ? L"Fetching…" : s.error) : L"Nothing more to show for this game yet", Rect(r.left, y, Width(r), 20), Weight::Regular, 12, t.textDim);
        return;
    }
    float most = (std::max)(0.0f, matchH + kEndRoom - Height(view));
    if (float n = ui.Wheel(view); n != 0) matchScroll = (std::clamp)(matchScroll - n * 60, 0.0f, most);
    matchScroll = (std::min)(matchScroll, most);
    float shown = ui.Smooth(Ui::Id("fc.sp.mscroll"), matchScroll, 18), start = y;
    c.PushClip(view);
    float gy = y - shown;
    auto title = [&](const std::wstring& text) {
        c.Text(text, Rect(r.left, gy, Width(r), kTitleH - 4), Weight::Semi, 11, t.accentHi);
        gy += kTitleH;
    };
    if (!d.events.empty()) { // home's moments down the left, away's down the right, each at its minute
        title(L"Timeline");
        for (const sports::Detail::Event& e : d.events) {
            float cy = gy + kEventH / 2;
            bool right = e.side == 1;
            float markX = right ? r.right - kWhenW - kMarkW / 2 : r.left + kWhenW + kMarkW / 2;
            if (e.kind == L"yellow" || e.kind == L"red") c.Fill(Rect(markX - 3.5f, cy - 5, 7, 10), 1.5f, Color::Rgb(e.kind == L"red" ? 0xEF4444 : 0xFACC15));
            else c.Dot({markX, cy}, e.kind == L"goal" ? 4.0f : 3.0f, e.kind == L"goal" ? t.accentHi : t.textDim);
            Weight w = e.kind == L"goal" || e.kind == L"score" ? Weight::Medium : Weight::Regular;
            if (right) {
                c.Text(e.when, Rect(r.right - kWhenW + 6, gy, kWhenW - 6, kEventH), Weight::Medium, 11, t.textDim);
                c.Text(e.text, Rect(r.left, gy, markX - kMarkW / 2 - 4 - r.left, kEventH), w, 12, t.text, Align::Right);
            } else {
                c.Text(e.when, Rect(r.left, gy, kWhenW - 6, kEventH), Weight::Medium, 11, t.textDim, Align::Right);
                c.Text(e.text, Rect(markX + kMarkW / 2 + 4, gy, r.right - markX - kMarkW / 2 - 4, kEventH), w, 12, t.text);
            }
            gy += kEventH;
        }
        gy += kGap;
    }
    if (!d.stats.empty()) { // each number side by side, a bar between them for who had more
        title(L"Stats");
        for (const sports::Detail::Stat& st : d.stats) {
            c.Text(st.a, Rect(r.left, gy, Width(r) / 3, 18), Weight::Semi, 12, t.text);
            c.Text(st.label, Rect(r.left + Width(r) / 3, gy, Width(r) / 3, 18), Weight::Regular, 11, t.textDim, Align::Center);
            c.Text(st.b, Rect(r.right - Width(r) / 3, gy, Width(r) / 3, 18), Weight::Semi, 12, t.text, Align::Right);
            double a = _wtof(st.a.c_str()), b = _wtof(st.b.c_str());
            if (a + b > 0) {
                float split = (float)(a / (a + b)) * Width(r), barY = gy + 20;
                c.Fill(Rect(r.left, barY, (std::max)(split - 1, 0.0f), kBarH), kBarH / 2, t.accent);
                c.Fill(Rect(r.left + split + 1, barY, (std::max)(Width(r) - split - 1, 0.0f), kBarH), kBarH / 2, t.text.Alpha(0.18f));
            }
            gy += kStatH;
        }
        gy += kGap;
    }
    for (const sports::Detail::Block& b : d.blocks) { // the running order, the boards, the matches, the leaders
        if (b.rows.empty()) continue;
        title(b.title);
        bool ranked = std::all_of(b.rows.begin(), b.rows.end(), [&](const sports::Detail::Row& row) { return row.left.size() <= kRankChars; }); // the whole list one way
        for (const sports::Detail::Row& row : b.rows) {
            Color midInk = row.live ? red : t.text;
            if (ranked) { // a position or a session: then who, then their number
                c.Text(row.left, Rect(r.left, gy, kPosW, kRowH), Weight::Semi, 11.5f, t.textDim);
                c.Text(row.mid, Rect(r.left + kPosW, gy, Width(r) * 0.6f, kRowH), Weight::Medium, 12, midInk);
                c.Text(row.right, Rect(r.left + kPosW + Width(r) * 0.6f, gy, Width(r) * 0.4f - kPosW, kRowH), Weight::Regular, 11.5f, t.textDim, Align::Right);
            } else { // two sides across what's between them
                float side = Width(r) * 0.4f;
                c.Text(row.left, Rect(r.left, gy, side, kRowH), Weight::Medium, 11.5f, t.text);
                c.Text(row.mid, Rect(r.left + side, gy, Width(r) - 2 * side, kRowH), Weight::Semi, 11.5f, row.live ? red : t.textDim, Align::Center);
                c.Text(row.right, Rect(r.right - side, gy, side, kRowH), Weight::Medium, 11.5f, t.text, Align::Right);
            }
            gy += kRowH;
        }
        gy += kGap;
    }
    matchH = gy + shown - start; // how tall it all is, for the next frame's scroll
    c.PopClip();
    if (most > 0) {
        float lane = Height(view) - 8, thumb = lane * Height(view) / (Height(view) + most);
        c.Fill(Rect(r.right + 2, view.top + 4 + (lane - thumb) * shown / most, 3, thumb), 1.5f, t.text.Alpha(0.18f));
    }
}

void Games(Ui& ui, const Theme& t, const D2D1_RECT_F& r, float& y, const Host& h, bool scores) { // scores: what's on and what's done, in full; fixtures: a few results, what's on, and what's coming, each to the calendar
    Canvas& c = ui.C();
    constexpr float kTabH = 28, kTabW = 96, kSecH = 20;
    constexpr size_t kResults = 4;
    float bottom = r.bottom - 4; // exactly the room the open card has: its lists scroll inside it, never past it
    if (w::Chip(ui, t, Ui::Id("fc.sp.games"), Rect(r.left, y, kTabW, kTabH), L"Games", sportTab == 0)) sportTab = 0;
    if (w::Chip(ui, t, Ui::Id("fc.sp.leagues"), Rect(r.left + kTabW + 8, y, kTabW, kTabH), L"Leagues", sportTab == 1)) {
        sportTab = 1;
        if (!OpenMatch().empty()) CloseMatch(h);
    }
    std::vector<std::string> followed = sports::Followed();
    std::wstring which;
    for (const std::string& l : followed)
        if (const sports::League* lg = sports::Find(l)) which += (which.empty() ? L"" : L"  ·  ") + std::wstring(lg->name);
    c.Text(which, Rect(r.left + 2 * kTabW + 24, y, Width(r) - 2 * kTabW - 24, kTabH), Weight::Regular, 11, t.textDim, Align::Right);
    y += kTabH + 10;
    if (sportTab == 1) { // the most followed first, then the sports and the chosen one's leagues: up to three followed
        static const char* const kPopular[] = {"racing/f1", "soccer/eng.1", "soccer/eng.w.1", "basketball/nba", "basketball/wnba", "football/nfl", "soccer/uefa.champions", "tennis/atp", "tennis/wta"};
        int flipPop = -1;
        D2D1_RECT_F view = {r.left - 4, y, r.right + 4, bottom}; // it scrolls: football alone fills rows
        float most = (std::max)(0.0f, leagueH + kEndRoom - Height(view));
        if (float n = ui.Wheel(view); n != 0) leagueScroll = (std::clamp)(leagueScroll - n * 60, 0.0f, most);
        leagueScroll = (std::min)(leagueScroll, most);
        float startY = y, shownScroll = ui.Smooth(Ui::Id("fc.sp.lscroll"), leagueScroll, 18);
        c.PushClip(view);
        y -= shownScroll;
        float x = r.left;
        c.Text(L"Popular", Rect(r.left, y, Width(r), 16), Weight::Semi, 11, t.accentHi);
        y += 20;
        for (int p = 0; p < (int)std::size(kPopular); ++p) {
            const sports::League* lg = sports::Find(kPopular[p]);
            if (!lg) continue;
            bool on = std::find(followed.begin(), followed.end(), lg->path) != followed.end();
            float w = c.Measure(lg->name, Weight::Medium, 11.5f) + 22;
            if (x + w > r.right) x = r.left, y += 30;
            if (w::Chip(ui, t, Ui::Id("fc.sp.pop", p), Rect(x, y, w, 26), lg->name, on)) flipPop = p;
            x += w + 6;
        }
        y += 36;
        x = r.left;
        for (int s = 0; s < sports::kSports; ++s) {
            float w = c.Measure(sports::SportName(s), Weight::Medium, 11.5f) + 22;
            if (x + w > r.right) x = r.left, y += 30;
            if (w::Chip(ui, t, Ui::Id("fc.sp.sport", s), Rect(x, y, w, 26), sports::SportName(s), sportPick == s)) sportPick = s;
            x += w + 6;
        }
        y += 38;
        c.Text(std::to_wstring(followed.size()) + L" of 3 followed. Tap a league to follow it or let it go", Rect(r.left, y, Width(r), 16), Weight::Regular, 11, t.textDim);
        y += 22;
        x = r.left;
        int flip = -1, i = 0;
        for (const sports::League& lg : sports::All()) {
            ++i;
            if (lg.sport != sportPick) continue;
            bool on = std::find(followed.begin(), followed.end(), lg.path) != followed.end();
            float w = c.Measure(lg.name, Weight::Medium, 11.5f) + 22;
            if (x + w > r.right) x = r.left, y += 30;
            if (w::Chip(ui, t, Ui::Id("fc.sp.league", i), Rect(x, y, w, 26), lg.name, on)) flip = i - 1;
            x += w + 6;
        }
        leagueH = y + 30 + shownScroll - startY; // how tall it all is, for the next frame's scroll
        c.PopClip();
        if (most > 0) {
            float lane = Height(view) - 8, thumb = lane * Height(view) / (Height(view) + most);
            c.Fill(Rect(r.right + 2, view.top + 4 + (lane - thumb) * shownScroll / most, 3, thumb), 1.5f, t.text.Alpha(0.18f));
        }
        y = bottom;
        if (flip < 0 && flipPop < 0) return;
        std::string path = flipPop >= 0 ? kPopular[flipPop] : sports::All()[(size_t)flip].path;
        auto at = std::find(followed.begin(), followed.end(), path);
        if (at != followed.end()) followed.erase(at);
        else {
            if (followed.size() >= sports::kMaxFollowed) followed.erase(followed.begin()); // full: the longest followed makes room
            followed.push_back(path);
        }
        sports::SetFollowed(followed);
        OpenMatch().clear(); // its league may be gone
        if (h.resync) h.resync(h.ctx); // the new leagues are fetched
        for (const std::string& l : followed) feeds::Refresh(sports::FeedId(l)), feeds::Refresh(sports::NextId(l));
        return;
    }
    std::vector<sports::Game> live, done, next;
    auto seen = [&](const std::vector<sports::Game>& in, const sports::Game& g) {
        return std::any_of(in.begin(), in.end(), [&](const sports::Game& o) { return o.a == g.a && o.b == g.b && o.when == g.when; });
    };
    std::map<std::wstring, std::wstring> leagueOf; // a game's league, by its sides: the calendar event says it
    for (const std::string& l : followed) {
        const sports::League* lg = sports::Find(l);
        for (const std::string& id : {sports::FeedId(l), sports::NextId(l)})
            for (const std::wstring& row : feeds::Get(id).rows) {
                sports::Game g = sports::Decode(row);
                g.path = l;
                std::vector<sports::Game>& to = g.state == sports::kLive ? live : g.state == sports::kFinal ? done : next;
                if (seen(to, g)) continue;
                leagueOf[g.a + L"|" + g.b] = lg ? lg->name : L"";
                to.push_back(std::move(g));
            }
    }
    live = sports::Order(std::move(live)), done = sports::Order(std::move(done)), next = sports::Order(std::move(next));
    if (!OpenMatch().empty()) { // a game opened: it alone, in full, its score kept fresh from the lists
        for (const auto* list : {&live, &done, &next})
            for (const sports::Game& g : *list)
                if (sports::MatchId(g) == OpenMatch()) openGame = g;
        Match(ui, t, r, y, bottom, h);
        y = bottom;
        return;
    }
    if (scores) next.clear(); // the scores' panel is about the scores: the fixtures have their own
    else if (done.size() > kResults) done.resize(kResults);
    if (live.empty() && done.empty() && next.empty()) {
        feeds::Series s = followed.empty() ? feeds::Series{} : feeds::Get(sports::FeedId(followed[0]));
        c.Text(followed.empty() ? L"Pick up to three leagues under Leagues" : s.ok ? L"Nothing played lately, nothing coming up" : s.error.empty() ? L"Fetching…" : s.error, Rect(r.left, y, Width(r), 20), Weight::Regular, 12.5f, t.textDim);
        y = bottom;
        return;
    }
    // every game as a soft card in one list that scrolls: live ones lit and breathing, the score on a pill, the coming ones with a way into the calendar
    constexpr float kCardGap = 6, kPillH = 24, kCalPillW = 96, kLineCardH = 40, kScoreCardH = 66;
    const float kCardH = scores ? kScoreCardH : kLineCardH; // the scores in full: each side on its own line
    float total = 0;
    for (const auto* list : {&live, &done, &next})
        if (!list->empty()) total += kSecH + list->size() * (kCardH + kCardGap) + 4;
    D2D1_RECT_F view = {r.left - 4, y, r.right + 4, bottom};
    float most = (std::max)(0.0f, total + kEndRoom - Height(view));
    if (float n = ui.Wheel(view); n != 0) gameScroll = (std::clamp)(gameScroll - n * (kCardH + kCardGap) * 1.5f, 0.0f, most);
    gameScroll = (std::min)(gameScroll, most);
    float shown = ui.Smooth(Ui::Id("fc.sp.scroll"), gameScroll, 18);
    c.PushClip(view);
    float gy = y - shown, pulse = 0.5f + 0.5f * std::sin((float)ui.Now() * 3.2f);
    int added = -1, cardN = 0;
    const sports::Game* opened = nullptr;
    auto section = [&](const wchar_t* title, const std::vector<sports::Game>& games, bool isLive, bool calendar) {
        if (games.empty()) return;
        c.Text(title, Rect(r.left, gy, Width(r), kSecH - 4), Weight::Semi, 11, isLive ? Color::Rgb(kDown) : t.accentHi);
        gy += kSecH;
        for (size_t i = 0; i < games.size(); ++i, gy += kCardH + kCardGap) {
            ++cardN;
            if (gy + kCardH < view.top || gy > view.bottom) continue; // out of the view: not drawn, not hit
            const sports::Game& g = games[i];
            D2D1_RECT_F card = Rect(r.left, gy, Width(r), kCardH);
            float hov = 0;
            if (g.state != sports::kPre && !sports::MatchId(g).empty()) { // on or done: it opens in full
                Ui::Hit open = ui.Button(Ui::Id("fc.sp.open", cardN), card);
                ui.Name(L"Open game");
                hov = ui.Smooth(Ui::Id("fc.sp.openh", cardN), open.hover ? 1.0f : 0, 16);
                if (open.clicked) opened = &g;
            }
            c.Fill(card, 12, isLive ? Color::Rgb(kDown).Alpha(0.07f + 0.05f * hov) : t.text.Alpha(0.04f + 0.04f * hov));
            float cy = gy + kCardH / 2, right = card.right - 12;
            if (calendar) {
                D2D1_RECT_F pill = Rect(right - kCalPillW, cy - kPillH / 2, kCalPillW, kPillH);
                bool isIn = sports::InCalendar(g); // added already: the same pill takes it back out
                Ui::Hit hit = ui.Button(Ui::Id("fc.sp.cal", (int)i), pill);
                ui.Name(isIn ? L"Remove from calendar" : L"Add to calendar");
                c.Fill(pill, kPillH / 2, isIn ? t.text.Alpha(hit.hover ? 0.16f : 0.08f) : t.accent.Alpha(hit.hover ? 0.3f : 0.16f));
                c.Icon(isIn ? glyph::Delete : glyph::Calendar, {pill.left + 14, cy}, 13, isIn ? t.textDim : t.accentHi);
                c.Text(isIn ? L"Remove" : L"Add", Rect(pill.left + 24, pill.top, kCalPillW - 30, kPillH), Weight::Medium, 11.5f, isIn ? t.textDim : t.accentHi, Align::Center);
                if (hit.clicked) added = (int)i;
                right = pill.left - 10;
            }
            if (!scores || g.solo) { // the scores' own cards say it under them
                float detailW = c.Measure(g.detail, Weight::Medium, 11.5f) + 8;
                c.Text(g.detail, Rect(right - detailW, gy, detailW, kCardH), Weight::Medium, 11.5f, isLive ? Color::Rgb(kDown) : t.textDim, Align::Right);
                right -= detailW + 10;
            }
            float x = card.left + 14;
            if (isLive) { // a breathing dot: it's on now
                c.Dot({x + 3, cy}, 3 + 1.2f * pulse, Color::Rgb(kDown).Alpha(0.6f + 0.4f * pulse));
                x += 14;
                ui.KeepAlive();
            }
            if (g.solo) {
                c.Text(g.a, Rect(x, gy + 4, right - x, 18), Weight::Semi, 12.5f, t.text);
                c.Text(g.b, Rect(x, gy + 20, right - x, 16), Weight::Regular, 11, t.textDim);
                continue;
            }
            if (scores) { // each side on its own line, its score large at the right, its periods beside it; the clock and the ground under
                bool aWon = g.state == sports::kFinal && _wtoi(g.as.c_str()) > _wtoi(g.bs.c_str()), bWon = g.state == sports::kFinal && _wtoi(g.bs.c_str()) > _wtoi(g.as.c_str());
                float scoreX = card.right - 14 - 60, linesW = 150;
                auto side = [&](const std::wstring& name, const std::wstring& score, const std::wstring& lines, float ly, bool won) {
                    c.Text(name, Rect(x, ly, scoreX - linesW - x - 8, 20), won ? Weight::Semi : Weight::Medium, 13, t.text);
                    c.Text(lines, Rect(scoreX - linesW, ly, linesW, 20), Weight::Regular, 11, t.textLow, Align::Right);
                    c.Text(score, Rect(scoreX, ly - 2, 60, 22), Weight::Semi, 16, isLive ? Color::Rgb(kDown) : won ? t.text : t.textDim, Align::Right);
                };
                side(g.a, g.as, g.aLines, gy + 6, aWon);
                side(g.b, g.bs, g.bLines, gy + 26, bWon);
                std::wstring foot = g.venue.empty() ? g.detail : g.detail + L"  ·  " + g.venue;
                c.Text(foot, Rect(x, gy + 46, card.right - 14 - x, 16), Weight::Regular, 10.5f, isLive ? Color::Rgb(kDown) : t.textLow);
                continue;
            }
            std::wstring score = g.state == sports::kPre ? L"v" : g.as + L"  –  " + g.bs;
            float sw = c.Measure(score, Weight::Semi, 13) + 22, mid = (x + right) / 2;
            D2D1_RECT_F sp = Rect(mid - sw / 2, cy - kPillH / 2, sw, kPillH);
            if (g.state != sports::kPre) c.Fill(sp, kPillH / 2, isLive ? Color::Rgb(kDown).Alpha(0.16f) : t.text.Alpha(0.08f));
            c.Text(score, sp, Weight::Semi, 13, isLive ? Color::Rgb(kDown) : g.state == sports::kPre ? t.textLow : t.text, Align::Center);
            c.Text(g.a, Rect(x, gy, sp.left - x - 10, kCardH), Weight::Medium, 12.5f, t.text, Align::Right);
            c.Text(g.b, Rect(sp.right + 10, gy, right - sp.right - 10, kCardH), Weight::Medium, 12.5f, t.text);
        }
        gy += 4;
    };
    section(L"Live now", live, true, false);
    section(L"Latest results", done, false, false);
    section(L"Coming up", next, false, true);
    c.PopClip();
    if (most > 0) { // a slim thumb: there's more below
        float lane = Height(view) - 8, thumb = lane * Height(view) / (Height(view) + most);
        c.Fill(Rect(r.right + 2, view.top + 4 + (lane - thumb) * shown / most, 3, thumb), 1.5f, t.text.Alpha(0.18f));
    }
    if (added >= 0) {
        const sports::Game& g = next[(size_t)added];
        if (sports::InCalendar(g)) sports::RemoveFromCalendar(g);
        else sports::AddToCalendar(g, leagueOf[g.a + L"|" + g.b]);
    }
    if (opened) OpenGame(*opened, h);
    y = bottom;
}

float DetailH(int kind) {
    auto n = [](size_t c) { return (float)(std::max)((size_t)1, (std::min)(c, (size_t)kMaxRows)) * kRow; };
    std::string key = Info(kind).key;
    if (key == "stock.one" || key == "stock.chart" || key == "stock.amex") return kQuoteH;
    if (key == "stock.us") return 6 * kMarketRowH; // a scrolling list: this much shows at once
    if (key == "stock.watch") return 3 * kMarketRowH;
    if (key.rfind("wx.", 0) == 0 || key == "clock.wx" || key == "clock.today") return kForecastH;
    if (key.rfind("sport.", 0) == 0) return kSportsH;
    if (key == "time.screen" || key == "time.timeline") return 86 + kMaxRows * kRow * 0.75f;
    if (key == "time.stopwatch") return 44 + n(state::WatchLaps().size());
    if (key == "time.timer") return 44;
    if (key == "list.notes") return 180;
    if (key == "list.habits") return n(state::Habits().size() + 1) + 8;
    if (key == "list.water") return 44;
    if (key == "sys.bright") return 40;
    if (key == "files.commands" || key == "files.websearch") return kRow + 8;
    if (key == "web.gmail") return n(feeds::Get("gmail").rows.size());
    if (key == "sys.drives" || key == "files.recent" || key == "files.shots" || key == "files.downloads" || key == "files.documents" || key == "files.wallpaper") return n(rows.size());
    return 0;
}

void SearchOrOpen(const std::wstring& what) {
    if (GetFileAttributesW(what.c_str()) != INVALID_FILE_ATTRIBUTES) return Open(what); // a path: open it
    std::wstring q;
    wchar_t b[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, what.c_str(), (int)what.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, what.c_str(), (int)what.size(), u.data(), n, nullptr, nullptr);
    for (unsigned char c : u) {
        if (isalnum(c)) q += (wchar_t)c;
        else swprintf_s(b, L"%%%02X", c), q += b;
    }
    Open(L"https://duckduckgo.com/?q=" + q);
}

}

void OpenPanel(int kind) {
    panelKind = kind;
    editing = -1;
    Wipe(typed);
    draft = Is(kind, "list.notes") ? feeds::Setting("notes") : L"";
    rows.clear();
    std::string key = Info(kind).key;
    if (key == "files.recent") ListFolder(Known(FOLDERID_Recent), false);
    else if (key == "files.shots") ListFolder(Known(FOLDERID_Pictures, L"\\Screenshots"), true);
    else if (key == "files.downloads") ListFolder(Known(FOLDERID_Downloads), false);
    else if (key == "files.documents") ListFolder(Known(FOLDERID_Documents), false);
    else if (key == "files.wallpaper") ListWallpapers();
    else if (key == "sys.drives") {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
            UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
            ULARGE_INTEGER freeBytes{}, total{};
            UINT was = SetErrorMode(SEM_FAILCRITICALERRORS); // an empty card reader stays quiet
            bool ok = GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr) != FALSE;
            SetErrorMode(was);
            if (!ok) continue;
            wchar_t b[64];
            swprintf_s(b, L"%lc:   %.0f GB free of %.0f GB", root[0], freeBytes.QuadPart / 1073741824.0, total.QuadPart / 1073741824.0);
            rows.push_back({b, root});
        }
    }
}

void ClosePanel() {
    if (panelKind >= 0) {
        if (Is(panelKind, "list.notes")) feeds::SetSetting("notes", draft);
        if (editing >= 0) Save(SetupFor(Info(panelKind).key));
    }
    Wipe(typed);
    draft.clear();
    rows.clear();
    panelKind = -1;
    editing = -1;
}

float PanelH(int kind, float) {
    const Setup* s = SetupFor(Info(kind).key);
    return kHead + 10 + DetailH(kind) + FieldCount(s) * kFieldH + (s && s->note ? kNoteH : 0) + 16;
}

bool WantsKeys(int kind) {
    return FieldCount(SetupFor(Info(kind).key)) > 0 || Is(kind, "list.notes") || Is(kind, "list.habits") || Is(kind, "files.commands") || Is(kind, "files.websearch");
}

bool Char(int kind, wchar_t ch) {
    const Setup* s = SetupFor(Info(kind).key);
    std::wstring* target = editing >= 0 ? &typed : (Is(kind, "list.notes") || Is(kind, "list.habits") || Is(kind, "files.commands") || Is(kind, "files.websearch")) ? &draft : nullptr;
    if (ch == VK_ESCAPE) {
        if (editing < 0) return false; // nothing being typed: the host closes the panel
        Wipe(typed);
        editing = -1;
        return true;
    }
    if (!target) return false;
    if (ch == VK_BACK) {
        if (!target->empty()) target->pop_back();
    } else if (ch == VK_RETURN) {
        if (editing >= 0) {
            Save(s);
        } else if (Is(kind, "list.notes")) {
            *target += L'\n';
        } else if (draft.empty()) {
            return true;
        } else if (Is(kind, "list.habits")) {
            state::HabitAdd(draft), draft.clear();
        } else if (Is(kind, "files.commands")) {
            Open(draft), draft.clear();
        } else if (Is(kind, "files.websearch")) {
            SearchOrOpen(draft), draft.clear();
        }
    } else if (ch >= 32 && target->size() < kMaxTyped) {
        *target += ch;
    }
    return true;
}

Act Click(int kind, bool right, const Host& h) {
    const Meta& m = Info(kind);
    std::string key = m.key;
    if (key == "time.timer") {
        if (right) return Act::kPanel;
        state::TimerToggle();
        return Act::kNone;
    }
    if (key == "time.stopwatch") {
        if (right) return Act::kPanel;
        state::WatchToggle();
        return Act::kNone;
    }
    if (key == "list.water") {
        state::WaterAdd(right ? -1 : 1);
        return Act::kNone;
    }
    if (key == "sys.lang") {
        state::NextLanguage();
        return Act::kNone;
    }
    if (key == "sys.theme") {
        state::SetLightTheme(!state::LightTheme());
        return Act::kNone;
    }
    if (key == "sys.tray") return Act::kTray;
    if (key == "sys.cpu" || key == "sys.ram" || key == "sys.temp") {
        Open(L"taskmgr.exe");
        return Act::kNone;
    }
    if (key == "sys.bright") return h.bright >= 0 ? Act::kPanel : Act::kNone;
    if (key == "sys.storage") {
        Open(L"ms-settings:storagesense");
        return Act::kNone;
    }
    if (key == "media.mini" || key == "media.compact") return right ? Act::kNext : Act::kPlay;
    if (key == "media.bar") return right ? Act::kPlay : Act::kNext;
    if (key == "files.wallpaper" && !right) {
        NextWallpaper();
        return Act::kNone;
    }
    if (key == "files.photo") {
        if (right) state::NextPhoto();
        else Open(Known(FOLDERID_Pictures));
        return Act::kNone;
    }
    if (key == "files.emoji") {
        INPUT in[4] = {};
        for (INPUT& i : in) i.type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[3].ki.wVk = VK_LWIN;
        in[1].ki.wVk = in[2].ki.wVk = VK_OEM_PERIOD;
        in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        return Act::kNone;
    }
    bool hasPanel = FieldCount(SetupFor(m.key)) > 0 || key == "time.screen" || key == "time.timeline" || key == "list.notes" || key == "list.habits" || key == "sys.drives" || key.rfind("files.", 0) == 0 ||
                    key.rfind("stock.", 0) == 0 || key.rfind("sport.", 0) == 0 || key.rfind("wx.", 0) == 0 || key == "clock.wx" || key == "clock.today"; // markets, games and the forecast open in full
    return hasPanel ? Act::kPanel : Act::kNone;
}

void Panel(Ui& ui, const Theme& t, int kind, const D2D1_RECT_F& r, const Host& h) {
    Canvas& c = ui.C();
    const Meta& m = Info(kind);
    std::string key = m.key;
    resyncHost = h.resync, resyncCtx = h.ctx;
    c.Text(m.name, Rect(r.left, r.top - 2, Width(r), 18), Weight::Semi, 13, t.text);
    float y = r.top + kHead;
    bool caret = ((int)(ui.Now() * 2) & 1) == 0;
    auto list = [&](const wchar_t* empty, auto click) {
        if (rows.empty()) {
            c.Text(empty, Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.textDim);
            y += kRow;
            return;
        }
        int picked = -1;
        for (size_t i = 0; i < rows.size() && i < (size_t)kMaxRows; ++i, y += kRow) {
            D2D1_RECT_F row = Rect(r.left - 6, y, Width(r) + 12, kRow - 2);
            Ui::Hit hit = ui.Button(Ui::Id("fc.row", (int)i), row);
            if (hit.hover) c.Fill(row, 10, t.text.Alpha(0.07f));
            c.Text(rows[i].name, Inset(row, 10, 0), Weight::Regular, 12, t.text);
            if (hit.clicked) picked = (int)i;
        }
        if (picked >= 0) click(rows[(size_t)picked]);
    };
    auto typedRow = [&](const wchar_t* hint) {
        D2D1_RECT_F f = Rect(r.left - 6, y, Width(r) + 12, kRow + 2);
        c.Fill(f, 10, t.text.Alpha(0.06f));
        if (draft.empty()) c.Text(hint, Inset(f, 12, 0), Weight::Regular, 12, t.textLow);
        c.Text(draft + (caret ? L"│" : L""), Inset(f, 12, 0), Weight::Regular, 12, t.text);
        ui.KeepAlive(); // the caret blinks
        y += kRow + 8;
    };
    if (key == "time.screen" || key == "time.timeline") {
        feeds::Screen s = feeds::ScreenTime();
        c.Text(L"Today " + Hours(s.today) + L"   ·   This week " + Hours(s.week), Rect(r.left, y, Width(r), 16), Weight::Medium, 11.5f, t.textDim);
        y += 22;
        int top = 1;
        for (int v : s.hours) top = (std::max)(top, v);
        float bw = Width(r) / 24;
        for (int i = 0; i < 24; ++i) {
            float bh = (std::max)(2.0f, 44.0f * s.hours[i] / top);
            c.Fill(Rect(r.left + bw * i + 1, y + 44 - bh, bw - 2, bh), 2, s.hours[i] ? t.accentHi : t.text.Alpha(0.08f));
        }
        y += 56;
        float rh = kRow * 0.75f;
        for (size_t i = 0; i < s.apps.size() && i < (size_t)kMaxRows; ++i, y += rh) {
            c.Text(s.apps[i].app, Rect(r.left, y, Width(r) * 0.6f, rh), Weight::Regular, 11.5f, t.text);
            c.Text(Hours(s.apps[i].seconds), Rect(r.left, y, Width(r), rh), Weight::Medium, 11.5f, t.textDim, Align::Right);
        }
    } else if (key == "time.timer") {
        if (w::Chip(ui, t, Ui::Id("fc.timer.go"), Rect(r.left, y, 90, 28), state::TimerRunning() ? L"Pause" : L"Start", state::TimerRunning())) state::TimerToggle();
        if (w::Chip(ui, t, Ui::Id("fc.timer.reset"), Rect(r.left + 98, y, 90, 28), L"Reset", false)) state::TimerReset();
        y += 44;
    } else if (key == "time.stopwatch") {
        if (w::Chip(ui, t, Ui::Id("fc.sw.go"), Rect(r.left, y, 80, 28), state::WatchRunning() ? L"Pause" : L"Start", state::WatchRunning())) state::WatchToggle();
        if (w::Chip(ui, t, Ui::Id("fc.sw.lap"), Rect(r.left + 88, y, 80, 28), L"Lap", false)) state::WatchLap();
        if (w::Chip(ui, t, Ui::Id("fc.sw.reset"), Rect(r.left + 176, y, 80, 28), L"Reset", false)) state::WatchReset();
        y += 44;
        const auto& laps = state::WatchLaps();
        for (size_t i = laps.size(); i-- > 0 && laps.size() - i <= (size_t)kMaxRows; y += kRow) {
            wchar_t b[48];
            swprintf_s(b, L"Lap %zu   %d:%05.2f", i + 1, (int)laps[i] / 60, std::fmod(laps[i], 60.0));
            c.Text(b, Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.text);
        }
    } else if (key == "list.notes") {
        D2D1_RECT_F box = Rect(r.left - 4, y, Width(r) + 8, 172);
        c.Fill(box, 12, t.text.Alpha(0.05f));
        std::wstring shown = draft + (caret ? L"│" : L" ");
        ComPtr<IDWriteTextLayout> lay;
        if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), gfx::TextFmt(Weight::Regular, 12.5f, Align::Left), Width(box) - 20, Height(box) - 16, &lay))) {
            ComPtr<ID2D1SolidColorBrush> ink;
            c.Target()->CreateSolidColorBrush(t.text.D2D(), &ink);
            if (ink) c.Target()->DrawTextLayout({box.left + 10, box.top + 8}, lay.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        ui.KeepAlive(); // the caret blinks
        y += 180;
    } else if (key == "list.habits") {
        auto habits = state::Habits();
        int today = state::Today();
        int flip = -1, drop = -1;
        for (size_t i = 0; i < habits.size() && i < (size_t)kMaxRows - 1; ++i, y += kRow) {
            D2D1_RECT_F row = Rect(r.left - 6, y, Width(r) + 12, kRow - 2);
            Ui::Hit hit = ui.Button(Ui::Id("fc.habit", (int)i), row);
            if (hit.hover) c.Fill(row, 10, t.text.Alpha(0.07f));
            bool done = habits[i].last == today;
            D2D1_POINT_2F o = {row.left + 16, (row.top + row.bottom) / 2};
            c.Fill(Rect(o.x - 8, o.y - 8, 16, 16), 5, done ? t.accent : t.text.Alpha(0.1f));
            if (done) c.Icon(glyph::Check, o, 13, t.onAccent);
            c.Text(habits[i].name, Rect(row.left + 32, row.top, Width(row) - 110, Height(row)), Weight::Regular, 12, t.text);
            c.Text(std::to_wstring(habits[i].streak) + L" days", Rect(row.left, row.top, Width(row) - 10, Height(row)), Weight::Semi, 11, t.accentHi, Align::Right);
            if (hit.clicked) flip = (int)i;
            if (ui.In().rclick && Inside(row, ui.In().mouse)) drop = (int)i;
        }
        typedRow(L"Name a new habit and press Enter");
        if (flip >= 0) state::HabitFlip((size_t)flip);
        if (drop >= 0) state::HabitDrop((size_t)drop);
    } else if (key == "list.water") {
        if (w::Chip(ui, t, Ui::Id("fc.water.less"), Rect(r.left, y, 90, 28), L"One less", false)) state::WaterAdd(-1);
        if (w::Chip(ui, t, Ui::Id("fc.water.more"), Rect(r.left + 98, y, 90, 28), L"One more", true)) state::WaterAdd(1);
        y += 44;
    } else if (key == "sys.bright") {
        float v = h.bright / 100.0f;
        D2D1_RECT_F rail = Rect(r.left, y + 10, Width(r), 6);
        constexpr float kThumb = 7;
        if (ui.Slider(Ui::Id("fc.bright"), Inset(rail, kThumb, -10), v, false) && h.setBright) h.setBright(h.ctx, (int)std::lround(v * 100));
        c.Fill(rail, 3, t.text.Alpha(0.14f));
        c.Fill(Rect(rail.left, rail.top, (std::max)(6.0f, Width(rail) * v), 6), 3, t.accentHi);
        c.Dot({w::ThumbAt(rail.left, rail.right, kThumb, v), rail.top + 3}, kThumb, t.text);
        y += 40;
    } else if (key == "files.commands") {
        typedRow(L"A program, a folder or a link, then Enter");
    } else if (key == "files.websearch") {
        typedRow(L"Search the web, or a folder path, then Enter");
    } else if (key == "web.gmail") {
        feeds::Series s = feeds::Get("gmail");
        for (size_t i = 0; i < s.rows.size() && i < (size_t)kMaxRows; ++i, y += kRow) c.Text(s.rows[i], Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.text);
    } else if (key == "files.recent" || key == "files.shots" || key == "files.downloads" || key == "files.documents" || key == "sys.drives") {
        list(L"Nothing here yet", [](const RowItem& it) { Open(it.path); });
    } else if (key == "files.wallpaper") {
        list(L"No pictures in Pictures\\Wallpapers or Pictures", [](const RowItem& it) { SetWallpaper(it.path); });
    } else if (key == "stock.one" || key == "stock.chart") {
        std::wstring tick = feeds::Setting("stock.id");
        QuoteDetail(c, t, r, y, tick.empty() ? L"AAPL" : tick, nullptr);
    } else if (key == "stock.amex") {
        QuoteDetail(c, t, r, y, kAmexTick, L"NYSE American");
    } else if (key == "stock.us") { // every market it knows and every ticker added, scrolling under the field to add more
        std::vector<std::pair<std::wstring, std::wstring>> list;
        for (const Market& k : UsMarkets()) list.push_back({k.label, k.tick});
        for (const std::wstring& one : UsMoreTickers()) list.push_back({one, one});
        D2D1_RECT_F view = {r.left - 4, y, r.right + 4, r.bottom - kFieldH - kNoteH - 6};
        float most = (std::max)(0.0f, list.size() * kMarketRowH + kEndRoom - Height(view));
        if (float n = ui.Wheel(view); n != 0) marketScroll = (std::clamp)(marketScroll - n * kMarketRowH, 0.0f, most);
        marketScroll = (std::min)(marketScroll, most);
        float shown = ui.Smooth(Ui::Id("fc.mk.scroll"), marketScroll, 18);
        c.PushClip(view);
        float ry = y - shown;
        MarketRows(c, t, {r.left, view.top, r.right, view.bottom}, ry, list);
        c.PopClip();
        if (most > 0) { // a slim thumb: there's more below
            float lane = Height(view) - 8, thumb = lane * Height(view) / (Height(view) + most);
            c.Fill(Rect(r.right + 2, view.top + 4 + (lane - thumb) * shown / most, 3, thumb), 1.5f, t.text.Alpha(0.18f));
        }
        y = view.bottom + 6;
    } else if (key == "stock.watch") {
        std::wstring all = feeds::Setting("stock.watch");
        if (all.empty()) all = L"AAPL,MSFT,NVDA";
        std::vector<std::pair<std::wstring, std::wstring>> list;
        for (size_t a = 0; a < all.size() && list.size() < 3;) {
            size_t e = (std::min)(all.find(L',', a), all.size());
            if (std::wstring one = all.substr(a, e - a); !one.empty()) list.push_back({one, one});
            a = e + 1;
        }
        MarketRows(c, t, r, y, list);
    } else if (key.rfind("wx.", 0) == 0 || key == "clock.wx" || key == "clock.today") {
        Forecast(c, t, r, y, h);
    } else if (key.rfind("sport.", 0) == 0) {
        Games(ui, t, r, y, h, key == "sport.scores");
    }

    const Setup* s = SetupFor(m.key);
    int n = FieldCount(s);
    for (int i = 0; i < n; ++i, y += kFieldH) {
        const Field& f = s->fields[i];
        bool on = editing == i;
        c.Text(f.label, Rect(r.left, y, Width(r), 14), Weight::Regular, 11, t.textDim);
        D2D1_RECT_F box = Rect(r.left - 2, y + 15, Width(r) + 4, 26);
        Ui::Hit hit = ui.Button(Ui::Id("fc.field", i), box);
        c.Fill(box, 8, t.text.Alpha(on ? 0.1f : 0.06f));
        if (on) c.Stroke(box, 8, t.accent.Alpha(0.8f), 1);
        D2D1_RECT_F in = Inset(box, 10, 0);
        if (on) {
            if (f.secret) c.TextOnce(std::wstring(typed.size(), L'•') + (caret ? L"│" : L""), in, Weight::Regular, 12, t.text); // never the key itself, never cached
            else c.Text(typed + (caret ? L"│" : L""), in, Weight::Regular, 12, t.text);
            ui.KeepAlive();
        } else if (f.secret) {
            bool set = !feeds::Setting(f.key).empty();
            c.Text(set ? L"Saved. Click to replace it" : f.hint[0] ? f.hint : L"Paste it here", in, Weight::Regular, 12, set ? t.textDim : t.textLow);
        } else {
            std::wstring v = feeds::Setting(f.key);
            c.Text(v.empty() ? std::wstring(f.hint) : v, in, Weight::Regular, 12, v.empty() ? t.textLow : t.text);
        }
        if (hit.clicked && !on) {
            Save(s);
            editing = i;
            typed = f.secret ? L"" : feeds::Setting(f.key);
        }
    }
    if (s && s->note) c.Text(s->note, Rect(r.left, y + 2, Width(r), 16), Weight::Light, 10.5f, t.textLow);
    if (m.feed) {
        feeds::Series got = feeds::Get(m.feed);
        if (!got.ok && !got.error.empty()) c.Text(got.error, Rect(r.left, r.top - 2, Width(r), 18), Weight::Regular, 11, Color::Rgb(0xFF6B6B), Align::Right);
    }
}

}
