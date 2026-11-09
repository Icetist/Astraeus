#include "feeds.h"

#include <wincrypt.h>
#include <dpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "sports.h"
#include "sys.h"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "winhttp.lib")

namespace feeds {
namespace {

std::atomic<bool> gQuit{false}; // the worker is being stopped: a fetch of many requests gives up between them
constexpr size_t kMaxBody = 4u << 20; // an answer larger than this is not the answer
constexpr int kTimeoutMs = 8000;
constexpr int kMaxPages = 10; // a store with more than this many pages of a month's sales: the sum stops here
constexpr int kMaxDepth = 64;
constexpr int64_t kDaySec = 86400;
constexpr int kWindowDays = 30;
constexpr int kChartDays = 14;
constexpr ULONGLONG kEveryQuote = 5 * 60000, kEveryMetric = 2 * 60000, kEveryRates = 6 * 3600000ULL, kEverySky = 30 * 60000, kEveryAccount = 15 * 60000, kEveryThermal = 15000;
constexpr ULONGLONG kEverySport = 60000; // scores: a live game moves by the minute
constexpr ULONGLONG kRetry = 3 * 60000; // a failure is asked again sooner than a success, never in a spin
constexpr DWORD kScreenTickMs = 15000; // how often the foreground app is noted
constexpr DWORD kIdleMs = 5 * 60000; // nothing touched this long: away, not counted
constexpr int kSaveEvery = 20; // ticks between writes of the log
constexpr int kKeepDays = 8;

// ---- a small json reader ----
struct J {
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
};

class Reader {
public:
    Reader(const char* begin, const char* end) : p(begin), e(end) {}
    bool Parse(J& out, int depth = 0) {
        if (depth > kMaxDepth) return false;
        Skip();
        if (p >= e) return false;
        char c = *p;
        if (c == '{') return Obj(out, depth);
        if (c == '[') return Arr(out, depth);
        if (c == '"') return out.type = J::kStr, Str(out.s);
        if (c == 't' || c == 'f') {
            size_t len = c == 't' ? 4 : 5;
            if ((size_t)(e - p) < len) return false;
            p += len, out.type = J::kBool, out.b = c == 't';
            return true;
        }
        if (c == 'n') {
            if (e - p < 4) return false;
            p += 4;
            return true;
        }
        std::string tmp(p, (std::min)((size_t)(e - p), (size_t)64));
        char* end = nullptr;
        out.n = strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        p += end - tmp.c_str(), out.type = J::kNum;
        return true;
    }

private:
    const char *p, *e;
    void Skip() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool Str(std::string& s) {
        ++p;
        while (p < e && *p != '"') {
            if (*p != '\\') {
                s += *p++;
                continue;
            }
            if (++p >= e) return false;
            char c = *p++;
            if (c == 'n') s += '\n';
            else if (c == 't') s += '\t';
            else if (c == 'r') s += '\r';
            else if (c == 'u') {
                if (e - p < 4) return false;
                unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp < 0x80) s += (char)cp;
                else if (cp < 0x800) s += (char)(0xC0 | (cp >> 6)), s += (char)(0x80 | (cp & 0x3F));
                else s += (char)(0xE0 | (cp >> 12)), s += (char)(0x80 | ((cp >> 6) & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
            } else s += c;
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool Arr(J& out, int depth) {
        out.type = J::kArr;
        ++p;
        Skip();
        if (p < e && *p == ']') return ++p, true;
        while (p < e) {
            out.a.emplace_back();
            if (!Parse(out.a.back(), depth + 1)) return false;
            Skip();
            if (p < e && *p == ',') {
                ++p;
                continue;
            }
            if (p < e && *p == ']') return ++p, true;
            return false;
        }
        return false;
    }
    bool Obj(J& out, int depth) {
        out.type = J::kObj;
        ++p;
        Skip();
        if (p < e && *p == '}') return ++p, true;
        while (p < e) {
            Skip();
            std::string key;
            if (p >= e || *p != '"' || !Str(key)) return false;
            Skip();
            if (p >= e || *p != ':') return false;
            ++p;
            out.o.emplace_back(std::move(key), J{});
            if (!Parse(out.o.back().second, depth + 1)) return false;
            Skip();
            if (p < e && *p == ',') {
                ++p;
                continue;
            }
            if (p < e && *p == '}') return ++p, true;
            return false;
        }
        return false;
    }
};

bool ParseBody(const std::string& body, J& out) { return Reader(body.data(), body.data() + body.size()).Parse(out); }

const J* At(const J& root, const char* path) { // "data.0.attributes.total": objects by name, arrays by number
    const J* j = &root;
    for (const char* s = path; s && *s && j;) {
        const char* dot = strchr(s, '.');
        std::string part = dot ? std::string(s, dot - s) : std::string(s);
        s = dot ? dot + 1 : nullptr;
        if (j->type == J::kArr) {
            size_t i = (size_t)atoi(part.c_str());
            j = i < j->a.size() ? &j->a[i] : nullptr;
        } else if (j->type == J::kObj) {
            const J* next = nullptr;
            for (const auto& kv : j->o)
                if (kv.first == part) {
                    next = &kv.second;
                    break;
                }
            j = next;
        } else {
            j = nullptr;
        }
    }
    return j;
}

double Num(const J* j, double fallback = 0) { // numbers often come as strings
    if (!j) return fallback;
    if (j->type == J::kNum) return j->n;
    if (j->type == J::kStr && !j->s.empty()) return atof(j->s.c_str());
    return fallback;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Esc(const std::wstring& s) { // for a url
    std::string u = Narrow(s);
    std::wstring out;
    wchar_t buf[8];
    for (unsigned char ch : u) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (wchar_t)ch;
        else swprintf_s(buf, L"%%%02X", ch), out += buf;
    }
    return out;
}

std::string JsonText(const std::wstring& s) { // for a value inside a json string
    std::string out;
    for (char ch : Narrow(s)) {
        if (ch == '"' || ch == '\\') out += '\\';
        if ((unsigned char)ch >= 0x20) out += ch;
    }
    return out;
}

// ---- http: https only, its own session, bounded ----
struct Resp {
    int status = 0;
    std::string body;
};

Resp Http(const std::wstring& url, const std::wstring& headers = L"", const std::string& post = "") {
    Resp out;
    if (url.rfind(L"https://", 0) != 0) return out; // keys never travel in the clear
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    URL_COMPONENTS uc{sizeof(uc)};
    uc.lpszHostName = host, uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path, uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return out;
    static const HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) WinHttpSetTimeouts(s, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        return s;
    }();
    if (!session) return out;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) return out;
    std::wstring object = std::wstring(path) + extra;
    HINTERNET req = WinHttpOpenRequest(conn, post.empty() ? L"GET" : L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req) {
        std::wstring all = headers;
        if (!post.empty()) all += L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(req, all.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : all.c_str(), all.empty() ? 0 : (DWORD)-1L, post.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)post.data(), (DWORD)post.size(), (DWORD)post.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            out.status = (int)status;
            char chunk[8192];
            DWORD got = 0;
            while (WinHttpReadData(req, chunk, sizeof(chunk), &got) && got && out.body.size() + got <= kMaxBody) out.body.append(chunk, got);
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
    return out;
}

// ---- secrets ----
std::wstring ToBase64(const BYTE* data, DWORD size) {
    DWORD chars = 0;
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) return {};
    std::wstring s(chars, L'\0');
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &chars)) return {};
    s.resize(chars);
    return s;
}

std::string FromBase64(const std::wstring& s) {
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(s.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return {};
    std::string out(bytes, '\0');
    if (!CryptStringToBinaryW(s.c_str(), 0, CRYPT_STRING_BASE64, (BYTE*)out.data(), &bytes, nullptr, nullptr)) return {};
    out.resize(bytes);
    return out;
}

std::wstring SettingName(const char* key) { return L"Wg." + Wide(key); }

// ---- dates ----
int64_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL);
}

