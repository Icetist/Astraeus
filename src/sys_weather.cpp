#include "sys.h"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <thread>

namespace sys {


namespace {

constexpr ULONGLONG kCacheMs = 30 * 60 * 1000;  // refetch every 30 min
constexpr ULONGLONG kRetryMs = 30 * 1000;  // backoff after failure, short: a dropped request shouldn't leave the card empty for long
constexpr ULONGLONG kGeoMs = 6 * 60 * 60 * 1000;  // where you are barely changes: look it up every few hours, not every refresh
constexpr wchar_t kGeoUrl[] = L"http://ip-api.com/json/?fields=status,city,lat,lon";  // free tier is http only
constexpr wchar_t kCityUrl[] = L"https://geocoding-api.open-meteo.com/v1/search?format=json&name=";  // a typed city, no key needed; the count follows the name

std::wstring UrlPart(const std::wstring& s) {  // percent-encoded utf-8, for the query string
    std::string u;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    u.resize(n > 0 ? n : 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr, nullptr);
    std::wstring out;
    const wchar_t* hex = L"0123456789ABCDEF";
    for (unsigned char c : u) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') out += static_cast<wchar_t>(c);
        else out += {L'%', hex[c >> 4], hex[c & 15]};
    }
    return out;
}

void SkipWs(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

size_t FindKey(std::string_view s, std::string_view key, size_t from = 0) {  // tiny json scan, no parser dep
    std::string pattern;
    pattern.reserve(key.size() + 2);
    pattern += '"';  // match quoted key so "lat" won't hit inside values
    pattern += key;
    pattern += '"';
    for (size_t pos = s.find(pattern, from); pos != std::string_view::npos; pos = s.find(pattern, pos + 1)) {
        size_t i = pos + pattern.size();
        SkipWs(s, i);
        if (i < s.size() && s[i] == ':') {  // colon means key, not a string value
            ++i;
            SkipWs(s, i);
            return i;  // index of value start
        }
    }
    return std::string_view::npos;
}

bool NumAt(std::string_view s, size_t i, double& out) {
    if (i == std::string_view::npos) return false;
    if (i < s.size() && s[i] == '[') {  // daily values are arrays, take first
        ++i;
        SkipWs(s, i);
    }
    if (i >= s.size()) return false;
    const char* begin = s.data() + i;
    const auto [ptr, ec] = std::from_chars(begin, s.data() + s.size(), out);  // locale independent
    return ec == std::errc() && ptr != begin;
}

bool StrAt(std::string_view s, size_t i, std::string& out) {
    out.clear();
    if (i == std::string_view::npos || i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') return true;  // closing quote
        if (c == '\\' && i + 1 < s.size()) {
            const char e = s[++i];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'u': i += 4; out += '?'; break;  // skip \uXXXX, placeholder char
                default: out += e; break;  // \" \\ \/ pass through
            }
        } else {
            out += c;
        }
    }
    return false;  // unterminated string
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);  // size pass
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void AddCoord(std::wstring& url, double v) {
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 4);  // 4 dp ~ 11m, always '.' decimal
    if (ec != std::errc()) return;
    for (const char* p = buf; p < ptr; ++p) url += static_cast<wchar_t>(*p);  // ascii only, direct widen
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ParseCity(std::string_view json, double& lat, double& lon, std::wstring& name, const std::string& hint = {}) {  // the first match open-meteo found; hint: "france" from "Paris, France" picks among them
    const size_t res = FindKey(json, "results");
    if (res == std::string_view::npos) return false;  // no such place
    size_t pick = FindKey(json, "name", res);
    if (!hint.empty()) {
        for (size_t at = pick; at != std::string_view::npos;) {
            size_t next = FindKey(json, "name", at + 1); // the next result starts at its name
            std::string country, region, code;
            auto in = [&](const char* key, std::string& out) { size_t k = FindKey(json, key, at); return k != std::string_view::npos && k < next && StrAt(json, k, out); };
            in("country", country), in("admin1", region), in("country_code", code);
            if (LowerAscii(country).find(hint) == 0 || LowerAscii(region).find(hint) == 0 || LowerAscii(code) == hint) {
                pick = at;
                break;
            }
            at = next;
        }
    }
    if (!NumAt(json, FindKey(json, "latitude", pick), lat) || !NumAt(json, FindKey(json, "longitude", pick), lon)) return false;
    std::string n;
    if (StrAt(json, pick, n)) name = ToWide(n);
    return true;
}

bool ParseGeo(std::string_view json, double& lat, double& lon, std::wstring& city) {
    std::string status;
    if (!StrAt(json, FindKey(json, "status"), status) || status != "success") return false;  // ip-api returns 200 even on fail
    if (!NumAt(json, FindKey(json, "lat"), lat) || !NumAt(json, FindKey(json, "lon"), lon)) return false;
    std::string c;
    if (StrAt(json, FindKey(json, "city"), c)) city = ToWide(c);  // city optional
    return true;
}

size_t Nth(std::string_view s, size_t i, int k) {  // where the k-th value of the array at i starts, npos past its end
    if (i == std::string_view::npos || i >= s.size() || s[i] != '[') return std::string_view::npos;
    ++i;
    for (int n = 0;; ++n) {
        SkipWs(s, i);
        if (i >= s.size() || s[i] == ']') return std::string_view::npos;
        if (n == k) return i;
        bool quoted = false;
        for (; i < s.size(); ++i) {
            if (s[i] == '"') quoted = !quoted;
            else if (!quoted && (s[i] == ',' || s[i] == ']')) break;
        }
        if (i >= s.size() || s[i] == ']') return std::string_view::npos;
        ++i;
    }
}

void ParseAhead(std::string_view json, Wx& w) {  // hours and days to come; optional, the card works without them
    const size_t hourly = FindKey(json, "hourly"), daily = FindKey(json, "daily");
    w.nHours = w.nDays = 0;
    if (hourly != std::string_view::npos) {
        size_t times = FindKey(json, "time", hourly), temps = FindKey(json, "temperature_2m", hourly), codes = FindKey(json, "weather_code", hourly),
               days = FindKey(json, "is_day", hourly);
        for (int k = 1; k <= 8 && w.nHours < 8; ++k) {  // [0] is this hour
            std::string at;
            double t = 0, c = 0, d = 1;
            if (!StrAt(json, Nth(json, times, k), at) || at.size() < 13 || !NumAt(json, Nth(json, temps, k), t) || !NumAt(json, Nth(json, codes, k), c)) break;
            NumAt(json, Nth(json, days, k), d);
            w.hours[w.nHours++] = {std::atoi(at.c_str() + 11), t, static_cast<int>(c), d != 0};
        }
    }
    if (daily != std::string_view::npos) {
        size_t times = FindKey(json, "time", daily), his = FindKey(json, "temperature_2m_max", daily), los = FindKey(json, "temperature_2m_min", daily),
               codes = FindKey(json, "weather_code", daily);
        for (int k = 1; k <= 5 && w.nDays < 5; ++k) {  // [0] is today
            std::string at;
            double hi = 0, lo = 0, c = 0;
            if (!StrAt(json, Nth(json, times, k), at) || at.size() < 10 || !NumAt(json, Nth(json, his, k), hi) || !NumAt(json, Nth(json, los, k), lo) ||
                !NumAt(json, Nth(json, codes, k), c))
                break;
            w.days[w.nDays++] = {std::atoi(at.c_str()), std::atoi(at.c_str() + 5), std::atoi(at.c_str() + 8), hi, lo, static_cast<int>(c)};
        }
    }
}

bool ParseWx(std::string_view json, Wx& w) {
    const size_t cur = FindKey(json, "current");  // search from block start, units block repeats keys
    const size_t daily = FindKey(json, "daily");
    if (cur == std::string_view::npos || daily == std::string_view::npos) return false;
    double temp = 0, code = 0, isDay = 1, hi = 0, lo = 0;  // isDay defaults to day
    if (!NumAt(json, FindKey(json, "temperature_2m", cur), temp) ||
        !NumAt(json, FindKey(json, "weather_code", cur), code) ||
        !NumAt(json, FindKey(json, "temperature_2m_max", daily), hi) ||
        !NumAt(json, FindKey(json, "temperature_2m_min", daily), lo)) {
        return false;
    }
    NumAt(json, FindKey(json, "is_day", cur), isDay);  // optional, ignore failure
    w.tempC = temp;
    w.code = static_cast<int>(code);  // wmo weather code
    w.isDay = isDay != 0;
    w.highC = hi;
    w.lowC = lo;
    w.valid = true;
    ParseAhead(json, w);
    return true;
}

}

