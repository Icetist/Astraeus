// Every sport the live page knows, where its scores come from, and the user's picks, saved by name.
#include "live.h"

#include <algorithm>

#include "glyphs.h"
#include "sys.h"

namespace live {

namespace {

// espn league slugs; the first of each list is the default pick
constexpr League kSoccerM[] = {
    {"eng.1", L"Premier League"}, {"esp.1", L"LALIGA"}, {"ger.1", L"Bundesliga"}, {"ita.1", L"Serie A"}, {"fra.1", L"Ligue 1"},
    {"uefa.champions", L"Champions League"}, {"uefa.europa", L"Europa League"}, {"uefa.europa.conf", L"Conference League"},
    {"fifa.world", L"FIFA World Cup"}, {"uefa.euro", L"European Championship"}, {"conmebol.america", L"Copa América"},
    {"uefa.nations", L"Nations League"}, {"fifa.worldq.uefa", L"World Cup qualifying, UEFA"}, {"fifa.friendly", L"International friendlies"},
    {"fifa.cwc", L"Club World Cup"}, {"caf.nations", L"Africa Cup of Nations"}, {"afc.asian.cup", L"AFC Asian Cup"}, {"concacaf.gold", L"Gold Cup"},
    {"conmebol.libertadores", L"Copa Libertadores"}, {"conmebol.sudamericana", L"Copa Sudamericana"}, {"afc.champions", L"AFC Champions League"},
    {"caf.champions", L"CAF Champions League"}, {"concacaf.champions", L"Concacaf Champions Cup"},
    {"eng.2", L"Championship"}, {"eng.3", L"League One"}, {"eng.fa", L"FA Cup"}, {"eng.league_cup", L"Carabao Cup"},
    {"esp.2", L"LALIGA 2"}, {"esp.copa_del_rey", L"Copa del Rey"}, {"ger.2", L"2. Bundesliga"}, {"ger.dfb_pokal", L"DFB-Pokal"},
    {"ita.2", L"Serie B"}, {"ita.coppa_italia", L"Coppa Italia"}, {"fra.2", L"Ligue 2"}, {"fra.coupe_de_france", L"Coupe de France"},
    {"ned.1", L"Eredivisie"}, {"por.1", L"Primeira Liga"}, {"sco.1", L"Scottish Premiership"}, {"bel.1", L"Belgian Pro League"},
    {"tur.1", L"Süper Lig"}, {"gre.1", L"Greek Super League"}, {"aut.1", L"Austrian Bundesliga"}, {"den.1", L"Danish Superliga"},
    {"swe.1", L"Allsvenskan"}, {"nor.1", L"Eliteserien"}, {"rus.1", L"Russian Premier League"}, {"usa.1", L"MLS"}, {"usa.open", L"U.S. Open Cup"},
    {"mex.1", L"Liga MX"}, {"bra.1", L"Brasileirão"}, {"bra.copa_do_brazil", L"Copa do Brasil"}, {"arg.1", L"Argentine Primera"},
    {"col.1", L"Colombian Primera A"}, {"chi.1", L"Chilean Primera"}, {"uru.1", L"Uruguayan Primera"}, {"per.1", L"Peruvian Liga 1"},
    {"ecu.1", L"Ecuador LigaPro"}, {"ksa.1", L"Saudi Pro League"}, {"jpn.1", L"J1 League"}, {"chn.1", L"Chinese Super League"},
    {"aus.1", L"A-League Men"}, {"ind.1", L"Indian Super League"}, {"rsa.1", L"South African Premiership"}, {"usa.usl.1", L"USL Championship"},
    {"fifa.world.u20", L"U-20 World Cup"}, {"uefa.euro_u21", L"European Under-21"}, {"fifa.olympics", L"Olympic tournament"},
    {"usa.ncaa.m.1", L"NCAA men"},
};
constexpr League kSoccerW[] = {
    {"eng.w.1", L"Women's Super League"}, {"usa.nwsl", L"NWSL"}, {"uefa.wchampions", L"Women's Champions League"}, {"fifa.wwc", L"Women's World Cup"},
    {"uefa.weuro", L"Women's Euro"}, {"esp.w.1", L"Liga F"}, {"fra.w.1", L"Première Ligue"}, {"ned.w.1", L"Vrouwen Eredivisie"},
    {"aus.w.1", L"A-League Women"}, {"mex.w.1", L"Liga MX Femenil"}, {"usa.w.usl.1", L"USL Super League"}, {"eng.w.fa", L"Women's FA Cup"},
    {"uefa.w.nations", L"Women's Nations League"}, {"conmebol.america.femenina", L"Copa América Femenina"}, {"caf.w.nations", L"Women's AFCON"},
    {"afc.w.asian.cup", L"Women's Asian Cup"}, {"concacaf.w.gold", L"W Gold Cup"}, {"fifa.w.olympics", L"Women's Olympic tournament"},
    {"fifa.friendly.w", L"Women's friendlies"}, {"usa.nwsl.cup", L"NWSL Challenge Cup"}, {"usa.ncaa.w.1", L"NCAA women"},
};
constexpr const char* kSoccerMajors[] = {"fifa.world", "uefa.euro", "conmebol.america", "fifa.cwc", nullptr};
constexpr const char* kSoccerMajorsW[] = {"fifa.wwc", "uefa.weuro", nullptr};

constexpr League kGridiron[] = {{"nfl", L"NFL"}, {"college-football", L"College football"}, {"cfl", L"CFL"}, {"ufl", L"UFL"}};
constexpr League kBasketM[] = {{"nba", L"NBA"}, {"mens-college-basketball", L"NCAA men"}, {"fiba", L"FIBA"}, {"nbl", L"NBL Australia"},
                               {"nba-development", L"G League"}, {"mens-olympics-basketball", L"Olympics, men"}};
constexpr League kBasketW[] = {{"wnba", L"WNBA"}, {"womens-college-basketball", L"NCAA women"}, {"womens-olympics-basketball", L"Olympics, women"}};
constexpr League kBaseball[] = {{"mlb", L"MLB"}, {"college-baseball", L"College baseball"}, {"world-baseball-classic", L"World Baseball Classic"},
                                {"caribbean-series", L"Caribbean Series"}, {"olympics-baseball", L"Olympics"}};
constexpr League kSoftball[] = {{"college-softball", L"College softball"}};
constexpr League kHockeyM[] = {{"nhl", L"NHL"}, {"mens-college-hockey", L"NCAA men"}, {"olympics-mens-ice-hockey", L"Olympics, men"}, {"hockey-world-cup", L"World Cup of Hockey"}};
constexpr League kHockeyW[] = {{"womens-college-hockey", L"NCAA women"}, {"olympics-womens-ice-hockey", L"Olympics, women"}};
constexpr League kRugbyM[] = {{"180659", L"Six Nations"}, {"244293", L"The Rugby Championship"}, {"289234", L"Test matches"}, {"271937", L"Champions Cup"},
                              {"267979", L"Premiership"}, {"270557", L"United Rugby Championship"}, {"270559", L"Top 14"}, {"242041", L"Super Rugby Pacific"},
                              {"272073", L"Challenge Cup"}, {"289262", L"Major League Rugby"}, {"268565", L"Lions tour"}, {"282", L"Olympic sevens"}};
constexpr League kRugbyW[] = {{"289237", L"Women's Rugby World Cup"}, {"283", L"Olympic women's sevens"}};
constexpr League kLeague13[] = {{"3", L"Rugby league"}};
constexpr League kAfl[] = {{"afl", L"AFL"}};
constexpr League kLacrosseM[] = {{"pll", L"Premier Lacrosse League"}, {"nll", L"NLL"}, {"mens-college-lacrosse", L"NCAA men"}};
constexpr League kLacrosseW[] = {{"womens-college-lacrosse", L"NCAA women"}};
constexpr League kVolleyM[] = {{"mens-college-volleyball", L"NCAA men"}};
constexpr League kVolleyW[] = {{"womens-college-volleyball", L"NCAA women"}};
constexpr League kPoloM[] = {{"mens-college-water-polo", L"NCAA men"}};
constexpr League kPoloW[] = {{"womens-college-water-polo", L"NCAA women"}};
constexpr League kFieldW[] = {{"womens-college-field-hockey", L"NCAA women"}};
constexpr League kCricketL[] = {{"intl", L"International"}, {"all", L"Every match"}, {"t20", L"T20 leagues"}, {"first", L"First-class"}};
constexpr League kTennisM[] = {{"atp", L"ATP"}};
constexpr League kTennisW[] = {{"wta", L"WTA"}};
constexpr League kGolfM[] = {{"pga", L"PGA Tour"}, {"eur", L"DP World Tour"}, {"liv", L"LIV Golf"}, {"champions-tour", L"Champions Tour"}};
constexpr League kGolfW[] = {{"lpga", L"LPGA"}};
constexpr League kF1[] = {{"f1", L"Formula 1"}};
constexpr League kIndy[] = {{"irl", L"IndyCar"}};
constexpr League kNascar[] = {{"nascar-premier", L"Cup Series"}, {"nascar-secondary", L"Xfinity Series"}, {"nascar-truck", L"Truck Series"}};
constexpr League kMotoL[] = {{"MotoGP", L"MotoGP"}, {"Moto2", L"Moto2"}, {"Moto3", L"Moto3"}};
constexpr League kMma[] = {{"ufc", L"UFC"}, {"pfl", L"PFL"}, {"bellator", L"Bellator"}, {"ksw", L"KSW"}, {"cage-warriors", L"Cage Warriors"}};
constexpr League kChessM[] = {{"open", L"Open"}};
constexpr League kChessW[] = {{"women", L"Women"}};
constexpr League kSkateL[] = {{"all", L"Every category"}, {"men", L"Men"}, {"women", L"Women"}, {"pairs", L"Pairs"}, {"dance", L"Ice dance"}};

#define L_(a) a, (int)std::size(a)
constexpr Sport kSports[] = {
    {"soccer", L"Football", kCatFootball, glyph::Soccer, kEspn, "soccer", L_(kSoccerM), L_(kSoccerW), L"football match result", kSoccerMajors, kSoccerMajorsW},
    {"cricket", L"Cricket", kCatBall, glyph::Cricket, kCricket, nullptr, L_(kCricketL), L_(kCricketL), L"cricket", nullptr, nullptr},
    {"basketball", L"Basketball", kCatBall, glyph::Basket, kEspn, "basketball", L_(kBasketM), L_(kBasketW), L"basketball", nullptr, nullptr},
    {"nfl", L"American football", kCatBall, glyph::Gridiron, kEspn, "football", L_(kGridiron), nullptr, 0, L"NFL", nullptr, nullptr},
    {"baseball", L"Baseball", kCatBall, glyph::Baseball, kEspn, "baseball", L_(kBaseball), nullptr, 0, L"baseball", nullptr, nullptr},
    {"softball", L"Softball", kCatBall, glyph::Baseball, kEspn, "baseball", L_(kSoftball), nullptr, 0, L"softball", nullptr, nullptr},
    {"hockey", L"Ice hockey", kCatBall, glyph::Hockey, kEspn, "hockey", L_(kHockeyM), L_(kHockeyW), L"ice hockey", nullptr, nullptr},
    {"rugby", L"Rugby union", kCatBall, glyph::Rugby, kEspn, "rugby", L_(kRugbyM), L_(kRugbyW), L"rugby union", nullptr, nullptr},
    {"league", L"Rugby league", kCatBall, glyph::Rugby, kEspn, "rugby-league", L_(kLeague13), nullptr, 0, L"rugby league", nullptr, nullptr},
    {"afl", L"Aussie rules", kCatBall, glyph::Rugby, kEspn, "australian-football", L_(kAfl), nullptr, 0, L"AFL", nullptr, nullptr},
    {"lacrosse", L"Lacrosse", kCatBall, glyph::Hockey, kEspn, "lacrosse", L_(kLacrosseM), L_(kLacrosseW), L"lacrosse", nullptr, nullptr},
    {"volleyball", L"Volleyball", kCatBall, glyph::Volley, kEspn, "volleyball", L_(kVolleyM), L_(kVolleyW), L"volleyball", nullptr, nullptr},
    {"waterpolo", L"Water polo", kCatBall, glyph::Swim, kEspn, "water-polo", L_(kPoloM), L_(kPoloW), L"water polo", nullptr, nullptr},
    {"fieldhockey", L"Field hockey", kCatBall, glyph::Hockey, kEspn, "field-hockey", L_(kFieldW), nullptr, 0, L"field hockey", nullptr, nullptr},
    {"handball", L"Handball", kCatBall, glyph::Volley, kHeads, nullptr, nullptr, 0, nullptr, 0, L"EHF IHF handball", nullptr, nullptr},
    {"tennis", L"Tennis", kCatRacket, glyph::Tennis, kEspn, "tennis", L_(kTennisM), L_(kTennisW), L"tennis", nullptr, nullptr},
    {"badminton", L"Badminton", kCatRacket, glyph::Badminton, kHeads, nullptr, nullptr, 0, nullptr, 0, L"badminton BWF", nullptr, nullptr},
    {"tabletennis", L"Table tennis", kCatRacket, glyph::Tennis, kHeads, nullptr, nullptr, 0, nullptr, 0, L"table tennis WTT", nullptr, nullptr},
    {"squash", L"Squash", kCatRacket, glyph::Tennis, kHeads, nullptr, nullptr, 0, nullptr, 0, L"squash PSA", nullptr, nullptr},
    {"golf", L"Golf", kCatRacket, glyph::Golf, kEspn, "golf", L_(kGolfM), L_(kGolfW), L"golf", nullptr, nullptr},
    {"f1", L"Formula 1", kCatMotor, glyph::Motor, kEspn, "racing", L_(kF1), nullptr, 0, L"Formula 1", nullptr, nullptr},
    {"motogp", L"MotoGP", kCatMotor, glyph::Motor, kMoto, nullptr, L_(kMotoL), nullptr, 0, L"MotoGP", nullptr, nullptr},
    {"indycar", L"IndyCar", kCatMotor, glyph::Motor, kEspn, "racing", L_(kIndy), nullptr, 0, L"IndyCar", nullptr, nullptr},
    {"nascar", L"NASCAR", kCatMotor, glyph::Motor, kEspn, "racing", L_(kNascar), nullptr, 0, L"NASCAR", nullptr, nullptr},
    {"wec", L"Endurance racing", kCatMotor, glyph::Motor, kHeads, nullptr, nullptr, 0, nullptr, 0, L"World Endurance Championship", nullptr, nullptr},
    {"rally", L"Rally", kCatMotor, glyph::Motor, kHeads, nullptr, nullptr, 0, nullptr, 0, L"WRC rally", nullptr, nullptr},
    {"mma", L"MMA", kCatCombat, glyph::Mma, kEspn, "mma", L_(kMma), nullptr, 0, L"UFC", nullptr, nullptr},
    {"boxing", L"Boxing", kCatCombat, glyph::Mma, kHeads, nullptr, nullptr, 0, nullptr, 0, L"boxing fight result", nullptr, nullptr},
    {"wrestling", L"Wrestling", kCatCombat, glyph::Mma, kHeads, nullptr, nullptr, 0, nullptr, 0, L"UWW wrestling championships", nullptr, nullptr},
    {"judo", L"Judo", kCatCombat, glyph::Mma, kHeads, nullptr, nullptr, 0, nullptr, 0, L"IJF judo World Tour", nullptr, nullptr},
    {"skating", L"Figure skating", kCatWinter, glyph::Skate, kSkate, nullptr, L_(kSkateL), nullptr, 0, L"figure skating", nullptr, nullptr},
    {"skiing", L"Skiing", kCatWinter, glyph::Ski, kHeads, nullptr, nullptr, 0, nullptr, 0, L"alpine skiing world cup", nullptr, nullptr},
    {"biathlon", L"Biathlon", kCatWinter, glyph::Ski, kHeads, nullptr, nullptr, 0, nullptr, 0, L"IBU biathlon World Cup", nullptr, nullptr},
    {"speedskating", L"Speed skating", kCatWinter, glyph::Skate, kHeads, nullptr, nullptr, 0, nullptr, 0, L"ISU speed skating World Cup", nullptr, nullptr},
    {"athletics", L"Athletics", kCatEndurance, glyph::Sprint, kHeads, nullptr, nullptr, 0, nullptr, 0, L"World Athletics track and field", nullptr, nullptr},
    {"swimming", L"Swimming", kCatEndurance, glyph::Swim, kHeads, nullptr, nullptr, 0, nullptr, 0, L"World Aquatics swimming", nullptr, nullptr},
    {"cycling", L"Cycling", kCatEndurance, glyph::Bike, kHeads, nullptr, nullptr, 0, nullptr, 0, L"cycling race stage", nullptr, nullptr},
    {"triathlon", L"Triathlon", kCatEndurance, glyph::Swim, kHeads, nullptr, nullptr, 0, nullptr, 0, L"World Triathlon", nullptr, nullptr},
    {"rowing", L"Rowing", kCatEndurance, glyph::Swim, kHeads, nullptr, nullptr, 0, nullptr, 0, L"World Rowing regatta", nullptr, nullptr},
    {"sailing", L"Sailing", kCatEndurance, glyph::Swim, kHeads, nullptr, nullptr, 0, nullptr, 0, L"sailing SailGP America's Cup", nullptr, nullptr},
    {"horses", L"Horse racing", kCatEndurance, glyph::Sprint, kHeads, nullptr, nullptr, 0, nullptr, 0, L"horse racing", nullptr, nullptr},
    {"chess", L"Chess", kCatMind, glyph::Chess, kChess, nullptr, L_(kChessM), L_(kChessW), L"FIDE chess", nullptr, nullptr},
    {"esports", L"Esports", kCatMind, glyph::Esports, kHeads, nullptr, nullptr, 0, nullptr, 0, L"esports final League of Legends OR Valorant OR Counter-Strike", nullptr, nullptr},
    {"darts", L"Darts", kCatOther, glyph::Score, kHeads, nullptr, nullptr, 0, nullptr, 0, L"PDC darts", nullptr, nullptr},
    {"snooker", L"Snooker", kCatOther, glyph::Score, kHeads, nullptr, nullptr, 0, nullptr, 0, L"snooker", nullptr, nullptr},
    {"kabaddi", L"Kabaddi", kCatOther, glyph::Volley, kHeads, nullptr, nullptr, 0, nullptr, 0, L"kabaddi", nullptr, nullptr},
    {"olympics", L"Olympics", kCatOther, glyph::Trophy, kHeads, nullptr, nullptr, 0, nullptr, 0, L"Olympic Games medal", nullptr, nullptr},
};
#undef L_
constexpr int kSportCount = (int)std::size(kSports);
constexpr wchar_t kPicksVal[] = L"LiveSports", kNewsVal[] = L"LiveNews", kSetupVal[] = L"LiveSetup";

std::string Narrow(const std::wstring& w) { // registry text is ascii keys and digits
    std::string s;
    for (wchar_t ch : w) s += ch < 128 ? (char)ch : '?';
    return s;
}

}

const Sport* Sports(int& n) {
    n = kSportCount;
    return kSports;
}
const Sport& SportAt(int i) { return kSports[(std::clamp)(i, 0, kSportCount - 1)]; }
int SportCount() { return kSportCount; }

int FindSport(const char* key) {
    for (int i = 0; i < kSportCount; ++i)
        if (strcmp(kSports[i].key, key) == 0) return i;
    return -1;
}

const wchar_t* CatName(int cat) {
    static const wchar_t* const kNames[kCats] = {L"Football", L"Team sports", L"Racket & golf", L"Motorsport", L"Combat", L"Winter", L"Endurance", L"Mind", L"More"};
    return cat >= 0 && cat < kCats ? kNames[cat] : L"All";
}

bool Cfg::Has(int sport) const {
    return std::any_of(picks.begin(), picks.end(), [&](const Pick& p) { return p.sport == sport; });
}

void Cfg::Load() { // "soccer:2:0:3,tennis:2:0:0": key, sexes, men's league, women's league
    picks.clear();
    std::string s = Narrow(sys::ReadText(kPicksVal));
    for (size_t at = 0; at < s.size() && picks.size() < (size_t)kMaxPicks;) {
        size_t end = (std::min)(s.find(',', at), s.size());
        std::string tok = s.substr(at, end - at);
        at = end + 1;
        size_t c1 = tok.find(':');
        int id = FindSport(tok.substr(0, c1).c_str());
        if (id < 0 || Has(id)) continue; // a sport that left the catalogue, or a repeat
        Pick p;
        p.sport = id;
        if (c1 != std::string::npos) sscanf_s(tok.c_str() + c1 + 1, "%d:%d:%d", &p.sex, &p.league[0], &p.league[1]);
        const Sport& sp = kSports[id];
        p.sex = sp.women ? (std::clamp)(p.sex, 0, 2) : 0;
        p.league[0] = (std::clamp)(p.league[0], 0, (std::max)(0, sp.nMen - 1));
        p.league[1] = (std::clamp)(p.league[1], 0, (std::max)(0, sp.nWomen - 1));
        picks.push_back(p);
    }
    news = sys::ReadNum(kNewsVal, 1) != 0;
    setup = sys::ReadNum(kSetupVal, 0) != 0;
}

void Cfg::Save() const {
    std::wstring s;
    for (const Pick& p : picks) {
        wchar_t buf[64];
        swprintf_s(buf, L"%hs:%d:%d:%d", kSports[p.sport].key, p.sex, p.league[0], p.league[1]);
        s += (s.empty() ? L"" : L",") + std::wstring(buf);
    }
    sys::SaveText(kPicksVal, s.empty() ? L"none" : s);
    sys::SaveNum(kNewsVal, news ? 1 : 0);
    sys::SaveNum(kSetupVal, setup ? 1 : 0);
}

Cfg& Config() {
    static Cfg cfg = [] {
        Cfg c;
        c.Load();
        return c;
    }();
    return cfg;
}

Setup& Sheet() {
    static Setup s;
    return s;
}

void OpenSetup() {
    Setup& s = Sheet();
    s.draft = Config();
    if (s.draft.picks.empty()) // a first look: three to start from, easy to change
        for (const char* k : {"soccer", "f1", "tennis"}) s.draft.picks.push_back({FindSport(k), 0, {0, 0}});
    s.open = true;
    s.done = false;
    s.cat = -1;
    s.search.clear();
    s.typing = false;
    s.scroll = s.scrollTo = 0;
}

bool Typing() {
    const Setup& s = Sheet();
    return s.open && s.typing && !s.done;
}

void OnChar(wchar_t ch) {
    Setup& s = Sheet();
    if (!s.typing) return;
    constexpr size_t kSearchMax = 24;
    if (ch == 8) {
        if (!s.search.empty()) s.search.pop_back();
    } else if (ch == 22) { // ctrl+v, the first line
        for (wchar_t c : sys::ClipText()) {
            if (c == L'\r' || c == L'\n' || s.search.size() >= kSearchMax) break;
            if (c >= 32) s.search += c;
        }
    } else if (ch >= 32 && ch != 127 && s.search.size() < kSearchMax) {
        s.search += ch;
    }
    s.scrollTo = 0;
}

void OnKey(WPARAM vk) {
    Setup& s = Sheet();
    if (vk == VK_ESCAPE) {
        if (s.search.empty()) s.typing = false;
        s.search.clear();
    } else if (vk == VK_RETURN) {
        s.typing = false;
    }
}

}
