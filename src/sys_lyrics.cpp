#include "lyrics.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

#include "inflate.h"
#include "sys.h"

namespace sys {
namespace {

constexpr wchar_t kApi[] = L"https://lrclib.net/api/"; // free, open, no key; asks only for a user agent
constexpr size_t kMaxBody = 4u << 20; // search results carry every candidate's full lyrics
constexpr size_t kKeep = 12; // songs remembered, so going back and forth costs nothing
constexpr DWORD kBusyWaitMs = 1500; // the service answers 503 when busy: one more try after a breath
constexpr ULONGLONG kFailWaitMs = 15000; // a song no source answered for is asked again after this
constexpr int kFailSteps = 4; // each miss in a row waits four times longer, up to this many times
constexpr size_t kMaxRow = 2000, kMaxLines = 5000; // a lyric line and a song, generously; anything bigger is hostile
constexpr double kMaxOff = 4; // seconds a candidate's length may differ before its timing is someone else's cut
constexpr double kExactOff = 2; // seconds: found by the title alone, or calling the song instrumental, it must be this recording
constexpr double kKugouOff = 2.5; // kugou rounds its lengths to the second
constexpr double kForeign = 0.1; // a latin title's lyrics with less latin than this share of their letters are another song's
constexpr auto kVerdictWait = std::chrono::seconds(4); // lyrics found elsewhere wait this long for lrclib to say the song is instrumental

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

std::wstring BareTitle(const std::wstring& raw) { // every aside gone: "Song (Live at X) - From Film" -> "Song", for a last try
    std::wstring s;
    int depth = 0;
    for (wchar_t c : raw) {
        if (c == L'(' || c == L'[') ++depth;
        else if ((c == L')' || c == L']') && depth > 0) --depth;
        else if (!depth) s += c;
    }
    if (size_t d = s.find(L" - "); d != std::wstring::npos && d > 0) s.erase(d);
    s = Trim(s);
    return s.empty() ? Trim(raw) : s;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// The disk cache: one small file per song in %LOCALAPPDATA%\Astraeus\lyrics, the newest 300 kept, read and written on the lyrics
// thread (a timing nudge writes from the ui, rarely). A song with no lyrics is believed for 3 days, then asked again.
constexpr size_t kDiskKeep = 300;
constexpr long long kNoneDays = 3;
constexpr char kDiskMagic[] = "ALYR3"; // 3: machine hums filtered; what the looser lookups saved is looked up again

std::wstring DiskDir() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(base) + L"\\Astraeus";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\lyrics";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const std::wstring& Disk() {
    static const std::wstring dir = DiskDir();
    return dir;
}

std::wstring DiskPath(const std::wstring& key) {
    if (Disk().empty()) return {};
    uint64_t h = 1469598103934665603ull; // fnv-1a 64
    for (wchar_t c : key) h = (h ^ (uint16_t)c) * 1099511628211ull;
    wchar_t name[24];
    swprintf_s(name, L"%016llx.lyr", (unsigned long long)h);
    return Disk() + L"\\" + name;
}

long long NowDays() { return (long long)(time(nullptr) / 86400); }

void DiskSave(const Lyrics& d) {
    std::wstring path = DiskPath(d.key);
    if (path.empty()) return;
    std::string out = std::string(kDiskMagic) + "\n" +ToUtf8(d.key) + "\n" + std::to_string(d.offsetMs) + " " + std::to_string(NowDays()) + " " + std::to_string(d.lines.size()) + "\n";
    for (const LyricLine& l : d.lines) {
        char head[64];
        snprintf(head, sizeof(head), "%.3f %.3f %zu", l.start, l.end, l.words.size());
        out += head;
        for (const auto& w : l.words) {
            snprintf(head, sizeof(head), " %.3f %d", w.first, w.second);
            out += head;
        }
        out += "\t" + ToUtf8(l.text) + "\n";
    }
    std::wstring tmp = path + L".tmp"; // whole or nothing: written aside, then swapped in
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr) && wrote == out.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

void DiskPrune() { // the least recently played go once there are more than kDiskKeep
    if (Disk().empty()) return;
    struct Entry {
        std::wstring name;
        FILETIME at;
    };
    std::vector<Entry> all;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Disk() + L"\\*.lyr").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do all.push_back({fd.cFileName, fd.ftLastWriteTime});
    while (FindNextFileW(h, &fd) && all.size() < 4096);
    FindClose(h);
    if (all.size() <= kDiskKeep) return;
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return CompareFileTime(&a.at, &b.at) < 0; });
    for (size_t i = 0; i + kDiskKeep < all.size(); ++i) DeleteFileW((Disk() + L"\\" + all[i].name).c_str());
}

