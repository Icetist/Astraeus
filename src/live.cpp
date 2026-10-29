// The live page's worker: fetches the picked sports and the headlines while the page is looked at, and nothing otherwise.
// Live matches every half minute, finished ones every twenty, the last of everything kept on disk for an instant show.
#include <shlobj.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "live_src.h"
#include "sys.h"

namespace live {

namespace {

constexpr int64_t kLiveEvery = 30; // seconds between refreshes of a match in progress
constexpr int64_t kRestEvery = 20 * 60; // finished results and headlines
constexpr int64_t kFailEvery = 60; // after a failed fetch
constexpr ULONGLONG kSeenMs = 45 * 1000; // the page counts as looked at this long after its last frame
constexpr size_t kMaxCache = 256 * 1024;

std::string KeyOf(const Pick& p) {
    char b[64];
    sprintf_s(b, "%s:%d:%d:%d", SportAt(p.sport).key, p.sex, p.league[0], p.league[1]);
    return b;
}
constexpr char kNewsKey[] = "#news";

std::wstring CacheFile() { // %LOCALAPPDATA%\Astraeus\live.json
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Astraeus";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\live.json";
    }
    CoTaskMemFree(base);
    return path;
}

struct Work { // shared with the detached worker, so the app never waits on a slow request to exit
    std::mutex lock;
    std::condition_variable wake;
    std::map<std::string, Card> cards;
    std::map<std::string, int64_t> due; // unix seconds each card wants fetching again
    std::vector<Pick> picks;
    bool news = true, force = false, busy = false, loaded = false, started = false;
    ULONGLONG seen = 0;
    HWND notify = nullptr;
    uint64_t version = 1;
    src::Net net;
};

std::shared_ptr<Work>& Shared() {
    static auto w = std::make_shared<Work>();
    return w;
}

std::string Dump(const std::map<std::string, Card>& cards) { // the cache: plain json, read back by the same reader
    using src::Quote;
    std::string s = "[";
    for (const auto& [key, c] : cards) {
        if (c.state == kLoading || c.state == kFailed) continue;
        char head[256];
        sprintf_s(head, "%s{\"key\":%s,\"state\":%d,\"major\":%d,\"list\":%d,\"at\":%lld,\"fetched\":%lld,", s.size() > 1 ? "," : "", Quote(src::Wide(key)).c_str(),
                  c.state == kLive ? kResult : c.state, c.major ? 1 : 0, c.list ? 1 : 0, (long long)c.at, (long long)c.fetched); // a live match read back later is no longer live
        s += head;
        s += "\"title\":" + Quote(c.title) + ",\"status\":" + Quote(c.status) + ",\"note\":" + Quote(c.note) + ",\"link\":" + Quote(c.link) + ",\"rows\":[";
        for (size_t i = 0; i < c.rows.size(); ++i)
            s += std::string(i ? "," : "") + "[" + Quote(c.rows[i].name) + "," + Quote(c.rows[i].sub) + "," + Quote(c.rows[i].score) + "," + (c.rows[i].win ? "1" : "0") + "]";
        s += "],\"heads\":[";
        for (size_t i = 0; i < c.heads.size(); ++i)
            s += std::string(i ? "," : "") + "[" + Quote(c.heads[i].title) + "," + Quote(c.heads[i].source) + "," + Quote(c.heads[i].link) + "," +
                 std::to_string(c.heads[i].at) + "]";
        s += "]}";
    }
    return s + "]";
}

void Save(const std::map<std::string, Card>& cards) {
    std::wstring path = CacheFile();
    if (path.empty()) return;
    std::string s = Dump(cards);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &wrote, nullptr);
    CloseHandle(f);
}

void Load(std::map<std::string, Card>& cards) {
    std::wstring path = CacheFile();
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::string s;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (LONGLONG)kMaxCache) {
        s.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr)) s.clear();
        s.resize(got);
    }
    CloseHandle(f);
    src::Val v;
    if (!src::Parse(s, v)) return;
    for (size_t i = 0; i < v.size(); ++i) {
        const src::Val& x = v[i];
        Card c;
        c.key = x["key"].Str();
        c.state = (int)x["state"].Num();
        c.major = x["major"].Num() != 0;
        c.list = x["list"].Num() != 0;
        c.at = (int64_t)x["at"].Num();
        c.fetched = (int64_t)x["fetched"].Num();
        c.title = src::Wide(x["title"].Str());
        c.status = src::Wide(x["status"].Str());
        c.note = src::Wide(x["note"].Str());
        c.link = src::Wide(x["link"].Str());
        for (size_t r = 0; r < x["rows"].size(); ++r) {
            const src::Val& y = x["rows"][r];
            c.rows.push_back({src::Wide(y[0].Str()), src::Wide(y[1].Str()), src::Wide(y[2].Str()), y[3].Num() != 0});
        }
        for (size_t h = 0; h < x["heads"].size(); ++h) {
            const src::Val& y = x["heads"][h];
            c.heads.push_back({src::Wide(y[0].Str()), src::Wide(y[1].Str()), src::Wide(y[2].Str()), (int64_t)y[3].Num()});
        }
        if (!c.key.empty()) cards[c.key] = std::move(c);
    }
}

