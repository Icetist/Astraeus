#include "lyrics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

#include "sys.h"

namespace sys {
namespace {

constexpr wchar_t kApi[] = L"https://lrclib.net/api/"; // free, open, no key; asks only for a user agent
constexpr size_t kMaxBody = 4u << 20; // search results carry every candidate's full lyrics
constexpr size_t kKeep = 12; // songs remembered, so going back and forth costs nothing
constexpr DWORD kBusyWaitMs = 1500; // the service answers 503 when busy: one more try after a breath
constexpr ULONGLONG kFailWaitMs = 15000; // a song no source answered for is asked again after this
constexpr double kMaxOff = 6; // seconds a candidate's length may differ before its timing is someone else's cut

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Esc(const std::wstring& w) { // percent-encoded utf-8 for a query value
    static const char kHex[] = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char ch : Utf8(w)) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (wchar_t)ch;
        else {
            out += L'%';
            out += (wchar_t)kHex[ch >> 4];
            out += (wchar_t)kHex[ch & 15];
        }
    }
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::wstring CleanTitle(const std::wstring& raw) { // "TRIM (with Future)", "Song - 2011 Remaster" -> the bare name lrclib files it under
    static const wchar_t* const kNoise[] = {L"feat", L"ft.", L"with ", L"remaster", L"live", L"version", L"official", L"video", L"lyric",
                                            L"audio", L"prod", L"edit", L"explicit", L"mono", L"stereo", L"bonus", L"deluxe", L"visuali"};
    auto noisy = [](const std::wstring& part) {
        std::wstring low = Lower(part);
        for (const wchar_t* n : kNoise)
            if (low.find(n) != std::wstring::npos) return true;
        return false;
    };
    std::wstring s = raw;
    for (wchar_t open : {L'(', L'['}) { // bracketed asides
        wchar_t close = open == L'(' ? L')' : L']';
        for (size_t a = s.find(open); a != std::wstring::npos; a = s.find(open, a)) {
            size_t b = s.find(close, a);
            if (b == std::wstring::npos) break;
            if (noisy(s.substr(a, b - a))) s.erase(a, b - a + 1);
            else a = b;
        }
    }
    if (size_t d = s.find(L" - "); d != std::wstring::npos && noisy(s.substr(d))) s.erase(d); // trailing " - Remastered 2011"
    s = Trim(s);
    return s.empty() ? Trim(raw) : s;
}

std::wstring MainArtist(const std::wstring& raw) { // first of "A, B & C feat. D"
    std::wstring low = Lower(raw);
    size_t cut = raw.size();
    for (const wchar_t* sep : {L", ", L" & ", L" x ", L" feat", L" ft.", L" / ", L"; "})
        if (size_t p = low.find(sep); p != std::wstring::npos && p > 0) cut = (std::min)(cut, p);
    std::wstring s = Trim(raw.substr(0, cut));
    if (s.size() > 8 && Lower(s.substr(s.size() - 8)) == L" - topic") s.resize(s.size() - 8); // youtube's auto channels
    return s;
}

// ---- a small json reader for the services' answers ----
void PutUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
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

struct Val {
    enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
    double num = 0;
    std::string str;
    std::vector<Val> items; // array elements, or object values
    std::vector<std::string> keys; // object keys, beside their values

