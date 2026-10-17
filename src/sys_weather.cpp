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

    Impl() {  // the last run's location: the first fetch is a single request
        std::wstring saved = ReadText(L"WxPlace");
        wchar_t place[64] = {};
        if (swscanf_s(saved.c_str(), L"%lf|%lf|%63[^\n]", &lat, &lon, place, (unsigned)_countof(place)) >= 2) city = place;
        else lat = lon = 0;
    }

    ~Impl() {
        detail::HttpCancel(stop, request);  // abort blocking read so join returns
        if (worker.joinable()) worker.join();
    }

    void Fetch(HWND notify, UINT msg) {
        try {
            std::string body;
            bool known = lat != 0 || lon != 0;
            if (!known || !geoAt || GetTickCount64() - geoAt > kGeoMs) {  // step 1: ip geolocation, refreshed now and then
                double la = 0, lo = 0;
                std::wstring c;
                if (!known) {  // nothing remembered: the forecast has to wait for it
                    if (!detail::HttpGet(kGeoUrl, body, stop, request) || !ParseGeo(body, la, lo, c)) {
                        OutputDebugStringW(L"[sys] weather: geolocation failed\n");
                        busy.store(false);  // nextOk already set to retry time
                        return;
                    }
                } else if (!(detail::HttpGet(kGeoUrl, body, stop, request) && ParseGeo(body, la, lo, c))) {
                    la = lat, lo = lon, c = city;  // lookup failed but we know roughly where you are: carry on with that
                }
                lat = la, lon = lo, city = std::move(c), geoAt = GetTickCount64();
                wchar_t saved[128];
                swprintf_s(saved, L"%.4f|%.4f|%s", lat, lon, city.c_str());
                SaveText(L"WxPlace", saved);  // next launch skips straight to the forecast
            }
            std::wstring url = L"https://api.open-meteo.com/v1/forecast?latitude=";  // step 2: forecast, no api key
            AddCoord(url, lat);
            url += L"&longitude=";
            AddCoord(url, lon);
            url += L"&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
                   L"&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=6&forecast_hours=9";  // timezone=auto so "today" is local
            Wx w;
            if (!detail::HttpGet(url.c_str(), body, stop, request) || !ParseWx(body, w)) {  // HttpGet clears body first
                OutputDebugStringW(L"[sys] weather: forecast failed\n");
                busy.store(false);
                return;
            }
            w.place = city;
            {
                std::lock_guard<std::mutex> lock(m);
                data = std::move(w);
            }
            nextOk.store(GetTickCount64() + kCacheMs);  // success: extend to cache window
            if (!stop.load() && notify && IsWindow(notify)) PostMessageW(notify, msg, 0, 0);  // tell ui to repaint
        } catch (...) {
            OutputDebugStringW(L"[sys] weather worker failed\n");
        }
        busy.store(false);
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