std::wstring IsoDay(int64_t unix) { // yyyy-mm-dd, utc
    ULONGLONG u = ((ULONGLONG)unix + 11644473600ULL) * 10000000ULL;
    FILETIME f = {(DWORD)u, (DWORD)(u >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&f, &st);
    wchar_t b[16];
    swprintf_s(b, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

int64_t ParseDay(const J* j) { // a unix time or an iso date, to whole days since 1970
    if (!j) return 0;
    if (j->type == J::kNum) return (int64_t)(j->n > 1e11 ? j->n / 1000 : j->n) / kDaySec;
    if (j->type != J::kStr || j->s.size() < 10) return 0;
    SYSTEMTIME st{};
    int y = 0, m = 0, d = 0;
    if (sscanf_s(j->s.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 2000) return 0;
    st.wYear = (WORD)y, st.wMonth = (WORD)m, st.wDay = (WORD)d;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (int64_t)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL) / kDaySec;
}

// ---- the accounts ----
enum Auth { kNoAuth, kBearer, kHeader }; // none; "Authorization: Bearer key"; a header of its own
enum Paging { kNoPages, kPageNumber, kNextUrl, kAfterLast };

struct Money { // a list of sales or days: summed by day over a month
    const char* id;
    const wchar_t* url; // {key} {key2} {since} {sinceu} {today} {page}
    Auth auth;
    const wchar_t* header;
    const char* key;
    const char* list; // "" the answer itself
    const char* amount; // null: each row counts as one
    const char* amount2; // the same field by another name
    const char* date;
    double divisor;
    Paging paging;
    const char* next; // kNextUrl: where the next page's url is; kAfterLast: the row's id
    const wchar_t* after; // kAfterLast: the query parameter that takes it
};

// read-only endpoints as their vendors document them; a mismatch shows as that tile's own error, never as anything else's
const Money kMoney[] = {
    {"stripe", L"https://api.stripe.com/v1/balance_transactions?limit=100&created[gte]={sinceu}", kBearer, nullptr, "stripe.key", "data", "net", nullptr, "created", 100, kAfterLast, "id", L"starting_after"},
    {"polar", L"https://api.polar.sh/v1/metrics/?start_date={since}&end_date={today}&interval=day", kBearer, nullptr, "polar.key", "periods", "net_revenue", "revenue", "timestamp", 100, kNoPages, nullptr, nullptr},
    {"dodo", L"https://live.dodopayments.com/payments?page_size=100&page_number={page}&status=succeeded&created_at_gte={since}T00:00:00Z", kBearer, nullptr, "dodo.key", "items", "total_amount", nullptr, "created_at", 100, kPageNumber, nullptr, nullptr},
    {"shopify", L"https://{key2}/admin/api/2025-01/orders.json?status=any&limit=250&created_at_min={since}T00:00:00Z&fields=id,total_price,created_at", kHeader, L"X-Shopify-Access-Token", "shopify.key", "orders", "total_price", nullptr, "created_at", 1, kAfterLast, "id", L"since_id"},
    {"lemon", L"https://api.lemonsqueezy.com/v1/orders?page[size]=100&page[number]={page}&sort=-created_at", kBearer, nullptr, "lemon.key", "data", "attributes.subtotal_usd", "attributes.total_usd", "attributes.created_at", 100, kPageNumber, nullptr, nullptr},
    {"paddle", L"https://api.paddle.com/transactions?status=completed&per_page=100&created_at[GTE]={since}T00:00:00Z", kBearer, nullptr, "paddle.key", "data", "details.totals.earnings", "details.totals.total", "created_at", 100, kNextUrl, "meta.pagination.next", nullptr},
    {"gumroad", L"https://api.gumroad.com/v2/sales?access_token={key}&after={since}", kNoAuth, nullptr, "gumroad.key", "sales", "price", nullptr, "created_at", 100, kNextUrl, "next_page_url", nullptr},
    {"plausible", L"https://plausible.io/api/v1/stats/timeseries?site_id={key2}&period=30d", kBearer, nullptr, "plausible.key", "results", "visitors", nullptr, "date", 1, kNoPages, nullptr, nullptr},
    {"dub", L"https://api.dub.co/analytics?event=clicks&interval=30d&groupBy=timeseries", kBearer, nullptr, "dub.key", "", "clicks", nullptr, "start", 1, kNoPages, nullptr, nullptr},
    {"datafast", L"https://datafa.st/api/v1/analytics/timeseries?fields=visitors&interval=day&startAt={since}&endAt={today}", kBearer, nullptr, "datafast.key", "data", "visitors", nullptr, "timestamp", 1, kNoPages, nullptr, nullptr},
    {"resend", L"https://api.resend.com/emails?limit=100", kBearer, nullptr, "resend.key", "data", nullptr, nullptr, "created_at", 1, kNoPages, nullptr, nullptr},
};

struct Count { // one number in one answer
    const char* id;
    const wchar_t* url;
    Auth auth;
    const char* key;
    const char* value;
    const wchar_t* unit;
};

const Count kCounts[] = {
    {"youtube", L"https://www.googleapis.com/youtube/v3/channels?part=statistics&id={key2}&key={key}", kNoAuth, "youtube.key", "items.0.statistics.subscriberCount", L"subscribers"},
    {"x", L"https://api.x.com/2/users/by/username/{key2}?user.fields=public_metrics", kBearer, "x.key", "data.public_metrics.followers_count", L"followers"},
    {"tiktok", L"https://open.tiktokapis.com/v2/user/info/?fields=follower_count", kBearer, "tiktok.key", "data.user.follower_count", L"followers"},
    {"instagram", L"https://graph.instagram.com/me?fields=followers_count&access_token={key}", kNoAuth, "instagram.key", "followers_count", L"followers"},
};

std::wstring Fill(std::wstring url, const std::wstring& key, const std::wstring& key2, int page, int64_t since) {
    auto put = [&](const wchar_t* tag, const std::wstring& v) {
        std::wstring t = tag;
        for (size_t at; (at = url.find(t)) != std::wstring::npos;) url.replace(at, t.size(), v);
    };
    put(L"{key2}", key2);
    put(L"{key}", Esc(key));
    put(L"{since}", IsoDay(since));
    put(L"{sinceu}", std::to_wstring(since));
    put(L"{today}", IsoDay(UnixNow()));
    put(L"{page}", std::to_wstring(page));
    return url;
}

std::wstring AuthHeaders(Auth a, const wchar_t* header, const std::wstring& key) {
    if (a == kBearer) return L"Authorization: Bearer " + key + L"\r\n";
    if (a == kHeader && header) return std::wstring(header) + L": " + key + L"\r\n";
    return L"";
}

std::wstring Why(int status) { // what a person can do about it
    if (status == 401 || status == 403) return L"The key was refused";
    if (status == 404) return L"Not found: check the settings";
    if (status == 429) return L"Too many requests: it will try again";
    if (status == 0) return L"Couldn't reach it";
    return L"It answered " + std::to_wstring(status);
}

Series Fail(const std::wstring& why) {
    Series out;
    out.error = why;
    return out;
}

constexpr wchar_t kUnreadable[] = L"An answer it couldn't read";

Series FetchMoney(const Money& m) {
    Series out;
    std::wstring key = Secret(m.key), key2 = Setting((std::string(m.id) + ".id").c_str());
    if (key.empty()) return Fail(L"Connect it with your key");
    int64_t now = UnixNow(), today = now / kDaySec, since = (today - kWindowDays) * kDaySec;
    double days[kWindowDays + 1] = {};
    std::wstring url = Fill(m.url, key, key2, 1, since);
    std::wstring lastId;
    for (int page = 1; page <= kMaxPages; ++page) {
        Resp r = Http(url, AuthHeaders(m.auth, m.header, key));
        if (r.status != 200) return Fail(Why(r.status));
        J doc;
        if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
        const J* list = *m.list ? At(doc, m.list) : &doc;
        if (!list || list->type != J::kArr) return Fail(kUnreadable);
        bool older = false;
        for (const J& row : list->a) {
            int64_t day = ParseDay(At(row, m.date));
            int back = (int)(today - day);
            if (back > kWindowDays) {
                older = true;
                continue;
            }
            if (back < 0) back = 0;
            days[back] += m.amount ? Num(At(row, m.amount), m.amount2 ? Num(At(row, m.amount2)) : 0) / m.divisor : 1;
        }
        if (older || list->a.empty() || m.paging == kNoPages) break;
        if (m.paging == kPageNumber) {
            url = Fill(m.url, key, key2, page + 1, since);
        } else if (m.paging == kNextUrl) {
            const J* next = At(doc, m.next);
            if (!next || next->type != J::kStr || next->s.empty()) break;
            std::wstring n = Wide(next->s);
            if (n[0] == L'/') n = url.substr(0, url.find(L'/', 8)) + n; // some give only the path
            url = n;
        } else {
            const J* id = At(list->a.back(), m.next);
            if (!id) break;
            std::wstring last = id->type == J::kStr ? Wide(id->s) : std::to_wstring((long long)id->n);
            if (last == lastId) break;
            lastId = last;
            url = Fill(m.url, key, key2, 1, since) + L"&" + m.after + L"=" + Esc(last);
        }
    }
    for (int i = 0; i <= kWindowDays; ++i) out.total += days[i];
    for (int i = kChartDays - 1; i >= 0; --i) out.points.push_back(days[i]);
    double prev = 0;
    for (int i = 0; i < 7; ++i) out.week += days[i];
    for (int i = 7; i < 14; ++i) prev += days[i];
    out.change = prev > 0 ? (out.week - prev) / prev : 0;
    out.value = out.total;
    out.ok = true;
    return out;
}

Series FetchCount(const Count& c) {
    Series out;
    std::wstring key = Secret(c.key), key2 = Setting((std::string(c.id) + ".id").c_str());
    if (key.empty()) return Fail(L"Connect it with your key");
    Resp r = Http(Fill(c.url, key, Esc(key2), 1, 0), AuthHeaders(c.auth, nullptr, key));
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc) || !At(doc, c.value)) return Fail(kUnreadable);
    out.value = Num(At(doc, c.value));
    out.text = c.unit;
    out.ok = true;
    return out;
}

Series FetchGithub(const std::string& id) {
    Series out;
    std::wstring key = Secret("github.key");
    if (key.empty()) return Fail(L"Connect it with your token");
    std::wstring head = L"Authorization: Bearer " + key + L"\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (id == "github.activity") { // the contribution grid, by day
        std::string q = "{\"query\":\"query{viewer{contributionsCollection{contributionCalendar{totalContributions weeks{contributionDays{contributionCount}}}}}}\"}";
        Resp r = Http(L"https://api.github.com/graphql", L"Authorization: Bearer " + key + L"\r\n", q);
        if (r.status != 200) return Fail(Why(r.status));
        J doc;
        if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
        const J* cal = At(doc, "data.viewer.contributionsCollection.contributionCalendar");
        const J* weeks = cal ? At(*cal, "weeks") : nullptr;
        if (!weeks || weeks->type != J::kArr) return Fail(kUnreadable);
        out.total = Num(At(*cal, "totalContributions"));
        for (const J& w : weeks->a)
            if (const J* d = At(w, "contributionDays"); d && d->type == J::kArr)
                for (const J& day : d->a) out.points.push_back(Num(At(day, "contributionCount")));
        int streak = 0;
        for (auto it = out.points.rbegin(); it != out.points.rend(); ++it) {
            if (*it > 0) ++streak;
            else if (it != out.points.rbegin()) break; // today can still be empty
        }
        out.value = streak;
        out.ok = true;
        return out;
    }
    Resp n = Http(L"https://api.github.com/notifications?per_page=50", head);
    if (n.status != 200) return Fail(Why(n.status));
    J list;
    if (ParseBody(n.body, list) && list.type == J::kArr) out.value = (double)list.a.size();
    if (id == "github.overview") {
        Resp p = Http(L"https://api.github.com/search/issues?q=is:pr+is:open+author:@me&per_page=1", head);
        J doc;
        if (p.status == 200 && ParseBody(p.body, doc)) out.extra = Num(At(doc, "total_count"));
        Resp s = Http(L"https://api.github.com/user/repos?per_page=100&affiliation=owner", head);
        J repos;
        if (s.status == 200 && ParseBody(s.body, repos) && repos.type == J::kArr)
            for (const J& one : repos.a) out.extra2 += Num(At(one, "stargazers_count"));
    }
    out.ok = true;
    return out;
}

