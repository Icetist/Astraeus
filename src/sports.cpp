#include "sports.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <thread>

#include "calendar.h"
#include "feeds.h"

namespace sports {
namespace {

constexpr wchar_t kSep = L'\x1f'; // between a row's fields: never in a name
constexpr char kDefault[] = "soccer/eng.1";

}

const std::vector<League>& All() {
    static const std::vector<League> kAll = {
        {"soccer/eng.1", L"Premier League (men)", kFootball},
        {"soccer/eng.w.1", L"Women's Super League", kFootball},
        {"soccer/esp.1", L"LaLiga (men)", kFootball},
        {"soccer/esp.w.1", L"Liga F (women)", kFootball},
        {"soccer/fra.1", L"Ligue 1 (men)", kFootball},
        {"soccer/fra.w.1", L"Première Ligue (women)", kFootball},
        {"soccer/ger.1", L"Bundesliga (men)", kFootball},
        {"soccer/ita.1", L"Serie A (men)", kFootball},
        {"soccer/ned.1", L"Eredivisie (men)", kFootball},
        {"soccer/por.1", L"Primeira Liga (men)", kFootball},
        {"soccer/ksa.1", L"Saudi Pro League (men)", kFootball},
        {"soccer/mex.1", L"Liga MX (men)", kFootball},
        {"soccer/usa.1", L"MLS (men)", kFootball},
        {"soccer/usa.nwsl", L"NWSL (women)", kFootball},
        {"soccer/uefa.champions", L"Champions League (men)", kFootball},
        {"soccer/uefa.wchampions", L"Women's Champions League", kFootball},
        {"soccer/uefa.europa", L"Europa League (men)", kFootball},
        {"soccer/fifa.world", L"World Cup (men)", kFootball},
        {"soccer/fifa.wwc", L"Women's World Cup", kFootball},
        {"basketball/nba", L"NBA (men)", kBasketball},
        {"basketball/wnba", L"WNBA (women)", kBasketball},
        {"basketball/mens-college-basketball", L"College basketball (men)", kBasketball},
        {"basketball/womens-college-basketball", L"College basketball (women)", kBasketball},
        {"basketball/fiba", L"FIBA World Cup", kBasketball},
        {"football/nfl", L"NFL", kAmerican},
        {"football/college-football", L"College football", kAmerican},
        {"hockey/nhl", L"NHL (men)", kHockey},
        {"hockey/mens-college-hockey", L"College hockey (men)", kHockey},
        {"hockey/womens-college-hockey", L"College hockey (women)", kHockey},
        {"baseball/mlb", L"MLB", kBaseball},
        {"baseball/college-baseball", L"College baseball", kBaseball},
        {"racing/f1", L"Formula 1", kMotor},
        {"racing/irl", L"IndyCar", kMotor},
        {"racing/nascar-premier", L"NASCAR Cup", kMotor},
        {"tennis/atp", L"ATP (men)", kTennis},
        {"tennis/wta", L"WTA (women)", kTennis},
        {"golf/pga", L"PGA Tour (men)", kGolf},
        {"golf/lpga", L"LPGA (women)", kGolf},
        {"golf/liv", L"LIV Golf (men)", kGolf},
        {"mma/ufc", L"UFC", kFighting},
        {"cricket/8676", L"International cricket", kCricket},
        {"rugby/267979", L"Premiership Rugby (men)", kRugby},
        {"rugby-league/3", L"Rugby league", kRugby},
        {"australian-football/afl", L"AFL", kAussie},
        {"volleyball/mens-college-volleyball", L"College volleyball (men)", kVolleyball},
        {"volleyball/womens-college-volleyball", L"College volleyball (women)", kVolleyball},
        {"lacrosse/pll", L"Premier Lacrosse League", kLacrosse},
        {"chess", L"Chess broadcasts", kChess},
    };
    return kAll;
}

const League* Find(const std::string& path) {
    for (const League& l : All())
        if (path == l.path) return &l;
    return nullptr;
}

const wchar_t* SportName(int sport) {
    static const wchar_t* const kNames[kSports] = {L"Football", L"Basketball", L"American football", L"Hockey", L"Baseball", L"F1 & motorsport", L"Tennis",
                                                   L"Golf", L"Fighting", L"Cricket", L"Rugby", L"Aussie rules", L"Volleyball", L"Lacrosse", L"Chess"};
    return kNames[(std::clamp)(sport, 0, kSports - 1)];
}

std::wstring Encode(const Game& g) {
    std::wstring out = std::to_wstring(g.state);
    for (const std::wstring* f : {&g.a, &g.as, &g.b, &g.bs, &g.detail}) out += kSep, out += *f;
    out += kSep, out += std::to_wstring((long long)g.when);
    out += kSep, out += g.solo ? L"1" : L"0";
    for (const std::wstring* f : {&g.aLines, &g.bLines, &g.venue, &g.aCode, &g.bCode}) out += kSep, out += *f;
    return out;
}

Game Decode(const std::wstring& row) {
    std::vector<std::wstring> f;
    for (size_t a = 0;;) {
        size_t e = row.find(kSep, a);
        f.push_back(row.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a));
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    Game g;
    if (f.size() < 8) return g;
    g.state = _wtoi(f[0].c_str()), g.a = f[1], g.as = f[2], g.b = f[3], g.bs = f[4], g.detail = f[5], g.when = _wtof(f[6].c_str()), g.solo = f[7] == L"1";
    if (f.size() >= 11) g.aLines = f[8], g.bLines = f[9], g.venue = f[10];
    if (f.size() >= 13) g.aCode = f[11], g.bCode = f[12];
    return g;
}

std::wstring Line(const Game& g) { // as few letters as a scoreline takes: the pill has little room
    constexpr size_t kMost = 22;
    auto surname = [](const std::wstring& n) { size_t dot = n.rfind(L". "); return dot == std::wstring::npos ? n : n.substr(dot + 2); }; // "C. Alcaraz": Alcaraz
    std::wstring out;
    if (g.solo) { // a race or a tournament: who leads it; a chess broadcast: its event, cut short, and its round
        constexpr size_t kEventMost = 14;
        size_t colon = g.b.find(L": ");
        if (colon != std::wstring::npos) {
            out = surname(g.b.substr(colon + 2));
        } else {
            std::wstring event = g.a.substr(0, g.a.find_first_of(L"|(·")); // "European Youth Chess Championship 2026 | O18": its name alone
            while (!event.empty() && event.back() == L' ') event.pop_back();
            if (event.size() > kEventMost) event = event.substr(0, kEventMost - 1) + L"…";
            std::wstring round = g.b;
            if (round.rfind(L"Round ", 0) == 0) round = L"R" + round.substr(6); // "Round 5": R5
            out = round.empty() ? event : event + L" " + round;
        }
    } else {
        std::wstring a = g.aCode.empty() ? surname(g.a) : g.aCode, b = g.bCode.empty() ? surname(g.b) : g.bCode;
        if (g.state == kPre) {
            size_t space = g.detail.find(L' '); // "Sat 20:00": the time is enough
            out = a + L"–" + b + L" " + (space == std::wstring::npos ? g.detail : g.detail.substr(space + 1));
        } else if (g.as.empty() && g.bs.empty()) {
            out = a + L"–" + b + L" LIVE";
        } else {
            out = a + L" " + g.as + L"–" + g.bs + L" " + b;
        }
    }
    return out.size() > kMost ? out.substr(0, kMost - 1) + L"…" : out;
}

std::vector<Game> Order(std::vector<Game> games) {
    std::stable_sort(games.begin(), games.end(), [](const Game& x, const Game& y) {
        auto rank = [](const Game& g) { return g.state == kLive ? 0 : g.state == kFinal ? 1 : 2; };
        if (rank(x) != rank(y)) return rank(x) < rank(y);
        return x.state == kFinal ? x.when > y.when : x.when < y.when; // the newest result, the soonest start
    });
    return games;
}

static std::vector<std::string> followedNow; // read once, kept: asked for every frame the tiles draw
static bool followedRead = false;

std::vector<std::string> Followed() {
    if (followedRead) return followedNow;
    followedRead = true;
    std::wstring saved = feeds::Setting("sport.leagues");
    std::vector<std::string>& out = followedNow;
    out.clear();
    for (size_t a = 0; a < saved.size() && out.size() < kMaxFollowed;) {
        size_t e = (std::min)(saved.find(L',', a), saved.size());
        std::string one;
        for (wchar_t ch : saved.substr(a, e - a)) one += (char)ch;
        if (Find(one)) out.push_back(one);
        a = e + 1;
    }
    if (out.empty() && saved != L"-") out.push_back(kDefault);
    return out;
}

void SetFollowed(const std::vector<std::string>& paths) {
    std::wstring s;
    for (const std::string& p : paths) s += (s.empty() ? L"" : L",") + std::wstring(p.begin(), p.end());
    feeds::SetSetting("sport.leagues", s.empty() ? L"-" : s);
    followedRead = false; // read again, as saved
}

std::string FeedId(const std::string& path) { return "sport:" + path; }
std::string NextId(const std::string& path) { return "next:" + path; }

static std::wstring CalTitle(const Game& g) { return g.solo ? g.a : g.a + L" v " + g.b; } // the one name add, check and remove all match on

bool AddToCalendar(const Game& g, const std::wstring& league) { // into astraeus' own calendar: it shows on its day beside the linked calendars
    constexpr int64_t kSlotSec = 2 * 3600;
    if (g.when <= 0) return false;
    sys::cal::Event ev;
    ev.start = (int64_t)g.when, ev.end = (int64_t)g.when + kSlotSec;
    ev.title = CalTitle(g);
    ev.where = league;
    return sys::cal::AddMine(ev);
}

bool InCalendar(const Game& g) { return g.when > 0 && sys::cal::HasMine((int64_t)g.when, CalTitle(g)); }

bool RemoveFromCalendar(const Game& g) { return g.when > 0 && sys::cal::RemoveMine((int64_t)g.when, CalTitle(g)); }

}