struct WxSvc::Impl {
    mutable std::mutex m;  // guards data; mutable for const Get
    Wx data;
    std::atomic<ULONGLONG> nextOk{0};  // earliest tick for next fetch
    std::thread worker;
    std::atomic<bool> busy{false};  // single fetch in flight
    std::atomic<bool> stop{false};
    std::atomic<void*> request{nullptr};  // live http handle for cancel
    double lat = 0, lon = 0;  // remembered location, worker thread only
    std::wstring city;
    ULONGLONG geoAt = 0;  // when it was looked up this run, 0 = only known from the last run
    std::wstring typed;  // a city typed in settings, guarded by m; empty = find you by ip
    bool typedKnown = false;  // its coordinates are below, guarded by m
    double typedLat = 0, typedLon = 0;
    std::wstring typedName;
    std::atomic<int> state{kCityNone};
    std::atomic<bool> redo{false};  // the city changed during a fetch: go again with the new one

    static bool Place(const std::wstring& saved, double& la, double& lo, std::wstring& name) {  // "lat|lon|name"
        wchar_t place[64] = {};
        if (swscanf_s(saved.c_str(), L"%lf|%lf|%63[^\n]", &la, &lo, place, (unsigned)_countof(place)) < 2) return false;
        name = place;
        return true;
    }