    const Val* operator[](const char* key) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    double Number(const char* key) const {
        const Val* v = (*this)[key];
        return v && v->kind == kNum ? v->num : 0;
    }
    std::string Text(const char* key) const {
        const Val* v = (*this)[key];
        return v && v->kind == kStr ? v->str : std::string();
    }
};

struct Json {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

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
    bool Str(std::string& out) { // at an opening quote
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
    bool Parse(Val& v) {
        Ws();
        if (i >= s.size() || depth > 64) return false;
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
                if (!Parse(v.items.back())) return false;
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
        char* end = nullptr;
        v.kind = Val::kNum;
        v.num = std::strtod(s.c_str() + i, &end);
        if (!end || end == s.c_str() + i) return false;
        i = (size_t)(end - s.c_str());
        return true;
    }
};

bool Noise(const std::wstring& text) { // section headers like "[Chorus]", credit lines, "instrumental, enjoy"
    if (text.size() >= 2 && ((text.front() == L'[' && text.back() == L']') || (text.front() == L'(' && text.back() == L')' && text.find(L':') != std::wstring::npos)))
        return true;
    if (text.find(L'\uFF1A') != std::wstring::npos) return true; // a full-width colon only ever appears in credit lines
    if (size_t colon = text.find(L':'); colon != std::wstring::npos && colon < 28) {
        std::wstring head = Lower(text.substr(0, colon));
        for (const wchar_t* k : {L"produc", L"writ", L"compos", L"lyric", L"arrang", L"mix", L"master", L"record", L"engineer", L"vocal", L"guitar", L"bass",
                                 L"drum", L"keyboard", L"program", L"label", L"publish"})
            if (head.find(k) != std::wstring::npos) return true;
    }
    for (const wchar_t* k : {L"\u4f5c\u8bcd", L"\u4f5c\u66f2", L"\u7f16\u66f2", L"\u5236\u4f5c", L"\u6df7\u97f3", L"\u5f55\u97f3", L"\u76d1\u5236", L"\u548c\u58f0", L"\u6bcd\u5e26",
                             L"\u7eaf\u97f3\u4e50"}) // lyricist, composer, arranger, producer, mixing, recording, supervisor, backing vocals, mastering, instrumental
        if (text.find(k) != std::wstring::npos) return true;
    return false;
}

bool Stamp(const std::wstring& s, size_t& i, wchar_t close, double& t) { // "mm:ss.xx" up to close, i moves past it
    size_t e = s.find(close, i);
    if (e == std::wstring::npos) return false;
    std::wstring in = s.substr(i, e - i);
    size_t colon = in.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    for (wchar_t c : in)
        if (!std::iswdigit(c) && c != L':' && c != L'.') return false;
    t = _wtoi(in.substr(0, colon).c_str()) * 60.0 + _wtof(in.substr(colon + 1).c_str());
    i = e + 1;
    return true;
}

LyricLine ParseText(const std::wstring& rest) { // the words after the line stamp; enhanced lrc puts <mm:ss.xx> before each word
    LyricLine line;
    double t;
    for (size_t k = 0; k < rest.size();) {
        if (rest[k] == L'<') {
            size_t j = k + 1;
            if (Stamp(rest, j, L'>', t)) {
                while (j < rest.size() && rest[j] == L' ') { // the space after a stamp belongs before the word
                    line.text += L' ';
                    ++j;
                }
                line.words.push_back({t, (int)line.text.size()});
                k = j;
                continue;
            }
        }
        if (rest[k] != L'\r') line.text += rest[k];
        ++k;
    }
    size_t lead = 0;
    while (lead < line.text.size() && std::iswspace(line.text[lead])) ++lead;
    line.text = Trim(line.text);
    for (auto& w : line.words) w.second = (std::clamp)(w.second - (int)lead, 0, (int)line.text.size());
    return line;
}

std::vector<LyricLine> ParseLrc(const std::wstring& lrc) {
    std::vector<std::pair<double, LyricLine>> raw;
    for (size_t pos = 0; pos < lrc.size();) {
        size_t nl = lrc.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = lrc.size();
        std::wstring row = lrc.substr(pos, nl - pos);
        pos = nl + 1;
        std::vector<double> times;
        size_t i = 0;
        double t;
        while (i < row.size() && row[i] == L'[') { // one or more line stamps
            size_t j = i + 1;
            if (!Stamp(row, j, L']', t)) break;
            times.push_back(t);
            i = j;
        }
        if (times.empty()) continue; // [ar:..] tags and junk
        LyricLine line = ParseText(row.substr(i));
        if (Noise(line.text)) line = {}; // kept as a blank: it still ends the line before it
        for (double at : times) raw.push_back({at, line});
    }
    std::stable_sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<LyricLine> out;
    for (size_t k = 0; k < raw.size(); ++k) {
        if (raw[k].second.text.empty()) continue; // a blank stamp only marks where the line before stops
        if (raw[k].second.words.empty() && raw[k].second.text.find_first_not_of(L"()[] ") == std::wstring::npos) continue; // brackets and nothing in them
        LyricLine l = std::move(raw[k].second);
        l.start = raw[k].first;
        double next = k + 1 < raw.size() ? raw[k + 1].first : l.start + 6, room = (std::max)(0.3, next - l.start - 0.05);
        double sing = (std::max)(1.0, l.text.size() * 0.085); // roughly how long it takes to sing
        if (!l.words.empty()) sing = (std::max)(sing, l.words.back().first - l.start + 0.45);
        l.end = l.start + (std::min)(room, sing);
        out.push_back(std::move(l));
    }
    return out;
}

}

struct LyricSrc::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<std::shared_ptr<const Lyrics>> kept;
    struct Ask {
        std::wstring key, title, artist, album;
        double seconds = 0;
    };
    Ask next; // waiting to start, key empty = none
    std::wstring busyKey; // being looked up
    std::thread th;
    std::atomic<bool> stop{false}; // shutting down
    struct Lane { // one per source, so the slower one can be cut off
        std::atomic<bool> halt{false};
        std::atomic<void*> req{nullptr};
    };
    Lane lanes[2];
    std::deque<std::pair<std::wstring, ULONGLONG>> failed; // songs no source could be reached for, not asked again for a while