void Worker(std::shared_ptr<Work> w) {
    std::unique_lock<std::mutex> lk(w->lock);
    for (;;) {
        bool looking = GetTickCount64() - w->seen < kSeenMs;
        int64_t now = Now();
        std::vector<std::pair<std::string, Pick>> todo; // key, and the pick (sport -1 for the news)
        int64_t soonest = INT64_MAX;
        auto check = [&](const std::string& key, const Pick& p) {
            auto it = w->due.find(key);
            int64_t at = it == w->due.end() ? 0 : it->second;
            if (w->force || at <= now) todo.push_back({key, p});
            else soonest = (std::min)(soonest, at);
        };
        if (looking || w->force) {
            for (const Pick& p : w->picks) check(KeyOf(p), p);
            if (w->news) check(kNewsKey, Pick{-1, 0, {0, 0}});
        }
        w->force = false;
        if (todo.empty()) {
            if (!looking) w->wake.wait(lk); // nobody's looking: sleep until the page shows again
            else w->wake.wait_for(lk, std::chrono::seconds((std::clamp)(soonest - now, (int64_t)1, (int64_t)kSeenMs / 1000)));
            continue;
        }
        w->busy = true;
        HWND notify = w->notify;
        lk.unlock();
        for (auto& [key, p] : todo) {
            Card c = p.sport < 0 ? src::World(w->net) : src::Fetch(w->net, p);
            c.key = key;
            if (w->net.stop) return;
            lk.lock();
            auto old = w->cards.find(key);
            int64_t next = c.state == kLive ? kLiveEvery : c.state == kFailed ? kFailEvery : kRestEvery;
            if (c.state == kNext && c.at > Now() && c.at - Now() < kRestEvery) next = (std::max)((int64_t)kLiveEvery, c.at - Now()); // kick-off is close: look again then
            w->due[key] = Now() + next;
            if (c.state != kFailed || old == w->cards.end() || old->second.state == kLoading) w->cards[key] = std::move(c); // a failed refresh keeps what was shown
            else old->second.state = old->second.state == kLive ? kResult : old->second.state; // stale: no longer claims to be live
            ++w->version;
            lk.unlock();
            if (notify) PostMessageW(notify, kMsg, 0, 0);
        }
        lk.lock();
        w->busy = false;
        std::map<std::string, Card> copy = w->cards;
        lk.unlock();
        Save(copy);
        lk.lock();
    }
}

void Sync(Work& w) { // the worker's copy of the picks; call under its lock
    const Cfg& cfg = Config();
    w.picks = cfg.picks;
    w.news = cfg.news;
}

}

int64_t Now() { return (int64_t)time(nullptr); }

std::vector<Card> Snapshot(uint64_t& version) {
    auto& w = Shared();
    std::lock_guard<std::mutex> g(w->lock);
    if (!w->loaded) {
        w->loaded = true;
        Load(w->cards);
        Sync(*w);
        for (const auto& [key, c] : w->cards) w->due[key] = c.fetched + (c.state == kLive ? kLiveEvery : kRestEvery); // cached: only refetched once stale
        ++w->version;
    }
    version = w->version;
    std::vector<Card> out;
    const Cfg& cfg = Config();
    auto add = [&](const std::string& key) {
        auto it = w->cards.find(key);
        Card c = it == w->cards.end() ? Card{} : it->second;
        c.key = key;
        out.push_back(std::move(c));
    };
    for (const Pick& p : cfg.picks) add(KeyOf(p));
    if (cfg.news) add(kNewsKey);
    return out;
}

void Seen(HWND notify) {
    auto& w = Shared();
    std::lock_guard<std::mutex> g(w->lock);
    bool was = GetTickCount64() - w->seen < kSeenMs;
    w->seen = GetTickCount64();
    w->notify = notify;
    if (!w->started) {
        w->started = true;
        Sync(*w);
        std::thread([s = w] { Worker(s); }).detach();
    }
    if (!was) w->wake.notify_all(); // it was asleep: look at what's due
}

void Refresh() {
    auto& w = Shared();
    {
        std::lock_guard<std::mutex> g(w->lock);
        w->force = true;
        w->seen = GetTickCount64();
    }
    w->wake.notify_all();
}

bool Busy() {
    auto& w = Shared();
    std::lock_guard<std::mutex> g(w->lock);
    return w->busy;
}

void Changed() {
    Config().Save();
    auto& w = Shared();
    {
        std::lock_guard<std::mutex> g(w->lock);
        Sync(*w);
    }
    w->wake.notify_all(); // new picks are due at once: they have no card yet
}

}
