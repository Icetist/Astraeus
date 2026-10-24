// Calendars from private ical links: fetched on a worker, parsed and expanded here, kept encrypted on disk.
// Same file in astraeus island and halobar.
#include "calendar.h"

#include <dpapi.h>
#include <icu.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "sys.h"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "icu.lib")

namespace sys::cal {

namespace {

constexpr int64_t kDay = 86400;
constexpr int64_t kPastDays = 62, kAheadDays = 400; // occurrences kept: two months back, a year and a bit ahead
constexpr size_t kMaxIcs = 8 << 20; // years of events stay well under this
constexpr int kMaxRepeats = 4000; // a rule with no end stops here even if the window didn't stop it
constexpr int kStoreTries = 10; // the other app may be reading the store just as this one replaces it
constexpr DWORD kStoreWaitMs = 30;
constexpr wchar_t kEntropy[] = L"Astraeus calendars";

// ---- days and dates, in plain civil arithmetic ----
int64_t DaysFromCivil(int y, int m, int d) { // days since 1970-01-01
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (m <= 2));
}

int Weekday(int64_t days) { return (int)(((days % 7) + 10) % 7); } // 0 monday .. 6 sunday; 1970-01-01 was a thursday
int DaysIn(int y, int m) {
    static const int k[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : k[m - 1];
}

// ---- time zones, through windows' own icu ----
class Zones { // one icu calendar per zone, for the length of a parse
public:
    Zones() = default;
    Zones(const Zones&) = delete;
    Zones& operator=(const Zones&) = delete;
    ~Zones() {
        for (auto& [k, c] : cals)
            if (c) ucal_close(c);
    }
    int64_t ToUnix(const std::wstring& tz, int y, int mo, int d, int h, int mi, int s) {
        UCalendar* c = For(tz);
        if (!c) return DaysFromCivil(y, mo, d) * kDay + h * 3600 + mi * 60 + s;
        UErrorCode st = U_ZERO_ERROR;
        ucal_clear(c);
        ucal_setDateTime(c, y, mo - 1, d, h, mi, s, &st);
        UDate ms = ucal_getMillis(c, &st);
        return U_SUCCESS(st) ? (int64_t)(ms / 1000) : 0;
    }
    int64_t LocalDay(int64_t unix) { // the local civil day an instant falls on
        UCalendar* c = For(L"");
        if (!c) return unix / kDay;
        UErrorCode st = U_ZERO_ERROR;
        ucal_setMillis(c, (UDate)unix * 1000, &st);
        int y = ucal_get(c, UCAL_YEAR, &st), m = ucal_get(c, UCAL_MONTH, &st) + 1, d = ucal_get(c, UCAL_DATE, &st);
        return U_SUCCESS(st) ? DaysFromCivil(y, m, d) : unix / kDay;
    }

private:
    UCalendar* For(const std::wstring& tz) {
        auto it = cals.find(tz);
        if (it != cals.end()) return it->second;
        std::wstring id = Canonical(tz);
        UErrorCode st = U_ZERO_ERROR;
        UCalendar* c = ucal_open(id.empty() ? nullptr : reinterpret_cast<const UChar*>(id.c_str()), (int32_t)id.size(), "", UCAL_GREGORIAN, &st);
        if (U_FAILURE(st)) c = nullptr;
        cals[tz] = c;
        return c;
    }
    static std::wstring Canonical(const std::wstring& tz) { // an iana name, or windows' own name as outlook writes it; empty = this pc's zone
        if (tz.empty()) return {};
        UChar out[128];
        UBool sys = 0;
        UErrorCode st = U_ZERO_ERROR;
        int32_t n = ucal_getCanonicalTimeZoneID(reinterpret_cast<const UChar*>(tz.c_str()), (int32_t)tz.size(), out, 128, &sys, &st);
        if (U_SUCCESS(st) && n > 0) return std::wstring(reinterpret_cast<wchar_t*>(out), n);
        st = U_ZERO_ERROR;
        n = ucal_getTimeZoneIDForWindowsID(reinterpret_cast<const UChar*>(tz.c_str()), (int32_t)tz.size(), nullptr, out, 128, &st);
        if (U_SUCCESS(st) && n > 0) return std::wstring(reinterpret_cast<wchar_t*>(out), n);
        return {};
    }
    std::unordered_map<std::wstring, UCalendar*> cals;
};

// ---- the ics text ----
struct Stamp { // a DTSTART-like value as written
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    bool date = false, utc = false;
    std::wstring tz;
    bool ok = false;
};

Stamp ParseStamp(const std::wstring& v, const std::wstring& tz, bool dateParam) {
    Stamp t;
    if (v.size() < 8) return t;
    for (size_t i = 0; i < 8; ++i)
        if (!iswdigit(v[i])) return t;
    auto num = [&](size_t at, size_t n) {
        int x = 0;
        for (size_t i = at; i < at + n && i < v.size(); ++i) x = x * 10 + (iswdigit(v[i]) ? v[i] - L'0' : 0);
        return x;
    };
    t.y = num(0, 4), t.mo = num(4, 2), t.d = num(6, 2);
    t.date = dateParam || v.size() < 15;
    if (!t.date) t.h = num(9, 2), t.mi = num(11, 2), t.s = num(13, 2), t.utc = v.size() > 15 && v[15] == L'Z';
    t.tz = t.utc ? L"UTC" : tz;
    t.ok = t.mo >= 1 && t.mo <= 12 && t.d >= 1 && t.d <= 31;
    return t;
}

int64_t ParseDuration(const std::wstring& v) { // P1D, PT1H30M, -PT15M...
    int64_t total = 0, n = 0;
    bool neg = !v.empty() && v[0] == L'-';
    for (wchar_t ch : v) {
        if (ch >= L'0' && ch <= L'9') {
            n = n * 10 + (ch - L'0');
            continue;
        }
        if (ch == L'W') total += n * 7 * kDay;
        else if (ch == L'D') total += n * kDay;
        else if (ch == L'H') total += n * 3600;
        else if (ch == L'M') total += n * 60;
        else if (ch == L'S') total += n;
        n = 0;
    }
    return neg ? -total : total;
}

std::wstring Unescape(const std::wstring& v) {
    std::wstring o;
    o.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t n = v[++i];
            o += n == L'n' || n == L'N' ? L' ' : n;
        } else {
            o += v[i];
        }
    }
    return o;
}