Series FetchLeetcode() {
    Series out;
    std::wstring who = Setting("leetcode.id");
    if (who.empty()) return Fail(L"Enter your username");
    std::string q = "{\"query\":\"query($u:String!){matchedUser(username:$u){submitStats{acSubmissionNum{difficulty count}} userCalendar{streak}}}\",\"variables\":{\"u\":\"" + JsonText(who) + "\"}}";
    Resp r = Http(L"https://leetcode.com/graphql", L"Referer: https://leetcode.com\r\n", q);
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
    const J* all = At(doc, "data.matchedUser.submitStats.acSubmissionNum.0.count");
    if (!all) return Fail(L"No such user");
    out.value = Num(all);
    out.extra = Num(At(doc, "data.matchedUser.userCalendar.streak"));
    out.ok = true;
    return out;
}

Series FetchLinear() {
    Series out;
    std::wstring key = Secret("linear.key");
    if (key.empty()) return Fail(L"Connect it with your key");
    std::string q = "{\"query\":\"query{viewer{assignedIssues(first:100,filter:{state:{type:{nin:[\\\"completed\\\",\\\"canceled\\\"]}}}){nodes{state{type}}}}}\"}";
    Resp r = Http(L"https://api.linear.app/graphql", L"Authorization: " + key + L"\r\n", q);
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
    const J* nodes = At(doc, "data.viewer.assignedIssues.nodes");
    if (!nodes || nodes->type != J::kArr) return Fail(kUnreadable);
    out.value = (double)nodes->a.size();
    for (const J& n : nodes->a) {
        const J* type = At(n, "state.type");
        if (type && type->s == "started") out.extra += 1;
    }
    out.ok = true;
    return out;
}

Series FetchSupabase() {
    Series out;
    std::wstring key = Secret("supabase.key"), ref = Setting("supabase.id");
    if (key.empty() || ref.empty()) return Fail(L"Connect it with your token and project");
    std::wstring head = L"Authorization: Bearer " + key + L"\r\n";
    Resp h = Http(L"https://api.supabase.com/v1/projects/" + Esc(ref) + L"/health?services=db", head);
    if (h.status != 200) return Fail(Why(h.status));
    J doc;
    if (ParseBody(h.body, doc)) out.text = At(doc, "0.healthy") && At(doc, "0.healthy")->b ? L"Healthy" : L"Unhealthy";
    Resp u = Http(L"https://api.supabase.com/v1/projects/" + Esc(ref) + L"/analytics/endpoints/usage.api-requests-count", head);
    J use;
    if (u.status == 200 && ParseBody(u.body, use)) out.value = Num(At(use, "result.0.count"));
    out.ok = true;
    return out;
}

Series FetchGmail() {
    Series out;
    std::wstring who = Setting("gmail.id"), pass = Secret("gmail.key");
    if (who.empty() || pass.empty()) return Fail(L"Connect it with an app password");
    std::string both = Narrow(who + L":" + pass);
    std::wstring basic = ToBase64((const BYTE*)both.data(), (DWORD)both.size());
    Resp r = Http(L"https://mail.google.com/mail/feed/atom", L"Authorization: Basic " + basic + L"\r\n");
    SecureZeroMemory(both.data(), both.size());
    if (r.status != 200) return Fail(Why(r.status));
    size_t at = r.body.find("<fullcount>");
    if (at == std::string::npos) return Fail(kUnreadable);
    out.value = atof(r.body.c_str() + at + 11);
    for (size_t t = r.body.find("<entry>"); t != std::string::npos && out.rows.size() < 6; t = r.body.find("<entry>", t + 7)) {
        size_t a = r.body.find("<title>", t), b = r.body.find("</title>", t);
        if (a != std::string::npos && b != std::string::npos && b > a) out.rows.push_back(Wide(r.body.substr(a + 7, b - a - 7)));
    }
    out.ok = true;
    return out;
}