    bool Get(Lane& ln, const std::wstring& url, std::string& body, int tries = 2) {
        for (int i = 0; i < tries && !ln.halt.load(); ++i) {
            if (i) ::Sleep(kBusyWaitMs);
            if (detail::HttpGet(url.c_str(), body, ln.halt, ln.req, kMaxBody)) return true;
        }
        return false;
    }

    static bool Synced(const std::vector<LyricLine>& lines) { // real timing, not every line stamped zero
        return lines.size() >= 3 && lines.back().start - lines.front().start > 10;
    }

    static std::vector<LyricLine> Best(const std::string& body, double seconds) { // lrclib: the synced candidate closest in length
        Val root;
        if (!Json{body}.Parse(root)) return {};
        std::vector<const Val*> all;
        if (root.kind == Val::kArr)
            for (const Val& v : root.items) all.push_back(&v);
        else if (root.kind == Val::kObj) all.push_back(&root);
        const Val* pick = nullptr;
        double off = 1e9;
        for (const Val* o : all) {
            if (o->Text("syncedLyrics").empty()) continue;
            double len = o->Number("duration"), d = seconds > 0 && len > 0 ? std::fabs(len - seconds) : 0;
            if (d < off) {
                off = d;
                pick = o;
            }
        }
        if (!pick || off > kMaxOff) return {};
        auto lines = ParseLrc(Wide(pick->Text("syncedLyrics")));
        return Synced(lines) ? lines : std::vector<LyricLine>{};
    }

    std::vector<LyricLine> Netease(Lane& ln, const std::wstring& title, const std::wstring& artist, double seconds, bool& reached) { // strong on hip-hop and pop
        std::string body;
        if (!Get(ln, L"https://music.163.com/api/search/get?s=" + Esc(artist + L" " + title) + L"&type=1&limit=8", body)) return {};
        reached = true;
        Val root;
        if (!Json{body}.Parse(root)) return {};
        const Val* result = root["result"];
        const Val* songs = result ? (*result)["songs"] : nullptr;
        if (!songs || songs->kind != Val::kArr) return {};
        const std::wstring t = Lower(title), a = Lower(artist);
        for (const Val& song : songs->items) {
            if (ln.halt.load()) return {};
            std::wstring name = Lower(CleanTitle(Wide(song.Text("name"))));
            bool titleOk = name == t || (!name.empty() && (name.find(t) != std::wstring::npos || t.find(name) != std::wstring::npos));
            bool artistOk = false;
            if (const Val* artists = song["artists"]; artists && artists->kind == Val::kArr)
                for (const Val& who : artists->items) {
                    std::wstring n = Lower(Wide(who.Text("name")));
                    artistOk |= !n.empty() && (n.find(a) != std::wstring::npos || a.find(n) != std::wstring::npos);
                }
            double len = song.Number("duration") / 1000;
            if (!titleOk || !artistOk || (seconds > 0 && len > 0 && std::fabs(len - seconds) > kMaxOff)) continue;
            std::string lyric;
            if (!Get(ln, L"https://music.163.com/api/song/lyric?id=" + std::to_wstring((long long)song.Number("id")) + L"&lv=1", lyric)) continue;
            Val doc;
            if (!Json{lyric}.Parse(doc)) continue;
            const Val* lrc = doc["lrc"];
            auto lines = lrc ? ParseLrc(Wide(lrc->Text("lyric"))) : std::vector<LyricLine>{};
            if (Synced(lines)) return lines;
        }
        return {};
    }

    std::vector<LyricLine> Lrclib(Lane& ln, const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds, bool& reached) {
        std::string body;
        if (!album.empty() && seconds > 0) { // exact match first: one small answer
            std::wstring url = std::wstring(kApi) + L"get?track_name=" + Esc(title) + L"&artist_name=" + Esc(artist) + L"&album_name=" + Esc(album) +
                               L"&duration=" + std::to_wstring((int)std::lround(seconds));
            if (Get(ln, url, body, 1)) {
                auto lines = Best(body, seconds);
                if (!lines.empty()) return lines;
            }
        }
        if (!Get(ln, std::wstring(kApi) + L"search?track_name=" + Esc(title) + L"&artist_name=" + Esc(artist), body)) return {};
        reached = true;
        auto lines = Best(body, seconds);
        if (!lines.empty()) return lines;
        if (Get(ln, std::wstring(kApi) + L"search?q=" + Esc(artist + L" " + title), body)) return Best(body, seconds); // loose words, last try
        return {};
    }