std::shared_ptr<Lyrics> DiskLoad(const std::wstring& key) { // null when not there, stale or not this song
    std::wstring path = DiskPath(key);
    if (path.empty()) return nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    std::string in;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (2 << 20)) {
        in.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, in.data(), (DWORD)in.size(), &got, nullptr) || got != in.size()) in.clear();
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now); // played = recent: it outlives the songs not heard in a while
    CloseHandle(f);
    size_t a = in.find('\n'), b = a == std::string::npos ? a : in.find('\n', a + 1), c = b == std::string::npos ? b : in.find('\n', b + 1);
    if (c == std::string::npos || in.compare(0, a, kDiskMagic) != 0 || Wide(in.substr(a + 1, b - a - 1)) != key) return nullptr;
    long long offset = 0, day = 0, count = 0;
    if (sscanf_s(in.c_str() + b + 1, "%lld %lld %lld", &offset, &day, &count) != 3 || count < 0 || count > (long long)kMaxLines) return nullptr;
    if (count == 0 && NowDays() - day > kNoneDays) return nullptr; // "none" is only believed for a few days
    auto doc = std::make_shared<Lyrics>();
    doc->key = key;
    doc->done = true;
    doc->offsetMs = (int)(std::clamp)(offset, -5000LL, 5000LL);
    for (size_t pos = c + 1; pos < in.size() && (long long)doc->lines.size() < count;) {
        size_t end = in.find('\n', pos), tab = in.find('\t', pos);
        if (end == std::string::npos || tab == std::string::npos || tab > end) break;
        std::string head = in.substr(pos, tab - pos);
        LyricLine l;
        size_t words = 0;
        int used = 0;
        if (sscanf_s(head.c_str(), "%lf %lf %zu%n", &l.start, &l.end, &words, &used) < 3 || words > 400) break;
        const char* p = head.c_str() + used;
        for (size_t k = 0; k < words; ++k) {
            double at = 0;
            int off = 0, n = 0;
            if (sscanf_s(p, " %lf %d%n", &at, &off, &n) < 2) break;
            l.words.push_back({at, off});
            p += n;
        }
        l.text = Wide(in.substr(tab + 1, end - tab - 1));
        for (auto& w : l.words) w.second = (std::clamp)(w.second, 0, (int)l.text.size());
        doc->lines.push_back(std::move(l));
        pos = end + 1;
    }
    if ((long long)doc->lines.size() != count) return nullptr; // cut short: look it up again
    return doc;
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

// ---- telling the playing song from a search's near misses ----
std::wstring Narrow(std::wstring s) { // full-width forms and cjk brackets to plain ascii, so "Song（Live）" cleans like "Song (Live)"
    for (auto& c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = (wchar_t)(c - 0xFEE0);
        else if (c == 0x3000) c = L' ';
        else if (c == 0x3010 || c == 0x300C || c == 0x300E) c = L'['; // 【 「 『
        else if (c == 0x3011 || c == 0x300D || c == 0x300F) c = L']';
        else if (c == 0x2018 || c == 0x2019) c = L'\'';
    }
    return s;
}

std::wstring Fold(const std::wstring& s) { // what a name says: letters and digits, lowercase, "&" read as "and"
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&') out += L"and";
        else if (std::iswalnum(c) || c >= 0x3040) out += (wchar_t)std::towlower(c);
    }
    return out.empty() ? Lower(Trim(s)) : out; // all symbols: compared as written
}

std::wstring TitleKey(const std::wstring& raw) { return Fold(CleanTitle(Narrow(raw))); }
std::wstring BareKey(const std::wstring& raw) { return Fold(BareTitle(Narrow(raw))); }

bool Backing(const std::wstring& title) { // a karaoke or instrumental cut, never the sung song's lyrics
    std::wstring low = Lower(title);
    for (const wchar_t* k : {L"instrumental", L"karaoke", L"off vocal", L"inst.", L"伴奏"}) // 伴奏: backing track
        if (low.find(k) != std::wstring::npos) return true;
    return false;
}

int Tongue(const std::wstring& title) { // a version sung in another language: the same tune, other words; 0 none, else which
    std::wstring low = Lower(title);
    int n = 0;
    for (const wchar_t* k : {L"japanese", L"korean", L"chinese", L"english", L"spanish", L"mandarin", L"cantonese", L"日本語", L"日语", L"韩语",
                             L"中文", L"国语", L"粤语"}) { // 日本語 日语 韩语 中文 国语 粤语
        ++n;
        if (low.find(k) != std::wstring::npos) return n;
    }
    return 0;
}

std::vector<std::wstring> Names(const std::wstring& raw) { // "A, B & C feat. D", "A、B", "A / B": each credited name, folded
    std::wstring s = Lower(Narrow(raw));
    for (const wchar_t* sep : {L" feat.", L" feat ", L" ft.", L" ft ", L" featuring ", L" with ", L" vs.", L" vs ", L" x ", L",", L"&", L"/", L";", L"、"})
        for (size_t p = s.find(sep); p != std::wstring::npos; p = s.find(sep, p + 1)) s.replace(p, wcslen(sep), L"\x1f");
    std::vector<std::wstring> out;
    for (size_t a = 0; a <= s.size();) {
        size_t b = s.find(L'\x1f', a);
        if (b == std::wstring::npos) b = s.size();
        if (std::wstring f = Fold(s.substr(a, b - a)); !f.empty()) out.push_back(f);
        a = b + 1;
    }
    return out;
}

struct Match { // the song that's playing, folded once for every candidate it meets
    std::wstring title, bare, artist; // folded; artist is the main one
    std::vector<std::wstring> artists; // every name credited, folded
    double seconds = 0;
    bool backing = false, latin = false, latinBy = false; // a karaoke cut itself; a title written only in latin letters; the artist too
    int tongue = 0; // the language version it names, if any