Series FetchQuote(const std::string& ticker) { // the day's line and change, from the public chart endpoint
    Series out;
    Resp r = Http(L"https://query1.finance.yahoo.com/v8/finance/chart/" + Esc(Wide(ticker)) + L"?range=1d&interval=15m");
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
    const J* res = At(doc, "chart.result.0");
    if (!res) return Fail(L"No such ticker");
    out.value = Num(At(*res, "meta.regularMarketPrice"));
    double prev = Num(At(*res, "meta.chartPreviousClose"), Num(At(*res, "meta.previousClose")));
    out.change = prev > 0 ? (out.value - prev) / prev : 0;
    if (const J* currency = At(*res, "meta.currency")) out.text = Wide(currency->s);
    const J* named = At(*res, "meta.shortName");
    if (!named || named->s.empty()) named = At(*res, "meta.longName");
    if (named) out.name = Wide(named->s);
    out.more.assign(kQuoteMore, 0);
    out.more[kPrevClose] = prev;
    out.more[kDayHigh] = Num(At(*res, "meta.regularMarketDayHigh"));
    out.more[kDayLow] = Num(At(*res, "meta.regularMarketDayLow"));
    out.more[kYearHigh] = Num(At(*res, "meta.fiftyTwoWeekHigh"));
    out.more[kYearLow] = Num(At(*res, "meta.fiftyTwoWeekLow"));
    out.more[kVolume] = Num(At(*res, "meta.regularMarketVolume"));
    out.more[kOpensAt] = Num(At(*res, "meta.currentTradingPeriod.regular.start"));
    out.more[kClosesAt] = Num(At(*res, "meta.currentTradingPeriod.regular.end"));
    out.more[kPreAt] = Num(At(*res, "meta.currentTradingPeriod.pre.start"));
    out.more[kPostAt] = Num(At(*res, "meta.currentTradingPeriod.post.end"));
    if (const J* closes = At(*res, "indicators.quote.0.close"); closes && closes->type == J::kArr)
        for (const J& c : closes->a)
            if (c.type == J::kNum) out.points.push_back(c.n);
    out.ok = out.value > 0;
    if (!out.ok) out.error = L"No such ticker";
    return out;
}

Series FetchMetric(const std::wstring& pair) { // a live price, btc-usd by default
    Series out;
    Resp r = Http(L"https://api.coinbase.com/v2/prices/" + Esc(pair) + L"/spot");
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc) || !At(doc, "data.amount")) return Fail(kUnreadable);
    out.value = Num(At(doc, "data.amount"));
    out.text = pair;
    out.ok = true;
    return out;
}

Series FetchRates(const std::wstring& from, const std::wstring& to) {
    Series out;
    Resp r = Http(L"https://open.er-api.com/v6/latest/" + Esc(from));
    if (r.status != 200) return Fail(Why(r.status));
    J doc;
    if (!ParseBody(r.body, doc)) return Fail(kUnreadable);
    const J* rate = At(doc, ("rates." + Narrow(to)).c_str());
    if (!rate) return Fail(L"No such currency");
    out.value = Num(rate);
    out.text = from + L" → " + to;
    out.ok = true;
    return out;
}