struct Raw {
    std::wstring uid, title, where, rrule;
    Stamp start, endAt;
    int64_t duration = -1;
    std::vector<Stamp> exdates;
    Stamp recurrenceId;
    bool cancelled = false;
};

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Param(const std::wstring& params, const wchar_t* key) { // ;TZID=Europe/London;VALUE=DATE
    size_t at = params.find(key);
    if (at == std::wstring::npos) return {};
    size_t b = at + wcslen(key), e = params.find(L';', b);
    std::wstring v = params.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    v.erase(std::remove(v.begin(), v.end(), L'"'), v.end());
    return v;
}

std::vector<Raw> ParseIcs(const std::string& body) {
    std::string flat; // unfolded as bytes first: a fold may split a multibyte character, widened apart it would turn into two broken ones
    flat.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\n' && i + 1 < body.size() && (body[i + 1] == ' ' || body[i + 1] == '\t')) {
            if (!flat.empty() && flat.back() == '\r') flat.pop_back();
            ++i; // the fold's space goes too
            continue;
        }
        flat += body[i];
    }
    std::wstring text = Widen(flat);
    std::vector<std::wstring> lines; // unfolded: a line that starts with a space continues the one before
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find(L'\n', at);
        std::wstring line = text.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && (line[0] == L' ' || line[0] == L'\t') && !lines.empty()) lines.back() += line.substr(1);
        else lines.push_back(std::move(line));
        if (e == std::wstring::npos) break;
        at = e + 1;
    }
    std::vector<Raw> out;
    Raw cur;
    bool in = false;
    int inner = 0; // inside an event's alarm: its summary and duration aren't the event's
    for (const auto& line : lines) {
        bool quoted = false;
        size_t colon = std::wstring::npos;
        for (size_t i = 0; i < line.size() && colon == std::wstring::npos; ++i) {
            if (line[i] == L'"') quoted = !quoted;
            else if (line[i] == L':' && !quoted) colon = i;
        }
        if (colon == std::wstring::npos) continue;
        std::wstring head = line.substr(0, colon), value = line.substr(colon + 1);
        size_t semi = head.find(L';');
        std::wstring name = head.substr(0, semi), params = semi == std::wstring::npos ? L"" : head.substr(semi);
        if (name == L"BEGIN" && value == L"VEVENT") {
            cur = Raw{}, in = true, inner = 0;
            continue;
        }
        if (name == L"END" && value == L"VEVENT") {
            if (in && cur.start.ok && (!cur.cancelled || cur.recurrenceId.ok)) out.push_back(std::move(cur)); // a cancelled occurrence still hides its slot in the series
            in = false;
            continue;
        }
        if (!in) continue;
        if (name == L"BEGIN") ++inner;
        else if (name == L"END" && inner > 0) --inner;
        if (inner > 0 || name == L"END") continue;
        std::wstring tz = Param(params, L"TZID=");
        bool dateParam = Param(params, L"VALUE=") == L"DATE";
        if (name == L"DTSTART") cur.start = ParseStamp(value, tz, dateParam);
        else if (name == L"DTEND") cur.endAt = ParseStamp(value, tz, dateParam);
        else if (name == L"DURATION") cur.duration = ParseDuration(value);
        else if (name == L"SUMMARY") cur.title = Unescape(value);
        else if (name == L"LOCATION") cur.where = Unescape(value);
        else if (name == L"UID") cur.uid = value;
        else if (name == L"RRULE") cur.rrule = value;
        else if (name == L"STATUS") cur.cancelled = value == L"CANCELLED";
        else if (name == L"RECURRENCE-ID") cur.recurrenceId = ParseStamp(value, tz, dateParam);
        else if (name == L"EXDATE") {
            for (size_t b = 0; b <= value.size();) {
                size_t e = value.find(L',', b);
                Stamp x = ParseStamp(value.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b), tz, dateParam);
                if (x.ok) cur.exdates.push_back(x);
                if (e == std::wstring::npos) break;
                b = e + 1;
            }
        }
    }
    return out;
}