    static bool Latin(const std::wstring& s) { // latin letters and no other script
        bool latin = false, other = false;
        for (wchar_t c : s) {
            if (c < 0x250 && std::iswalpha(c)) latin = true;
            else if (c >= 0x370 && (std::iswalpha(c) || c >= 0x2E80)) other = true;
        }
        return latin && !other;
    }
    Match(const std::wstring& t, const std::wstring& a, double sec)
        : title(TitleKey(t)), bare(BareKey(t)), artist(Fold(MainArtist(a))), artists(Names(a)), seconds(sec), backing(Backing(t)), latin(Latin(t)), latinBy(Latin(a)),
          tongue(Tongue(t)) {}
    bool Title(const std::wstring& got, const std::vector<std::wstring>& aliases = {}) const { // the same name, or a translation the source itself gives
        if (Backing(got) != backing || Tongue(got) != tongue) return false; // "(Japanese Version)" cleans away like "(Album Version)", but it's other words
        auto same = [&](const std::wstring& name) { std::wstring k = TitleKey(name); return k == title || k == bare; }; // ours may carry an aside theirs lacks, never the reverse
        if (same(got)) return true;
        for (const auto& a : aliases)
            if (!a.empty() && same(a)) return true;
        return false;
    }
    bool Artist(const std::vector<std::wstring>& got) const { // one name in common, or one inside the other ("Kanye West", "Kanye")
        for (const auto& g : got)
            for (const auto& w : artists) {
                const std::wstring& shorter = g.size() < w.size() ? g : w;
                const std::wstring& longer = g.size() < w.size() ? w : g;
                bool longEnough = shorter.size() >= 3 || (shorter.size() >= 2 && shorter[0] >= 0x2E80); // two cjk characters make a name, two letters don't
                if (g == w || (longEnough && longer.find(shorter) != std::wstring::npos)) return true;
            }
        return false;
    }
    bool Exact(const std::wstring& got, const std::vector<std::wstring>& names) const { // word for word, no alias, no overlap
        return TitleKey(got) == title && std::find(names.begin(), names.end(), artist) != names.end();
    }
    bool Length(double len, double tol) const { return seconds <= 0 || len <= 0 || std::fabs(len - seconds) <= tol; } // only when both sides know it
};

bool Machine(const std::wstring& text) { // "本字幕由 TME AI 技术生成": a machine's guess at an instrumental's hums, not lyrics
    return text.find(L"字幕") != std::wstring::npos && (text.find(L"AI") != std::wstring::npos || text.find(L"生成") != std::wstring::npos);
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
    bool Flag(const char* key) const {
        const Val* v = (*this)[key];
        return v && v->kind == kBool && v->num != 0;
    }
    std::vector<std::wstring> Strings(const char* key) const; // an array of names, as wide strings
};

struct Json {
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
        if (i >= s.size() || depth > 64 || ++nodes > 60000) return false; // deep or huge: not a lyrics answer
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
        if (c != '-' && (c < '0' || c > '9')) return false; // json numbers only: no nan, inf or hex
        v.kind = Val::kNum;
        v.num = std::strtod(s.c_str() + i, &end);
        if (!end || end == s.c_str() + i || !std::isfinite(v.num)) return false;
        i = (size_t)(end - s.c_str());
        return true;
    }
};

std::vector<std::wstring> Val::Strings(const char* key) const {
    std::vector<std::wstring> out;
    if (const Val* v = (*this)[key]; v && v->kind == kArr)
        for (const Val& s : v->items)
            if (s.kind == kStr && out.size() < 16) out.push_back(Wide(s.str));
    return out;
}

bool Hum(const std::wstring& text) { // "嗯嗯", "啦啦啦": a transcriber's hums and la-las, nothing to sing along to
    bool any = false;
    for (wchar_t c : text) {
        if (std::iswspace(c) || std::iswpunct(c) || (c >= 0x3000 && c < 0x3040) || (c >= 0xFF00 && c < 0xFF20)) continue;
        if (!std::wcschr(L"嗯啦啊哦喔呜哈嘿噢呀", c)) return false; // 嗯啦啊哦喔呜哈嘿噢呀
        any = true;
    }
    return any;
}