    Impl() {  // the last run's location: the first fetch is a single request
        if (!Place(ReadText(L"WxPlace"), lat, lon, city)) lat = lon = 0;
        typed = ReadText(L"WxCity");
        if (!typed.empty()) {
            typedKnown = Place(ReadText(L"WxCityPlace"), typedLat, typedLon, typedName);
            state = typedKnown ? kCityFound : kCitySearching;
        }
    }

    ~Impl() {
        detail::HttpCancel(stop, request);  // abort blocking read so join returns
        if (worker.joinable()) worker.join();
    }

    bool CityPlace(double& la, double& lo, std::wstring& name, std::string& body) {  // the typed city's coordinates, looked up once; false: no such place, or not reachable
        std::wstring want;
        {
            std::lock_guard<std::mutex> lock(m);
            if (typedKnown) {
                la = typedLat, lo = typedLon, name = typedName;
                return true;
            }
            want = typed;
        }
        std::wstring place = want, hint; // "Paris, France": the search takes the city alone, the rest picks among the matches
        if (size_t comma = want.find(L','); comma != std::wstring::npos) {
            auto trim = [](const std::wstring& s) { size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t"); return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1); };
            place = trim(want.substr(0, comma));
            hint = trim(want.substr(comma + 1));
        }
        std::wstring url = std::wstring(kCityUrl) + UrlPart(place) + (hint.empty() ? L"&count=1" : L"&count=10");
        double a = 0, b = 0;
        std::wstring n;
        std::string h;
        for (wchar_t c : hint) h += c < 0x80 ? static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c) : '?';
        if (!detail::HttpGet(url.c_str(), body, stop, request)) return false;  // offline: tried again on the next refresh
        if (!ParseCity(body, a, b, n, h)) {
            std::lock_guard<std::mutex> lock(m);
            if (typed == want) { // an answer for a name typed since doesn't count
                state = kCityMissing;
                nextOk.store(GetTickCount64() + kCacheMs); // no such place: not searched again every half minute, a new name asks at once
            }
            return false;
        }
        if (n.empty()) n = want;
        if (n.size() > 63) n.resize(63);  // what Place() reads back, and the saved line stays in its buffer
        std::lock_guard<std::mutex> lock(m);
        if (typed != want) return false;  // changed meanwhile: the redo handles it
        typedKnown = true, typedLat = a, typedLon = b, typedName = n;
        la = a, lo = b, name = n;
        wchar_t saved[128];
        swprintf_s(saved, L"%.4f|%.4f|%s", a, b, n.c_str());
        SaveText(L"WxCityPlace", saved);  // the next launch skips the search
        state = kCityFound;
        return true;
    }

    void Fetch(HWND notify, UINT msg) {
        for (;;) {
            FetchOnce(notify, msg);
            std::lock_guard<std::mutex> lock(m);  // SetCity looks at busy under the same lock: a change is never lost between the two
            if (!redo.exchange(false) || stop.load()) {
                busy.store(false);
                return;
            }
        }
    }