struct Rule {
    enum Freq { None, Daily, Weekly, Monthly, Yearly } freq = None;
    int interval = 1, count = 0;
    int64_t until = 0; // unix, 0 = none
    std::vector<std::pair<int, int>> byDay; // (ordinal, weekday 0 monday): ordinal 0 = every one
    std::vector<int> byMonthDay;
    std::vector<int> byMonth; // a yearly series' months, "the last monday of may"
};

int ToInt(const std::wstring& s) { return (int)wcstol(s.c_str(), nullptr, 10); }

Rule ParseRule(const std::wstring& r, Zones& z) {
    Rule o;
    for (size_t b = 0; b < r.size();) {
        size_t e = r.find(L';', b);
        std::wstring part = r.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
        size_t eq = part.find(L'=');
        std::wstring k = part.substr(0, eq), v = eq == std::wstring::npos ? L"" : part.substr(eq + 1);
        if (k == L"FREQ") o.freq = v == L"DAILY" ? Rule::Daily : v == L"WEEKLY" ? Rule::Weekly : v == L"MONTHLY" ? Rule::Monthly : v == L"YEARLY" ? Rule::Yearly : Rule::None;
        else if (k == L"INTERVAL") o.interval = (std::max)(1, ToInt(v));
        else if (k == L"COUNT") o.count = (std::max)(0, ToInt(v));
        else if (k == L"UNTIL") {
            Stamp u = ParseStamp(v, L"", false);
            if (u.ok) o.until = u.date ? z.ToUnix(L"", u.y, u.mo, u.d, 23, 59, 59) : z.ToUnix(u.utc ? L"UTC" : L"", u.y, u.mo, u.d, u.h, u.mi, u.s);
        } else if (k == L"BYDAY" || k == L"BYMONTHDAY" || k == L"BYMONTH") {
            for (size_t c = 0; c <= v.size();) {
                size_t f = v.find(L',', c);
                std::wstring one = v.substr(c, f == std::wstring::npos ? std::wstring::npos : f - c);
                if (k == L"BYMONTHDAY" && !one.empty()) o.byMonthDay.push_back(ToInt(one));
                else if (k == L"BYMONTH" && ToInt(one) >= 1 && ToInt(one) <= 12) o.byMonth.push_back(ToInt(one));
                else if (k == L"BYDAY" && one.size() >= 2) {
                    static const wchar_t* const kDays[] = {L"MO", L"TU", L"WE", L"TH", L"FR", L"SA", L"SU"};
                    std::wstring day = one.substr(one.size() - 2), ord = one.substr(0, one.size() - 2);
                    for (int i = 0; i < 7; ++i)
                        if (day == kDays[i]) o.byDay.push_back({ord.empty() ? 0 : ToInt(ord), i});
                }
                if (f == std::wstring::npos) break;
                c = f + 1;
            }
        }
        if (e == std::wstring::npos) break;
        b = e + 1;
    }
    return o;
}