bool Noise(const std::wstring& text) { // section headers like "[Chorus]", credit lines, "instrumental, enjoy"
    if (Hum(text)) return true;
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

bool Stamp(const std::wstring& s, size_t& i, wchar_t close, double& t) { // "mm:ss.xx" up to close, i moves past it; looks no further than a stamp can be
    size_t e = i, colon = std::wstring::npos;
    for (; e < s.size() && e - i <= 12 && s[e] != close; ++e) {
        if (s[e] == L':' && colon == std::wstring::npos) colon = e;
        else if (!std::iswdigit(s[e]) && s[e] != L'.') return false;
    }
    if (e >= s.size() || s[e] != close || colon == std::wstring::npos || colon == i) return false;
    t = _wtoi(s.c_str() + i) * 60.0 + _wtof(s.c_str() + colon + 1); // both stop at the first non-digit
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

bool Latin(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'\''; }

// Word-timed lines: netease's yrc "[start,dur](ms,dur,0)word" with absolute word times, kugou's krc "[start,dur]<off,dur,0>word" relative to the line.
std::vector<LyricLine> ParseTimed(const std::wstring& doc, wchar_t open, bool relative) {
    const wchar_t close = open == L'(' ? L')' : L'>';
    std::vector<LyricLine> out;
    for (size_t pos = 0; pos < doc.size();) {
        size_t nl = doc.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = doc.size();
        std::wstring row = doc.substr(pos, (std::min)(nl - pos, kMaxRow));
        pos = nl + 1;
        if (out.size() >= kMaxLines) break;
        if (row.size() < 4 || row[0] != L'[' || !std::iswdigit(row[1])) continue; // metadata and credit blocks
        size_t rb = row.find(L']'), comma = row.find(L',');
        if (rb == std::wstring::npos) continue;
        LyricLine line;
        line.start = _wtof(row.c_str() + 1) / 1000;
        line.end = line.start + (comma < rb ? _wtof(row.c_str() + comma + 1) / 1000 : 0);
        for (size_t k = rb + 1; k < row.size();) {
            size_t e = k + 1, commas = 0; // a timing tuple is digits and two commas, nothing else: "(Yeah)" in the words stays words
            while (e < row.size() && e - k < 32 && (std::iswdigit(row[e]) || row[e] == L',')) commas += row[e++] == L',';
            if (row[k] != open || e >= row.size() || row[e] != close || commas != 2 || line.words.size() >= 400) {
                if (!line.words.empty()) line.text += row[k]; // part of the last word
                ++k;
                continue;
            }
            double at = _wtof(row.c_str() + k + 1) / 1000 + (relative ? line.start : 0);
            size_t j = e + 1;
            std::wstring word;
            while (j < row.size() && row[j] != L'\r') { // up to the next real tuple
                if (row[j] == open) {
                    size_t q = j + 1, c2 = 0;
                    while (q < row.size() && q - j < 32 && (std::iswdigit(row[q]) || row[q] == L',')) c2 += row[q++] == L',';
                    if (q < row.size() && row[q] == close && c2 == 2) break;
                }
                word += row[j++];
            }
            if (!word.empty()) {
                if (open == L'(' && !line.text.empty() && Latin(line.text.back()) && Latin(word.front())) line.text += L' '; // yrc drops some spaces between words
                size_t lead = 0;
                while (lead < word.size() && word[lead] == L' ') ++lead, line.text += L' ';
                line.words.push_back({at, (int)line.text.size()});
                line.text += word.substr(lead);
            }
            k = j;
        }
        size_t lead = 0;
        while (lead < line.text.size() && std::iswspace(line.text[lead])) ++lead;
        line.text = Trim(line.text);
        for (auto& w : line.words) w.second = (std::clamp)(w.second - (int)lead, 0, (int)line.text.size());
        if (line.text.empty() || Noise(line.text)) continue;
        if (line.end <= line.start) line.end = line.start + 2;
        out.push_back(std::move(line));
    }
    std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.start < b.start; });
    return out;
}

bool Worded(const std::vector<LyricLine>& lines) { // real word timing on most lines
    size_t n = 0;
    for (const auto& l : lines) n += l.words.empty() ? 0 : 1;
    return !lines.empty() && n * 2 >= lines.size();
}

// Line-timed lyrics still light word by word: each word gets a beat from its syllables, a breath after commas, the last word held a little.
void Placeholder(LyricLine& l) {
    size_t latin = 0, cjk = 0;
    for (wchar_t c : l.text) latin += c < 0x250 && std::iswalpha(c), cjk += c >= 0x2E80 && c < 0xFF00;
    if (latin > cjk) // netease writes a latin line's asides in full-width brackets: "empty （Oh）" reads as "empty (Oh)"; one for one, word offsets stay
        for (wchar_t& c : l.text) c = c == L'（' ? L'(' : c == L'）' ? L')' : c;
    if (!l.words.empty() || l.text.empty()) return;
    struct Beat {
        int at;
        double weight;
    };
    std::vector<Beat> beats;
    const std::wstring& s = l.text;
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && std::iswspace(s[i])) ++i;
        if (i >= s.size()) break;
        bool wide = s[i] >= 0x2E80; // cjk: a character is a syllable
        size_t j = i + 1;
        if (!wide)
            while (j < s.size() && !std::iswspace(s[j]) && s[j] < 0x2E80) ++j;
        double syllables = 0;
        bool vowel = false;
        for (size_t k = i; k < j; ++k) {
            bool v = std::wcschr(L"aeiouyAEIOUY", s[k]) != nullptr && s[k] != 0;
            if (v && !vowel) syllables += 1;
            vowel = v;
        }
        double w = wide ? 1.0 : (std::max)(1.0, syllables);
        if (std::wcschr(L",;:-", s[j - 1])) w += 0.6; // a breath before the next word
        beats.push_back({(int)i, w});
        i = j;
    }
    if (beats.size() < 2) return; // one word: the plain sweep is the same thing
    beats.back().weight *= 1.35;
    double total = 0;
    for (const Beat& b : beats) total += b.weight;
    double span = (std::max)(0.3, l.end - l.start), t = l.start;
    for (const Beat& b : beats) {
        l.words.push_back({t, b.at});
        t += span * b.weight / total;
    }
}