Series FetchSky(const std::wstring& place) { // sunrise, sunset, wind and rain, for the place the forecast already uses
    Series out;
    if (place.empty()) return Fail(L"Finding the place");
    std::wstring city = place.substr(0, place.find(L',')); // the forecast's "city, country": the search takes the city
    Resp g = Http(L"https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + Esc(city));
    J where;
    if (g.status != 200 || !ParseBody(g.body, where) || !At(where, "results.0.latitude")) return Fail(L"Couldn't place it");
    wchar_t q[256];
    swprintf_s(q, L"https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=wind_speed_10m,relative_humidity_2m&daily=sunrise,sunset,precipitation_sum&forecast_days=1&timezone=auto",
               Num(At(where, "results.0.latitude")), Num(At(where, "results.0.longitude")));
    Resp r = Http(q);
    J doc;
    if (r.status != 200 || !ParseBody(r.body, doc)) return Fail(Why(r.status));
    out.value = Num(At(doc, "current.wind_speed_10m"));
    out.extra = Num(At(doc, "daily.precipitation_sum.0"));
    out.extra2 = Num(At(doc, "current.relative_humidity_2m"));
    auto clock = [&](const char* path) { // "2026-11-05T06:12": minutes into the local day
        const J* j = At(doc, path);
        if (!j || j->s.size() < 16) return -1.0;
        return atoi(j->s.c_str() + 11) * 60.0 + atoi(j->s.c_str() + 14);
    };
    out.points = {clock("daily.sunrise.0"), clock("daily.sunset.0")};
    out.ok = true;
    return out;
}

// ---- sports: espn's public scoreboards, lichess's broadcasts ----
double IsoUnix(const J* j) { // "2026-11-08T15:00Z", seconds optional: unix seconds, utc
    if (!j || j->type != J::kStr || j->s.size() < 16) return 0;
    SYSTEMTIME st{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (sscanf_s(j->s.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    st.wYear = (WORD)y, st.wMonth = (WORD)mo, st.wDay = (WORD)d, st.wHour = (WORD)h, st.wMinute = (WORD)mi;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (double)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL);
}

std::wstring LocalStart(double unix) { // "Sat 15:00", in this pc's time
    ULONGLONG u = ((ULONGLONG)unix + 11644473600ULL) * 10000000ULL;
    FILETIME f = {(DWORD)u, (DWORD)(u >> 32)};
    SYSTEMTIME utc, st;
    if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st)) return {};
    wchar_t day[16] = {}, time[16] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd", day, 16, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, 16);
    return std::wstring(day) + L" " + time;
}

std::wstring Str(const J* j) { // trimmed: some names come with a space on the end
    if (!j || j->type != J::kStr) return {};
    std::wstring s = Wide(j->s);
    size_t a = s.find_first_not_of(L' '), b = s.find_last_not_of(L' ');
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

std::wstring Ymd(int64_t unix) { // espn's dates: yyyymmdd
    std::wstring d = IsoDay(unix);
    return d.substr(0, 4) + d.substr(5, 2) + d.substr(8, 2);
}

std::wstring SideName(const J& c) { // a team's short name, else the athlete's, else a doubles pair's
    for (const char* p : {"team.shortDisplayName", "team.displayName", "athlete.shortName", "athlete.displayName", "roster.shortDisplayName", "roster.displayName"})
        if (std::wstring n = Str(At(c, p)); !n.empty()) return n;
    if (const J* athletes = At(c, "roster.athletes"); athletes && athletes->type == J::kArr) { // a pair, by their names
        std::wstring pair;
        for (const J& a : athletes->a)
            if (std::wstring n = Str(At(a, "shortName")); !n.empty()) pair += (pair.empty() ? L"" : L" / ") + n;
        if (!pair.empty()) return pair;
    }
    return {};
}

std::wstring SideScore(const J& c) { // the score, else a tennis player's sets
    if (std::wstring s = Str(At(c, "score")); !s.empty()) return s;
    std::wstring sets;
    if (const J* ls = At(c, "linescores"); ls && ls->type == J::kArr)
        for (const J& l : ls->a) sets += (sets.empty() ? L"" : L" ") + std::to_wstring((int)Num(At(l, "value")));
    return sets;
}

int StateOf(const J* status) {
    std::string s = status && At(*status, "type.state") ? At(*status, "type.state")->s : "";
    return s == "in" ? sports::kLive : s == "post" ? sports::kFinal : sports::kPre;
}

std::vector<const J*> Comps(const J& ev) { // a tennis draw's matches, a race weekend's sessions, a fight card's bouts, or the one game
    std::vector<const J*> comps;
    if (const J* gs = At(ev, "groupings"); gs && gs->type == J::kArr)
        for (const J& g : gs->a)
            if (const J* cs = At(g, "competitions"); cs && cs->type == J::kArr)
                for (const J& c : cs->a) comps.push_back(&c);
    if (const J* cs = At(ev, "competitions"); comps.empty() && cs && cs->type == J::kArr)
        for (const J& c : cs->a) comps.push_back(&c);
    return comps;
}

const J* PickComp(const std::vector<const J*>& comps) { // the one on now, else the latest one done, else the first
    const J* pick = nullptr;
    for (const J* c : comps)
        if (StateOf(At(*c, "status")) == sports::kLive) pick = c;
    if (!pick)
        for (const J* c : comps)
            if (StateOf(At(*c, "status")) == sports::kFinal) pick = c;
    return pick ? pick : comps.front();
}

void ReadEvents(const J& doc, std::vector<sports::Game>& out, size_t most = 24) { // most: a busy college saturday fills the panel, not the whole slate
    const J* events = At(doc, "events");
    if (!events || events->type != J::kArr) return;
    for (const J& ev : events->a) {
        if (out.size() >= most) break;
        std::vector<const J*> comps = Comps(ev);
        if (comps.empty()) continue;
        const J* pick = PickComp(comps);
        const J* status = At(*pick, "status") ? At(*pick, "status") : At(ev, "status");
        sports::Game g;
        g.id = Str(At(ev, "id"));
        g.state = StateOf(status);
        g.when = IsoUnix(At(*pick, "date")) > 0 ? IsoUnix(At(*pick, "date")) : IsoUnix(At(ev, "date"));
        g.detail = g.state == sports::kPre ? LocalStart(g.when) : Str(status ? At(*status, "type.shortDetail") : nullptr);
        const J* sides = At(*pick, "competitors");
        size_t n = sides && sides->type == J::kArr ? sides->a.size() : 0;
        if (n == 2) { // two sides: home first, as the scorelines read
            const J* first = &sides->a[0];
            const J* second = &sides->a[1];
            if (Str(At(*second, "homeAway")) == L"home") std::swap(first, second);
            g.a = SideName(*first), g.as = SideScore(*first), g.b = SideName(*second), g.bs = SideScore(*second);
            auto periods = [](const J& side) { // quarters, periods, innings or sets
                std::wstring s;
                if (const J* ls = At(side, "linescores"); ls && ls->type == J::kArr)
                    for (const J& l : ls->a) s += (s.empty() ? L"" : L" ") + std::to_wstring((int)Num(At(l, "value")));
                return s;
            };
            g.aLines = periods(*first), g.bLines = periods(*second);
            g.aCode = Str(At(*first, "team.abbreviation")), g.bCode = Str(At(*second, "team.abbreviation")); // "BAR", "RMA": players keep their short names
            g.venue = Str(At(*pick, "venue.fullName"));
            if (g.a.empty() || g.b.empty()) continue; // nobody named yet: a draw's empty slot, nothing to show
        } else { // a field: the event, and who leads it
            g.solo = true;
            g.a = Str(At(ev, "shortName"));
            if (g.a.empty()) g.a = Str(At(ev, "name"));
            if (comps.size() > 1)
                if (std::wstring kind = Str(At(*pick, "type.abbreviation")); !kind.empty()) g.a += L"  ·  " + kind;
            const J* lead = nullptr;
            for (size_t i = 0; i < n && !lead; ++i)
                if (Num(At(sides->a[i], "order")) == 1) lead = &sides->a[i];
            if (!lead && n > 0) lead = &sides->a[0];
            if (lead && g.state != sports::kPre) g.b = (g.state == sports::kFinal ? L"Won by " : L"Leading: ") + SideName(*lead);
        }
        out.push_back(std::move(g));
    }
}

Series FetchSport(const std::string& path) {
    Series out;
    const sports::League* league = sports::Find(path);
    if (!league) return Fail(L"Unknown league");
    out.text = league->name;
    std::vector<sports::Game> games;
    std::wstring base = L"https://site.api.espn.com/apis/site/v2/sports/" + Wide(path) + L"/scoreboard";
    Resp r = Http(base);
    J doc;
    if (r.status != 200 || !ParseBody(r.body, doc)) return Fail(r.status == 200 ? std::wstring(kUnreadable) : Why(r.status));
    ReadEvents(doc, games);
    bool played = std::any_of(games.begin(), games.end(), [](const sports::Game& g) { return g.state != sports::kPre; });
    constexpr int kLookBack = 14; // nothing on today and nothing done: the latest results, a day at a time back to the last that had some
    for (int day = 1; !played && day <= kLookBack && !gQuit.load(); ++day) {
        Resp back = Http(base + L"?dates=" + Ymd(UnixNow() - day * kDaySec));
        J past;
        if (back.status != 200 || !ParseBody(back.body, past)) continue;
        std::vector<sports::Game> before;
        ReadEvents(past, before);
        for (sports::Game& g : before)
            if (g.state == sports::kFinal) games.push_back(std::move(g)), played = true;
    }
    for (const sports::Game& g : sports::Order(std::move(games))) out.rows.push_back(sports::Encode(g));
    out.value = (double)std::count_if(out.rows.begin(), out.rows.end(), [](const std::wstring& row) { return sports::Decode(row).state == sports::kLive; }); // how many are live
    out.ok = true;
    return out;
}

Series FetchFixtures(const std::string& path) { // a league around today: the past week's results and its coming days
    Series out;
    const sports::League* league = sports::Find(path);
    if (!league || path == "chess") return Fail(L"Unknown league");
    out.text = league->name;
    constexpr int kBackDays = 2, kAheadDays = 7; // espn takes one day at a time: ranges are refused
    constexpr size_t kEnoughAhead = 16, kPerDay = 40;
    std::wstring base = L"https://site.api.espn.com/apis/site/v2/sports/" + Wide(path) + L"/scoreboard?dates=";
    std::vector<sports::Game> games;
    int answered = 0, lastStatus = 0;
    for (int day = -kBackDays; day <= kAheadDays && !gQuit.load(); ++day) {
        if (day == 0) continue; // today is the live feed's
        size_t ahead = (size_t)std::count_if(games.begin(), games.end(), [](const sports::Game& g) { return g.state == sports::kPre; });
        if (day > 0 && ahead >= kEnoughAhead) break; // plenty coming: no need to ask further out
        Resp r = Http(base + Ymd(UnixNow() + day * kDaySec));
        lastStatus = r.status;
        J doc;
        if (r.status != 200 || !ParseBody(r.body, doc)) continue;
        ++answered;
        ReadEvents(doc, games, games.size() + kPerDay);
    }
    if (!answered) return Fail(Why(lastStatus));
    games.erase(std::remove_if(games.begin(), games.end(), [](const sports::Game& g) { return g.state == sports::kLive; }), games.end()); // what's on is the live feed's
    for (const sports::Game& g : sports::Order(std::move(games))) out.rows.push_back(sports::Encode(g));
    out.ok = true;
    return out;
}

// ---- one game opened in the panel: what's happened, its numbers, its lists ----
constexpr size_t kDetailRows = 40, kDetailStats = 10, kDetailLeaders = 4;

std::wstring SetLine(const J& a, const J& b) { // two sides' periods as one line: "6-4 3-6"
    const J* la = At(a, "linescores");
    const J* lb = At(b, "linescores");
    std::wstring out;
    if (!la || !lb || la->type != J::kArr || lb->type != J::kArr) return out;
    for (size_t i = 0; i < la->a.size() && i < lb->a.size(); ++i)
        out += (out.empty() ? L"" : L" ") + std::to_wstring((int)Num(At(la->a[i], "value"))) + L"-" + std::to_wstring((int)Num(At(lb->a[i], "value")));
    return out;
}

bool FromSummary(const J& doc, const std::string& path, sports::Detail& d) { // a team game: its moments, its leaders, its numbers side by side
    const J* comp = At(doc, "header.competitions.0");
    if (!comp) return false;
    d.status = Str(At(*comp, "status.type.detail"));
    std::wstring home; // the first side, as the game's own row has it
    if (const J* cs = At(*comp, "competitors"); cs && cs->type == J::kArr)
        for (const J& c : cs->a)
            if (Str(At(c, "homeAway")) == L"home") home = Str(At(c, "id"));
    auto side = [&](const J* team) {
        std::wstring id = team ? Str(At(*team, "id")) : L"";
        return id.empty() ? -1 : id == home ? 0 : 1;
    };
    if (const J* ks = At(doc, "keyEvents"); ks && ks->type == J::kArr) // football: goals and cards, by the minute
        for (const J& k : ks->a) {
            std::wstring type = Str(At(k, "type.text"));
            bool goal = At(k, "scoringPlay") && At(k, "scoringPlay")->b;
            std::wstring kind = goal ? L"goal" : type.find(L"Red") != std::wstring::npos ? L"red" : type.find(L"Yellow") != std::wstring::npos ? L"yellow" : L"";
            if (kind.empty()) continue;
            std::wstring who = Str(At(k, "participants.0.athlete.displayName"));
            if (goal) {
                std::wstring by = Str(At(k, "participants.1.athlete.displayName"));
                if (type.find(L"Own") != std::wstring::npos) who += L" (own goal)";
                else if (type.find(L"Penalty") != std::wstring::npos) who += L" (pen)";
                else if (!by.empty()) who += L", " + by;
            }
            d.events.push_back({side(At(k, "team")), Str(At(k, "clock.displayValue")), kind, who.empty() ? type : who});
        }
    if (const J* ps = At(doc, "scoringPlays"); d.events.empty() && ps && ps->type == J::kArr) { // the other games: every score, by its period
        const wchar_t* per = path.rfind("hockey/", 0) == 0 ? L"P" : path.rfind("baseball/", 0) == 0 ? L"Inn " : L"Q";
        for (const J& p : ps->a) {
            std::wstring when = per + std::to_wstring((int)Num(At(p, "period.number")));
            if (std::wstring clock = Str(At(p, "clock.displayValue")); !clock.empty()) when += L" " + clock;
            d.events.push_back({side(At(p, "team")), when, L"score", Str(At(p, "text"))});
        }
    }
    if (const J* ls = At(doc, "leaders"); ls && ls->type == J::kArr) { // each side's best in each category, the two set across from each other
        sports::Detail::Block b{L"Leaders", {}};
        for (const J& team : ls->a) {
            int s = side(At(team, "team"));
            const J* cats = At(team, "leaders");
            if (s < 0 || !cats || cats->type != J::kArr) continue;
            for (size_t i = 0; i < cats->a.size() && i < kDetailLeaders; ++i) {
                const J& cat = cats->a[i];
                std::wstring who = Str(At(cat, "leaders.0.athlete.shortName")), value = Str(At(cat, "leaders.0.displayValue"));
                std::wstring line = who.empty() ? value : who + L"  " + value;
                if (b.rows.size() <= i) b.rows.push_back({L"", Str(At(cat, "displayName")), L"", false});
                (s == 0 ? b.rows[i].left : b.rows[i].right) = line;
            }
        }
        if (!b.rows.empty()) d.blocks.push_back(std::move(b));
    }
    if (const J* ts = At(doc, "boxscore.teams"); ts && ts->type == J::kArr) { // the numbers, side by side, as the first side lists them
        std::vector<std::pair<std::wstring, std::wstring>> mine[2];
        for (const J& team : ts->a) {
            int s = side(At(team, "team"));
            const J* st = At(team, "statistics");
            if (s < 0 || !st || st->type != J::kArr) continue;
            for (const J& one : st->a)
                if (std::wstring label = Str(At(one, "label")); !label.empty()) mine[s].push_back({label, Str(At(one, "displayValue"))});
        }
        for (const auto& [label, a] : mine[0]) {
            if (d.stats.size() >= kDetailStats) break;
            auto other = std::find_if(mine[1].begin(), mine[1].end(), [&](const auto& p) { return p.first == label; });
            if (other != mine[1].end()) d.stats.push_back({label, a, other->second});
        }
    }
    return true;
}

void FromBoard(const J& ev, sports::Detail& d) { // no summary to ask: the event's own matches, or its sessions and its order, off the scoreboard
    std::vector<const J*> comps = Comps(ev);
    if (comps.empty()) return;
    auto sides = [](const J& c) -> const std::vector<J>* {
        const J* s = At(c, "competitors");
        return s && s->type == J::kArr ? &s->a : nullptr;
    };
    bool pairs = comps.size() > 1 && std::all_of(comps.begin(), comps.end(), [&](const J* c) { return sides(*c) && sides(*c)->size() == 2; });
    if (pairs) { // a draw's matches or a card's bouts: what's on, then what's done, then what's coming
        sports::Detail::Block b{L"Matches", {}};
        int on = 0;
        for (int want : {sports::kLive, sports::kFinal, sports::kPre})
            for (const J* c : comps) {
                int st = StateOf(At(*c, "status"));
                if (st != want || b.rows.size() >= kDetailRows) continue;
                const std::vector<J>& s = *sides(*c);
                std::wstring mid = st == sports::kPre ? LocalStart(IsoUnix(At(*c, "date"))) : SetLine(s[0], s[1]);
                if (mid.empty()) mid = Str(At(s[0], "score")) + L"-" + Str(At(s[1], "score"));
                b.rows.push_back({SideName(s[0]), mid, SideName(s[1]), st == sports::kLive});
                on += st == sports::kLive;
            }
        d.status = on ? std::to_wstring(on) + L" on now" : Str(At(ev, "status.type.detail"));
        d.blocks.push_back(std::move(b));
        return;
    }
    const J* pick = PickComp(comps); // a field: the weekend's sessions, then the order of the one on now or the latest done
    std::wstring abbr = Str(At(*pick, "type.abbreviation")), detail = Str(At(*pick, "status.type.detail"));
    d.status = abbr.empty() ? detail : abbr + L"  ·  " + detail;
    auto winner = [&](const J& c) -> std::wstring {
        if (const std::vector<J>* s = sides(c))
            for (const J& one : *s)
                if (Num(At(one, "order")) == 1) return SideName(one);
        return {};
    };
    if (comps.size() > 1) {
        sports::Detail::Block b{L"Sessions", {}};
        for (const J* c : comps) {
            int st = StateOf(At(*c, "status"));
            std::wstring when = st == sports::kPre ? LocalStart(IsoUnix(At(*c, "date"))) : Str(At(*c, "status.type.shortDetail"));
            b.rows.push_back({Str(At(*c, "type.abbreviation")), when, st == sports::kPre ? L"" : winner(*c), st == sports::kLive});
        }
        d.blocks.push_back(std::move(b));
    }
    if (const std::vector<J>* s = sides(*pick); s && !s->empty()) {
        std::vector<const J*> order;
        for (const J& one : *s) order.push_back(&one);
        std::stable_sort(order.begin(), order.end(), [](const J* x, const J* y) { return Num(At(*x, "order"), 999) < Num(At(*y, "order"), 999); });
        sports::Detail::Block b{abbr.empty() ? L"Standings" : abbr, {}};
        bool live = StateOf(At(*pick, "status")) == sports::kLive;
        for (const J* one : order) {
            if (b.rows.size() >= kDetailRows) break;
            std::wstring pos = Num(At(*one, "order")) > 0 ? std::to_wstring((int)Num(At(*one, "order"))) : L"";
            std::wstring right = Str(At(*one, "score")); // golf: to par; a race: its gap, when the board has it
            if (right.empty())
                if (const J* st = At(*one, "statistics"); st && st->type == J::kArr && !st->a.empty()) right = Str(At(st->a[0], "displayValue"));
            b.rows.push_back({pos, SideName(*one), right, live});
        }
        d.blocks.push_back(std::move(b));
    }
}

Series FetchMatch(const std::string& spec) { // "soccer/eng.1|401878758|20261108": one game, opened in the panel
    size_t a = spec.find('|'), b = a == std::string::npos ? std::string::npos : spec.find('|', a + 1);
    if (b == std::string::npos) return Fail(L"Unknown game");
    std::string path = spec.substr(0, a), id = spec.substr(a + 1, b - a - 1), day = spec.substr(b + 1);
    sports::Detail d;
    if (path == "chess") { // the round's boards: who has white, who has black, and how it went
        Resp r = Http(L"https://lichess.org/api/broadcast/" + Wide(id));
        J doc;
        if (r.status != 200 || !ParseBody(r.body, doc)) return Fail(r.status == 200 ? std::wstring(kUnreadable) : Why(r.status));
        bool on = At(doc, "round.ongoing") && At(doc, "round.ongoing")->b;
        d.status = Str(At(doc, "round.name")) + (on ? L"  ·  live" : L"");
        sports::Detail::Block boards{L"Boards", {}};
        if (const J* gs = At(doc, "games"); gs && gs->type == J::kArr)
            for (const J& g : gs->a) {
                if (boards.rows.size() >= kDetailRows) break;
                auto player = [&](const char* at) {
                    const J* p = At(g, at);
                    if (!p) return std::wstring();
                    std::wstring title = Str(At(*p, "title")), name = Str(At(*p, "name"));
                    int rating = (int)Num(At(*p, "rating"));
                    return (title.empty() ? L"" : title + L" ") + name + (rating > 0 ? L" (" + std::to_wstring(rating) + L")" : L"");
                };
                std::wstring result = Str(At(g, "status"));
                bool live = result == L"*";
                boards.rows.push_back({player("players.0"), live ? L"live" : result, player("players.1"), live});
            }
        d.blocks.push_back(std::move(boards));
    } else {
        std::wstring base = L"https://site.api.espn.com/apis/site/v2/sports/" + Wide(path);
        Resp r = Http(base + L"/summary?event=" + Wide(id));
        J doc;
        bool got = r.status == 200 && ParseBody(r.body, doc) && FromSummary(doc, path, d) && (!d.events.empty() || !d.stats.empty() || !d.blocks.empty());
        if (!got) { // races, tennis, golf and fight cards have no summary: the scoreboard's own event, today's board first, then its day's
            std::wstring said = d.status;
            d = {};
            bool found = false;
            for (const std::wstring& q : {std::wstring(), L"?dates=" + Wide(day)}) {
                if (found || gQuit.load()) break;
                Resp s = Http(base + L"/scoreboard" + q);
                J board;
                if (s.status != 200 || !ParseBody(s.body, board)) continue;
                if (const J* evs = At(board, "events"); evs && evs->type == J::kArr)
                    for (const J& ev : evs->a)
                        if (Str(At(ev, "id")) == Wide(id)) {
                            FromBoard(ev, d);
                            found = true;
                            break;
                        }
            }
            if (!found) {
                if (said.empty()) return Fail(L"No details for this game yet");
                d.status = said;
            }
        }
    }
    Series out;
    out.rows = sports::EncodeDetail(d);
    out.ok = true;
    return out;
}

Series FetchChess() { // the official broadcasts on lichess: what's being played now, the women's events among them by name
    Series out;
    out.text = L"Chess broadcasts";
    Resp r = Http(L"https://lichess.org/api/broadcast/top?page=1");
    J doc;
    if (r.status != 200 || !ParseBody(r.body, doc)) return Fail(r.status == 200 ? std::wstring(kUnreadable) : Why(r.status));
    constexpr size_t kMost = 12;
    std::vector<sports::Game> games;
    if (const J* active = At(doc, "active"); active && active->type == J::kArr)
        for (const J& b : active->a) {
            if (games.size() >= kMost) break;
            sports::Game g;
            g.solo = true;
            g.a = Str(At(b, "tour.name"));
            g.b = Str(At(b, "round.name"));
            g.id = Str(At(b, "tour.slug")) + L"/" + Str(At(b, "round.slug")) + L"/" + Str(At(b, "round.id")); // the round's boards are fetched by it
            bool on = At(b, "round.ongoing") && At(b, "round.ongoing")->b;
            g.state = on ? sports::kLive : sports::kPre;
            g.when = Num(At(b, "round.startsAt")) / 1000;
            g.detail = on ? L"Live" : g.when > 0 ? LocalStart(g.when) : L"";
            if (!g.a.empty()) games.push_back(std::move(g));
        }
    for (const sports::Game& g : sports::Order(std::move(games))) out.rows.push_back(sports::Encode(g));
    out.ok = true;
    return out;
}

// ---- the thermal zone: what windows reports without elevation ----
Series ReadThermal() {
    Series out;
    constexpr wchar_t kNoSensor[] = L"Not reported by this PC";
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER counter = nullptr;
    if (!query) {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return Fail(kNoSensor);
        if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0, &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return Fail(kNoSensor);
        }
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) return Fail(kNoSensor);
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !bytes) return Fail(kNoSensor);
    std::vector<BYTE> buf(bytes);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return Fail(kNoSensor);
    double hottest = 0;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS) hottest = (std::max)(hottest, items[i].FmtValue.doubleValue);
    if (hottest <= 200) return Fail(kNoSensor); // kelvin: anything lower isn't a reading
    out.value = hottest - 273.15;
    out.ok = true;
    return out;
}

