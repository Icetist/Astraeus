#pragma once
// The live page: scores for a few picked sports and the world's headlines, fetched on a worker only while the page is looked at.
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace glyph { // the live page's marks, in the symbol subsets
constexpr int Live = 0xE63A, Soccer = 0xEA2F, Cricket = 0xEA27, Motor = 0xEA2D, Chess = 0xF5E7, Skate = 0xE50B, Tennis = 0xEA32;
constexpr int Basket = 0xEA26, Gridiron = 0xEA29, Baseball = 0xEA51, Hockey = 0xEA2B, Golf = 0xEA2A, Mma = 0xEA2C, Rugby = 0xEA2E;
constexpr int Volley = 0xEA31, Globe = 0xE80B, News = 0xEB81, Refresh = 0xE5D5, Offline = 0xE2C1, Chevron = 0xE5CC, Trophy = 0xEA23;
constexpr int Stadium = 0xEB90, Ski = 0xE509, Bike = 0xE52F, Esports = 0xEA28, Score = 0xF06E, Swim = 0xEB48, Sprint = 0xF81F;
constexpr int Badminton = 0xF2A8, Clock = 0xEFD6;
}

namespace live {

constexpr UINT kMsg = WM_APP + 70; // the worker has something new for the page
constexpr int kMinPicks = 3, kMaxPicks = 5;

// ---- the catalogue ----
enum Src { kEspn, kCricket, kChess, kSkate, kMoto, kHeads }; // where a sport's scores come from; kHeads: news only
enum Cat { kCatFootball, kCatBall, kCatRacket, kCatMotor, kCatCombat, kCatWinter, kCatEndurance, kCatMind, kCatOther, kCats };
struct League {
    const char* slug; // espn league, or the source's own key
    const wchar_t* name;
};
struct Sport {
    const char* key; // saved by name
    const wchar_t* name;
    int cat, glyph, src;
    const char* path; // espn sport path, e.g. "soccer"
    const League* men; // the first list; for sports without sexes, the only one
    int nMen;
    const League* women; // null: no men / women choice
    int nWomen;
    const wchar_t* query; // a news search for its headlines
    const char* const* majors; // espn leagues that outrank the pick while they're on: world cups, euros
    const char* const* majorsW;
};
const Sport* Sports(int& n);
const Sport& SportAt(int i);
int SportCount();
int FindSport(const char* key); // -1 unknown
const wchar_t* CatName(int cat);

// ---- what the user picked ----
struct Pick {
    int sport = 0;
    int sex = 0; // 0 men, 1 women, 2 both; ignored where the sport has one list
    int league[2] = {0, 0}; // into men, women
};
struct Cfg {
    std::vector<Pick> picks;
    bool news = true;
    bool setup = false; // the setup sheet was saved once
    void Load();
    void Save() const;
    bool Has(int sport) const;
};
Cfg& Config();
void Changed(); // picks or news switch changed: the page refetches what's new

// ---- what the page shows ----
struct Row {
    std::wstring name, sub, score; // a team or a placed competitor: name, small line (nation, team, rating), score or points
    bool win = false;
};
struct Head {
    std::wstring title, source, link;
    int64_t at = 0; // unix seconds
};
enum State { kLoading, kResult, kLive, kNext, kHeadlines, kFailed };
struct Card {
    std::string key; // the sport's key, plus "#news" for world news
    int state = kLoading;
    bool major = false; // a final, a world cup, a grand slam: drawn with a trophy
    bool list = false; // rows are standings, not two sides
    std::wstring title, status, note, link; // competition, clock or round, a key stat, where a click goes
    int64_t at = 0, fetched = 0; // the match's start, and when this was fetched, unix seconds
    std::vector<Row> rows;
    std::vector<Head> heads;
};
std::vector<Card> Snapshot(uint64_t& version); // the page's copy, cheap when version is unchanged
void Seen(HWND notify); // the page is on screen: fetch what's due, keep going while it stays
void Refresh(); // fetch everything now
bool Busy(); // a fetch is running

// ---- the setup sheet ----
struct Setup {
    bool open = false, done = false; // shown; just saved, the confirmation shows
    double doneAt = 0;
    Cfg draft;
    int cat = -1; // gallery tab, -1 all
    std::wstring search;
    bool typing = false;
    float scroll = 0, scrollTo = 0;
};
Setup& Sheet();
void OpenSetup(); // starts from what's saved
bool Typing(); // the search field holds the keyboard
void OnChar(wchar_t ch); // the search field while it has the keyboard
void OnKey(WPARAM vk);
int64_t Now(); // unix seconds

}