int64_t ToUnix(Zones& z, const Stamp& t) { return t.date ? z.ToUnix(L"", t.y, t.mo, t.d, 0, 0, 0) : z.ToUnix(t.tz, t.y, t.mo, t.d, t.h, t.mi, t.s); }

std::vector<int> MonthDays(const Rule& r, const Stamp& start, int y, int m) { // a monthly or yearly series' days in one month
    std::vector<int> out;
    bool pattern = r.freq == Rule::Monthly || (r.freq == Rule::Yearly && !r.byMonth.empty()); // a yearly rule with its months picks days within them the monthly way
    if (pattern && !r.byDay.empty()) {
        int64_t firstOfMonth = DaysFromCivil(y, m, 1);
        for (auto [ord, wd] : r.byDay) {
            std::vector<int> hits;
            for (int d = 1 + (wd - Weekday(firstOfMonth) + 7) % 7; d <= DaysIn(y, m); d += 7) hits.push_back(d);
            if (ord == 0) out.insert(out.end(), hits.begin(), hits.end());
            else if (ord > 0 && ord <= (int)hits.size()) out.push_back(hits[ord - 1]);
            else if (ord < 0 && -ord <= (int)hits.size()) out.push_back(hits[hits.size() + ord]);
        }
    } else if (pattern && !r.byMonthDay.empty()) {
        for (int d : r.byMonthDay) {
            int dd = d > 0 ? d : DaysIn(y, m) + d + 1;
            if (dd >= 1 && dd <= DaysIn(y, m)) out.push_back(dd);
        }
    } else if ((pattern || m == start.mo) && start.d <= DaysIn(y, m)) {
        out.push_back(start.d); // the 31st skips short months, feb 29 skips other years
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Expand(const Raw& ev, const std::unordered_set<std::wstring>& overridden, Zones& z, int64_t from, int64_t to, std::vector<Event>& out) {
    if (ev.cancelled) return; // a deleted occurrence of a series: it only hides the one it replaces
    int64_t first = ToUnix(z, ev.start);
    int64_t length = ev.duration >= 0 ? ev.duration : ev.endAt.ok ? ToUnix(z, ev.endAt) - first : ev.start.date ? kDay : 0;
    length = (std::max)(length, (int64_t)0);
    auto emit = [&](int64_t s) {
        if (s + length < from || s > to) return;
        for (const auto& x : ev.exdates)
            if (std::llabs(ToUnix(z, x) - s) < 60) return; // taken out of the series
        if (!ev.uid.empty() && overridden.count(ev.uid + L"@" + std::to_wstring(s))) return; // moved or edited: that copy is its own event
        out.push_back({s, s + length, ev.start.date, ev.title, ev.where});
    };
    if (ev.rrule.empty() || ev.recurrenceId.ok) return emit(first);
    Rule r = ParseRule(ev.rrule, z);
    if (r.freq == Rule::None) return emit(first);
    int64_t startDay = DaysFromCivil(ev.start.y, ev.start.mo, ev.start.d);
    int made = 0, guard = 0;
    auto occur = [&](int64_t day) { // false once the series is over
        if (day < startDay) return true;
        Stamp t = ev.start;
        CivilFromDays(day, t.y, t.mo, t.d);
        int64_t s = ToUnix(z, t);
        if ((r.until && s > r.until) || (r.count && made >= r.count)) return false;
        ++made;
        emit(s);
        return s <= to;
    };
    int64_t fromDay = from / kDay - 2; // a day of slack each side for zones; COUNT needs every occurrence from the start, so no skipping then
    if (r.freq == Rule::Daily) {
        int64_t day = startDay;
        if (!r.count && fromDay > day) day += (fromDay - day) / r.interval * r.interval; // a daily habit begun years ago: straight to the window, it would run out of repeats first
        for (; guard++ < kMaxRepeats && occur(day); day += r.interval) {
        }
    } else if (r.freq == Rule::Weekly) {
        std::vector<int> days;
        for (auto [ord, wd] : r.byDay) days.push_back(wd);
        if (days.empty()) days.push_back(Weekday(startDay));
        std::sort(days.begin(), days.end());
        bool going = true;
        int64_t week = startDay - Weekday(startDay), step = 7LL * r.interval;
        if (!r.count && fromDay - 7 > week) week += (fromDay - 7 - week) / step * step; // same for a weekly one: the same weeks of its cycle, just later
        for (; going && guard++ < kMaxRepeats; week += step) // weeks start on monday, as ical's default
            for (int wd : days)
                if (!(going = occur(week + wd))) break;
    } else {
        bool going = true;
        std::vector<int> months = r.byMonth; // yearly: its months, in order, or the start's own
        if (months.empty()) months.push_back(ev.start.mo);
        std::sort(months.begin(), months.end());
        for (int y = ev.start.y, m = ev.start.mo; going && guard++ < kMaxRepeats;) {
            if (r.freq == Rule::Monthly) {
                for (int d : MonthDays(r, ev.start, y, m))
                    if (!(going = occur(DaysFromCivil(y, m, d)))) break;
                m += r.interval;
                while (m > 12) m -= 12, ++y;
            } else {
                for (int mo : months)
                    for (int d : MonthDays(r, ev.start, y, mo))
                        if (going && !(going = occur(DaysFromCivil(y, mo, d)))) break;
                y += r.interval;
            }
        }
    }
}

// ---- the encrypted list of links ----
std::wstring StorePath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(base) + L"\\Astraeus";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\accounts";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\calendars.dat";
}

std::vector<Link> ReadLinks() {
    std::vector<Link> links;
    std::wstring path = StorePath();
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr); // share delete: the other app's save can swap the file in meanwhile
    if (f == INVALID_HANDLE_VALUE) return links;
    std::vector<BYTE> blob;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        blob.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, blob.data(), (DWORD)blob.size(), &got, nullptr) || got != blob.size()) blob.clear();
    }
    CloseHandle(f);
    DATA_BLOB in{(DWORD)blob.size(), blob.data()}, out{};
    DATA_BLOB salt{(DWORD)sizeof(kEntropy), (BYTE*)kEntropy};
    if (blob.empty() || !CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return links; // another user's, or damaged
    std::wstring text(reinterpret_cast<wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find(L'\n', at), tab = text.find(L'\t', at);
        if (e == std::wstring::npos) e = text.size();
        if (tab != std::wstring::npos && tab < e) links.push_back({text.substr(at, tab - at), text.substr(tab + 1, e - tab - 1)});
        at = e + 1;
    }
    SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    return links;
}