// ---- screen time: the foreground app, noted every few seconds, kept in a small local file ----
struct Tally {
    std::map<int, std::map<int, std::map<std::wstring, int>>> byDay; // yyyymmdd -> hour -> app -> seconds
    std::wstring path;
    int sinceSave = 0;
};

int Today(int shift = 0) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += (ULONGLONG)((LONGLONG)shift * 864000000000LL);
    ft = {u.LowPart, u.HighPart};
    SYSTEMTIME utc, st;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

std::wstring ForegroundApp() {
    HWND w = GetForegroundWindow();
    if (!w) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        name.assign(buf, len);
        name = name.substr(name.find_last_of(L'\\') + 1);
        if (size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.resize(dot);
    }
    CloseHandle(h);
    return name;
}

void LoadTally(Tally& t) {
    HANDLE f = CreateFileW(t.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string buf(1 << 20, '\0');
    DWORD got = 0;
    ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(f);
    buf.resize(got);
    int oldest = Today(-kKeepDays);
    for (size_t a = 0; a < buf.size();) {
        size_t e = (std::min)(buf.find('\n', a), buf.size());
        int day = 0, hour = 0, secs = 0;
        char app[260] = {};
        if (sscanf_s(buf.substr(a, e - a).c_str(), "%d\t%d\t%d\t%259[^\r\n]", &day, &hour, &secs, app, (unsigned)sizeof(app)) == 4 && day >= oldest && hour >= 0 && hour < 24) t.byDay[day][hour][Wide(app)] = secs;
        a = e + 1;
    }
}

void SaveTally(const Tally& t) {
    std::string out;
    for (const auto& [day, hours] : t.byDay)
        for (const auto& [hour, apps] : hours)
            for (const auto& [app, secs] : apps) out += std::to_string(day) + "\t" + std::to_string(hour) + "\t" + std::to_string(secs) + "\t" + Narrow(app) + "\n";
    std::wstring tmp = t.path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr) && wrote == out.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), t.path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

