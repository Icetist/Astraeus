// Where the live page's scores come from: espn's public scoreboards, lichess broadcasts, the isu's result pages, motogp's results
// service, and news feeds for the rest. Everything here runs on the live worker.
#include "live_src.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "sys.h"

namespace live::src {

namespace {

constexpr int kTries = 3; // dns and the scoreboards both drop the odd request
constexpr int64_t kDay = 24 * 3600;
constexpr int64_t kMajorRecent = 3 * kDay; // a world cup result this fresh outranks the picked league
constexpr int kRows = 3; // places shown on a standings card
constexpr int kHeadCount = 4; // headlines on a news card
constexpr wchar_t kEspnUrl[] = L"https://site.api.espn.com/apis/site/v2/sports/";

const Val kNullVal{};

// ---- json ----
void PutUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | cp >> 6);
        out += (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | cp >> 12);
        out += (char)(0x80 | (cp >> 6 & 63));
        out += (char)(0x80 | (cp & 63));
    } else {
        out += (char)(0xF0 | cp >> 18);
        out += (char)(0x80 | (cp >> 12 & 63));
        out += (char)(0x80 | (cp >> 6 & 63));
        out += (char)(0x80 | (cp & 63));
    }
}

struct Reader {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    size_t nodes = 0;
    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    unsigned Hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && i < s.size(); ++k, ++i) {
            char c = s[i];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    bool Str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size() && s[i] != '"';) {
            char ch = s[i++];
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (i >= s.size()) return false;
            char e = s[i++];
            if (e == 'u') {
                unsigned cp = Hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { // surrogate pair
                    i += 2;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (Hex4() - 0xDC00);
                }
                PutUtf8(out, cp);
            } else {
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool Value(Val& v) {
        Ws();
        if (i >= s.size() || depth > 96 || ++nodes > 2000000) return false; // scoreboards run deep and wide, never this much
        char c = s[i];
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            v.kind = obj ? Val::kObj : Val::kArr;
            ++i;
            ++depth;
            Ws();
            if (i < s.size() && s[i] == (obj ? '}' : ']')) {
                ++i;
                --depth;
                return true;
            }
            for (;;) {
                if (obj) {
                    Ws();
                    std::string key;
                    if (!Str(key)) return false;
                    Ws();
                    if (i >= s.size() || s[i++] != ':') return false;
                    v.keys.push_back(std::move(key));
                }
                v.items.emplace_back();
                if (!Value(v.items.back())) return false;
                Ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == (obj ? '}' : ']')) {
                    ++i;
                    --depth;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.kind = Val::kStr;
            return Str(v.str);
        }
        if (s.compare(i, 4, "true") == 0 || s.compare(i, 5, "false") == 0) {
            v.kind = Val::kBool;
            v.num = c == 't';
            i += c == 't' ? 4 : 5;
            return true;
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return true;
        }
        if (c != '-' && (c < '0' || c > '9')) return false;
        char* end = nullptr;
        v.kind = Val::kNum;
        v.num = std::strtod(s.c_str() + i, &end);
        if (!end || end == s.c_str() + i) return false;
        i = (size_t)(end - s.c_str());
        return true;
    }
};

// ---- text and time ----
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
bool Has(const std::string& hay, const char* needle) { return Lower(hay).find(Lower(needle)) != std::string::npos; }

int64_t Iso(const std::string& s) { // "2026-10-25T23:00Z", seconds and fractions optional, always utc here
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    std::tm t{};
    t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d, t.tm_hour = h, t.tm_min = mi, t.tm_sec = se;
    return (int64_t)_mkgmtime(&t);
}

int64_t Rfc822(const std::string& s) { // "Wed, 28 Oct 2026 21:38:54 GMT" or "+0100"
    static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t comma = s.find(',');
    const char* p = s.c_str() + (comma == std::string::npos ? 0 : comma + 1);
    int d = 0, y = 0, h = 0, mi = 0, se = 0;
    char mon[8] = {}, zone[8] = {};
    if (sscanf_s(p, "%d %3s %d %d:%d:%d %7s", &d, mon, (unsigned)sizeof(mon), &y, &h, &mi, &se, zone, (unsigned)sizeof(zone)) < 6) return 0;
    int m = -1;
    for (int k = 0; k < 12; ++k)
        if (_stricmp(mon, kMon[k]) == 0) m = k;
    if (m < 0) return 0;
    std::tm t{};
    t.tm_year = y - 1900, t.tm_mon = m, t.tm_mday = d, t.tm_hour = h, t.tm_min = mi, t.tm_sec = se;
    int64_t at = (int64_t)_mkgmtime(&t);
    if (zone[0] == '+' || zone[0] == '-') { // an offset: back to utc
        int z = atoi(zone + 1);
        int64_t off = (z / 100) * 3600 + (z % 100) * 60;
        at += zone[0] == '+' ? -off : off;
    }
    return at;
}

std::wstring Ymd(int64_t at) { // espn's dates= key, in utc
    std::tm t{};
    time_t tt = (time_t)at;
    gmtime_s(&t, &tt);
    wchar_t buf[16];
    swprintf_s(buf, L"%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

std::wstring W(const Val& v) { return Wide(v.Str()); }

std::string Decode(std::string s) { // xml and html entities, the few feeds use
    struct E {
        const char* from;
        const char* to;
    } static const kEnt[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
    for (const E& e : kEnt)
        for (size_t at = s.find(e.from); at != std::string::npos; at = s.find(e.from, at + 1)) s.replace(at, strlen(e.from), e.to);
    for (size_t at = s.find("&#"); at != std::string::npos; at = s.find("&#", at + 1)) { // numeric references
        size_t semi = s.find(';', at);
        if (semi == std::string::npos || semi - at > 9) continue;
        bool hex = s[at + 2] == 'x' || s[at + 2] == 'X';
        unsigned cp = (unsigned)strtoul(s.c_str() + at + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        std::string u;
        PutUtf8(u, cp ? cp : '?');
        s.replace(at, semi - at + 1, u);
    }
    return s;
}

std::string Tag(const std::string& s, const char* tag, size_t from = 0, size_t* end = nullptr) { // an element's inner text, cdata unwrapped
    std::string open = std::string("<") + tag, close = std::string("</") + tag + ">";
    size_t a = s.find(open, from);
    if (a == std::string::npos) return {};
    a = s.find('>', a);
    size_t b = a == std::string::npos ? a : s.find(close, a);
    if (b == std::string::npos) return {};
    std::string in = s.substr(a + 1, b - a - 1);
    if (end) *end = b + close.size();
    if (in.rfind("<![CDATA[", 0) == 0) in = in.substr(9, in.size() >= 12 ? in.size() - 12 : 0);
    return Decode(in);
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// ---- news ----
std::wstring Esc(const std::wstring& s) {
    std::string u = Utf8(s), out;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_') out += (char)c;
        else if (c == ' ') out += '+';
        else {
            char b[4];
            sprintf_s(b, "%%%02X", c);
            out += b;
        }
    }
    return Wide(out);
}

std::vector<Head> Rss(const std::string& xml, const std::wstring& fallbackSource, size_t most) {
    std::vector<Head> out;
    size_t at = 0;
    for (size_t item = xml.find("<item", at); item != std::string::npos && out.size() < most; item = xml.find("<item", at)) {
        size_t stop = xml.find("</item>", item);
        if (stop == std::string::npos) break;
        std::string it = xml.substr(item, stop - item);
        at = stop + 7;
        Head h;
        std::string title = Trim(Tag(it, "title")), source = Trim(Tag(it, "source"));
        if (title.empty()) continue;
        if (!source.empty() && title.size() > source.size() + 3 && title.compare(title.size() - source.size() - 3, std::string::npos, " - " + source) == 0)
            title.resize(title.size() - source.size() - 3); // google's titles end with their source
        h.title = Wide(title);
        h.source = source.empty() ? fallbackSource : Wide(source);
        h.link = Wide(Trim(Tag(it, "link")));
        h.at = Rfc822(Trim(Tag(it, "pubDate")));
        out.push_back(std::move(h));
    }
    std::stable_sort(out.begin(), out.end(), [](const Head& a, const Head& b) { return a.at > b.at; }); // newest first
    return out;
}

Card Headlines(Net& n, const std::wstring& query) {
    Card c;
    c.state = kFailed;
    std::string xml;
    std::wstring url = L"https://news.google.com/rss/search?q=" + Esc(query) + L"+when:7d&hl=en-US&gl=US&ceid=US:en"; // this week only
    if (!Get(n, url, xml, 2u << 20)) return c;
    c.heads = Rss(xml, L"News", kHeadCount);
    c.state = c.heads.empty() ? kFailed : kHeadlines;
    if (!c.heads.empty()) c.at = c.heads.front().at;
    return c;
}

// ---- espn ----
struct Match {
    const Val* ev = nullptr;
    const Val* comp = nullptr;
    int64_t at = 0;
    int state = 0; // 0 pre, 1 in, 2 post
};

int StateOf(const Val& status) {
    std::string s = status["type"]["state"].Str();
    return s == "in" ? 1 : s == "post" ? 2 : 0;
}

bool Race(const Val& comp) { // a racing weekend's sessions: only the race decides it
    std::string a = comp["type"]["abbreviation"].Str();
    return a.empty() || a == "Race";
}

void Collect(const Val& root, bool singlesFirst, bool racing, std::vector<Match>& out) {
    const Val& evs = root["events"];
    for (size_t e = 0; e < evs.size(); ++e) {
        const Val& ev = evs[e];
        auto add = [&](const Val& comp) {
            Match m;
            m.ev = &ev;
            m.comp = &comp;
            const Val& st = comp["status"].Null() ? ev["status"] : comp["status"];
            m.state = StateOf(st);
            m.at = Iso(comp["date"].Str().empty() ? ev["date"].Str() : comp["date"].Str());
            if (m.state == 2 && comp["competitors"].size() == 0) return; // a finished session nobody's listed in
            out.push_back(m);
        };
        const Val& groups = ev["groupings"];
        if (groups.size()) { // tennis: each draw is a grouping of matches
            for (size_t g = 0; g < groups.size(); ++g) {
                bool singles = Has(groups[g]["grouping"]["displayName"].Str(), "singles");
                if (singlesFirst && !singles) continue;
                const Val& comps = groups[g]["competitions"];
                for (size_t k = 0; k < comps.size(); ++k) add(comps[k]);
            }
            continue;
        }
        const Val& comps = ev["competitions"];
        for (size_t k = 0; k < comps.size(); ++k)
            if (!racing || Race(comps[k]) || StateOf(comps[k]["status"]) == 1) add(comps[k]); // a session in progress counts while it runs
    }
}

const Match* Best(const std::vector<Match>& ms, int state) { // the latest in a state; live ones prefer finals
    const Match* best = nullptr;
    for (const Match& m : ms)
        if (m.state == state && (!best || m.at >= best->at)) best = &m;
    return best;
}

const Match* Next(const std::vector<Match>& ms) {
    const Match* best = nullptr;
    for (const Match& m : ms)
        if (m.state == 0 && (!best || m.at < best->at)) best = &m;
    return best;
}

bool MajorText(const std::string& s) {
    for (const char* k : {"final", "super bowl", "world series", "stanley cup", "grand final", "championship game", "wimbledon", "australian open", "roland garros",
                          "french open", "us open", "masters tournament", "pga championship", "the open", "u.s. open"})
        if (Has(s, k) && !Has(s, "semifinal") && !Has(s, "quarterfinal") && !Has(s, "semi-final") && !Has(s, "quarter-final")) return true;
    return false;
}

std::wstring ShortName(const Val& c) {
    const Val& t = c["team"];
    if (!t.Null()) {
        std::string s = t["shortDisplayName"].Str();
        return Wide(s.empty() ? t["displayName"].Str() : s);
    }
    const Val& a = c["athlete"];
    if (!a.Null()) return W(a["displayName"]);
    const Val& r = c["roster"]; // doubles
    if (!r.Null()) return W(r["shortDisplayName"].Null() ? r["displayName"] : r["shortDisplayName"]);
    return W(c["displayName"]);
}

std::wstring Sets(const Val& c) { // tennis: no score, the sets are the score
    const Val& ls = c["linescores"];
    std::wstring s;
    for (size_t i = 0; i < ls.size(); ++i) {
        double v = ls[i]["value"].Num();
        s += (s.empty() ? L"" : L" ") + std::to_wstring((int)v);
    }
    return s;
}

void Goals(const Val& comp, Card& c) { // soccer's scorers, rugby's tries: who and when
    const Val& det = comp["details"];
    std::wstring s;
    int n = 0;
    for (size_t i = 0; i < det.size() && n < 4; ++i) {
        const Val& d = det[i];
        if (!d["scoringPlay"].Truth() && !d["redCard"].Truth()) continue;
        const Val& who = d["athletesInvolved"];
        std::wstring name = who.size() ? W(who[0]["shortName"].Null() ? who[0]["displayName"] : who[0]["shortName"]) : std::wstring();
        if (name.empty()) continue;
        s += (s.empty() ? L"" : L" · ") + name + L" " + W(d["clock"]["displayValue"]) + (d["redCard"].Truth() ? L" (red)" : L"");
        ++n;
    }
    if (!s.empty()) c.note = s;
}

Card FromMatch(const Match& m, const Val& root, const char* path) {
    Card c;
    const Val& ev = *m.ev;
    const Val& comp = *m.comp;
    const Val& st = comp["status"].Null() ? ev["status"] : comp["status"];
    c.state = m.state == 1 ? kLive : m.state == 2 ? kResult : kNext;
    c.at = m.at;
    std::string league = root["leagues"][0]["abbreviation"].Str();
    bool code = league.find(' ') == std::string::npos && std::none_of(league.begin(), league.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
    if (league.empty() || (code && league.size() > 4)) league = root["leagues"][0]["name"].Str(); // "CFHOC" says nothing, "NBA" does
    std::string headline = comp["notes"][0]["headline"].Str();
    bool solo = !comp["competitors"][0]["athlete"].Null() || !comp["competitors"][0]["roster"].Null(); // tennis, golf, racing, fights: the event names it
    std::string title = solo ? ev["name"].Str() : league;
    if (solo && !comp["round"]["displayName"].Null()) headline = comp["round"]["displayName"].Str(); // tennis' round
    c.title = Wide(title);
    if (!headline.empty()) c.title += L" · " + Wide(headline);
    c.major = MajorText(headline) || MajorText(ev["name"].Str()) || (solo && Has(headline, "final") && MajorText(title + " final"));
    if (Has(headline, "semifinal") || Has(headline, "quarterfinal")) c.major = false;
    std::string detail = st["type"]["shortDetail"].Str();
    if (detail.empty()) detail = st["type"]["detail"].Str();
    c.status = Wide(detail);
    const Val& links = ev["links"];
    for (size_t i = 0; i < links.size() && c.link.empty(); ++i) // the match page, not the ticket ads that sometimes come first
        if (Has(links[i]["href"].Str(), "espn.")) c.link = W(links[i]["href"]);
    const Val& cs = comp["competitors"];
    if (cs.size() > 2) { // a field: golf, racing
        c.list = true;
        std::vector<const Val*> order;
        for (size_t i = 0; i < cs.size(); ++i) order.push_back(&cs[i]);
        std::stable_sort(order.begin(), order.end(), [](const Val* a, const Val* b) { return (*a)["order"].Num() < (*b)["order"].Num(); });
        for (size_t i = 0; i < order.size() && (int)i < kRows; ++i) {
            const Val& x = *order[i];
            Row r;
            r.name = ShortName(x);
            r.sub = W(x["athlete"]["flag"]["alt"]);
            if (r.sub.empty()) r.sub = W(x["team"]["displayName"]);
            r.score = W(x["score"]);
            if (r.score == L"0" && strcmp(path, "racing") == 0) r.score.clear();
            r.win = x["winner"].Truth();
            c.rows.push_back(std::move(r));
        }
        if (c.state == kLive && !st["period"].Null() && strcmp(path, "racing") == 0) c.status = L"Lap " + W(st["period"]); // a race's period is its lap
        return c;
    }
    bool awayFirst = strcmp(path, "basketball") == 0 || strcmp(path, "football") == 0 || strcmp(path, "baseball") == 0 || strcmp(path, "hockey") == 0;
    std::vector<const Val*> sides;
    for (size_t i = 0; i < cs.size(); ++i) sides.push_back(&cs[i]);
    std::stable_sort(sides.begin(), sides.end(), [&](const Val* a, const Val* b) {
        bool ah = (*a)["homeAway"].Str() == "home", bh = (*b)["homeAway"].Str() == "home";
        if (ah == bh) return (*a)["order"].Num() < (*b)["order"].Num();
        return awayFirst ? bh : ah;
    });
    for (const Val* x : sides) {
        Row r;
        r.name = ShortName(*x);
        r.score = (*x)["score"].Null() ? Sets(*x) : W((*x)["score"]);
        if ((*x)["score"].kind == Val::kObj) r.score = W((*x)["score"]["displayValue"]);
        r.sub = W((*x)["athlete"]["flag"]["alt"]);
        const Val& rec = (*x)["records"][0]["summary"];
        if (r.sub.empty() && !rec.Null()) r.sub = W(rec);
        r.win = (*x)["winner"].Truth();
        c.rows.push_back(std::move(r));
    }
    Goals(comp, c);
    std::string last = comp["situation"]["lastPlay"]["text"].Str();
    if (c.note.empty() && m.state == 1 && !last.empty()) c.note = Wide(last);
    if (c.note.empty()) c.note = W(comp["headlines"][0]["shortLinkText"]);
    return c;
}

bool Board(Net& n, const char* path, const std::string& slug, const std::wstring& query, Val& root) {
    std::string body;
    std::wstring url = kEspnUrl + Wide(path) + L"/" + Wide(slug) + L"/scoreboard" + (query.empty() ? L"" : L"?" + query);
    return Get(n, url, body) && Parse(body, root) && root.kind == Val::kObj;
}

struct Day {
    int64_t at; // for ordering
    std::wstring query; // the scoreboard's query for it
};

std::vector<Day> Days(const Val& root, int64_t before) { // the calendar's match days, events or weeks before a moment, latest first
    constexpr int64_t kEastern = 5 * 3600; // espn files events under their us eastern date
    std::vector<Day> out;
    const Val& cal = root["leagues"][0]["calendar"];
    auto add = [&](const Val& v, const Val* season) {
        if (v.kind == Val::kStr) { // a match day, already a date
            int64_t at = Iso(v.str);
            if (at && at < before) out.push_back({at, L"dates=" + Ymd(at)});
            return;
        }
        int64_t from = Iso(v["startDate"].Str()), to = Iso(v["endDate"].Str());
        if (!from || (to ? to : from) >= before) return;
        if (season && !v["value"].Null() && !(*season)["value"].Null()) out.push_back({from, L"seasontype=" + W((*season)["value"]) + L"&week=" + W(v["value"])}); // a week of games
        else out.push_back({from, L"dates=" + Ymd(from - kEastern)}); // an event: a fight night, a race weekend, a tournament
    };
    for (size_t i = 0; i < cal.size(); ++i) {
        const Val& e = cal[i]["entries"];
        if (e.size())
            for (size_t k = 0; k < e.size(); ++k) add(e[k], &cal[i]);
        else add(cal[i], nullptr);
    }
    std::sort(out.begin(), out.end(), [](const Day& a, const Day& b) { return a.at > b.at; });
    return out;
}

Card EspnLeague(Net& n, const char* path, const std::string& slug, bool tennis, bool onlyActive) {
    Card none;
    none.state = kFailed;
    Val root;
    if (!Board(n, path, slug, L"", root)) return none;
    std::vector<Match> ms;
    Collect(root, tennis, strcmp(path, "racing") == 0, ms);
    if (const Match* m = Best(ms, 1)) return FromMatch(*m, root, path);
    if (const Match* m = Best(ms, 2)) {
        Card c = FromMatch(*m, root, path);
        if (!onlyActive || Now() - c.at < kMajorRecent) return c;
    }
    if (onlyActive) return none;
    const Match* next = Next(ms);
    int64_t first = next ? next->at : Now();
    std::vector<Day> days = Days(root, (std::min)(first, Now()));
    for (size_t d = 0; d < days.size() && d < 2; ++d) { // the last match day, or the one before when that day's still to finish
        Val older;
        if (!Board(n, path, slug, days[d].query, older)) continue;
        std::vector<Match> om;
        Collect(older, tennis, strcmp(path, "racing") == 0, om);
        if (const Match* m = Best(om, 2)) return FromMatch(*m, older, path);
        if (const Match* m = Best(om, 1)) return FromMatch(*m, older, path);
    }
    if (next) return FromMatch(*next, root, path);
    Card c;
    c.state = kNext;
    c.title = W(root["leagues"][0]["name"]);
    c.status = L"No matches scheduled";
    return c;
}

Card Better(Card a, Card b) { // of a men's and a women's card: live first, then the newer result
    auto rank = [](const Card& c) { return c.state == kLive ? 3 : c.state == kResult ? 2 : c.state == kNext ? 1 : 0; };
    if (rank(a) != rank(b)) return rank(a) > rank(b) ? a : b;
    if (a.state == kNext) return a.at <= b.at ? a : b;
    return a.at >= b.at ? a : b;
}

Card Espn(Net& n, const Pick& p, const Sport& sp) {
    bool tennis = strcmp(sp.path, "tennis") == 0;
    std::vector<int> sexes;
    if (!sp.women || p.sex != 1) sexes.push_back(0);
    if (sp.women && p.sex != 0) sexes.push_back(1);
    for (int sex : sexes) { // a world cup that's on outranks the weekly league
        const char* const* majors = sex ? sp.majorsW : sp.majors;
        const League& pick = sex ? sp.women[p.league[1]] : sp.men[p.league[0]];
        for (const char* const* m = majors; m && *m; ++m) {
            if (strcmp(*m, pick.slug) == 0) continue;
            Card c = EspnLeague(n, sp.path, *m, tennis, true);
            if (c.state == kLive || c.state == kResult) {
                c.major = true;
                return c;
            }
        }
    }
    Card best;
    best.state = kFailed;
    for (int sex : sexes) {
        const League& lg = sex ? sp.women[p.league[1]] : sp.men[p.league[0]];
        Card c = EspnLeague(n, sp.path, lg.slug, tennis, false);
        best = best.state == kFailed ? c : c.state == kFailed ? best : Better(best, c);
    }
    return best;
}

// ---- cricket: every match on now, from espn's header feed ----
Card Cricket(Net& n, const Pick& p, const Sport& sp) {
    Card c;
    c.state = kFailed;
    std::string body;
    Val root;
    if (!Get(n, L"https://site.web.api.espn.com/apis/v2/scoreboard/header?sport=cricket&lang=en&region=us", body) || !Parse(body, root)) return c;
    std::string kind = sp.men[p.league[p.sex == 1 ? 1 : 0]].slug;
    struct Pickd {
        const Val* ev;
        const Val* lg;
        int state;
        int64_t at;
        bool intl;
    };
    std::vector<Pickd> all;
    const Val& sports = root["sports"];
    for (size_t s = 0; s < sports.size(); ++s)
        for (size_t l = 0; l < sports[s]["leagues"].size(); ++l) {
            const Val& lg = sports[s]["leagues"][l];
            for (size_t e = 0; e < lg["events"].size(); ++e) {
                const Val& ev = lg["events"][e];
                std::string cls = ev["class"]["generalClassCard"].Str(), name = ev["name"].Str() + " " + lg["name"].Str();
                bool women = Has(name, "women"), intl = ev["class"]["internationalClassId"].Str() != "0" && !ev["class"]["internationalClassId"].Null();
                if ((p.sex == 0 && women) || (p.sex == 1 && !women)) continue;
                if ((kind == "intl" && !intl) || (kind == "t20" && !Has(cls, "t20") && !Has(cls, "twenty")) || (kind == "first" && !Has(cls, "first"))) continue;
                std::string st = ev["status"].Str();
                all.push_back({&ev, &lg, st == "in" ? 1 : st == "post" ? 2 : 0, Iso(ev["date"].Str()), intl});
            }
        }
    const Pickd* best = nullptr;
    auto score = [](const Pickd& d) { return (d.state == 1 ? 4 : d.state == 2 ? 2 : 0) + (d.intl ? 1 : 0); }; // live internationals first
    for (const Pickd& d : all)
        if (!best || score(d) > score(*best) || (score(d) == score(*best) && (d.state == 0 ? d.at < best->at : d.at > best->at))) best = &d;
    if (!best) {
        c.state = kNext;
        c.title = L"Cricket";
        c.status = L"No matches on right now";
        return c;
    }
    const Val& ev = *best->ev;
    c.state = best->state == 1 ? kLive : best->state == 2 ? kResult : kNext;
    c.at = best->at;
    c.title = W((*best->lg)["name"]);
    std::string desc = ev["class"]["generalClassCard"].Str();
    if (!desc.empty()) c.title += L" · " + Wide(desc);
    c.major = Has(ev["description"].Str(), "final") && !Has(ev["description"].Str(), "semi") && best->intl;
    c.status = W(ev["summary"]);
    c.note = W(ev["fullStatus"]["longSummary"]);
    if (c.note.empty()) c.note = W(ev["description"]);
    c.link = W(ev["link"]);
    for (size_t k = 0; k < ev["competitors"].size(); ++k) {
        const Val& x = ev["competitors"][k];
        Row r;
        r.name = W(x["displayName"]);
        r.score = W(x["score"]);
        r.win = x["winner"].Truth();
        c.rows.push_back(std::move(r));
    }
    return c;
}

// ---- chess: the top broadcast on lichess, fide's own events first ----
std::wstring Surname(const std::string& name) { // "Nakamura, Hikaru" -> "Nakamura"
    size_t comma = name.find(',');
    return Wide(comma == std::string::npos ? name : name.substr(0, comma));
}

Card Chess(Net& n, const Pick& p) {
    Card c;
    c.state = kFailed;
    std::string body;
    Val top;
    if (!Get(n, L"https://lichess.org/api/broadcast/top?page=1", body) || !Parse(body, top)) return c;
    const Val* best = nullptr;
    int bestScore = -1;
    bool bestActive = false;
    auto consider = [&](const Val& b, bool active) {
        std::string name = b["tour"]["name"].Str();
        bool women = Has(name, "women");
        if ((p.sex == 0 && women) || (p.sex == 1 && !women)) return;
        int s = (int)b["tour"]["tier"].Num() * 10 + (active ? 25 : 0);
        for (const char* k : {"world championship", "candidates", "olympiad", "world cup", "grand swiss", "world rapid", "world blitz", "grand prix"})
            if (Has(name, k)) s += 100; // fide's majors outrank everything else on
        if (Has(name, "fide")) s += 20;
        if (Has(name, "senior") || Has(name, "youth") || Has(name, "junior") || Has(name, "cadet") || Has(name, "u18") || Has(name, "school")) s -= 40;
        if (s > bestScore) best = &b, bestScore = s, bestActive = active;
    };
    for (size_t i = 0; i < top["active"].size(); ++i) consider(top["active"][i], true);
    const Val& past = top["past"]["currentPageResults"];
    for (size_t i = 0; i < past.size() && i < 10; ++i) consider(past[i], false);
    if (!best) return c;
    const Val& b = *best;
    std::string roundId = b["round"]["id"].Str();
    if (bestActive && !b["round"]["ongoing"].Truth() && !b["roundToLink"].Null()) roundId = b["roundToLink"]["id"].Str(); // between rounds: the last one played
    c.title = W(b["tour"]["name"]);
    c.link = W(b["tour"]["url"]);
    std::string name = b["tour"]["name"].Str();
    c.major = bestScore >= 100;
    Val round;
    if (roundId.empty() || !Get(n, L"https://lichess.org/api/broadcast/-/-/" + Wide(roundId), body) || !Parse(body, round)) return c;
    c.status = W(round["round"]["name"]);
    c.at = (int64_t)(round["round"]["startsAt"].Num() / 1000);
    const Val& games = round["games"];
    bool going = false;
    for (size_t g = 0; g < games.size(); ++g)
        if (games[g]["status"].Str() == "*") going = true;
    c.state = going ? kLive : kResult;
    for (size_t g = 0; g < games.size() && (int)c.rows.size() < kRows - 1; ++g) { // the top boards
        const Val& gm = games[g];
        const Val& pl = gm["players"];
        if (pl.size() < 2) continue;
        Row r;
        r.name = Surname(pl[0]["name"].Str()) + L" – " + Surname(pl[1]["name"].Str());
        r.sub = W(pl[0]["rating"]) + L" · " + W(pl[1]["rating"]);
        std::string st = gm["status"].Str();
        r.score = st == "*" ? L"playing" : Wide(st);
        c.rows.push_back(std::move(r));
    }
    Val players;
    if (Get(n, L"https://lichess.org/broadcast/" + W(b["tour"]["id"]) + L"/players", body) && Parse(body, players) && players.size()) {
        const Val* lead = &players[0];
        for (size_t i = 0; i < players.size(); ++i)
            if (players[i]["rank"].Num() == 1 || players[i]["score"].Num() > (*lead)["score"].Num()) lead = &players[i];
        wchar_t buf[64];
        swprintf_s(buf, L"%g / %g", (*lead)["score"].Num(), (*lead)["played"].Num());
        c.note = L"Leader " + Surname((*lead)["name"].Str()) + L", " + buf;
    }
    return c;
}

// ---- figure skating: the isu's own result pages ----
std::vector<std::string> Cells(const std::string& row) { // a table row's texts, tags dropped
    std::vector<std::string> out;
    std::string cur;
    bool tag = false;
    for (char ch : row) {
        if (ch == '<') {
            tag = true;
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(Decode(t));
            cur.clear();
        } else if (ch == '>') {
            tag = false;
        } else if (!tag) {
            cur += ch;
        }
    }
    std::string t = Trim(cur);
    if (!t.empty()) out.push_back(Decode(t));
    return out;
}

struct SkateEvent {
    std::wstring base, name;
    int64_t from = 0, to = 0;
};

bool SkateIndex(Net& n, const std::wstring& base, SkateEvent& ev, std::string& html) {
    if (!Get(n, base + L"index.htm", html, 1u << 20)) return false;
    ev.base = base;
    ev.name = Wide(Trim(Tag(html, "title")));
    for (const char* drop : {"ISU GP ", "ISU "})
        if (ev.name.rfind(Wide(drop), 0) == 0) ev.name = ev.name.substr(strlen(drop));
    int d1, m1, y1, d2, m2, y2;
    for (size_t at = html.find('.'); at != std::string::npos && at > 2; at = html.find('.', at + 1)) // "23.10.2026 - 25.10.2026"
        if (sscanf_s(html.c_str() + at - 2, "%d.%d.%d - %d.%d.%d", &d1, &m1, &y1, &d2, &m2, &y2) == 6 && y1 > 2000) {
            std::tm a{}, b{};
            a.tm_year = y1 - 1900, a.tm_mon = m1 - 1, a.tm_mday = d1;
            b.tm_year = y2 - 1900, b.tm_mon = m2 - 1, b.tm_mday = d2, b.tm_hour = 23;
            ev.from = (int64_t)_mkgmtime(&a);
            ev.to = (int64_t)_mkgmtime(&b);
            return true;
        }
    return false;
}

Card Skating(Net& n, const Pick& p, const Sport& sp) {
    static SkateEvent found; // the newest event, looked for every few hours: it's a dozen small pages
    static int64_t foundAt = 0;
    static std::string foundHtml;
    constexpr int64_t kLookEvery = 6 * 3600;
    int64_t now = Now();
    std::tm t{};
    time_t tt = (time_t)now;
    gmtime_s(&t, &tt);
    int year = t.tm_year + 1900, start = t.tm_mon >= 6 ? year : year - 1; // seasons turn in july
    if (now - foundAt > kLookEvery || found.base.empty()) {
        SkateEvent best;
        std::string bestHtml;
        for (int s = start; s >= start - 1 && best.base.empty(); --s) {
            wchar_t season[32];
            swprintf_s(season, L"season%02d%02d/", s % 100, (s + 1) % 100);
            std::vector<std::wstring> codes;
            for (const wchar_t* gp : {L"gpusa", L"gpcan", L"gpchn", L"gpfra", L"gpjpn", L"gpfin", L"gpf"}) codes.push_back(gp + std::to_wstring(s));
            for (const wchar_t* ch : {L"ec", L"fc", L"wc"}) codes.push_back(ch + std::to_wstring(s + 1));
            if ((s + 1) % 4 == 2) codes.push_back(L"owg" + std::to_wstring(s + 1)); // winter games years
            for (const std::wstring& code : codes) {
                SkateEvent ev;
                std::string html;
                if (n.stop) return Card{.state = kFailed};
                if (!SkateIndex(n, std::wstring(L"https://results.isu.org/results/") + season + code + L"/", ev, html) || ev.from > now) continue;
                if (ev.from > best.from) best = ev, bestHtml = html;
            }
        }
        if (best.base.empty()) {
            Card c = Headlines(n, sp.query); // no result pages reachable: the news has it
            return c;
        }
        found = best, foundHtml = bestHtml, foundAt = now;
    } else if (now >= found.from && now <= found.to + kDay) { // on now: the index gains result links as segments finish
        SkateEvent ev;
        std::string html;
        if (SkateIndex(n, found.base, ev, html)) foundHtml = html;
    }
    Card c;
    c.state = now >= found.from && now <= found.to ? kLive : kResult;
    c.title = found.name;
    c.at = found.to;
    c.major = true; // only grand prix and championships are looked at
    c.link = found.base + L"index.htm";
    c.list = true;
    static const char* const kCats[] = {"", "Men", "Women", "Pairs", "Ice Dance"};
    std::string want = kCats[(std::clamp)(p.league[0], 0, 4)];
    int shown = 0;
    for (size_t at = foundHtml.find("<tr"); at != std::string::npos && (int)c.rows.size() < kRows + 1; at = foundHtml.find("<tr", at + 3)) { // category rows link their result page
        size_t end = foundHtml.find("</tr>", at);
        std::string row = foundHtml.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t rs = row.find("RS.htm");
        if (rs == std::string::npos) continue;
        std::vector<std::string> cells = Cells(row);
        if (cells.empty()) continue;
        std::string cat = cells[0];
        if (!want.empty() && _stricmp(cat.c_str(), want.c_str()) != 0) continue;
        size_t href = row.rfind("href=", rs);
        if (href == std::string::npos) continue;
        std::string page = row.substr(href + 5, rs + 6 - href - 5);
        if (!page.empty() && (page[0] == '"' || page[0] == '\'')) page.erase(0, 1);
        std::string res;
        if (!Get(n, found.base + Wide(page), res, 1u << 20)) continue;
        int got = 0;
        auto outer = [&](size_t from) { // a placing's row; the nation cell nests a table whose row has the same class
            for (size_t r = res.find("<tr class=\"Line", from); r != std::string::npos; r = res.find("<tr class=\"Line", r + 10))
                if (r < 7 || res.compare(r - 7, 7, "<table>") != 0) return r;
            return std::string::npos;
        };
        for (size_t r = outer(0); r != std::string::npos && got < (want.empty() ? 1 : kRows); r = outer(r + 10)) {
            size_t next = outer(r + 10);
            std::string line = res.substr(r, (next == std::string::npos ? res.size() : next) - r);
            std::vector<std::string> v = Cells(line);
            if (v.size() < 4 || atoi(v[0].c_str()) <= 0) continue;
            Row rw;
            rw.name = Wide(v[1]);
            rw.sub = want.empty() ? Wide(cat) + L" · " + Wide(v[2]) : Wide(v[2]);
            rw.score = Wide(v[3]);
            rw.win = v[0] == "1";
            c.rows.push_back(std::move(rw));
            ++got;
        }
        ++shown;
        if (!want.empty()) break;
    }
    if (c.rows.empty()) {
        c.state = c.state == kLive ? kLive : kNext;
        c.status = c.state == kLive ? L"Under way, no results yet" : L"Results not published yet";
    } else {
        c.status = c.state == kLive ? L"Standings so far" : want.empty() ? L"Winners" : Wide(want);
    }
    c.note = L"Total points";
    return c;
}

// ---- motogp: the series' own results service ----
Card Moto(Net& n, const Pick& p) {
    Card c;
    c.state = kFailed;
    const std::wstring base = L"https://api.motogp.pulselive.com/motogp/v1/results/";
    std::string body;
    Val seasons, cats, evs, sessions, cls;
    if (!Get(n, base + L"seasons", body) || !Parse(body, seasons)) return c;
    std::string season;
    for (size_t i = 0; i < seasons.size(); ++i)
        if (seasons[i]["current"].Truth()) season = seasons[i]["id"].Str();
    if (season.empty() || !Get(n, base + L"categories?seasonUuid=" + Wide(season), body) || !Parse(body, cats)) return c;
    static const char* const kCat[] = {"MotoGP", "Moto2", "Moto3"};
    std::string cat;
    for (size_t i = 0; i < cats.size(); ++i)
        if (cats[i]["name"].Str().rfind(kCat[(std::clamp)(p.league[0], 0, 2)], 0) == 0) cat = cats[i]["id"].Str();
    if (cat.empty() || !Get(n, base + L"events?seasonUuid=" + Wide(season), body) || !Parse(body, evs)) return c;
    int64_t now = Now();
    const Val* pick = nullptr;
    bool live = false;
    for (size_t i = 0; i < evs.size(); ++i) { // the weekend on now, else the last one finished
        const Val& e = evs[i];
        if (e["test"].Truth()) continue;
        int64_t from = Iso(e["date_start"].Str() + "T00:00Z"), to = Iso(e["date_end"].Str() + "T23:59Z");
        if (now >= from && now <= to) pick = &e, live = true;
        else if (!live && to < now && e["status"].Str() == "FINISHED" && (!pick || to > Iso((*pick)["date_end"].Str() + "T23:59Z"))) pick = &e;
    }
    if (!pick || !Get(n, base + L"sessions?eventUuid=" + W((*pick)["id"]) + L"&categoryUuid=" + Wide(cat), body) || !Parse(body, sessions)) return c;
    const Val* ses = nullptr;
    for (size_t i = 0; i < sessions.size(); ++i) { // the race if it ran, else the latest finished session
        const Val& s = sessions[i];
        if (s["status"].Str() != "FINISHED") continue;
        std::string ty = s["type"].Str();
        if (!ses || ty == "RAC" || ((*ses)["type"].Str() != "RAC" && (ty == "SPR" || !live))) ses = &s;
        if (live && ty != "RAC" && ty != "SPR") ses = &s; // a live weekend shows what just ran
    }
    c.title = W((*pick)["sponsored_name"].Null() ? (*pick)["name"] : (*pick)["name"]);
    c.state = live ? kLive : kResult;
    c.at = Iso((*pick)["date_end"].Str() + "T12:00Z");
    c.list = true;
    c.link = L"https://www.motogp.com/en/gp-results";
    if (!ses) {
        c.state = live ? kLive : kNext;
        c.status = L"Sessions to come";
        return c;
    }
    std::string ty = (*ses)["type"].Str();
    c.status = ty == "RAC" ? L"Race" : ty == "SPR" ? L"Sprint" : ty == "Q" ? L"Qualifying " + W((*ses)["number"]) : ty == "FP" ? L"Practice " + W((*ses)["number"]) : Wide(ty);
    c.title = Wide(kCat[(std::clamp)(p.league[0], 0, 2)]) + L" · " + c.title;
    if (!Get(n, base + L"session/" + W((*ses)["id"]) + L"/classification?test=false", body) || !Parse(body, cls)) return c;
    const Val& rows = cls["classification"];
    for (size_t i = 0; i < rows.size() && (int)i < kRows; ++i) {
        Row r;
        r.name = W(rows[i]["rider"]["full_name"]);
        r.sub = W(rows[i]["team"]["name"]);
        std::string gap = rows[i]["gap"]["first"].Str();
        r.score = i == 0 ? W(rows[i]["time"]) : L"+" + Wide(gap);
        if (r.score == L"+") r.score.clear();
        r.win = i == 0;
        c.rows.push_back(std::move(r));
    }
    return c;
}

}

// ---- shared ----
const Val& Val::operator[](const char* key) const {
    if (kind == kObj)
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return items[i];
    return kNullVal;
}
const Val& Val::operator[](size_t i) const { return kind == kArr && i < items.size() ? items[i] : kNullVal; }
std::string Val::Str() const {
    if (kind == kStr) return str;
    if (kind == kNum) {
        char b[32];
        if (num == std::floor(num) && std::fabs(num) < 1e15) sprintf_s(b, "%lld", (long long)num);
        else sprintf_s(b, "%g", num);
        return b;
    }
    if (kind == kBool) return num ? "true" : "false";
    return {};
}
double Val::Num() const { return kind == kNum ? num : kind == kStr ? atof(str.c_str()) : 0; }

bool Parse(const std::string& text, Val& out) {
    out = Val{};
    Reader r{text};
    return r.Value(out);
}

std::string Quote(const std::wstring& s) {
    std::string u = Utf8(s), out = "\"";
    for (unsigned char c : u) {
        if (c == '"' || c == '\\') out += '\\', out += (char)c;
        else if (c < 0x20) {
            char b[8];
            sprintf_s(b, "\\u%04x", c);
            out += b;
        } else out += (char)c;
    }
    return out + "\"";
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u.data(), n, nullptr, nullptr);
    return u;
}

bool Get(Net& n, const std::wstring& url, std::string& body, size_t maxBytes) {
    for (int k = 0; k < kTries && !n.stop; ++k) {
        if (sys::detail::HttpGet(url.c_str(), body, n.stop, n.slot, maxBytes)) return true;
        if (k + 1 < kTries) Sleep(400 * (k + 1)); // a beat before the next try
    }
    return false;
}

Card Fetch(Net& n, const Pick& p) {
    const Sport& sp = SportAt(p.sport);
    Card c;
    switch (sp.src) {
    case kEspn: c = Espn(n, p, sp); break;
    case kCricket: c = Cricket(n, p, sp); break;
    case kChess: c = Chess(n, p); break;
    case kSkate: c = Skating(n, p, sp); break;
    case kMoto: c = Moto(n, p); break;
    default: c = Headlines(n, sp.query); break;
    }
    if (c.state == kFailed && sp.src != kHeads && !n.stop) { // scores out of reach: the news still says how it went
        Card h = Headlines(n, sp.query);
        if (h.state == kHeadlines) c = h;
    }
    c.fetched = Now();
    return c;
}

Card World(Net& n) {
    Card c;
    c.state = kFailed;
    std::string xml;
    if (Get(n, L"https://feeds.bbci.co.uk/news/world/rss.xml", xml, 2u << 20)) c.heads = Rss(xml, L"BBC News", kHeadCount);
    if (c.heads.empty() && Get(n, L"https://news.google.com/rss/headlines/section/topic/WORLD?hl=en-US&gl=US&ceid=US:en", xml, 2u << 20))
        c.heads = Rss(xml, L"News", kHeadCount); // the second source when the first is down
    if (!c.heads.empty()) c.state = kHeadlines, c.at = c.heads.front().at;
    c.title = L"World news";
    c.fetched = Now();
    return c;
}

}