    void FetchOnce(HWND notify, UINT msg) {
        try {
            std::string body;
            bool manual;
            {
                std::lock_guard<std::mutex> lock(m);
                manual = !typed.empty();
            }
            double atLat = lat, atLon = lon;  // where the forecast is for
            std::wstring place = city;
            if (manual && !CityPlace(atLat, atLon, place, body)) {  // a typed city: your ip address is never looked up
                if (state == kCityMissing && !stop.load() && notify && IsWindow(notify)) PostMessageW(notify, msg, 0, 0);  // settings says so
                return;
            }
            bool known = lat != 0 || lon != 0;
            if (!manual && (!known || !geoAt || GetTickCount64() - geoAt > kGeoMs)) {  // step 1: ip geolocation, refreshed now and then
                double la = 0, lo = 0;
                std::wstring c;
                if (!known) {  // nothing remembered: the forecast has to wait for it
                    if (!detail::HttpGet(kGeoUrl, body, stop, request) || !ParseGeo(body, la, lo, c)) {
                        OutputDebugStringW(L"[sys] weather: geolocation failed\n");
                        return;  // nextOk already set to retry time
                    }
                } else if (!(detail::HttpGet(kGeoUrl, body, stop, request) && ParseGeo(body, la, lo, c))) {
                    la = lat, lo = lon, c = city;  // lookup failed but we know roughly where you are: carry on with that
                }
                lat = la, lon = lo, city = std::move(c), geoAt = GetTickCount64();
                wchar_t saved[128];
                swprintf_s(saved, L"%.4f|%.4f|%s", lat, lon, city.c_str());
                SaveText(L"WxPlace", saved);  // next launch skips straight to the forecast
            }
            if (!manual) atLat = lat, atLon = lon, place = city;
            std::wstring url = L"https://api.open-meteo.com/v1/forecast?latitude=";  // step 2: forecast, no api key
            AddCoord(url, atLat);
            url += L"&longitude=";
            AddCoord(url, atLon);
            url += L"&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
                   L"&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=6&forecast_hours=9";  // timezone=auto so "today" is local
            Wx w;
            if (!detail::HttpGet(url.c_str(), body, stop, request) || !ParseWx(body, w)) {  // HttpGet clears body first
                OutputDebugStringW(L"[sys] weather: forecast failed\n");
                return;
            }
            w.place = place;
            {
                std::lock_guard<std::mutex> lock(m);
                data = std::move(w);
            }
            nextOk.store(GetTickCount64() + kCacheMs);  // success: extend to cache window
            if (!stop.load() && notify && IsWindow(notify)) PostMessageW(notify, msg, 0, 0);  // tell ui to repaint
        } catch (...) {
            OutputDebugStringW(L"[sys] weather worker failed\n");
        }
    }
};

WxSvc::WxSvc() {
    try {
        impl = std::make_unique<Impl>();
    } catch (...) {
        OutputDebugStringW(L"[sys] WeatherService init failed\n");
    }
}

WxSvc::~WxSvc() = default;

void WxSvc::Refresh(HWND notify, UINT msg) {
    try {
        if (!impl || impl->busy.load()) return;
        const ULONGLONG now = GetTickCount64();
        if (now < impl->nextOk.load()) return;
        if (impl->worker.joinable()) impl->worker.join();  // previous worker done (busy false), reap it
        impl->busy.store(true);
        impl->nextOk.store(now + kRetryMs);  // pessimistic, fetch bumps on success
        try {
            impl->worker = std::thread([p = impl.get(), notify, msg] { p->Fetch(notify, msg); });  // raw ptr ok, dtor joins
        } catch (...) {
            impl->busy.store(false);
            throw;  // logged by outer catch
        }
    } catch (...) {
        OutputDebugStringW(L"[sys] WeatherService::Refresh failed\n");
    }
}

void WxSvc::SetCity(const std::wstring& name, HWND notify, UINT msg) {
    if (!impl) return;
    {
        std::lock_guard<std::mutex> lock(impl->m);
        impl->typed = name;
        impl->typedKnown = false;
        impl->state = name.empty() ? kCityNone : kCitySearching;
    }
    SaveText(L"WxCity", name);
    SaveText(L"WxCityPlace", L"");  // found again for the new name
    impl->nextOk.store(0);  // fetch now, whatever the cache says
    bool running;
    {
        std::lock_guard<std::mutex> lock(impl->m);
        running = impl->busy.load();
        if (running) impl->redo.store(true);  // a fetch for the old place is under way: it goes again
    }
    if (!running) Refresh(notify, msg);
}

std::wstring WxSvc::City() const {
    if (!impl) return {};
    std::lock_guard<std::mutex> lock(impl->m);
    return impl->typed;
}

int WxSvc::State() const { return impl ? impl->state.load() : kCityNone; }

Wx WxSvc::Get() const {
    try {
        if (impl) {
            std::lock_guard<std::mutex> lock(impl->m);
            return impl->data;  // copy under lock
        }
    } catch (...) {
        OutputDebugStringW(L"[sys] WeatherService::Get failed\n");
    }
    return {};
}

}