// ---- the worker ----
struct State {
    HWND notify = nullptr;
    UINT msg = 0;
    std::wstring folder;
    std::mutex mu;
    std::vector<std::string> wanted; // guarded by mu
    std::map<std::string, Series> got; // guarded by mu
    std::map<std::string, ULONGLONG> due; // guarded by mu
    Tally tally; // guarded by mu
    bool tallyLoaded = false;
    std::thread th;
    HANDLE wake = nullptr;
    std::atomic<bool> stop{false};
};

State& S() {
    static State s;
    return s;
}

ULONGLONG Pace(const std::string& id) {
    if (id.rfind("quote:", 0) == 0) return kEveryQuote;
    if (id == "sport:chess" || id.rfind("next:", 0) == 0) return kEverySky; // fixtures, results and broadcasts don't move by the minute
    if (id.rfind("sport:", 0) == 0 || id.rfind("match:", 0) == 0) return kEverySport;
    if (id.rfind("metric", 0) == 0) return kEveryMetric;
    if (id.rfind("rates", 0) == 0) return kEveryRates;
    if (id.rfind("sky", 0) == 0) return kEverySky;
    if (id == "thermal") return kEveryThermal;
    return kEveryAccount;
}

Series Fetch(const std::string& id) {
    if (id.rfind("quote:", 0) == 0) return FetchQuote(id.substr(6));
    if (id == "sport:chess") return FetchChess();
    if (id.rfind("next:", 0) == 0) return FetchFixtures(id.substr(5));
    if (id.rfind("match:", 0) == 0) return FetchMatch(id.substr(6));
    if (id.rfind("sport:", 0) == 0) return FetchSport(id.substr(6));
    if (id == "metric") {
        std::wstring p = Setting("metric.id");
        return FetchMetric(p.empty() ? L"BTC-USD" : p);
    }
    if (id == "rates") {
        std::wstring a = Setting("rates.from"), b = Setting("rates.to");
        return FetchRates(a.empty() ? L"USD" : a, b.empty() ? L"EUR" : b);
    }
    if (id == "sky") return FetchSky(Setting("sky.place"));
    if (id == "thermal") return ReadThermal();
    if (id.rfind("github", 0) == 0) return FetchGithub(id);
    if (id == "leetcode") return FetchLeetcode();
    if (id == "linear") return FetchLinear();
    if (id == "supabase") return FetchSupabase();
    if (id == "gmail") return FetchGmail();
    for (const Money& m : kMoney)
        if (id == m.id) return FetchMoney(m);
    for (const Count& c : kCounts)
        if (id == c.id) return FetchCount(c);
    return Fail(L"Unknown");
}