bool WriteLinks(const std::vector<Link>& links) {
    std::wstring text;
    for (const auto& l : links) text += l.url + L"\t" + l.name + L"\n";
    DATA_BLOB in{(DWORD)(text.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(text.data())}, out{};
    DATA_BLOB salt{(DWORD)sizeof(kEntropy), (BYTE*)kEntropy};
    bool ok = CryptProtectData(&in, L"Astraeus calendars", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE; // only this windows user can read it back
    SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    if (!ok) return false;
    std::wstring path = StorePath(), temp = path + L".new";
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD done = 0;
    ok = f != INVALID_HANDLE_VALUE && WriteFile(f, out.pbData, out.cbData, &done, nullptr) && done == out.cbData;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    LocalFree(out.pbData);
    for (int i = 0; ok && i < kStoreTries; ++i) { // whole or not at all; the other app may be reading it this instant
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(kStoreWaitMs);
    }
    DeleteFileW(temp.c_str());
    return false;
}

std::wstring NameFor(const std::wstring& url) {
    if (url.find(L"google.com") != std::wstring::npos) return L"Google Calendar";
    if (url.find(L"outlook.") != std::wstring::npos || url.find(L"office365") != std::wstring::npos) return L"Outlook";
    if (url.find(L"icloud.com") != std::wstring::npos) return L"iCloud";
    return L"Calendar";
}

} // namespace

struct Calendars::Impl {
    mutable std::mutex lock;
    std::vector<Event> events; // sorted by start
    std::unordered_set<int64_t> days; // local days with something on
    std::vector<Link> links;
    std::atomic<int> busy{0};
    int64_t syncedAt = 0;
    bool again = false; // asked for while a fetch ran (a link just added): one more right after
    HWND againTo = nullptr;
    UINT againMsg = 0;
    std::atomic<bool> stop{false};
    std::atomic<void*> req{nullptr};
};

Calendars::Calendars() : impl(std::make_shared<Impl>()) { impl->links = ReadLinks(); }

Calendars::~Calendars() {
    detail::HttpCancel(impl->stop, impl->req); // a fetch in flight ends now; the worker sees the flag and leaves quietly
}

bool Calendars::Linked() const {
    std::lock_guard g(impl->lock);
    return !impl->links.empty();
}

bool Calendars::Busy() const { return impl->busy > 0; }

std::vector<Link> Calendars::Links() const {
    std::lock_guard g(impl->lock);
    return impl->links;
}

void Calendars::Sync(HWND notify, UINT msg, int maxAge) {
    int64_t now = (int64_t)time(nullptr);
    {
        std::lock_guard g(impl->lock);
        if (impl->busy > 0 && maxAge == 0) impl->again = true, impl->againTo = notify, impl->againMsg = msg; // the running fetch may predate a new link
        if (now - impl->syncedAt < maxAge || impl->busy > 0) return;
        impl->syncedAt = now;
        ++impl->busy;
    }
    auto s = impl;
    std::thread([s, notify, msg, now] {
        std::vector<Link> links = ReadLinks(); // the other app may have added one
        std::vector<Raw> raws;
        bool any = false;
        for (const auto& l : links) {
            std::string body;
            if (s->stop || !detail::HttpGet(l.url.c_str(), body, s->stop, s->req, kMaxIcs)) continue;
            if (body.find("BEGIN:VCALENDAR") == std::string::npos) continue; // a login page or an error, not a calendar
            auto more = ParseIcs(body);
            raws.insert(raws.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
            any = true;
        }
        if (s->stop) return void(--s->busy);
        Zones z;
        std::unordered_set<std::wstring> overridden; // series occurrences replaced by an edited copy
        for (const auto& r : raws)
            if (r.recurrenceId.ok && !r.uid.empty()) overridden.insert(r.uid + L"@" + std::to_wstring(ToUnix(z, r.recurrenceId)));
        std::vector<Event> events;
        int64_t from = now - kPastDays * kDay, to = now + kAheadDays * kDay;
        for (const auto& r : raws) Expand(r, overridden, z, from, to, events);
        std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
        std::unordered_set<int64_t> days;
        for (const auto& e : events) {
            int64_t d0 = z.LocalDay(e.start), d1 = z.LocalDay((std::max)(e.start, e.end - 1));
            for (int64_t d = d0; d <= d1 && d - d0 < 60; ++d) days.insert(d);
        }
        bool more = false;
        HWND moreTo = nullptr;
        UINT moreMsg = 0;
        {
            std::lock_guard g(s->lock);
            s->links = links;
            if (any || links.empty()) s->events = std::move(events), s->days = std::move(days); // offline: keep what we had
            else s->syncedAt = 0; // nothing answered (early at sign-in, offline): the next open asks again at once
            std::swap(more, s->again);
            moreTo = s->againTo, moreMsg = s->againMsg;
            --s->busy;
        }
        if (notify && msg) PostMessageW(notify, msg, 0, 0);
        if (more && moreTo && moreMsg) PostMessageW(moreTo, moreMsg, 1, 0); // wParam 1: the owner syncs again, now that this one is done
    }).detach();
}

std::vector<Event> Calendars::Day(int y, int m, int d) const {
    Zones z;
    int ny, nm, nd;
    CivilFromDays(DaysFromCivil(y, m, d) + 1, ny, nm, nd);
    int64_t s = z.ToUnix(L"", y, m, d, 0, 0, 0), e = z.ToUnix(L"", ny, nm, nd, 0, 0, 0); // a dst day is 23 or 25 hours
    std::vector<Event> out;
    {
        std::lock_guard g(impl->lock);
        for (const auto& ev : impl->events) {
            if (ev.start >= e) break;
            if (ev.end > s || (ev.end == ev.start && ev.start >= s)) out.push_back(ev);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.allDay > b.allDay; });
    return out;
}

bool Calendars::HasEvents(int y, int m, int d) const {
    std::lock_guard g(impl->lock);
    return impl->days.count(DaysFromCivil(y, m, d)) != 0;
}

bool Calendars::LooksLikeCalendar(const std::wstring& text, std::wstring* url) {
    size_t b = text.find_first_not_of(L" \t\r\n"), e = text.find_last_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return false;
    std::wstring s = text.substr(b, e - b + 1);
    if (s.size() > 2048 || s.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
    if (s.rfind(L"webcal://", 0) == 0) s = L"https://" + s.substr(9);
    if (s.rfind(L"https://", 0) != 0) return false;
    std::wstring lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    std::wstring path = lower.substr(0, lower.find(L'?'));
    if (lower.find(L"/owa/calendar/") != std::wstring::npos && path.size() > 5 && path.compare(path.size() - 5, 5, L".html") == 0) { // outlook shares an html page and an ics link side by side: the page is a website, the ics is the calendar
        s.replace(path.size() - 5, 5, L".ics");
        lower.replace(path.size() - 5, 5, L".ics");
        path.replace(path.size() - 5, 5, L".ics");
    }
    bool ics = path.size() > 4 && path.compare(path.size() - 4, 4, L".ics") == 0;
    bool known = lower.find(L"calendar.google.com/calendar/ical/") != std::wstring::npos || lower.find(L"/owa/calendar/") != std::wstring::npos ||
                 lower.find(L"icloud.com/published/") != std::wstring::npos;
    if (!ics && !known) return false;
    if (url) *url = s;
    return true;
}

bool Calendars::Add(const std::wstring& text, HWND notify, UINT msg) {
    std::wstring url;
    if (!LooksLikeCalendar(text, &url)) return false;
    std::vector<Link> links = ReadLinks();
    for (const auto& l : links)
        if (l.url == url) { // already connected: copied again to try again, so it's fetched now
            Sync(notify, msg, 0);
            return true;
        }
    links.push_back({url, NameFor(url)});
    if (!WriteLinks(links)) return false;
    {
        std::lock_guard g(impl->lock);
        impl->links = links;
    }
    Sync(notify, msg, 0);
    return true;
}

void Calendars::Remove(size_t i, HWND notify, UINT msg) {
    std::vector<Link> links = ReadLinks();
    if (i >= links.size()) return;
    links.erase(links.begin() + (ptrdiff_t)i);
    WriteLinks(links);
    {
        std::lock_guard g(impl->lock);
        impl->links = links;
    }
    Sync(notify, msg, 0);
}

std::wstring Calendars::NewEventUrl(int y, int m, int d) {
    int ny, nm, nd;
    CivilFromDays(DaysFromCivil(y, m, d) + 1, ny, nm, nd);
    wchar_t buf[160];
    swprintf_s(buf, L"https://calendar.google.com/calendar/render?action=TEMPLATE&dates=%04d%02d%02d/%04d%02d%02d", y, m, d, ny, nm, nd); // all day, the user sets a time there
    return buf;
}

} // namespace sys::cal
