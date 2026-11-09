#pragma once
#include <string>
#include <vector>

// The sports widget's catalogue and its games. Scores come from ESPN's public scoreboards, chess from Lichess's broadcasts;
// a league's games travel from the fetcher to the faces as rows of fields, read back here.
namespace sports {

enum Sport { kFootball, kBasketball, kAmerican, kHockey, kBaseball, kMotor, kTennis, kGolf, kFighting, kCricket, kRugby, kAussie, kVolleyball, kLacrosse, kChess, kSports };

struct League {
    const char* path; // espn's sport/league, or "chess"
    const wchar_t* name; // as it reads on the tile, men's and women's said where both exist
    int sport;
};

const std::vector<League>& All();
const League* Find(const std::string& path);
const wchar_t* SportName(int sport);

enum State { kPre, kLive, kFinal };
struct Game {
    int state = kPre;
    std::wstring a, as, b, bs; // two sides and their scores; one-sided events: a is the event, b its leader or what's on
    std::wstring detail; // the clock, "FT", a start time
    double when = 0; // unix seconds it starts: the latest finished first among results
    bool solo = false; // a race, a tournament, a broadcast: one line, not two sides
    std::wstring aLines, bLines, venue; // each side's periods, innings or sets, space apart; where it's played
    std::wstring aCode, bCode; // "BAR", "RMA": the short names a scoreline uses
    std::wstring id; // espn's event id, or a broadcast round's "tour/round/id": its detail is fetched by it
    std::string path; // its league, filled in as it's read: never in the row
    int sport = kFootball; // its league's, the same way: the long pill shows its mark
};

struct Detail { // one game opened: what's happened in it, its numbers, and its lists
    std::wstring status; // "67'", "Race · Lap 34 of 57", "Round 9 · live"
    struct Event { int side = -1; std::wstring when, kind, text; }; // side 0 the first, 1 the second, -1 neither; kind: goal, yellow, red, score
    struct Stat { std::wstring label, a, b; };
    struct Row { std::wstring left, mid, right; bool live = false; };
    struct Block { std::wstring title; std::vector<Row> rows; }; // leaders, the running order, the boards, the matches
    std::vector<Event> events;
    std::vector<Stat> stats;
    std::vector<Block> blocks;
};
std::vector<std::wstring> EncodeDetail(const Detail& d); // a feed's rows
Detail DecodeDetail(const std::vector<std::wstring>& rows);
std::string MatchId(const Game& g); // "match:soccer/eng.1|401878758|20261108"; empty: nothing to open
std::wstring Line(const Game& g); // "BAR 1 – 0 RMA  67'", "BAR – RMA  Sat 20:00": one game in a few words, for the pills

std::wstring Encode(const Game& g); // one row of the feed
Game Decode(const std::wstring& row);
std::vector<Game> Order(std::vector<Game> games); // live first, then the latest results, then what's next

std::vector<std::string> Followed(); // the leagues picked, at most kMaxFollowed; the premier league to start
void SetFollowed(const std::vector<std::string>& paths);
constexpr size_t kMaxFollowed = 3;
std::string FeedId(const std::string& path); // "sport:soccer/eng.1": now and lately
std::string NextId(const std::string& path); // "next:soccer/eng.1": the past week's results and the coming days
bool AddToCalendar(const Game& g, const std::wstring& league); // into astraeus' own calendar, on the game's day
bool InCalendar(const Game& g); // added already: the panel offers remove instead
bool RemoveFromCalendar(const Game& g);

}