Series TotalRevenue(State& s) { // every connected store's month, added up; the caller holds mu
    Series out;
    for (const char* id : {"stripe", "polar", "dodo", "shopify", "lemon", "paddle", "gumroad"}) {
        auto it = s.got.find(id);
        if (it != s.got.end() && it->second.ok) out.total += it->second.total, out.week += it->second.week, out.ok = true;
    }
    out.value = out.total;
    if (!out.ok) out.error = L"Connect a store";
    return out;
}

void Loop() {
    State& s = S();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); // nothing here is in a hurry
    THREAD_POWER_THROTTLING_STATE eco{THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, THREAD_POWER_THROTTLING_EXECUTION_SPEED};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &eco, sizeof(eco)); // efficient cores and low clocks, whatever the app is doing
    DWORD lastTick = GetTickCount();
    bool wantsScreen = false;
    while (!s.stop.load()) {
        std::vector<std::string> list;
        ULONGLONG now = GetTickCount64();
        ULONGLONG soonest = now + 60000;
        {
            std::lock_guard lock(s.mu);
            wantsScreen = false;
            for (const std::string& id : s.wanted) {
                if (id == "screen") {
                    wantsScreen = true;
                    continue;
                }
                if (id == "total") continue;
                ULONGLONG& d = s.due[id];
                if (d <= now) list.push_back(id);
                else soonest = (std::min)(soonest, d);
            }
        }
        bool moved = false;
        for (const std::string& id : list) {
            if (s.stop.load()) break;
            Series got = Fetch(id);
            bool ok = got.ok;
            std::lock_guard lock(s.mu);
            s.got[id] = std::move(got);
            ULONGLONG next = GetTickCount64() + (ok ? Pace(id) : (std::min)(kRetry, Pace(id)));
            s.due[id] = next;
            soonest = (std::min)(soonest, next);
            s.got["total"] = TotalRevenue(s);
            moved = true;
        }
        if (wantsScreen) {
            DWORD tick = GetTickCount();
            if (tick - lastTick >= kScreenTickMs) {
                int secs = (int)((tick - lastTick) / 1000);
                lastTick = tick;
                LASTINPUTINFO li{sizeof(li)};
                bool here = GetLastInputInfo(&li) && tick - li.dwTime < kIdleMs;
                std::wstring app = here ? ForegroundApp() : L"";
                std::lock_guard lock(s.mu);
                if (!s.tallyLoaded) {
                    s.tallyLoaded = true;
                    LoadTally(s.tally);
                }
                if (!app.empty()) {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    s.tally.byDay[Today()][st.wHour][app] += (std::min)(secs, (int)(kScreenTickMs / 500)); // a sleep in between isn't screen time
                    moved = true;
                }
                if (++s.tally.sinceSave >= kSaveEvery) s.tally.sinceSave = 0, SaveTally(s.tally);
            }
            soonest = (std::min)(soonest, GetTickCount64() + kScreenTickMs);
        } else {
            lastTick = GetTickCount();
        }
        if (moved) PostMessageW(s.notify, s.msg, 0, 0);
        ULONGLONG t = GetTickCount64();
        WaitForSingleObject(s.wake, soonest > t ? (DWORD)(std::min)(soonest - t, (ULONGLONG)60000) : 0);
    }
    {
        std::lock_guard lock(s.mu);
        if (s.tallyLoaded) SaveTally(s.tally);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

}

Phase MarketPhase(const Series& q) {
    if (q.more.size() < kQuoteMore || q.more[kOpensAt] <= 0) return kClosed;
    double now = (double)UnixNow();
    if (now >= q.more[kOpensAt] && now < q.more[kClosesAt]) return kOpen;
    if (q.more[kPreAt] > 0 && now >= q.more[kPreAt] && now < q.more[kOpensAt]) return kPreMarket;
    if (q.more[kPostAt] > 0 && now >= q.more[kClosesAt] && now < q.more[kPostAt]) return kAfterHours;
    return kClosed; // overnight, the weekend, a holiday: its last day's sessions are behind it
}

const wchar_t* PhaseName(Phase p) {
    switch (p) {
    case kOpen: return L"Open";
    case kPreMarket: return L"Pre-market";
    case kAfterHours: return L"After hours";
    default: return L"Closed";
    }
}

std::wstring Setting(const char* key) { return sys::ReadText(SettingName(key).c_str()); }
void SetSetting(const char* key, const std::wstring& value) { sys::SaveText(SettingName(key).c_str(), value); }

std::wstring Secret(const char* key) {
    std::wstring stored = sys::ReadText(SettingName(key).c_str());
    if (stored.empty()) return {};
    std::string blob = FromBase64(stored);
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::wstring plain((const wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

void SetSecret(const char* key, const std::wstring& value) {
    if (value.empty()) return sys::SaveText(SettingName(key).c_str(), L"");
    DATA_BLOB in{(DWORD)(value.size() * sizeof(wchar_t)), (BYTE*)value.data()}, out{};
    if (!CryptProtectData(&in, L"widget key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    sys::SaveText(SettingName(key).c_str(), ToBase64(out.pbData, out.cbData));
    LocalFree(out.pbData);
}

void Start(HWND notify, UINT msg, const wchar_t* folder) {
    State& s = S();
    static const bool stops = [] { return atexit(Stop) == 0; }(); // a thread still running at exit would take the process down with it
    (void)stops;
    s.notify = notify, s.msg = msg;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        s.folder = std::wstring(base) + L"\\" + folder;
        CreateDirectoryW(s.folder.c_str(), nullptr);
        s.tally.path = s.folder + L"\\screentime.txt";
    }
    CoTaskMemFree(base);
}

void Want(const std::vector<std::string>& ids) {
    State& s = S();
    bool any = !ids.empty();
    {
        std::lock_guard lock(s.mu);
        if (s.wanted == ids && any == s.th.joinable()) return;
        s.wanted = ids;
    }
    if (any && !s.th.joinable()) {
        if (!s.wake) s.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s.stop.store(false), gQuit.store(false);
        s.th = std::thread(Loop);
    } else if (s.wake) {
        SetEvent(s.wake); // nothing wanted: it idles, never joined here, so a fetch under way can't hold the ui; Stop at exit ends it
    }
}

void Refresh(const std::string& id) {
    State& s = S();
    {
        std::lock_guard lock(s.mu);
        s.due[id] = 0;
    }
    if (s.wake) SetEvent(s.wake);
}

void Stop() {
    State& s = S();
    if (!s.th.joinable()) return;
    s.stop.store(true), gQuit.store(true);
    SetEvent(s.wake);
    s.th.join();
}

Series Get(const std::string& id) {
    State& s = S();
    std::lock_guard lock(s.mu);
    auto it = s.got.find(id);
    return it == s.got.end() ? Series{} : it->second;
}

Screen ScreenTime() {
    State& s = S();
    Screen out;
    std::lock_guard lock(s.mu);
    if (!s.tallyLoaded && !s.tally.path.empty()) {
        s.tallyLoaded = true;
        LoadTally(s.tally);
    }
    std::map<std::wstring, int> today, week;
    int now = Today();
    for (int back = 0; back < 7; ++back) {
        auto day = s.tally.byDay.find(Today(-back));
        if (day == s.tally.byDay.end()) continue;
        for (const auto& [hour, apps] : day->second)
            for (const auto& [app, secs] : apps) {
                out.week += secs;
                out.days[6 - back] += secs;
                week[app] += secs;
                if (day->first == now) out.today += secs, out.hours[hour] += secs, today[app] += secs;
            }
    }
    auto sorted = [](const std::map<std::wstring, int>& m) {
        std::vector<Used> v;
        for (const auto& [app, secs] : m) v.push_back({app, secs});
        std::sort(v.begin(), v.end(), [](const Used& a, const Used& b) { return a.seconds > b.seconds; });
        return v;
    };
    out.apps = sorted(today);
    out.appsWeek = sorted(week);
    return out;
}

}