    std::vector<LyricLine> Look(const Ask& a, bool& reached) { // both sources at once, the first with lyrics wins; reached: one answered, so empty means none
        const std::wstring title = CleanTitle(a.title), artist = MainArtist(a.artist);
        struct Out {
            std::vector<LyricLine> lines;
            bool reached = false, done = false;
        } out[2];
        std::mutex m;
        std::condition_variable cv2;
        for (Lane& ln : lanes) {
            ln.halt.store(stop.load());
            ln.req.store(nullptr);
        }
        auto run = [&](int k) {
            std::vector<LyricLine> got;
            bool hit = false;
            try {
                got = k == 0 ? Lrclib(lanes[0], title, artist, a.album, a.seconds, hit) : Netease(lanes[1], title, artist, a.seconds, hit);
            } catch (...) {
                OutputDebugStringW(L"[sys] lyrics source failed\n");
            }
            std::lock_guard g(m);
            out[k].lines = std::move(got);
            out[k].reached = hit;
            out[k].done = true;
            cv2.notify_all();
        };
        std::thread t0(run, 0), t1(run, 1);
        {
            std::unique_lock g(m);
            auto found = [&](int k) { return out[k].done && !out[k].lines.empty(); };
            cv2.wait(g, [&] { return found(0) || found(1) || (out[0].done && out[1].done); });
            if (found(1) && !out[0].done) cv2.wait_for(g, std::chrono::milliseconds(1200), [&] { return out[0].done; }); // lrclib's timing is usually finer: a moment's grace
        }
        for (int k = 0; k < 2; ++k)
            if (!out[k].done) detail::HttpCancel(lanes[k].halt, lanes[k].req); // the slower one is no longer needed
        t0.join();
        t1.join();
        reached = out[0].reached || out[1].reached;
        if (!out[0].lines.empty()) return std::move(out[0].lines);
        return std::move(out[1].lines);
    }

    void Loop() {
        for (;;) {
            Ask a;
            {
                std::unique_lock lock(mu);
                cv.wait(lock, [this] { return stop.load() || !next.key.empty(); });
                if (stop.load()) return;
                a = std::move(next);
                next = {};
                busyKey = a.key;
            }
            auto doc = std::make_shared<Lyrics>();
            doc->key = a.key;
            bool reached = false;
            try {
                doc->lines = Look(a, reached);
            } catch (...) {
                OutputDebugStringW(L"[sys] lyrics lookup failed\n");
            }
            doc->done = true;
            std::lock_guard lock(mu);
            busyKey.clear();
            if (stop.load()) return;
            if (doc->lines.empty() && !reached) { // offline or both services busy: not remembered, asked again in a while
                failed.push_back({doc->key, GetTickCount64()});
                if (failed.size() > 16) failed.pop_front();
                continue;
            }
            kept.push_back(std::move(doc));
            if (kept.size() > kKeep) kept.pop_front();
        }
    }
};

LyricSrc::LyricSrc() = default;

LyricSrc::~LyricSrc() {
    if (!impl) return;
    {
        std::lock_guard lock(impl->mu);
        impl->stop.store(true);
    }
    impl->cv.notify_all();
    for (auto& ln : impl->lanes) detail::HttpCancel(ln.halt, ln.req);
    if (impl->th.joinable()) impl->th.join();
}

void LyricSrc::Want(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds) {
    if (title.empty()) return;
    std::wstring key = Key(title, artist);
    try {
        if (!impl) impl = std::make_shared<Impl>();
        {
            std::lock_guard lock(impl->mu);
            if (impl->busyKey == key || impl->next.key == key) return;
            for (const auto& d : impl->kept)
                if (d->key == key) return;
            for (const auto& f : impl->failed)
                if (f.first == key && GetTickCount64() - f.second < kFailWaitMs) return;
            impl->next = {key, title, artist, album, seconds}; // a newer song replaces one still waiting
        }
        if (!impl->th.joinable()) impl->th = std::thread([p = impl.get()] { p->Loop(); }); // one worker, started the first time lyrics are wanted
        impl->cv.notify_one();
    } catch (...) {
        OutputDebugStringW(L"[sys] lyrics request failed\n");
    }
}

std::shared_ptr<const Lyrics> LyricSrc::Get(const std::wstring& key) const {
    if (!impl) return nullptr;
    std::lock_guard lock(impl->mu);
    for (auto it = impl->kept.rbegin(); it != impl->kept.rend(); ++it)
        if ((*it)->key == key) return *it;
    return nullptr;
}

}