std::vector<LyricLine> ParseLrc(const std::wstring& lrc) {
    std::vector<std::pair<double, LyricLine>> raw;
    for (size_t pos = 0; pos < lrc.size();) {
        size_t nl = lrc.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = lrc.size();
        std::wstring row = lrc.substr(pos, (std::min)(nl - pos, kMaxRow)); // no line is that long; a hostile one is cut
        pos = nl + 1;
        if (raw.size() >= kMaxLines) break;
        std::vector<double> times;
        size_t i = 0;
        double t;
        while (i < row.size() && row[i] == L'[') { // one or more line stamps
            size_t j = i + 1;
            if (!Stamp(row, j, L']', t) || times.size() >= 32) break; // a few stamps share a line at most
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
        else sing = (std::max)(sing, (std::min)(room * 0.82, sing * 2.2)); // no word timing: the song's own pace, most of the way to the next line, so slow songs don't race ahead
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
    std::atomic<bool> queued{false}; // a newer song is waiting: a lookup still hoping for word timing gives up
    std::wstring busyKey; // being looked up
    std::thread th;
    unsigned saved = 0; // songs written to disk this session, the lyrics thread's own count
    std::atomic<bool> stop{false}; // shutting down
    struct Lane { // one per source, so the slower one can be cut off
        std::atomic<bool> halt{false};
        std::atomic<void*> req{nullptr};
    };
    Lane lanes[3]; // lrclib, netease, kugou
    struct Failed {
        std::wstring key;
        ULONGLONG at;
        int tries; // misses in a row: one source down for good mustn't mean a lookup every few seconds
    };
    std::deque<Failed> failed; // songs no source could be reached for, not asked again for a while

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

    static bool Fits(const std::vector<LyricLine>& lines, const Match& m, bool exact) { // timed, written by people, and in the song's own script
        if (!Synced(lines)) return false;
        size_t latin = 0, other = 0;
        for (const LyricLine& l : lines) {
            if (Machine(l.text)) return false;
            for (wchar_t c : l.text) {
                if (c < 0x250 && std::iswalpha(c)) ++latin;
                else if (c >= 0x370 && (std::iswalpha(c) || c >= 0x2E80)) ++other;
            }
        }
        if (!m.latin || exact || latin + other == 0) return true; // a song named in its own script, or matched word for word: trusted as it is
        return latin >= (latin + other) * kForeign; // a latin title with cjk-only lyrics is someone else's song
    }

    // lrclib: of the candidates that are this song, the synced one closest in length; inst: the closest is instrumental, so none is right
    static std::vector<LyricLine> Best(const std::string& body, const Match& m, bool& inst, double maxOff = kMaxOff) {
        Val root;
        if (!Json{body}.Parse(root)) return {};
        std::vector<const Val*> all;
        if (root.kind == Val::kArr)
            for (const Val& v : root.items) all.push_back(&v);
        else if (root.kind == Val::kObj) all.push_back(&root);
        std::vector<std::pair<double, const Val*>> synced;
        size_t bare = 0;
        double bareOff = 1e9;
        for (const Val* o : all) {
            std::wstring name = Wide(o->Text("trackName"));
            double len = o->Number("duration");
            if (!m.Title(name) || !m.Artist(Names(Wide(o->Text("artistName")))) || !m.Length(len, maxOff)) continue; // another song that shares a word or two
            double d = m.seconds > 0 && len > 0 ? std::fabs(len - m.seconds) : 0;
            if (o->Flag("instrumental")) ++bare, bareOff = (std::min)(bareOff, d);
            else if (!o->Text("syncedLyrics").empty()) synced.push_back({d, o});
        }
        std::stable_sort(synced.begin(), synced.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (bare && bare >= synced.size() && (synced.empty() || bareOff <= synced[0].first + kExactOff)) { // most copies, the nearest among them, say no words
            inst = true;
            return {};
        }
        for (const auto& [d, o] : synced) {
            auto lines = ParseLrc(Wide(o->Text("syncedLyrics")));
            if (Fits(lines, m, m.Exact(Wide(o->Text("trackName")), Names(Wide(o->Text("artistName")))))) return lines;
        }
        return {};
    }

    std::vector<LyricLine> Netease(Lane& ln, const std::wstring& title, const std::wstring& artist, const Match& m, bool& reached, bool& inst) { // strong on hip-hop and pop
        const std::wstring bare = BareTitle(title);
        const std::wstring queries[3] = {artist + L" " + title, artist + L" " + bare, bare}; // with the artist, the bare title, then the title alone
        for (int q = 0; q < 3; ++q) {
            if (q == 1 && bare == title) continue; // nothing was bare-able
            if (q == 2 && m.seconds <= 0) break; // the title alone needs the exact length to be trusted
            auto lines = NeteaseTry(ln, queries[q], m, q == 2, reached, inst);
            if (!lines.empty() || inst || ln.halt.load()) return lines;
        }
        return {};
    }

    // one search; strict: found by the title alone, so the length must match exactly
    std::vector<LyricLine> NeteaseTry(Lane& ln, const std::wstring& query, const Match& m, bool strict, bool& reached, bool& inst) {
        std::string body;
        if (!Get(ln, L"https://music.163.com/api/search/get?s=" + Esc(query) + L"&type=1&limit=8", body)) return {};
        reached = true;
        Val root;
        if (!Json{body}.Parse(root)) return {};
        const Val* result = root["result"];
        const Val* songs = result ? (*result)["songs"] : nullptr;
        if (!songs || songs->kind != Val::kArr) return {};
        for (const Val& song : songs->items) {
            if (ln.halt.load()) return {};
            std::wstring name = Wide(song.Text("name"));
            std::vector<std::wstring> aliases = song.Strings("alias"), trans = song.Strings("transNames"), names;
            aliases.insert(aliases.end(), trans.begin(), trans.end()); // a translated title counts only when netease itself gives it
            if (const Val* artists = song["artists"]; artists && artists->kind == Val::kArr)
                for (const Val& who : artists->items) {
                    for (const auto& n : Names(Wide(who.Text("name")))) names.push_back(n);
                    for (const auto& a : who.Strings("alias")) // the name they go by elsewhere ("Ye")
                        for (const auto& n : Names(a)) names.push_back(n);
                }
            double len = song.Number("duration") / 1000;
            if (!m.Title(name, aliases) || !m.Artist(names) || !m.Length(len, strict ? kExactOff : kMaxOff)) continue; // a remix by someone else, a cover, a different cut
            std::string lyric;
            if (!Get(ln, L"https://music.163.com/api/song/lyric/v1?id=" + std::to_wstring((long long)song.Number("id")) + L"&cp=false&lv=0&kv=0&tv=0&rv=0&yv=0&ytv=0&yrv=0", lyric)) continue;
            Val doc;
            if (!Json{lyric}.Parse(doc)) continue;
            bool exact = m.Exact(name, names);
            if (doc.Flag("pureMusic")) { // this very song, marked as having no words
                inst = exact && m.Length(len, kExactOff);
                return {};
            }
            if (const Val* yrc = doc["yrc"]) { // word-timed, when they have it
                auto words = ParseTimed(Wide(yrc->Text("lyric")), L'(', false);
                if (Fits(words, m, exact)) return words;
            }
            const Val* lrc = doc["lrc"];
            auto lines = lrc ? ParseLrc(Wide(lrc->Text("lyric"))) : std::vector<LyricLine>{};
            if (Fits(lines, m, exact)) return lines;
        }
        return {};
    }

    std::vector<LyricLine> Kugou(Lane& ln, const std::wstring& title, const std::wstring& artist, const Match& m, bool& reached) { // word-timed across much of the western catalogue
        if (m.seconds <= 0) { // matched on title and exact length: without the length there's nothing to match, a definite no
            reached = true;
            return {};
        }
        const std::wstring queries[2] = {artist + L" " + title, BareTitle(title)}; // with the artist, then the bare title on its own
        for (int q = 0; q < 2; ++q) {
            auto lines = KugouTry(ln, queries[q], m, reached);
            if (!lines.empty() || ln.halt.load()) return lines;
        }
        return {};
    }

    std::vector<LyricLine> KugouKrc(Lane& ln, const Val& c) { // one lyric candidate, downloaded and unpacked
        std::wstring id = Wide(c.Text("id"));
        if (id.empty()) id = std::to_wstring((long long)c.Number("id"));
        std::string got;
        if (!Get(ln, L"https://lyrics.kugou.com/download?ver=1&client=pc&fmt=krc&charset=utf8&id=" + Esc(id) + L"&accesskey=" + Esc(Wide(c.Text("accesskey"))), got))
            return {};
        Val doc;
        std::string packed, krc;
        if (!Json{got}.Parse(doc) || !detail::Unbase64(doc.Text("content"), packed) || packed.size() < 8 || packed.compare(0, 4, "krc1") != 0) return {};
        static const uint8_t kKey[16] = {64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105}; // krc's fixed xor mask
        std::string raw(packed.begin() + 4, packed.end());
        for (size_t i = 0; i < raw.size(); ++i) raw[i] = (char)(raw[i] ^ kKey[i % 16]);
        if (!detail::Unzlib(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), krc, 1u << 20)) return {};
        std::wstring text = Wide(krc);
        if (text.find(L"[by:AI") != std::wstring::npos) return {}; // tagged as machine-made: hums guessed over an instrumental
        auto lines = ParseTimed(text, L'<', true);
        size_t head = 0;
        while (head < lines.size() && head < 3 && lines[head].start < 10 && lines[head].text.find(L" - ") != std::wstring::npos) ++head;
        lines.erase(lines.begin(), lines.begin() + head); // "Artist - Title" up top
        return lines;
    }

    std::vector<LyricLine> KugouTry(Lane& ln, const std::wstring& query, const Match& m, bool& reached) {
        std::string body;
        if (!Get(ln, L"https://mobileservice.kugou.com/api/v3/search/song?format=json&page=1&pagesize=8&showtype=1&keyword=" + Esc(query), body)) return {};
        reached = true;
        Val root;
        if (!Json{body}.Parse(root)) return {};
        const Val* data = root["data"];
        const Val* info = data ? (*data)["info"] : nullptr;
        if (!info || info->kind != Val::kArr) return {};
        constexpr size_t kCands = 3; // kugou's guesses for a recording, best first: further down they're other songs entirely
        for (const Val& song : info->items) {
            if (ln.halt.load()) return {};
            std::wstring listed = Wide(song.Text("songname"));
            std::vector<std::wstring> singers = Names(Wide(song.Text("singername")));
            double len = song.Number("duration");
            if (len <= 0 || !m.Title(listed) || !m.Artist(singers) || !m.Length(len, kKugouOff)) continue; // the title, a shared name and the length, all three
            std::string got;
            if (!Get(ln, L"https://lyrics.kugou.com/search?ver=1&man=yes&client=mobi&keyword=&hash=" + Esc(Wide(song.Text("hash"))) + L"&duration=" +
                             std::to_wstring((long long)std::lround(len * 1000)), got))
                continue;
            Val found;
            if (!Json{got}.Parse(found)) continue;
            const Val* cands = found["candidates"];
            if (!cands || cands->kind != Val::kArr) continue;
            for (size_t k = 0; k < cands->items.size() && k < kCands && !ln.halt.load(); ++k) {
                const Val& c = cands->items[k];
                std::wstring cs = Wide(c.Text("song")), who = Wide(c.Text("singer"));
                std::vector<std::wstring> cn = Names(who);
                if ((!cs.empty() && !m.Title(cs)) || (!who.empty() && !m.Artist(cn)) || !m.Length(c.Number("duration") / 1000, kKugouOff)) continue; // its guess for this hash may be another song
                bool exact = !(m.latin && m.latinBy) && m.Exact(listed, singers) && (cs.empty() || m.Exact(cs, cn.empty() ? singers : cn)); // a latin song by a latin name never takes cjk lines on trust: kugou's machine hums for instrumentals ("Moog City") list exactly so
                auto lines = KugouKrc(ln, c);
                if (Fits(lines, m, exact)) return lines;
            }
        }
        return {};
    }

    std::vector<LyricLine> Lrclib(Lane& ln, const std::wstring& title, const std::wstring& artist, const std::wstring& album, const Match& m, bool& reached, bool& inst) {
        std::string body;
        std::vector<LyricLine> lines;
        auto ask = [&](const std::wstring& url, double off) { // one search; an instrumental verdict ends the lookup too
            if (!Get(ln, url, body)) return false;
            reached = true;
            lines = Best(body, m, inst, off);
            return !lines.empty() || inst;
        };
        if (!album.empty() && m.seconds > 0) { // exact match first: one small answer
            std::wstring url = std::wstring(kApi) + L"get?track_name=" + Esc(title) + L"&artist_name=" + Esc(artist) + L"&album_name=" + Esc(album) +
                               L"&duration=" + std::to_wstring((int)std::lround(m.seconds));
            if (Get(ln, url, body, 1)) {
                lines = Best(body, m, inst);
                if (!lines.empty() || inst) {
                    reached = true;
                    return lines;
                }
            }
        }
        if (ask(std::wstring(kApi) + L"search?track_name=" + Esc(title) + L"&artist_name=" + Esc(artist), kMaxOff)) return lines;
        if (!reached) return {}; // not answering: the other sources decide
        if (ask(std::wstring(kApi) + L"search?q=" + Esc(artist + L" " + title), kMaxOff)) return lines; // loose words, strict matching
        const std::wstring bare = BareTitle(title);
        if (bare != title && ask(std::wstring(kApi) + L"search?track_name=" + Esc(bare) + L"&artist_name=" + Esc(artist), kMaxOff)) return lines;
        if (m.seconds > 0 && ask(std::wstring(kApi) + L"search?track_name=" + Esc(bare), kExactOff)) return lines; // the title alone: only the exact length is trusted
        return {};
    }

    void Keep(std::shared_ptr<Lyrics> doc) { // caller holds mu
        kept.push_back(std::move(doc));
        if (kept.size() > kKeep) kept.pop_front();
    }

    std::vector<LyricLine> Look(const Ask& a, bool& reached) { // both sources at once, the first with lyrics wins; reached: one answered, so empty means none
        const std::wstring title = CleanTitle(a.title), artist = MainArtist(a.artist);
        const Match song(a.title, a.artist, a.seconds);
        struct Out {
            std::vector<LyricLine> lines;
            bool reached = false, done = false, inst = false; // inst: this song has no words, whatever the others found
        } out[3];
        std::mutex m;
        std::condition_variable cv2;
        for (Lane& ln : lanes) {
            ln.halt.store(stop.load());
            ln.req.store(nullptr);
        }
        auto run = [&](int k) {
            std::vector<LyricLine> got;
            bool hit = false, inst = false;
            try {
                got = k == 0 ? Lrclib(lanes[0], title, artist, a.album, song, hit, inst)
                    : k == 1 ? Netease(lanes[1], title, artist, song, hit, inst)
                             : Kugou(lanes[2], title, artist, song, hit);
            } catch (...) {
                OutputDebugStringW(L"[sys] lyrics source failed\n");
            }
            std::lock_guard g(m);
            out[k].lines = std::move(got);
            out[k].reached = hit;
            out[k].inst = inst;
            out[k].done = true;
            cv2.notify_all();
        };
        std::thread t0(run, 0), t1(run, 1), t2(run, 2);
        {
            std::unique_lock g(m);
            auto found = [&](int k) { return out[k].done && !out[k].lines.empty(); };
            auto worded = [&] { return (found(1) && Worded(out[1].lines)) || (found(2) && Worded(out[2].lines)); };
            auto all = [&] { return out[0].done && out[1].done && out[2].done; };
            auto bare = [&] { return out[0].inst || out[1].inst; }; // lrclib or netease say instrumental: nothing shows, not even early
            cv2.wait(g, [&] { return found(0) || found(1) || found(2) || bare() || all(); });
            auto judged = [&] { return out[0].done && out[1].done; }; // the two sources that can call a song instrumental
            if (!bare() && !judged()) // found elsewhere first: lrclib and netease get a moment to call it instrumental before anything shows
                cv2.wait_for(g, kVerdictWait, [&] { return judged() || bare() || stop.load(); });
            if (!bare() && !worded() && !all()) { // show what came first now; word timing from the slower sources replaces it if it arrives
                for (int k : {0, 1, 2})
                    if (found(k)) {
                        auto early = std::make_shared<Lyrics>();
                        early->key = a.key;
                        early->lines = out[k].lines;
                        for (LyricLine& l : early->lines) Placeholder(l);
                        early->done = early->interim = true;
                        std::lock_guard lock(mu);
                        Keep(std::move(early));
                        break;
                    }
                auto until = std::chrono::steady_clock::now() + std::chrono::seconds(25); // the early lines show meanwhile, so waiting costs nothing you can see
                while (!worded() && !bare() && !all() && !stop.load() && !queued.load() && std::chrono::steady_clock::now() < until)
                    cv2.wait_for(g, std::chrono::milliseconds(250));
            }
        }
        for (int k = 0; k < 3; ++k)
            if (!out[k].done) detail::HttpCancel(lanes[k].halt, lanes[k].req); // the slower ones are no longer needed
        t0.join();
        t1.join();
        t2.join();
        reached = out[0].reached && out[1].reached && out[2].reached; // "no lyrics" only counts when every source answered; a timeout means ask again
        if (out[0].inst || out[1].inst) { // an instrumental: no words beat a machine's hums or another song's lyrics
            reached = true;
            return {};
        }
        int pick = -1;
        for (int k : {1, 2, 0}) // word-timed first: netease's yrc, then kugou, then lrclib's rare enhanced lrc
            if (pick < 0 && Worded(out[k].lines)) pick = k;
        for (int k : {0, 1, 2}) // otherwise lrclib's lines are usually the finest
            if (pick < 0 && !out[k].lines.empty()) pick = k;
        if (pick < 0) return {};
        for (LyricLine& l : out[pick].lines) Placeholder(l); // lines without word timing still light word by word
        return std::move(out[pick].lines);
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
                queued = false;
                busyKey = a.key;
            }
            if (auto disk = DiskLoad(a.key)) { // played before: straight from disk, no network at all
                std::lock_guard lock(mu);
                busyKey.clear();
                Keep(std::move(disk));
                continue;
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
            {
                std::lock_guard lock(mu);
                busyKey.clear();
                if (stop.load()) return;
                if (doc->lines.empty() && !reached) { // offline, or a source didn't answer: not remembered, asked again in a while, a longer while each time
                    int tries = 1;
                    for (auto it = failed.begin(); it != failed.end(); ++it)
                        if (it->key == doc->key) {
                            tries = it->tries + 1;
                            failed.erase(it);
                            break;
                        }
                    failed.push_back({doc->key, GetTickCount64(), tries});
                    if (failed.size() > 16) failed.pop_front();
                    continue;
                }
                for (auto it = kept.begin(); it != kept.end();) { // the final version replaces an early one, keeping a nudge made meanwhile
                    if ((*it)->key == doc->key) {
                        doc->offsetMs = (*it)->offsetMs;
                        it = kept.erase(it);
                    } else {
                        ++it;
                    }
                }
                Keep(doc);
            }
            DiskSave(*doc); // next time this song plays, it's instant
            if (++saved % 16 == 1) DiskPrune(); // now and then, not every song
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
                if (f.key == key && GetTickCount64() - f.at < kFailWaitMs << (2 * (std::min)(f.tries - 1, kFailSteps))) return; // 15 s, 1 min, 4 min, 16 min, about an hour
            impl->next = {key, title, artist, album, seconds}; // a newer song replaces one still waiting
            impl->queued = true;
        }
        if (!impl->th.joinable()) impl->th = std::thread([p = impl.get()] { p->Loop(); }); // one worker, started the first time lyrics are wanted
        impl->cv.notify_one();
    } catch (...) {
        OutputDebugStringW(L"[sys] lyrics request failed\n");
    }
}

void LyricSrc::SetOffset(const std::wstring& key, int ms) {
    if (!impl) return;
    std::shared_ptr<Lyrics> copy;
    {
        std::lock_guard lock(impl->mu);
        for (auto& d : impl->kept)
            if (d->key == key) {
                copy = std::make_shared<Lyrics>(*d);
                copy->offsetMs = ms;
                d = copy;
            }
    }
    if (copy && !copy->interim) DiskSave(*copy); // an early version waits: the final one carries the nudge and saves it
}

std::shared_ptr<const Lyrics> LyricSrc::Get(const std::wstring& key) const {
    if (!impl) return nullptr;
    std::lock_guard lock(impl->mu);
    for (auto it = impl->kept.rbegin(); it != impl->kept.rend(); ++it)
        if ((*it)->key == key) return *it;
    return nullptr;
}

}
